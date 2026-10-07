# Building SkyDoom

These notes describe the source layout used for the `v0.1.0-beta` release
candidate.

## SkyDoom SKSE bridge

The bridge source is under:

`SkyDoomSKSE/`

The release candidate was built on Windows with CMake, MSVC and the dependency
configuration contained in the project.

The command used for the final clean release build was:

```bat
cd /d C:\SkyDoom\skyrim-bridge\SkyDoomSKSE
cmake --build --preset release-windows --clean-first
```

The resulting release DLL identified itself as version `0.1.0`.

The exact DLL tested for `v0.1.0-beta` had SHA256:

`E362497D0C8AC1772C9A23801339DC2C631C4F6B5E71315C025B3690A17D7DA4`

Paths above document the development machine used for the release build; they
are not runtime requirements. The public plugin itself no longer contains the
development machine's hard-coded Chocolate Doom or DOOM.WAD paths.

## Modified Chocolate Doom

The corresponding modified Chocolate Doom source is under:

`chocolate-doom/`

The release runtime was produced from this tree.

The final development build command used was:

```bat
cd /d C:\SkyDoom\chocolate-doom
cmake --build build --config Release --clean-first
```

Chocolate Doom remains subject to its own GPL-2.0 licence.

## Dependencies

The checked source tree contains the project metadata and dependency source
that was present for the release candidate, excluding generated build/package
/download caches.

CommonLibSSE-NG licence and exception files remain in the SkyDoomSKSE source
tree.

## Proprietary game data

No DOOM IWAD, Skyrim game asset or SKSE binary is required to build the source
itself and none is included in this source release.

Running SkyDoom requires the user's own legitimate supported game
installations.