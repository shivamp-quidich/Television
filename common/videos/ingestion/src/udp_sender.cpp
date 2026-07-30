#include "udp_sender.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "logger.h"

namespace {

constexpr std::uint8_t kFreeDType = 0xD1;
constexpr int kFreeDLen = 29;
constexpr char kSyncMagic[4] = {'S', 'T', 'Q', '1'};
constexpr int kSyncHeaderLen = 16;
constexpr int kPacketLen = kSyncHeaderLen + kFreeDLen;

std::shared_ptr<spdlog::logger> log()
{
    static auto logger = getModuleLogger("udp_sender");
    return logger;
}

void encodeInt24(std::uint8_t* buf, int off, std::int32_t value)
{
    const auto v = static_cast<std::uint32_t>(value) & 0xFFFFFFu;
    buf[off] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    buf[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    buf[off + 2] = static_cast<std::uint8_t>(v & 0xFF);
}

void encodeUint24(std::uint8_t* buf, int off, std::uint32_t value)
{
    encodeInt24(buf, off, static_cast<std::int32_t>(value & 0xFFFFFFu));
}

std::int32_t angleToFreeD(double deg)
{
    const double scaled = std::round(deg * 32768.0);
    return static_cast<std::int32_t>(std::clamp(scaled, -8388608.0, 8388607.0));
}

std::int32_t positionToFreeD(double mm)
{
    const double scaled = std::round(mm * 64.0);
    return static_cast<std::int32_t>(std::clamp(scaled, -8388608.0, 8388607.0));
}

std::uint8_t freedChecksum(const std::uint8_t* bytes28)
{
    unsigned sum = 0;
    for (int i = 0; i < 28; ++i)
        sum += bytes28[i];
    return static_cast<std::uint8_t>((0x40 - sum) & 0xFF);
}

} // namespace

UdpSender::UdpSender() = default;

UdpSender::~UdpSender()
{
    close();
}

void UdpSender::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    status_.configured = false;
}

bool UdpSender::configure(const std::string& host, int port, std::string& error)
{
    close();
    if (port < 1 || port > 65535) {
        error = "UDP send port must be in 1..65535";
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = std::string("UDP send socket() failed: ") + std::strerror(errno);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        error = "invalid UDP send host: " + host;
        return false;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        error = std::string("UDP send connect(") + host + ":" + std::to_string(port)
              + ") failed: " + std::strerror(errno);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    socket_fd_ = fd;
    status_.host = host;
    status_.port = port;
    status_.configured = true;
    status_.packets_sent = 0;
    status_.last_error.clear();
    log()->info("UDP sender ready -> {}:{}", host, port);
    return true;
}

bool UdpSender::sendRecord(const stype::Record& record, std::uint8_t camera_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_fd_ < 0 || !status_.configured) {
        status_.last_error = "UDP sender not configured";
        return false;
    }

    std::uint8_t packet[kPacketLen] = {};
    // STQ1 header (little-endian frame id + timestamp), matching UdpReceiver.
    std::memcpy(packet, kSyncMagic, 4);
    const auto frame_id = static_cast<std::uint32_t>(record.frame_id);
    const auto ts_ms = static_cast<std::uint64_t>(record.timestamp_ms);
    std::memcpy(packet + 4, &frame_id, sizeof(frame_id));
    std::memcpy(packet + 8, &ts_ms, sizeof(ts_ms));

    std::uint8_t* freed = packet + kSyncHeaderLen;
    freed[0] = kFreeDType;
    freed[1] = camera_id;
    encodeInt24(freed, 2, angleToFreeD(record.pan_deg));
    encodeInt24(freed, 5, angleToFreeD(record.tilt_deg));
    encodeInt24(freed, 8, angleToFreeD(record.roll_deg));
    // Match UdpReceiver slot order: X @11, Z @14, Y @17.
    encodeInt24(freed, 11, positionToFreeD(record.x_mm));
    encodeInt24(freed, 14, positionToFreeD(record.z_mm));
    encodeInt24(freed, 17, positionToFreeD(record.y_mm));
    encodeUint24(freed, 20, static_cast<std::uint32_t>(std::max<std::int64_t>(0, record.zoom_raw)));
    encodeUint24(freed, 23, static_cast<std::uint32_t>(std::max<std::int64_t>(0, record.focus_raw)));
    freed[26] = 0;
    freed[27] = 0;
    freed[28] = freedChecksum(freed);

    const ssize_t sent = ::send(socket_fd_, packet, sizeof(packet), 0);
    if (sent != static_cast<ssize_t>(sizeof(packet))) {
        status_.last_error = std::string("UDP send failed: ") + std::strerror(errno);
        return false;
    }
    status_.packets_sent += 1;
    status_.last_error.clear();
    return true;
}

UdpSender::Status UdpSender::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
