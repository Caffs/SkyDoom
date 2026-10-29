# v0.1.0-beta release provenance

Public release label:

`v0.1.0-beta`

SkyDoom SKSE binary metadata:

`0.1.0`

Exact tested SkyDoomSKSE.dll SHA256:

`E362497D0C8AC1772C9A23801339DC2C631C4F6B5E71315C025B3690A17D7DA4`

Tested game environment:

- SkyrimSE.exe 1.7.104.0
- SKSE 2.3.1
- Steam DOOM + DOOM II AppID 2280 layout

The public binary package must not be published unless this corresponding
source is publicly available.

Corresponding source for this release: https://github.com/Caffs/SkyDoom at tag `v0.1.0-beta` (commit `9292ee03634c973f261c374e567e09ce5d842b16`).

Statically linked into the DLL (source in this repository):

- CommonLibSSE-NG 9.1.0, `SkyDoomSKSE/lib/commonlibsse-ng` (GPL-3.0-or-later with exceptions)
- spdlog, DirectXTK and rapidcsv through vcpkg, pinned by `builtin-baseline 4bc07e3eb00c5a9539a5a7a83415150a9260f8db` in `SkyDoomSKSE/vcpkg.json` (exact versions to be recorded at the next release)

Bundled runtime: modified Chocolate Doom 3.1.1 (source in `chocolate-doom/`).

