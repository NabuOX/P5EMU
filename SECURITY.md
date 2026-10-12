# Security Policy

P5EMU is an experimental, open-source emulator architecture (Apache-2.0). It is
**not** production software, and its public repository is not yet published.
This policy sets out how security issues will be handled.

## Supported versions

None. P5EMU has no released versions and no supported release branches yet.
Security expectations will be defined when the project is published and the
first release exists.

## Reporting a vulnerability

P5EMU's reporting channel is **GitHub Private Vulnerability Reporting**
(repository → *Security* → *Report a vulnerability*), which keeps reports private
until a fix is available. This becomes available as soon as the GitHub repository
is published, and is a **publication-configuration item** that must be enabled
*before* P5EMU is made public.

**Do not send security reports to any address claiming to be P5EMU** — no
security email exists, and none is invented here. Until the repository is
published and private reporting is enabled, please do not publicly disclose a
suspected vulnerability.

## What counts as a security issue here

P5EMU processes untrusted input (guest executables, ELF files, byte streams).
Issues of interest include, but are not limited to:

- memory-safety defects in parsing or execution of untrusted input
  (`BinaryReader`, ELF/relocation parsing, the decoder, the interpreter, the
  loader/runtime, the HLE bridge);
- out-of-bounds or permission-check bypasses in guest memory handling
  (`GuestMemoryPort`, `LinearGuestMemory`, `MappedGuestMemory`);
- defects in the AArch64 harness transport (region bounds, permission
  enforcement, malformed request handling);
- anything that lets malformed input escape the checked/validated path.

## Our process (intent)

When a channel exists, we intend to:

1. Acknowledge the report privately.
2. Reproduce and assess it against the evidence standards in
   `CONTRIBUTING.md`.
3. Prepare a fix with regression tests (fuzz/property coverage where relevant).
4. Coordinate disclosure with the reporter.

## Scope boundaries

The AArch64 execution harness and the external tools it runs (e.g.
`qemu-aarch64`, the cross toolchain) are **test infrastructure**, not part of
the shipped core; issues in those external tools should be reported to their own
projects. See `PROVENANCE.md`.
