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

constexpr std::uint8_t kStypeHfType = 0x0F;
constexpr int kStypeHfLen = 67;
constexpr char kSyncMagic[4] = {'S', 'T', 'Q', '1'};
constexpr int kSyncHeaderLen = 16;
constexpr int kPacketLen = kSyncHeaderLen + kStypeHfLen;

constexpr float kDefaultHfovDeg = 40.0f;
constexpr float kDefaultAspect = 16.0f / 9.0f;
constexpr float kDefaultPaWidthMm = 9.6f;

std::shared_ptr<spdlog::logger> log()
{
    static auto logger = getModuleLogger("udp_sender");
    return logger;
}

void writeLeFloat(std::uint8_t* packet, int offset, float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t), "Stype HF needs IEEE-754 floats");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    packet[offset] = static_cast<std::uint8_t>(bits & 0xFFu);
    packet[offset + 1] = static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
    packet[offset + 2] = static_cast<std::uint8_t>((bits >> 16) & 0xFFu);
    packet[offset + 3] = static_cast<std::uint8_t>((bits >> 24) & 0xFFu);
}

float resolveHfov(double hfov_deg)
{
    return (std::isfinite(hfov_deg) && hfov_deg > 0.5 && hfov_deg < 180.0)
        ? static_cast<float>(hfov_deg)
        : kDefaultHfovDeg;
}

float resolveAspect(double hf_ar)
{
    return (std::isfinite(hf_ar) && hf_ar > 0.0)
        ? static_cast<float>(hf_ar)
        : kDefaultAspect;
}

float resolvePaWidth(double hf_pa_width_mm)
{
    return (std::isfinite(hf_pa_width_mm) && hf_pa_width_mm > 0.0)
        ? static_cast<float>(hf_pa_width_mm)
        : kDefaultPaWidthMm;
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
    log()->info("UDP sender ready -> {}:{} (Stype HF)", host, port);
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
    std::memcpy(packet, kSyncMagic, 4);
    const auto frame_id = static_cast<std::uint32_t>(record.frame_id);
    const auto ts_ms = static_cast<std::uint64_t>(record.timestamp_ms);
    std::memcpy(packet + 4, &frame_id, sizeof(frame_id));
    std::memcpy(packet + 8, &ts_ms, sizeof(ts_ms));

    std::uint8_t* hf = packet + kSyncHeaderLen;
    const float hfov = resolveHfov(record.hfov_deg);
    const float aspect = resolveAspect(record.hf_ar);
    const float pa_width = resolvePaWidth(record.hf_pa_width_mm);
    const int zoom_raw = static_cast<int>(std::clamp(record.zoom_raw, std::int64_t{0},
                                                     std::int64_t{65535}));
    const int focus_raw = static_cast<int>(std::clamp(record.focus_raw, std::int64_t{0},
                                                      std::int64_t{65535}));

    hf[0] = kStypeHfType;
    hf[1] = static_cast<std::uint8_t>(camera_id & 0x0F);
    hf[5] = static_cast<std::uint8_t>(record.frame_id & 0xFF);

    writeLeFloat(hf, 6, static_cast<float>(record.x_mm / 1000.0));
    writeLeFloat(hf, 10, static_cast<float>(record.y_mm / 1000.0));
    writeLeFloat(hf, 14, static_cast<float>(record.z_mm / 1000.0));
    writeLeFloat(hf, 18, static_cast<float>(record.pan_deg));
    writeLeFloat(hf, 22, static_cast<float>(record.tilt_deg));
    writeLeFloat(hf, 26, static_cast<float>(record.roll_deg));
    writeLeFloat(hf, 30, hfov);
    writeLeFloat(hf, 34, aspect);
    writeLeFloat(hf, 38, focus_raw / 65535.0f);
    writeLeFloat(hf, 42, zoom_raw / 65535.0f);
    writeLeFloat(hf, 46, static_cast<float>(record.hf_k1));
    writeLeFloat(hf, 50, static_cast<float>(record.hf_k2));
    writeLeFloat(hf, 54, static_cast<float>(record.hf_cx_mm));
    writeLeFloat(hf, 58, static_cast<float>(record.hf_cy_mm));
    writeLeFloat(hf, 62, pa_width);

    std::uint8_t checksum = 0;
    for (int i = 0; i < kStypeHfLen - 1; ++i)
        checksum = static_cast<std::uint8_t>(checksum + hf[i]);
    hf[kStypeHfLen - 1] = checksum;

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
