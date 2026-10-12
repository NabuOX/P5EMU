#!/usr/bin/env bash
# Phase-gate check: proves the source tree is the provenance relationship.
#
# A P5EMU repository that has silently absorbed GPL code is worthless. This
# script is cheap and runs before every commit.
#
# It does NOT prove the provenance relationship (that is a human/legal judgement).
# It proves the *mechanical* signals are absent: no vendored third-party
# trees, no copyleft SPDX tags, no firmware/key-like blobs.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

fail=0

note() { printf '%-52s %s\n' "$1" "$2"; }

# 1. No vendored third-party source trees.
if [[ -d third_party ]]; then
  note "third_party/ present" "REVIEW (every subtree needs a THIRD_PARTY_LICENSES/ entry)"
else
  note "no vendored third_party/ tree" "OK"
fi

# 2. No copyleft SPDX identifiers in OUR OWN SOURCE.
#    Scoped to source trees only: docs/research/licenses.md legitimately quotes
#    GPL/LGPL identifiers because it *is* the licence analysis. A copyleft tag in
#    code is what would contaminate the tree.
if grep -RIl \
     -E 'SPDX-License-Identifier: (GPL-|LGPL-|AGPL-|CDDL-|MPL-)' \
     core frontend tests cmake scripts aarch64 2>/dev/null >/tmp/p5emu_copyleft.txt; then
  note "copyleft SPDX tag in source" "FAIL"
  cat /tmp/p5emu_copyleft.txt
  fail=1
else
  note "no copyleft SPDX in source" "OK"
fi

# 2b. Markdown must not claim a copyleft licence for P5EMU itself. (Quoting a
#     licence identifier inside prose is fine; a bare SPDX *line* is not.)
if grep -RIl -E '^SPDX-License-Identifier: (GPL-|LGPL-|AGPL-)' \
     --include='*.md' . 2>/dev/null >/tmp/p5emu_md_copyleft.txt; then
  note "copyleft SPDX claim in markdown" "FAIL"
  cat /tmp/p5emu_md_copyleft.txt
  fail=1
else
  note "no copyleft SPDX claim in markdown" "OK"
fi

# 3. Every tracked source file carries an SPDX-License-Identifier.
missing=0
while IFS= read -r f; do
  if ! head -n 5 "${f}" | grep -q 'SPDX-License-Identifier:'; then
    note "missing SPDX header: ${f}" "FAIL"
    missing=1
  fi
done < <(find core frontend tests cmake scripts aarch64 -type f \
           \( -name '*.c' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null)
if [[ ${missing} -eq 0 ]]; then
  note "all source files carry SPDX headers" "OK"
else
  fail=1
fi

# 4. No binary blobs of a size typical of firmware/keys.
#
# Build directories are pruned by PATTERN, not by the single name "build":
# verify.sh emits build-<type>[-<sanitizer>] directories, and a hardcoded
# ./build prune silently walked straight into them and reported their object
# files as suspicious binaries.
if find . \( -name '.git' -o -name 'build' -o -name 'build-*' \) -prune -o \
     -type f -size +2M -print 2>/dev/null | grep -q .; then
  note "large binary file(s) found" "REVIEW"
  find . \( -name '.git' -o -name 'build' -o -name 'build-*' \) -prune -o \
       -type f -size +2M -print
else
  note "no large binary files" "OK"
fi

# 5. Provenance log exists and is non-empty.
if [[ -s PROVENANCE.md ]]; then
  note "PROVENANCE.md present" "OK"
else
  note "PROVENANCE.md missing/empty" "FAIL"
  fail=1
fi

echo
if [[ ${fail} -ne 0 ]]; then
  echo "provenance gate: FAIL"
  exit 1
fi
echo "provenance gate: OK"
