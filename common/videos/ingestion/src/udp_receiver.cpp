#include "udp_receiver.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logger.h"

namespace {
constexpr std::uint8_t kFreeDType = 0xD1;
constexpr int kFreeDLen = 29;
// The Stype simulator prefixes the FreeD payload with a sync header:
// "STQ1" + uint32 frame id + uint64 timestamp, all little-endian.
constexpr char kSyncMagic[4] = {'S', 'T', 'Q', '1'};
constexpr int kSyncHeaderLen = 16;
constexpr int kSyncPacketLen = kSyncHeaderLen + kFreeDLen;
constexpr int kRecvTimeoutMs = 200;
constexpr int kMaxDatagram = 2048;

std::shared_ptr<spdlog::logger> log()
{
    static auto logger = getModuleLogger("udp_receiver");
    return logger;
}

std::int32_t decodeInt24(const std::uint8_t* buf, int off)
{
    const std::uint32_t v = (static_cast<std::uint32_t>(buf[off]) << 16)
                          | (static_cast<std::uint32_t>(buf[off + 1]) << 8)
                          | static_cast<std::uint32_t>(buf[off + 2]);
    return (v & 0x800000u) ? static_cast<std::int32_t>(v | 0xFF000000u)
                           : static_cast<std::int32_t>(v);
}

std::uint32_t decodeUint24(const std::uint8_t* buf, int off)
{
    return (static_cast<std::uint32_t>(buf[off]) << 16)
         | (static_cast<std::uint32_t>(buf[off + 1]) << 8)
         | static_cast<std::uint32_t>(buf[off + 2]);
}
} // namespace

UdpReceiver::UdpReceiver() = default;

UdpReceiver::~UdpReceiver()
{
    stop();
}

bool UdpReceiver::openSocket_(const std::string& bind_ip, int port, std::string& error)
{
    closeSocket_();

    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }

    const int reuse = 1;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = kRecvTimeoutMs * 1000;
    ::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
        error = "invalid bind IP: " + bind_ip;
        closeSocket_();
        return false;
    }

    if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = std::string("bind(") + bind_ip + ":" + std::to_string(port)
              + ") failed: " + std::strerror(errno);
        closeSocket_();
        return false;
    }

    return true;
}

void UdpReceiver::closeSocket_()
{
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool UdpReceiver::openRawOutput_(const std::string& path, std::string& error)
{
    raw_output_.close();

    std::error_code filesystem_error;
    const std::filesystem::path output_path(path);
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "cannot create UDP output directory: " + filesystem_error.message();
        return false;
    }

    // Truncate removes data from the previous app/receiver session.
    raw_output_.open(path, std::ios::out | std::ios::trunc);
    if (!raw_output_) {
        error = "cannot open raw UDP output: " + path;
        return false;
    }
    return true;
}

bool UdpReceiver::parseFreeD(const std::uint8_t* buf, int len, STypeState::CameraData& out)
{
    // Accept both the simulator's sync-wrapped datagram and the bare 29-byte
    // payload a real FreeD source sends.
    if (len >= kSyncPacketLen && std::memcmp(buf, kSyncMagic, sizeof(kSyncMagic)) == 0) {
        buf += kSyncHeaderLen;
        len = kFreeDLen;
    }
    if (len < kFreeDLen || buf[0] != kFreeDType)
        return false;

    out = {};
    out.camera_id = buf[1];
    out.pan_deg = static_cast<float>(decodeInt24(buf, 2)) / 32768.0f;
    out.tilt_deg = static_cast<float>(decodeInt24(buf, 5)) / 32768.0f;
    out.roll_deg = static_cast<float>(decodeInt24(buf, 8)) / 32768.0f;
    out.x_mm = static_cast<float>(decodeInt24(buf, 11)) / 64.0f;
    out.z_mm = static_cast<float>(decodeInt24(buf, 14)) / 64.0f;
    out.y_mm = static_cast<float>(decodeInt24(buf, 17)) / 64.0f;
    out.zoom_raw = static_cast<int>(decodeUint24(buf, 20));
    out.focus_raw = static_cast<int>(decodeUint24(buf, 23));
    out.is_valid = true;
    out.last_update = std::chrono::steady_clock::now();
    return true;
}

bool UdpReceiver::start(const std::string& bind_ip, int port, const std::string& raw_output_path)
{
    if (port <= 0 || port > 65535) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.last_error = "port must be in 1..65535";
        status_.listening = false;
        return false;
    }

    stop();

    std::string error;
    if (!openSocket_(bind_ip, port, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.bind_ip = bind_ip;
        status_.port = port;
        status_.listening = false;
        status_.last_error = error;
        log()->error("{}", error);
        return false;
    }
    if (!openRawOutput_(raw_output_path, error)) {
        closeSocket_();
        std::lock_guard<std::mutex> lock(mutex_);
        status_.bind_ip = bind_ip;
        status_.port = port;
        status_.raw_output_path = raw_output_path;
        status_.listening = false;
        status_.last_error = error;
        log()->error("{}", error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.bind_ip = bind_ip;
        status_.port = port;
        status_.raw_output_path = raw_output_path;
        status_.listening = true;
        status_.last_error.clear();
        status_.packets_received = 0;
        status_.bytes_received = 0;
        status_.last_packet_size = 0;
        status_.last_packet_valid = false;
        status_.last_camera = {};
    }

    running_ = true;
    thread_ = std::thread(&UdpReceiver::receiverLoop_, this);
    log()->info("UDP receiver listening on {}:{}; raw packets -> {}",
                bind_ip, port, raw_output_path);
    return true;
}

void UdpReceiver::stop()
{
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    closeSocket_();
    raw_output_.close();

    std::lock_guard<std::mutex> lock(mutex_);
    status_.listening = false;
}

UdpReceiver::Status UdpReceiver::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void UdpReceiver::receiverLoop_()
{
    std::uint8_t buffer[kMaxDatagram];
    while (running_) {
        const ssize_t received = ::recvfrom(socket_fd_, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            if (!running_)
                break;
            std::lock_guard<std::mutex> lock(mutex_);
            status_.last_error = std::string("recvfrom failed: ") + std::strerror(errno);
            log()->warn("{}", status_.last_error);
            continue;
        }

        STypeState::CameraData camera;
        const bool valid = parseFreeD(buffer, static_cast<int>(received), camera);

        if (valid) {
            raw_output_
                << std::fixed << std::setprecision(6)
                << "camera_id=" << camera.camera_id
                << " pan_deg=" << camera.pan_deg
                << " tilt_deg=" << camera.tilt_deg
                << " roll_deg=" << camera.roll_deg
                << " x_mm=" << camera.x_mm
                << " y_mm=" << camera.y_mm
                << " z_mm=" << camera.z_mm
                << " zoom_raw=" << camera.zoom_raw
                << " focus_raw=" << camera.focus_raw
                << '\n';
        } else {
            raw_output_ << "invalid_packet size=" << received << " bytes=";
            for (ssize_t i = 0; i < received; ++i) {
                if (i != 0)
                    raw_output_ << ' ';
                raw_output_ << static_cast<unsigned int>(buffer[i]);
            }
            raw_output_ << '\n';
        }
        raw_output_.flush();

        std::lock_guard<std::mutex> lock(mutex_);
        status_.packets_received += 1;
        status_.bytes_received += static_cast<std::uint64_t>(received);
        status_.last_packet_size = static_cast<int>(received);
        status_.last_packet_valid = valid;
        if (valid)
            status_.last_camera = camera;
    }
}
