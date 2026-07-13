#pragma once

// ============================================================================
// core/log_compat.h — HD2D -> aima logging macro shim.
//
// The logging implementation now lives in aima_framework (core/log.h provides
// aima::init_logging + AIMA_TRACE/INFO/WARN/ERROR, spdlog-backed). HD2D's source
// has ~243 HD2D_TRACE/INFO/WARN/ERROR call sites across ~29 files; rather than
// rewriting them all, this one header maps the HD2D macros onto the aima ones.
// Every TU that used `#include "core/log.h"` now includes this instead.
// ============================================================================

#include "core/log.h"   // resolves to aima_framework/src/core/log.h (aima::*)

#define HD2D_TRACE(...) AIMA_TRACE(__VA_ARGS__)
#define HD2D_INFO(...)  AIMA_INFO(__VA_ARGS__)
#define HD2D_WARN(...)  AIMA_WARN(__VA_ARGS__)
#define HD2D_ERROR(...) AIMA_ERROR(__VA_ARGS__)
