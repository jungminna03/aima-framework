#pragma once

#include <spdlog/spdlog.h>

namespace aima {

// Initialize the global logger (colored console sink + a truncated file sink).
// Call once at startup, before anything else logs. `log_file` is created next to
// the cwd; pass an empty string for console-only.
void init_logging(const char* log_file = "aima.log");

} // namespace aima

// Convenience macros so call sites stay short and consistent.
#define AIMA_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define AIMA_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define AIMA_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define AIMA_ERROR(...) ::spdlog::error(__VA_ARGS__)
