# Third-party and open-source components

SkyDoom builds on open-source software including:

- Chocolate Doom - GPL-2.0
- CommonLibSSE-NG - GPL-3.0 with the exceptions included in its source tree
- SDL2
- SDL2_mixer
- SDL2_net
- libpng
- libsamplerate
- libogg
- libvorbis
- WavPack
- zlib
- FluidSynth-related build/runtime support where applicable

The authoritative licence/copyright text for source components is retained in
their respective source trees.

The binary release additionally includes the relevant runtime notices.

No DOOM or Skyrim proprietary game data is part of this repository.

## Credits and licences of what SkyDoom builds on

SkyDoom's Skyrim bridge is statically linked with CommonLibSSE-NG, which is licensed GPL-3.0-or-later with a Modding Exception and a GPL-3.0 Linking Exception. A plugin that links it must itself be GPL-3.0-or-later or GPL-compatible, which is why SkyDoom's bridge is GPL-3.0 (see `LICENSE`).

| Component | Credit | Licence |
| --- | --- | --- |
| [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) 9.1.0 (copy in `SkyDoomSKSE/lib/commonlibsse-ng`) | CharmedBaryon, alandtse and contributors, built on the original CommonLibSSE by Ryan McKenzie | GPL-3.0-or-later with Modding and Linking Exceptions (`COPYING.txt`, `EXCEPTIONS.md` in that folder) |
| [Chocolate Doom](https://www.chocolate-doom.org/) 3.1.1 (modified copy in `chocolate-doom/`) | Simon Howard and contributors | GPL-2.0-or-later (`chocolate-doom/COPYING.md`) |
| [SKSE64](https://skse.silverlock.org/) | the SKSE team (ianpatt, behippo, scruggsywuggsy the ferret, purple lunchbox) | Required at runtime, installed by the user, not redistributed |
| [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) | meh321 | Required at runtime, installed by the user, not redistributed |
| [SkyCraft](https://github.com/chasmlol) | chasmlol, whose project inspired SkyDoom | MIT; add its notice to any file that uses its code |
| [spdlog](https://github.com/gabime/spdlog) (and fmt) | gabime and contributors | MIT, linked statically (via vcpkg) |
| [DirectXTK](https://github.com/microsoft/DirectXTK) | Microsoft | MIT, linked statically (via vcpkg) |
| [rapidcsv](https://github.com/d99kris/rapidcsv) | d99kris | BSD-3-Clause, header only (via vcpkg) |

Doom and Doom II are owned by id Software / Bethesda Softworks and Skyrim by Bethesda Game Studios / ZeniMax; none of their files are included.
