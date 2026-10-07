# SkyDoom

SkyDoom is an experimental passthrough mod that connects a modified build of
Chocolate Doom to Skyrim through SKSE and CommonLibSSE-NG.

The project began after seeing what was possible with SkyCraft and wondering
whether the same general idea could be pushed in a very different direction:
running DOOM-side weapon/HUD/state logic while interacting with Skyrim's world
and actors.

## Current public beta

The first public beta is `v0.1.0-beta`.

Tested configuration:

- Skyrim runtime 1.7.104.0 (Steam)
- SKSE 2.3.1
- Steam DOOM + DOOM II / AppID 2280 installation layout
- Windows 10/11

The beta automatically discovers the player's own legitimate `DOOM.WAD` from
their Steam libraries.

## What SkyDoom currently does

- Bridges Chocolate Doom weapon/HUD state into Skyrim
- Provides all seven current SkyDoom weapon slots from the start
- Applies DOOM weapon damage to Skyrim actors
- Uses Skyrim-native knockback/ragdoll interaction for rocket impacts
- Bridges DOOM health, armour and ammunition state
- Adds physical DOOM-style resource drops from Skyrim enemies
- Renders the DOOM HUD and first-person weapon overlay
- Integrates DOOM sound and standard Chocolate Doom music behaviour
- Uses a portable Chocolate Doom runtime inside the Skyrim mod layout

## Game data is NOT included

This repository does not contain:

- `DOOM.WAD`
- `DOOM2.WAD`
- `extras.wad`
- enhanced DOOM soundtrack files
- Skyrim game assets
- SKSE binaries

Users must supply legitimate supported installations of Skyrim and DOOM /
DOOM II.

## Repository layout

- `SkyDoomSKSE/` - Skyrim/SKSE bridge source
- `chocolate-doom/` - modified Chocolate Doom source corresponding to the
  runtime used by SkyDoom
- `shared/` - shared SkyDoom protocol definitions
- `LICENSE` - SkyDoom SKSE bridge GPL-3.0 licence
- `BUILDING.md` - development/build notes
- `THIRD_PARTY.md` - third-party/open-source information

## Enhanced soundtrack

`v0.1.0-beta` uses Chocolate Doom's standard music setup.

For a future/full release, optional support for the enhanced soundtrack
available with newer DOOM releases is being considered. Any such support would
use files from the player's own legitimate installation; those files will not
be distributed with SkyDoom.

## Inspiration

SkyCraft by chasmlol was the inspiration for attempting this project in the
first place. Huge credit to that project for demonstrating what was possible
with this kind of passthrough experimentation.

## Status

Beta / work in progress.

Expect bugs, narrow compatibility and rough edges while the public test base
is expanded.

## Disclaimer

SkyDoom is an unofficial fan-made project and is not affiliated with or
endorsed by Bethesda Softworks, Bethesda Game Studios, ZeniMax, id Software,
or Microsoft.

Skyrim, DOOM and their associated trademarks and game assets belong to their
respective owners.