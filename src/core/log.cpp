#include "core/log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <vector>

namespace aima {

void init_logging(const char* log_file) {
    // Console + a truncated file next to the cwd: a double-clicked exe has no
    // console, so without the file every play session is undebuggable hearsay.
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (log_file && *log_file) {
        try {
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                log_file, /*truncate*/ true));
        } catch (const spdlog::spdlog_ex&) {
            // read-only cwd: console-only is fine
        }
    }
    auto logger = std::make_shared<spdlog::logger>("aima", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("%^[%T.%e] [%l]%$ %v");
#ifdef NDEBUG
    spdlog::set_level(spdlog::level::info);
#else
    spdlog::set_level(spdlog::level::trace);
#endif
    AIMA_INFO("logging initialized");
}

} // namespace aima
