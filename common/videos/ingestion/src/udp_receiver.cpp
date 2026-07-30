#include "udp_receiver.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "logger.h"

namespace {
constexpr std::uint8_t kStypeHfType = 0x0F;
constexpr int kStypeHfLen = 67;
// Optional sync header from the internal sender / Stype simulator:
// "STQ1" + uint32 frame id + uint64 timestamp, all little-endian.
constexpr char kSyncMagic[4] = {'S', 'T', 'Q', '1'};
constexpr int kSyncHeaderLen = 16;
constexpr int kSyncPacketLen = kSyncHeaderLen + kStypeHfLen;
constexpr int kRecvTimeoutMs = 200;
constexpr int kMaxDatagram = 2048;

std::shared_ptr<spdlog::logger> log()
{
    static auto logger = getModuleLogger("udp_receiver");
    return logger;
}

float readLeFloat(const std::uint8_t* bytes)
{
    float value = 0.0f;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

int normalizedToRaw(float normalized)
{
    if (!std::isfinite(normalized))
        return 0;
    const float clamped = std::clamp(normalized, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clamped * 65535.0f));
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

bool UdpReceiver::parseStypeHf(const std::uint8_t* buf, int len, STypeState::CameraData& out)
{
    // Accept STQ1-wrapped datagrams from the internal sender, and bare 67-byte
    // Stype HF payloads from a real tracking source.
    if (len >= kSyncPacketLen && std::memcmp(buf, kSyncMagic, sizeof(kSyncMagic)) == 0) {
        buf += kSyncHeaderLen;
        len = kStypeHfLen;
    }
    if (len < kStypeHfLen || buf[0] != kStypeHfType)
        return false;

    std::uint8_t checksum = 0;
    for (int i = 0; i < kStypeHfLen - 1; ++i)
        checksum = static_cast<std::uint8_t>(checksum + buf[i]);
    if (checksum != buf[kStypeHfLen - 1])
        return false;

    out = {};
    out.camera_id = buf[1] & 0x0F;
    out.packet_no = buf[5];
    // Packet stores metres; CameraData keeps millimetres.
    out.x_mm = readLeFloat(buf + 6) * 1000.0f;
    out.y_mm = readLeFloat(buf + 10) * 1000.0f;
    out.z_mm = readLeFloat(buf + 14) * 1000.0f;
    out.pan_deg = readLeFloat(buf + 18);
    out.tilt_deg = readLeFloat(buf + 22);
    out.roll_deg = readLeFloat(buf + 26);
    out.hfov_deg = readLeFloat(buf + 30);
    out.hf_ar = readLeFloat(buf + 34);
    out.focus_raw = normalizedToRaw(readLeFloat(buf + 38));
    out.zoom_raw = normalizedToRaw(readLeFloat(buf + 42));
    out.hf_k1 = readLeFloat(buf + 46);
    out.hf_k2 = readLeFloat(buf + 50);
    out.hf_cx_mm = readLeFloat(buf + 54);
    out.hf_cy_mm = readLeFloat(buf + 58);
    out.hf_pa_width_mm = readLeFloat(buf + 62);
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
    log()->info("UDP receiver listening on {}:{} (Stype HF); raw packets -> {}",
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
        const bool valid = parseStypeHf(buffer, static_cast<int>(received), camera);

        if (valid) {
            raw_output_
                << std::fixed << std::setprecision(6)
                << "camera_id=" << camera.camera_id
                << " packet_no=" << camera.packet_no
                << " pan_deg=" << camera.pan_deg
                << " tilt_deg=" << camera.tilt_deg
                << " roll_deg=" << camera.roll_deg
                << " x_mm=" << camera.x_mm
                << " y_mm=" << camera.y_mm
                << " z_mm=" << camera.z_mm
                << " zoom_raw=" << camera.zoom_raw
                << " focus_raw=" << camera.focus_raw
                << " hfov_deg=" << camera.hfov_deg
                << " hf_ar=" << camera.hf_ar
                << " hf_k1=" << camera.hf_k1
                << " hf_k2=" << camera.hf_k2
                << " hf_cx_mm=" << camera.hf_cx_mm
                << " hf_cy_mm=" << camera.hf_cy_mm
                << " hf_pa_width_mm=" << camera.hf_pa_width_mm
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
