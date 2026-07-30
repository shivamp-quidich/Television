#include "app_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "logger.h"

namespace {

std::shared_ptr<spdlog::logger> log()
{
    static auto logger = getModuleLogger("app_config");
    return logger;
}

std::string trim(std::string s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool parseInt(const std::string& value, int& out)
{
    try {
        std::size_t idx = 0;
        const int v = std::stoi(trim(value), &idx);
        if (idx == 0)
            return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

void applyKey(AppConfig& cfg, const std::string& section, const std::string& key, const std::string& value)
{
    if (section == "decklink") {
        if (key == "input_device")
            parseInt(value, cfg.input_device);
        else if (key == "output_device")
            parseInt(value, cfg.output_device);
        return;
    }
    if (section == "udp") {
        if (key == "bind_ip")
            cfg.udp_bind_ip = trim(value);
        else if (key == "port")
            parseInt(value, cfg.udp_port);
        else if (key == "send_ip")
            cfg.udp_send_ip = trim(value);
        else if (key == "send_port")
            parseInt(value, cfg.udp_send_port);
        else if (key == "recv_delay_ms")
            parseInt(value, cfg.udp_recv_delay_ms);
        else if (key == "raw_output_path")
            cfg.udp_raw_output_path = trim(value);
        return;
    }
    if (section == "window") {
        if (key == "width")
            parseInt(value, cfg.window_width);
        else if (key == "height")
            parseInt(value, cfg.window_height);
        else if (key == "title")
            cfg.window_title = trim(value);
        return;
    }
    if (section == "graphic") {
        if (key == "data_dir")
            cfg.graphic_data_dir = trim(value);
        return;
    }
    if (section == "recording") {
        if (key == "recordings_dir")
            cfg.recordings_dir = trim(value);
    }
}

bool looksLikeConfigPath(const std::string& arg)
{
    if (arg.empty() || arg[0] == '-')
        return false;
    // Device indices are plain integers; everything else is treated as a path.
    std::size_t idx = 0;
    try {
        (void)std::stoi(arg, &idx);
        if (idx == arg.size())
            return false;
    } catch (...) {
    }
    return true;
}

} // namespace

bool AppConfig::load(const std::string& file_path, std::string& error)
{
    std::ifstream in(file_path);
    if (!in) {
        error = "cannot open config: " + file_path;
        return false;
    }

    std::string section;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            std::transform(section.begin(), section.end(), section.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            log()->warn("config {}:{}: ignoring line without '='", file_path, line_no);
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        applyKey(*this, section, key, value);
    }

    if (udp_port < 1)
        udp_port = 1;
    if (udp_port > 65535)
        udp_port = 65535;
    if (udp_send_port < 1)
        udp_send_port = 1;
    if (udp_send_port > 65535)
        udp_send_port = 65535;
    if (udp_send_ip.empty())
        udp_send_ip = udp_bind_ip;
    if (udp_recv_delay_ms < 0)
        udp_recv_delay_ms = 0;
    if (udp_recv_delay_ms > 5000)
        udp_recv_delay_ms = 5000;
    if (window_width < 640)
        window_width = 640;
    if (window_height < 480)
        window_height = 480;

    path = file_path;
    error.clear();
    return true;
}

bool AppConfig::save(const std::string& file_path, std::string& error) const
{
    std::error_code filesystem_error;
    const std::filesystem::path output_path(file_path);
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "cannot create config directory: " + filesystem_error.message();
        return false;
    }

    std::ofstream out(file_path, std::ios::out | std::ios::trunc);
    if (!out) {
        error = "cannot write config: " + file_path;
        return false;
    }

    out << "# Televison operator configuration\n"
        << "# Edit and restart, or use Save config in the status panel.\n"
        << "# CLI device indices override [decklink] when provided.\n\n"
        << "[decklink]\n"
        << "input_device = " << input_device << '\n'
        << "output_device = " << output_device << "\n\n"
        << "[udp]\n"
        << "bind_ip = " << udp_bind_ip << '\n'
        << "port = " << udp_port << '\n'
        << "send_ip = " << udp_send_ip << '\n'
        << "send_port = " << udp_send_port << '\n'
        << "recv_delay_ms = " << udp_recv_delay_ms << '\n'
        << "raw_output_path = " << udp_raw_output_path << "\n\n"
        << "[window]\n"
        << "width = " << window_width << '\n'
        << "height = " << window_height << '\n'
        << "title = " << window_title << "\n\n"
        << "[graphic]\n"
        << "data_dir = " << graphic_data_dir << "\n\n"
        << "[recording]\n"
        << "recordings_dir = " << recordings_dir << '\n';

    if (!out) {
        error = "failed while writing config: " + file_path;
        return false;
    }
    error.clear();
    return true;
}

std::string AppConfig::resolvePath(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--config" && i + 1 < argc)
            return argv[i + 1];
        if (arg.rfind("--config=", 0) == 0)
            return arg.substr(std::string("--config=").size());
    }

    if (const char* env = std::getenv("TELEVISON_CONFIG")) {
        if (env[0] != '\0')
            return env;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.rfind("--", 0) == 0)
            continue;
        if (looksLikeConfigPath(arg))
            return arg;
    }

    const char* candidates[] = {
        "config/televison.ini",
        "../config/televison.ini",
        "../../config/televison.ini",
    };
    for (const char* candidate : candidates) {
        if (std::filesystem::exists(candidate))
            return candidate;
    }

    return "config/televison.ini";
}
