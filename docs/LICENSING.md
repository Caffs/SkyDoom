# Licensing checklist

SkyDoom's Skyrim bridge is distributed under **GPL-3.0** (see `LICENSE`).

## Why GPL-3.0

The bridge is statically linked with **CommonLibSSE-NG**, whose licence is **GPL-3.0-or-later with a Modding Exception and a GPL-3.0 Linking Exception** (`SkyDoomSKSE/lib/commonlibsse-ng/EXCEPTIONS.md`). The Modding Exception covers interoperation with Skyrim itself, not plugin code: a plugin that links the library must itself be GPL-3.0-or-later or GPL-compatible, and the source of a distributed binary must be available.

## Every release must have

| Requirement | Where |
| --- | --- |
| Full, unmodified GPL-3.0 text | `LICENSE` in the repository and `LICENSE.txt` inside the release zip |
| Source code available | This repository at the release tag; `RELEASE_PROVENANCE.md` names the commit and the DLL hash |
| Copyright statement and GPL notice | `README.md` and `README.txt` in the zip |
| Credits | `THIRD_PARTY.md` and `README.txt` in the zip: CommonLibSSE-NG (CharmedBaryon, alandtse, contributors; original CommonLibSSE by Ryan McKenzie), Chocolate Doom, SKSE team, meh321, SkyCraft, spdlog, DirectXTK, rapidcsv |
| Runtime licence notices of bundled libraries (SDL2 and the other Chocolate Doom runtime libraries) | inside the zip |
| No game data | no `DOOM.WAD`, `DOOM2.WAD`, `extras.wad`, soundtrack, Skyrim files, SKSE or Address Library binaries |

If SkyDoom is also posted on Nexus Mods: set the page permissions to open (modification and redistribution), state GPL-3.0 on the page, include a source link, and remove or offer source for any older upload that had none.

Licences are read from the actual files of the exact version used, never from memory: CommonLibSSE was MIT, CommonLibSSE-NG is not.

This page is a working checklist, not legal advice.
