# P5EMU Licence and Use Notice

**P5EMU is a private / proprietary project. All rights reserved.**

No open-source licence is granted to the P5EMU source at this time.

---

## 1. Status

| Item | Status |
|---|---|
| P5EMU-owned source | **Proprietary. All rights reserved.** No licence granted. |
| Project visibility | **Private.** Not published. |
| Copyright holder | The P5EMU project authors. |
| Redistribution | Not granted. |

Files carry `SPDX-License-Identifier: LicenseRef-P5EMU-Proprietary`, which
denotes *all rights reserved* — not a licence, and not an OSI-approved
identifier.

---

## 2. Why no project-wide licence has been chosen

An earlier Phase-0 draft proposed Apache-2.0 on the reasoning that permissive
dependencies are expected later, and that the Apple App Store makes copyleft
awkward.

**That reasoning was incomplete and the proposal is withdrawn.** Two corrections:

1. **Dependency compatibility does not determine our licence.** The licence of a
   project is a distribution strategy decision, not a function of what it links.
   Adopting a permissive licence "because we might use MIT libraries" reverses
   the causality and surrenders control of the project's terms.

2. **The relationship with upstream research has not been fully characterised.**
   P5EMU's architecture was informed by inspecting the source of several
   GPL-licensed and permissively-licensed projects during research. See
   §4 and [`PROVENANCE.md`](../PROVENANCE.md). Until counsel has reviewed what
   that means, granting a permissive licence would be a decision made without
   the information needed to make it.

P5EMU therefore keeps its own terms closed until that review concludes.

---

## 3. Third-party components

A third-party component may be introduced into P5EMU **only** after a recorded
licence and provenance review. The requirements:

| Requirement | Detail |
|---|---|
| **Review recorded first** | In `THIRD_PARTY_LICENSES/` and `PROVENANCE.md`, before the code enters the tree |
| **Copyleft excluded** | GPL, LGPL, AGPL, CDDL, MPL and similar are **not vendored into P5EMU** |
| **Permissive may be considered** | MIT, BSD-2/3, Apache-2.0, ISC, 0BSD — where compatible with a proprietary project and with the §4 relationship |
| **Notices preserved** | Verbatim upstream licence text and copyright notices retained in `THIRD_PARTY_LICENSES/` for any permitted permissive component |
| **No silent vendoring** | A component that is *convenient* is not a reason to import it. If an import is contemplated, the decision is reported, not taken |

A component being MIT-licensed is **not** by itself authorisation to reuse.
Reuse of an existing implementation requires a positive, recorded decision.

---

## 4. Relationship to upstream emulator projects

P5EMU is an **independent implementation informed by public research and
documented architecture behaviour**, with **no copied copyleft source code**.

This phrasing is deliberate and precise. It is weaker than "clean-room", and
P5EMU does not claim "clean-room", because strict clean-room separation has not
been established: the same engineering process that designed P5EMU also
inspected upstream project sources during research.

What that means concretely:

| Statement | True |
|---|---|
| No copyleft source has been copied, vendored, transcribed, or adapted into P5EMU | **Yes** |
| P5EMU's source expression is independently authored | **Yes** |
| P5EMU was designed by people who read upstream sources | **Yes** |
| Strict clean-room separation between design reading and implementation was established and independently verified | **No** |

Facts — instruction encodings, architectural flag definitions, file-format
constants, published specifications — are not the protected expression of a
licence and may be learned and independently re-expressed. Entire design
approaches were also studied. No *lines* were taken.

The full study record is [`PROVENANCE.md`](../PROVENANCE.md); the per-project
licence analysis is [`docs/research/licenses.md`](../docs/research/licenses.md).

---

## 5. Prohibited material

The following may never be present in this repository, and P5EMU will not
distribute, link, or incorporate them:

- PlayStation firmware, system software, or extracted sysmodules
- Console crypt keys or any key material
- Game executables, discs, save data, or container dumps
- Sony SDKs, GDK or OpenGDK headers, or developer tools
- Sony trademarks, UI assets, icons, sounds, fonts, or firmware artwork
- GPL, LGPL, AGPL, CDDL, MPL or other copyleft source

The boundaries P5EMU observes are recorded in
[`docs/research/ps5-subsystems.md`](../docs/research/ps5-subsystems.md).

---

## 6. Change control

This file changes only by explicit decision. Any future change to P5EMU's terms
requires:

1. A recorded rationale, including the §4 question resolved by counsel.
2. An audit of every file in the tree against [`PROVENANCE.md`](../PROVENANCE.md).
3. A re-run of `scripts/check_provenance.sh`.
4. Explicit authorisation to publish, which has not been given.
