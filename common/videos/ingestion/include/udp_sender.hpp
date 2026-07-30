#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "stype_csv_overlay.hpp"

// Sends Stype HF (0x0F, 67-byte) tracking datagrams, STQ1-wrapped, to a local
// UDP endpoint — used to loop recording CSV poses into UdpReceiver.
class UdpSender {
public:
    struct Status {
        bool configured = false;
        std::string host = "127.0.0.1";
        int port = 6305;
        std::uint64_t packets_sent = 0;
        std::string last_error;
    };

    UdpSender();
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    bool configure(const std::string& host, int port, std::string& error);
    void close();

    // Encodes the record as Stype HF (with STQ1 header) and sends one datagram.
    bool sendRecord(const stype::Record& record, std::uint8_t camera_id = 1);

    Status status() const;

private:
    mutable std::mutex mutex_;
    int socket_fd_ = -1;
    Status status_;
};
