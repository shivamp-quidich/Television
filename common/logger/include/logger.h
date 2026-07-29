#pragma once

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

// Initialize the logger (should be called once from main thread)
void initLogger(const std::string &log_file = "logs/stiqy.log");

// Retrieve the shared logger instance (thread-safe, initializes if necessary)
std::shared_ptr<spdlog::logger> getQLogger();

// Get a child logger with a custom name (thread-safe, shares sinks with parent)
std::shared_ptr<spdlog::logger> getModuleLogger(const std::string &module_name);

// Set log level for all loggers (affects parent and all children)
void setGlobalLogLevel(spdlog::level::level_enum level);
