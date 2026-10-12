// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary

#include "p5emu/core/Log.h"

#include <cstdarg>
#include <cstdio>

namespace p5emu {
namespace {

// Phase 0 defaults: quiet enough not to pollute CI, loud enough to be useful.
LogLevel g_level = LogLevel::kWarning;

void DefaultSink(LogLevel level, const char* message) {
  std::fprintf(stderr, "[p5emu][%s] %s\n", LogLevelName(level), message);
}

LogSink g_sink = &DefaultSink;

constexpr std::size_t kMessageCapacity = 1024;

}  // namespace

const char* LogLevelName(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kTrace:
      return "trace";
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarning:
      return "warn";
    case LogLevel::kError:
      return "error";
    case LogLevel::kFatal:
      return "fatal";
    case LogLevel::kOff:
      return "off";
  }
  return "unknown";
}

void SetLogLevel(LogLevel level) noexcept { g_level = level; }
LogLevel GetLogLevel() noexcept { return g_level; }

bool ShouldLog(LogLevel level) noexcept {
  return level != LogLevel::kOff && level >= g_level;
}

void SetLogSink(LogSink sink) noexcept {
  g_sink = (sink != nullptr) ? sink : &DefaultSink;
}

void LogWrite(LogLevel level, const char* message) noexcept {
  if (!ShouldLog(level)) {
    return;
  }
  g_sink(level, message != nullptr ? message : "");
}

void LogPrintf(LogLevel level, const char* format, ...) noexcept {
  if (!ShouldLog(level)) {
    return;
  }

  char buffer[kMessageCapacity];

  va_list args;
  va_start(args, format);
  const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  if (written < 0) {
    return;
  }
  if (static_cast<std::size_t>(written) >= sizeof(buffer)) {
    // Truncated. Emit a marker rather than silently losing the tail.
    std::snprintf(buffer + sizeof(buffer) - 4, 4, "...");
  }

  g_sink(level, buffer);
}

}  // namespace p5emu
