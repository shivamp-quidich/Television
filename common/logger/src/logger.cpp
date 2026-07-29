// Add spdlog logger
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <vector>
#include <string>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <mutex>

// Global logger instance
static std::shared_ptr<spdlog::logger> g_logger = nullptr;

// Cache for child loggers (thread-safe)
static std::mutex logger_mutex;  // Single mutex for all logger operations
static std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> module_loggers;

void initLogger(const std::string &log_file)
{
    std::lock_guard<std::mutex> lock(logger_mutex);
    
    // Prevent double initialization
    if (g_logger)
    {
        return;
    }

    // Ensure logs directory exists
    std::filesystem::create_directories("logs");

    try
    {
        // Rotate when file reaches 500 MB, keep 5 rotated files
        const std::size_t max_size = 500ULL * 1024 * 1024; // 500 MB
        const std::size_t max_files = 5;
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file, max_size, max_files);
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};

        // Pattern: [level][YYYY-MM-DD HH:MM:SS.mmm][module_name][message]
        const std::string plain_pattern = "[%l][%Y-%m-%d %H:%M:%S.%e][%n][%v]";
        
        // Console: color the log level, yellow timestamp, and cyan module name
        const std::string console_pattern = "[%^%l%$][\033[33m%Y-%m-%d %H:%M:%S.%e\033[0m][\033[36m%n\033[0m][%v]";

        console_sink->set_pattern(console_pattern);
        file_sink->set_pattern(plain_pattern);

        g_logger = std::make_shared<spdlog::logger>("stiqy", sinks.begin(), sinks.end());
        spdlog::register_logger(g_logger);
        g_logger->set_level(spdlog::level::info);
        
        // Flush on error and above (critical)
        g_logger->flush_on(spdlog::level::err);
        
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        std::cerr << "Logger initialization failed: " << ex.what() << std::endl;
    }
}

std::shared_ptr<spdlog::logger> getQLogger()
{
    // ✅ Thread-safe lazy initialization using double-checked locking
    if (!g_logger)
    {
        std::lock_guard<std::mutex> lock(logger_mutex);
        if (!g_logger)  // Double-check after acquiring lock
        {
            initLogger("logs/stiqy.log");
        }
    }
    return g_logger;
}

std::shared_ptr<spdlog::logger> getModuleLogger(const std::string &module_name)
{
    std::lock_guard<std::mutex> lock(logger_mutex);
    
    // Check if logger already exists
    auto it = module_loggers.find(module_name);
    if (it != module_loggers.end())
    {
        return it->second;
    }
    
    // Ensure parent logger is initialized (already thread-safe via lock)
    if (!g_logger)
    {
        initLogger("logs/stiqy.log");
    }
    
    // Create child logger that shares the same sinks as parent
    auto module_logger = std::make_shared<spdlog::logger>(
        module_name, 
        g_logger->sinks().begin(), 
        g_logger->sinks().end()
    );
    
    // Inherit settings from parent
    module_logger->set_level(g_logger->level());
    module_logger->flush_on(g_logger->flush_level());
    
    // Register and cache
    try
    {
        spdlog::register_logger(module_logger);
        module_loggers[module_name] = module_logger;
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        // Logger might already be registered (shouldn't happen with our cache)
        std::cerr << "Failed to register logger '" << module_name << "': " << ex.what() << std::endl;
    }
    
    return module_logger;
}

void setGlobalLogLevel(spdlog::level::level_enum level)
{
    std::lock_guard<std::mutex> lock(logger_mutex);
    
    if (g_logger)
    {
        g_logger->set_level(level);
    }
    
    // Update all child loggers
    for (auto &[name, logger] : module_loggers)
    {
        logger->set_level(level);
    }
    
    spdlog::info("Global log level set to: {}", spdlog::level::to_string_view(level));
}