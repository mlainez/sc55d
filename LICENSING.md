# Licensing, and what you may not do

sc55d's own code is MIT-licensed. **That does not make a working sc55d
MIT-licensed**, and the difference matters before you distribute anything.

sc55d is a frontend. The thing that actually makes the sound — the Nuked-SC55
emulation core — is under the original (pre-2016) **MAME licence**, which is not
an open-source licence and **forbids selling and commercial use**. Every binary
you build here contains that core, so every binary carries those terms. The MIT
licence on the frontend removes none of them.

If you take one thing from this page: **you may share sc55d, and you may not
sell it or use it commercially.**

## What is covered by what

| Part of the tree | Licence | |
|---|---|---|
| `src/`, `tests/`, `scripts/`, `contrib/`, `cmake/`, `docs/`, `CMakeLists.txt`, `README.md` | **MIT** | sc55d's own work — see [`LICENSE`](LICENSE) |
| `patches/` (including `patches/tests/`) | **MAME licence** (the core's) | Contains core source as diff context, and models core logic closely enough to be treated as derived from it |
| `vendor/nuked-sc55/` | **MAME licence** | The core itself, a git submodule — not part of this repository |
| **Any binary built from this repository** | **MAME licence terms apply** | The core is linked in |
| SC-55 ROM files | Roland's copyright | Not included, not downloaded, not ours to license — see [ROMs](#the-roms-are-not-covered-by-anything-here) |

The core's licence text is reproduced in full in [`NOTICE`](NOTICE), because that
licence requires it to travel with any redistribution.

## What the MIT licence gives you

For the files listed as MIT above, and for those files alone: use, copy, modify,
merge, publish, distribute, sublicense and sell, for any purpose including
commercial ones, provided the copyright notice and permission notice travel with
them. No warranty, no liability.

That is worth having for two reasons. The frontend is reusable on its own — the
period ring, the MIDI queue, the realtime setup and the ALSA plumbing are not
specific to this emulator — and if the backend is ever replaced with something
that has friendlier terms (see [backend options](docs/backend-options.md), where
libEmuSC is LGPL-2.1 with no commercial restriction), the resulting program
would be free of everything below.

## Limitations that apply to any binary you build

These come from the core's licence, not from MIT, and MIT cannot waive them:

1. **You may not sell it.** Not the binary, not a disk image containing it, not
   a device with it preinstalled.
2. **You may not use it in a commercial product or activity.** This is broader
   than selling: it rules out using it in a paid service, in a commercial
   studio's production pipeline, or as part of a product you charge for. A
   personal build for your own music is exactly what is fine.
3. **If you distribute a modified build, you must ship the complete
   corresponding source** — including the source of everything a binary built
   from those modified sources uses, excepting the normal parts of the operating
   system it runs on (compiler, kernel, and so on).
4. **The notice must travel with every redistribution.** Ship
   [`NOTICE`](NOTICE), unmodified, alongside whatever you hand on.
5. **You cannot relicense the combination under the GPL.** The GPL does not
   permit the added non-commercial restriction, so the two are incompatible.
   This is not a formality that can be worked around with an exception clause;
   it is why sc55d itself is not GPL.
6. **There is no warranty from anyone**, under either licence. Both disclaim it
   in the strongest terms available.

The core's patches in `patches/` carry the same six points, since they quote its
source. Sharing a patch file is redistribution.

## The ROMs are not covered by anything here

sc55d ships no ROM data and downloads none. The SC-55's firmware and sample ROMs
are **Roland's copyright**. They are not licensed to you by this project, by the
core, or by anyone else, and neither licence on this page grants you any right to
them.

Dump them from hardware you own. Do not redistribute them, and do not ask this
project for them.

## If you want to use this commercially

You cannot, with this backend, and no arrangement of licences on sc55d's side
changes that — the restriction is the core's to lift. The realistic path is to
replace the emulation core with one whose terms allow it; the analysis of what
that would cost in fidelity and effort is in
[`docs/backend-options.md`](docs/backend-options.md).

## Contributing

Contributions to the MIT-licensed parts are taken as MIT. Contributions to
`patches/` are taken under the core's licence, since that is what they derive
from. No copyright assignment is asked for, and no Developer Certificate of
Origin sign-off is required.
