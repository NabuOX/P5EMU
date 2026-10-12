// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary
//
// Logging foundation.
//
// Design constraints (these come from the iOS audit, see docs/research/):
//   * Must be usable from a signal-adjacent context, therefore no locking on
//     the hot path and no allocation for levels below the configured threshold.
//   * Must not depend on any OS API beyond write(2)-equivalent, because the
//     iOS app cannot own a console handle.
//   * Backend selection is a platform concern (apps/ios/, apps/...), NOT a
//     dependency of core/.

#ifndef P5EMU_CORE_LOG_H
#define P5EMU_CORE_LOG_H

#include <cstdint>

namespace p5emu {

enum class LogLevel : std::uint8_t {
  kTrace = 0,
  kDebug,
  kInfo,
  kWarning,
  kError,
  kFatal,
  kOff,
};

const char* LogLevelName(LogLevel level) noexcept;

// Process-wide log level. Lower numeric value == more verbose.
void SetLogLevel(LogLevel level) noexcept;
LogLevel GetLogLevel() noexcept;

bool ShouldLog(LogLevel level) noexcept;

// Single sink for Phase 0. Replaced by platform sinks (os_log, Android log,
// stderr) in later phases; core/ never learns which.
using LogSink = void (*)(LogLevel level, const char* message);

void SetLogSink(LogSink sink) noexcept;

// Formats into a fixed buffer and emits through the sink.
void LogWrite(LogLevel level, const char* message) noexcept;

// Convenience. Implemented in Log.cpp so call sites stay allocation-light.
void LogPrintf(LogLevel level, const char* format, ...) noexcept
    __attribute__((format(printf, 2, 3)));

#define P5EMU_LOG_TRACE(...) \
  ::p5emu::LogPrintf(::p5emu::LogLevel::kTrace, __VA_ARGS__)
#define P5EMU_LOG_DEBUG(...) \
  ::p5emu::LogPrintf(::p5emu::LogLevel::kDebug, __VA_ARGS__)
#define P5EMU_LOG_INFO(...) \
  ::p5emu::LogPrintf(::p5emu::LogLevel::kInfo, __VA_ARGS__)
#define P5EMU_LOG_WARN(...) \
  ::p5emu::LogPrintf(::p5emu::LogLevel::kWarning, __VA_ARGS__)
#define P5EMU_LOG_ERROR(...) \
  ::p5emu::LogPrintf(::p5emu::LogLevel::kError, __VA_ARGS__)

}  // namespace p5emu

#endif  // P5EMU_CORE_LOG_H
