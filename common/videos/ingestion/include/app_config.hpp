#pragma once

#include <string>

// Operator-facing settings persisted as a simple INI file. DeckLink device
// indices, UDP bind address, and window size all live here so a restart does
// not require remembering command-line flags.
struct AppConfig {
    // [decklink]
    int input_device = 0;
    int output_device = 1;

    // [udp]
    std::string udp_bind_ip = "127.0.0.1";
    int udp_port = 6305;
    std::string udp_send_ip = "127.0.0.1";
    int udp_send_port = 6305;
    // Hold received FreeD poses this many milliseconds before UI/plots/use, so
    // the tracking stream can be lagged to match video.
    int udp_recv_delay_ms = 0;
    std::string udp_raw_output_path = "udpOut/udp_raw.txt";

    // [window]
    int window_width = 1600;
    int window_height = 900;
    std::string window_title = "TV monitor";

    // [graphic]
    std::string graphic_data_dir = "data";

    // [recording] — stype_handover-style video + *_stype.csv pairs
    std::string recordings_dir = "/home/quidich/stype_handover/recordings";

    // Path the file was loaded from / last saved to (empty until a load/save).
    std::string path;

    // Loads key=value sections from path. Missing keys keep the current values,
    // so callers can start from defaults then overlay the file.
    bool load(const std::string& file_path, std::string& error);

    // Writes every managed field. Creates parent directories when needed.
    bool save(const std::string& file_path, std::string& error) const;

    // Resolves the config path to open: --config, TELEVISON_CONFIG, then the
    // usual search locations next to the working directory / binary.
    static std::string resolvePath(int argc, char* argv[]);
};
