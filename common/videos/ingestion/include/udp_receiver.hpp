#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "shared_state.h"

// Background UDP listener for Stype FreeD D1 tracking packets.
// Binds a local address (default 127.0.0.1) and port (default 6305).
class UdpReceiver {
public:
    struct Status {
        bool listening = false;
        std::string bind_ip = "127.0.0.1";
        int port = 6305;
        std::uint64_t packets_received = 0;
        std::uint64_t bytes_received = 0;
        int last_packet_size = 0;
        bool last_packet_valid = false;
        std::string raw_output_path;
        std::string last_error;
        STypeState::CameraData last_camera;
    };

    UdpReceiver();
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Starts a new receiver session. raw_output_path is truncated before the
    // first packet, then each FreeD datagram is written as readable pose text.
    bool start(const std::string& bind_ip, int port, const std::string& raw_output_path);

    void stop();

    Status status() const;

private:
    void receiverLoop_();
    bool openSocket_(const std::string& bind_ip, int port, std::string& error);
    bool openRawOutput_(const std::string& path, std::string& error);
    void closeSocket_();

    static bool parseFreeD(const std::uint8_t* buf, int len, STypeState::CameraData& out);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int socket_fd_ = -1;
    std::ofstream raw_output_;
    Status status_;
};
