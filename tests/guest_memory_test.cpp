// SPDX-License-Identifier: Apache-2.0
//
// GuestMemoryPort tests.
//
// Covers the contract that a future port must also satisfy: bounded access,
// atomic all-or-nothing failure, separate code/data permissions, deterministic
// Status, SMC notification, and the direct-access optimisation query.
//
// Note what is NOT here: any PS5 virtual memory. LinearGuestMemory is a test
// and Phase-1 execution substrate only.

#include <cstring>

#include "TestHarness.h"
#include "p5emu/cpu/LinearGuestMemory.h"

namespace {

using namespace p5emu;         // NOLINT - test-local convenience
using namespace p5emu::memory; // NOLINT

constexpr GuestAddr kBase = 0x0000'1000ull;
constexpr GuestSize kSize = 0x0000'0100ull;

P5EMU_TEST(GuestMemory, ReadWriteRoundTrip) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);

  std::uint64_t v = 0;
  REQUIRE(mem.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, std::uint64_t{0});

  REQUIRE(mem.WriteData(kBase, 8, 0x0123'4567'89ab'cdefull).ok());
  REQUIRE(mem.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, 0x0123'4567'89ab'cdefull);

  // Little-endian byte order must be visible, because the interpreter relies on
  // the port, not on the host's byte order.
  std::uint8_t raw[8] = {};
  REQUIRE(mem.DebugPeek(kBase, raw, 8).ok());
  CHECK_EQ(static_cast<int>(raw[0]), 0xef);
  CHECK_EQ(static_cast<int>(raw[7]), 0x01);
}

P5EMU_TEST(GuestMemory, AllAccessWidths) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  std::uint64_t v = 0;

  REQUIRE(mem.WriteData(kBase, 1, 0xffull).ok());
  REQUIRE(mem.ReadData(kBase, 1, &v).ok());
  CHECK_EQ(v, 0xffull);

  REQUIRE(mem.WriteData(kBase, 2, 0xbeefull).ok());
  REQUIRE(mem.ReadData(kBase, 2, &v).ok());
  CHECK_EQ(v, 0xbeefull);

  REQUIRE(mem.WriteData(kBase, 4, 0xdeadbeefull).ok());
  REQUIRE(mem.ReadData(kBase, 4, &v).ok());
  CHECK_EQ(v, 0xdeadbeefull);

  REQUIRE(mem.WriteData(kBase, 8, ~std::uint64_t{0}).ok());
  REQUIRE(mem.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, ~std::uint64_t{0});
}

P5EMU_TEST(GuestMemory, ZeroExtendedLoadIsPartialNotGarbage) {
  // A 4-byte read must not touch the 4 bytes above it. This is the behaviour the
  // IR's "loads zero-extend to 64" rule depends on.
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  REQUIRE(mem.WriteData(kBase, 8, 0xaaaa'bbbb'cccc'ddddull).ok());

  std::uint64_t v = 0;
  REQUIRE(mem.ReadData(kBase, 4, &v).ok());
  CHECK_EQ(v, 0xcccc'ddddull);
}

P5EMU_TEST(GuestMemory, BoundaryAccessIsAllowedExactlyAtTheEdge) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  const GuestAddr last = kBase + kSize - 8;

  std::uint64_t v = 0;
  REQUIRE(mem.ReadData(last, 8, &v).ok());
  REQUIRE(mem.WriteData(last, 8, 0x1111'2222'3333'4444ull).ok());
  REQUIRE(mem.ReadData(last, 8, &v).ok());
  CHECK_EQ(v, 0x1111'2222'3333'4444ull);
}

P5EMU_TEST(GuestMemory, OutOfRangeAccessFailsDeterministically) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  std::uint64_t v = 0;

  // One byte past the end: the first byte is mapped, the 8-byte access is not.
  // This is the case that a naive bounds check gets wrong.
  const GuestAddr straddling = kBase + kSize - 4;
  const Status s1 = mem.ReadData(straddling, 8, &v);
  const Status s2 = mem.ReadData(straddling, 8, &v);
  CHECK(!s1.ok());
  CHECK(s1.code() == s2.code());  // deterministic

  // Entirely below the region.
  CHECK(mem.ReadData(kBase - 1, 1, &v).code() == StatusCode::kUnmappedAccess);

  // Address-space wrap must be rejected explicitly: silent wraparound in a guest
  // memory port is a sandbox-escape primitive.
  CHECK(mem.ReadData(~GuestAddr{0}, 8, &v).code() ==
        StatusCode::kInvalidArgument);
  CHECK(mem.ReadData(~GuestAddr{0} - 2, 8, &v).code() ==
        StatusCode::kInvalidArgument);
}

P5EMU_TEST(GuestMemory, InvalidWidthIsRejected) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  std::uint64_t v = 0;
  CHECK(mem.ReadData(kBase, 0, &v).code() == StatusCode::kInvalidArgument);
  CHECK(mem.ReadData(kBase, 3, &v).code() == StatusCode::kInvalidArgument);
  CHECK(mem.ReadData(kBase, 5, &v).code() == StatusCode::kInvalidArgument);
  CHECK(mem.WriteData(kBase, 16, 0).code() == StatusCode::kInvalidArgument);
}

P5EMU_TEST(GuestMemory, FailedWriteLeavesMemoryByteIdentical) {
  // Atomicity: a rejected write must not partially apply. If it did, an
  // interpreter bug could corrupt guest memory before the fault is noticed.
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  REQUIRE(mem.WriteData(kBase, 8, 0x0000'0000'0000'0000ull).ok());

  // Straddling write that must fail.
  const Status st = mem.WriteData(kBase + kSize - 4, 8, ~std::uint64_t{0});
  CHECK(!st.ok());

  std::uint64_t v = 0;
  REQUIRE(mem.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, std::uint64_t{0});
}

P5EMU_TEST(GuestMemory, FetchCodeRequiresExecutePermission) {
  auto rwx = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  auto rw = LinearGuestMemory::MakeDataOnly(kBase, kSize);
  auto x = LinearGuestMemory::MakeExecuteOnly(kBase, kSize);

  std::uint8_t buf[16] = {};
  std::size_t got = 0;

  REQUIRE(rwx.FetchCode(kBase, 16, buf, &got).ok());
  CHECK_EQ(got, static_cast<std::size_t>(16));

  // Data-only: fetch must fail, even though read succeeds.
  std::uint64_t v = 0;
  REQUIRE(rw.ReadData(kBase, 8, &v).ok());
  CHECK(rw.FetchCode(kBase, 16, buf, &got).code() ==
        StatusCode::kAccessViolation);

  // Execute-only: fetch succeeds, read and write both fail. NX and data access
  // are independent axes. Both are "not permitted" rather than "unmapped",
  // because the address IS mapped.
  REQUIRE(x.FetchCode(kBase, 16, buf, &got).ok());
  CHECK_EQ(got, static_cast<std::size_t>(16));
  CHECK_EQ(std::string(x.ReadData(kBase, 1, &v).name()),
           std::string("access-violation"));
  CHECK_EQ(std::string(x.WriteData(kBase, 1, 0).name()),
           std::string("read-only-access"));
}

P5EMU_TEST(GuestMemory, FetchReportsAvailableBytesAtARegionEdge) {
  // A caller decoding at a page boundary needs to know how much it actually got
  // so it can advance rather than faulting.
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  std::uint8_t buf[64] = {};
  std::size_t got = 0;

  REQUIRE(mem.FetchCode(kBase + kSize - 4, 64, buf, &got).ok());
  CHECK_EQ(got, static_cast<std::size_t>(4));

  // Starting entirely outside reports zero available, not a fault of 4.
  REQUIRE(mem.FetchCode(kBase + kSize, 64, buf, &got).code() ==
        StatusCode::kUnmappedAccess);
}

P5EMU_TEST(GuestMemory, WriteToReadOnlyRegionIsDistinct) {
  // Execute-only regions are read-only, and the status must say so specifically:
  // "read-only" and "unmapped" lead to different debugging conclusions.
  auto mem = LinearGuestMemory::MakeExecuteOnly(kBase, kSize);
  CHECK(mem.WriteData(kBase, 1, 0).code() == StatusCode::kReadOnlyAccess);
}

P5EMU_TEST(GuestMemory, SelfModifyingCodeNotificationFires) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  mem.ResetExecutableWriteNotifications();

  // Write to an executable region: must notify.
  REQUIRE(mem.WriteData(kBase, 8, 0x9090'9090'9090'9090ull).ok());
  CHECK_EQ(mem.ExecutableWriteNotifications(), std::uint64_t{1});
  CHECK_EQ(mem.LastExecutableWriteAddress(), kBase);

  // A rejected write must NOT notify: nothing was written, so nothing changed.
  (void)mem.WriteData(kBase + kSize - 4, 8, 0);
  CHECK_EQ(mem.ExecutableWriteNotifications(), std::uint64_t{1});
}

P5EMU_TEST(GuestMemory, SelfModifyingCodeNotificationDoesNotFireForDataRegion) {
  auto mem = LinearGuestMemory::MakeDataOnly(kBase, kSize);
  mem.ResetExecutableWriteNotifications();
  REQUIRE(mem.WriteData(kBase, 8, 1).ok());
  CHECK_EQ(mem.ExecutableWriteNotifications(), std::uint64_t{0});
}

P5EMU_TEST(GuestMemory, OverlappingAccessesComposeCorrectly) {
  // Overlapping accesses are where a bounds bug hides: write 8 bytes, then
  // write 4 bytes at an offset 4, then read 8. Each step must be exact.
  //
  // Byte order note: the port stores little-endian, so writing 0x1111111111111111
  // puts 0x11 in bytes 0..7 and overwriting bytes 4..7 with 0x22222222 yields the
  // value 0x2222222211111111. Getting this backwards is exactly the kind of
  // mistake these tests exist to prevent.
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  REQUIRE(mem.WriteData(kBase, 8, 0x1111'1111'1111'1111ull).ok());
  REQUIRE(mem.WriteData(kBase + 4, 4, 0x2222'2222ull).ok());

  std::uint64_t v = 0;
  REQUIRE(mem.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, 0x2222'2222'1111'1111ull);

  // And the reverse overlap.
  REQUIRE(mem.WriteData(kBase, 4, 0x3333'3333ull).ok());
  REQUIRE(mem.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, 0x2222'2222'3333'3333ull);
}

P5EMU_TEST(GuestMemory, ContiguousAccessesDoNotAlias) {
  // Distinct single-byte writes across a span must each land in their own byte.
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  for (GuestAddr i = 0; i < 32; ++i) {
    REQUIRE(mem.WriteData(kBase + i, 1, i + 1).ok());
  }
  std::uint64_t v = 0;
  for (GuestAddr i = 0; i < 32; ++i) {
    REQUIRE(mem.ReadData(kBase + i, 1, &v).ok());
    CHECK_EQ(v, i + 1);
  }
}

P5EMU_TEST(GuestMemory, PermissionAndMappingQueries) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  CHECK(mem.IsMapped(kBase));
  CHECK(mem.IsMapped(kBase + kSize - 1));
  CHECK(!mem.IsMapped(kBase + kSize));
  CHECK(!mem.IsMapped(kBase - 1));

  CHECK_EQ(mem.PermissionsAt(kBase),
           static_cast<std::uint32_t>(kPermReadWriteExecute));
  CHECK_EQ(mem.PermissionsAt(kBase + kSize),
           static_cast<std::uint32_t>(kPermNone));
}

P5EMU_TEST(GuestMemory, DirectAccessQueryIsAnOptimisationNotAnAssumption) {
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);

  // This port offers direct access for an executable in-range span.
  std::uint64_t host_base = 0;
  CHECK(mem.QueryDirectAccess(kBase, 8, &host_base) == DirectAccessStatus::kOk);
  CHECK(mem.IdentityMapped(kBase, 8));

  // A span leaving the single region cannot be one contiguous access.
  CHECK(mem.QueryDirectAccess(kBase + kSize - 4, 8, &host_base) ==
        DirectAccessStatus::kCrossesRegion);
  CHECK(!mem.IdentityMapped(kBase + kSize - 4, 8));

  // An unmapped span is reported as unmapped, not as "unsupported".
  CHECK(mem.QueryDirectAccess(kBase + kSize, 8, &host_base) ==
        DirectAccessStatus::kUnmapped);

  // Address-space wrap is rejected rather than producing a bogus host pointer.
  CHECK(mem.QueryDirectAccess(~GuestAddr{0}, 16, &host_base) ==
        DirectAccessStatus::kUnmapped);
}

P5EMU_TEST(GuestMemory, DirectAccessIsNotRequiredForCorrectness) {
  // The port's own results must be identical whether or not a caller consults
  // the optimisation query. This is the property that keeps guest VA == host VA
  // from becoming an architectural assumption.
  auto mem = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  const GuestAddr addr = kBase + 0x20;

  REQUIRE(mem.WriteData(addr, 8, 0x0f0f'0f0f'0f0f'0f0full).ok());

  // Path A: through ReadData/WriteData only.
  std::uint64_t a = 0;
  REQUIRE(mem.ReadData(addr, 8, &a).ok());

  // Path B: consult the optimisation, then still read through the port. If a
  // backend used the host pointer it would get the same bytes; if it ignored the
  // query it would also get the same bytes. Both are correct.
  std::uint64_t host_base = 0;
  bool direct = (mem.QueryDirectAccess(addr, 8, &host_base) ==
                 DirectAccessStatus::kOk);
  std::uint64_t b = 0;
  REQUIRE(mem.ReadData(addr, 8, &b).ok());

  CHECK(direct);
  CHECK_EQ(a, b);
}

P5EMU_TEST(GuestMemory, PortIsUsableThroughTheAbstractInterface) {
  // Everything above goes through the concrete type. This test goes through the
  // interface, because that is what a future backend will hold, and because a
  // virtual call must not behave differently from a direct one.
  LinearGuestMemory concrete = LinearGuestMemory::MakeCodeAndData(kBase, kSize);
  GuestMemoryPort& port = concrete;

  std::uint64_t v = 0;
  REQUIRE(port.WriteData(kBase, 8, 0xabcd'ef01'2345'6789ull).ok());
  REQUIRE(port.ReadData(kBase, 8, &v).ok());
  CHECK_EQ(v, 0xabcd'ef01'2345'6789ull);
  CHECK(port.CheckAccess(kBase, 8, kPermReadWriteExecute).ok());
  CHECK(!port.CheckAccess(kBase + kSize, 1, kPermRead).ok());
}

}  // namespace
