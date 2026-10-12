// SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary

#include "TestHarness.h"
#include "p5emu/core/Log.h"
#include "p5emu/core/Status.h"
#include "p5emu/core/Version.h"

namespace {

using namespace p5emu;   // NOLINT(build/namespaces) - test-local convenience

std::vector<std::string> g_captured;

void CaptureSink(LogLevel level, const char* message) {
  g_captured.push_back(std::string(LogLevelName(level)) + ":" + message);
}

P5EMU_TEST(Log, RespectsLevelThreshold) {
  const LogLevel saved = GetLogLevel();
  g_captured.clear();
  SetLogSink(&CaptureSink);

  SetLogLevel(LogLevel::kWarning);
  P5EMU_LOG_TRACE("trace-should-be-dropped");
  P5EMU_LOG_DEBUG("debug-should-be-dropped");
  P5EMU_LOG_INFO("info-should-be-dropped");
  P5EMU_LOG_WARN("warn-should-be-emitted");
  P5EMU_LOG_ERROR("error-should-be-emitted");

  CHECK_EQ(g_captured.size(), static_cast<std::size_t>(2));
  if (g_captured.size() == 2) {
    CHECK_EQ(g_captured[0], std::string("warn:warn-should-be-emitted"));
    CHECK_EQ(g_captured[1], std::string("error:error-should-be-emitted"));
  }

  SetLogLevel(LogLevel::kOff);
  P5EMU_LOG_ERROR("error-should-be-dropped-at-off");
  CHECK_EQ(g_captured.size(), static_cast<std::size_t>(2));

  SetLogLevel(saved);
  SetLogSink(nullptr);
}

P5EMU_TEST(Log, ShouldLogMatchesEmitBehaviour) {
  const LogLevel saved = GetLogLevel();

  SetLogLevel(LogLevel::kInfo);
  CHECK(!ShouldLog(LogLevel::kDebug));
  CHECK(ShouldLog(LogLevel::kInfo));
  CHECK(ShouldLog(LogLevel::kFatal));

  SetLogLevel(LogLevel::kOff);
  CHECK(!ShouldLog(LogLevel::kFatal));

  SetLogLevel(saved);
}

P5EMU_TEST(Status, DefaultIsOkAndNamesAreDistinct) {
  const Status s;
  CHECK(s.ok());
  CHECK_EQ(static_cast<std::int32_t>(s.code()),
           static_cast<std::int32_t>(StatusCode::kOk));

  CHECK(std::strcmp(StatusCodeName(StatusCode::kUnimplemented),
                    "unimplemented") == 0);
  CHECK(std::strcmp(StatusCodeName(StatusCode::kUnimplementedInstruction),
                    "unimplemented-instruction") == 0);
  CHECK(std::strcmp(StatusCodeName(StatusCode::kDecodeError),
                    "decode-error") == 0);
}

P5EMU_TEST(Version, StringMatchesConstant) {
  CHECK_EQ(std::string(VersionString()),
           std::to_string(kVersion.major) + "." +
               std::to_string(kVersion.minor) + "." +
               std::to_string(kVersion.patch));
}

}  // namespace
