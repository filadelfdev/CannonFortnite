#pragma once
#include "core/cf_api.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

namespace cf::log {

enum class Level { Trace, Info, Warn, Error };

CF_CORE_API void SetLevel(Level);
CF_CORE_API void Line(Level, const char* fmt, ...);

#define CF_INFO(...)  cf::log::Line(cf::log::Level::Info,  __VA_ARGS__)
#define CF_WARN(...)  cf::log::Line(cf::log::Level::Warn,  __VA_ARGS__)
#define CF_ERR(...)   cf::log::Line(cf::log::Level::Error, __VA_ARGS__)
#define CF_TRACE(...) cf::log::Line(cf::log::Level::Trace, __VA_ARGS__)

} // namespace cf::log
