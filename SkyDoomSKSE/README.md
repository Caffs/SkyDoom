# skydoomskse

A starter template for Skyrim Special Edition / Anniversary Edition SKSE plugins using
[CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG), [CMake](https://cmake.org), and [vcpkg](https://vcpkg.io).

Supports building on **Linux** (cross-compilation via `clang-cl` + [xwin](https://github.com/Jake-Shadle/xwin)) and **Windows** (MSVC).

---

<!-- nexus:start -->

## Requirements

- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) for SE and AE, or [VR Address Library for SKSEVR](https://www.nexusmods.com/skyrimspecialedition/mods/58101) for VR

## Installation

**Mod manager (recommended):**

1. Install the requirements above.
2. Install skydoomskse via your mod manager.
3. Launch Skyrim via SKSE.

**Manual:**

1. Install the requirements above.
2. Copy `skydoomskse.dll` to `Data\SKSE\Plugins\`.
3. Launch Skyrim via SKSE.

## Compatibility

- Compatible with Skyrim SE, AE, and VR.
- No ESP/ESL required.

<!-- nexus:end -->

---

## What the Starter Plugin Does

When loaded by Skyrim the plugin:

1. **Writes a log** to `Data/SKSE/Plugins/skydoomskse.log` via spdlog.
2. **Hooks `kDataLoaded`** (fires once all game data is loaded).
3. **Prints to the in-game console** (`~` key): `[skydoomskse] Loaded successfully!`

---

## Development

### Prerequisites

#### All platforms

- [Git](https://git-scm.com/)
- [CMake](https://cmake.org/download/) 3.21+
- [vcpkg](https://vcpkg.io/en/getting-started) â€” set `VCPKG_ROOT` in your environment

#### Linux

- LLVM/Clang (provides `clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`, `llvm-mt`)
- [xwin](https://github.com/Jake-Shadle/xwin) â€” downloads the real Windows SDK and MSVC CRT headers/libs
- [Ninja](https://ninja-build.org/)

```bash
# Arch / CachyOS
sudo pacman -S clang lld llvm ninja

# Install xwin (requires Rust/cargo)
cargo install xwin

# Fetch Windows SDK + MSVC CRT headers to ~/.xwin  (one-time, ~700 MB)
xwin splat --output ~/.xwin
```

> **Note:** On first configure, `cmake/toolchains/clang-cl-cross.cmake` creates
> TitleCase symlinks inside your xwin installation, e.g.:
>
> ```
> ~/.xwin/sdk/lib/um/x86_64/Advapi32.lib  ->  advapi32.lib
> ```
>
> lld-link is case-sensitive but CommonLibSSE-NG references libs with mixed-case names.
> The originals are untouched.

#### Windows

- [Visual Studio 2022 or newer](https://visualstudio.microsoft.com/) (or the standalone Build Tools) with **Desktop development with C++**, which provides MSVC, CMake, and Ninja

---

### Getting Started

#### 1. Create your repo from the template

With the GitHub CLI, create your own repo from the template and clone it with submodules:

```bash
# Pick visibility: --public (free GitHub Actions) or --private
gh repo create your-org/your-mod --template codepuncher/CommonLibSSE-NG-template --public
gh repo clone your-org/your-mod -- --recurse-submodules
cd your-mod
```

Or click **"Use this template"** on GitHub, then clone your new repo with submodules:

```bash
git clone --recurse-submodules https://github.com/your-org/your-mod.git
cd your-mod
```

The `--recurse-submodules` flag (or `-- --recurse-submodules` for `gh repo clone`) is required: the build needs the vcpkg and CommonLibSSE-NG submodules. If you cloned without it, run `git submodule update --init --recursive`.

#### 2. Set your mod name and Caffs

For a repo created from the template, the **Template Setup** workflow (`setup.yml`) renames the `skydoomskse` and `Caffs` placeholders, deriving them from the repo name and owner, commits the result, and removes itself. It can run automatically when the repo is created; if it hasn't, trigger it from the **Actions** tab ("Template Setup" > "Run workflow"). Once it has run, `git pull` to get the renamed sources.

Prefer to rename locally, or cloned the template directly? Run the init script instead. It replaces the placeholders, initialises the submodules (CommonLibSSE-NG + vcpkg), bootstraps vcpkg, and copies `.env.example` to `.env`.

**Linux** â€” run interactively or pass arguments directly:

```bash
./scripts/init.sh
# or:
./scripts/init.sh "CaffsName" "ModName"
```

**Windows (PowerShell)**:

```powershell
.\scripts\init.ps1
# or:
.\scripts\init.ps1 "CaffsName" "ModName"
```

#### 3. Configure deploy path

Copy `.env.example` to `.env` if you don't have one yet (the init script does this for you), then set `SKYRIM_MODS_FOLDER` to your mod manager's staging folder:

```bash
# Vortex (Linux, Steam):
SKYRIM_MODS_FOLDER=$HOME/.local/share/Steam/steamapps/common/Vortex Mods/skyrimse

# MO2 (Linux):
# SKYRIM_MODS_FOLDER=$HOME/MO2/mods
```

#### 4. Build

```bash
./scripts/build.sh
# or directly:
cmake --preset release-linux
cmake --build --preset release-linux
```

The DLL lands in `build/release-linux/skydoomskse.dll`.

##### Deploy to mod manager

```bash
./scripts/deploy.sh
# or directly (deploy.sh sources .env for you; cmake does not):
source .env && cmake --workflow --preset deploy
```

This configures, builds, and copies `skydoomskse.dll` + `skydoomskse.pdb` directly into:

```
$SKYRIM_MODS_FOLDER/skydoomskse/SKSE/Plugins/
```

Vortex will detect the new mod folder automatically. Enable it in Vortex, then launch Skyrim.

On subsequent builds (no config change needed):

```bash
./scripts/deploy.sh
```

##### Windows (MSVC)

```bash
cmake --preset release-windows
cmake --build --preset release-windows
```

> Run these from a **Developer Command Prompt for VS** (or after sourcing `vcvars64.bat`) so `cl.exe` and the Windows SDK are on `PATH`. The preset uses the Ninja generator, which builds with whatever MSVC toolset the environment provides.

The DLL lands in `build/msvc/skydoomskse.dll`.

#### 5. Running tests

Unit tests build without CommonLibSSE, so they run as a native executable. Two presets are available:

- **Linux** (`test-linux`) uses the system Catch2 3 package. Install it with `sudo pacman -S catch2` on Arch/CachyOS, or your distribution's equivalent.
- **Windows** (`test-windows`) uses Catch2 from vcpkg.

```bash
cmake --preset test-linux
cmake --build --preset test-linux
ctest --preset test-linux
```

Tests live in `test/` and use [Catch2](https://github.com/catchorg/Catch2). Only pure-logic code (no RE::/SKSE:: APIs) can be tested this way. See `src/Utils.h` and `test/ExampleTests.cpp` for the pattern.

---

### Git Hooks (Lefthook)

Prerequisites:

- `go install github.com/evilmartians/lefthook@latest`
- `clang-format` (part of LLVM â€” already required for development)
- `clang-tidy` (part of LLVM â€” already required for development)
- `cmake-format` (`sudo pacman -S cmake-format` on Arch/CachyOS; `pip install cmakelang` elsewhere)
- `shellcheck` (`sudo pacman -S shellcheck` on Arch/CachyOS)

```bash
lefthook install
```

---

### Editor Setup (clangd / Neovim)

CMake writes `compile_commands.json` to the build directory automatically.
Copy or symlink it to the project root so clangd picks it up:

```bash
# After configuring:
ln -sf build/release-linux/compile_commands.json compile_commands.json
```

The `.clangd` file already sets `--target=x86_64-pc-windows-msvc` so clangd
resolves Windows headers correctly on Linux.

Recommended Neovim plugins: [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig)
with `clangd`, and [clangd_extensions.nvim](https://github.com/p00f/clangd_extensions.nvim).

---

### Updating CommonLibSSE-NG

The script pins the submodule to a release tag (`v*`) on the `ng` branch and stages it. Without an argument it picks the latest tag:

```bash
./scripts/update.sh          # latest release tag
./scripts/update.sh v10.1.0  # a specific tag
git commit -m "chore(deps): update CommonLibSSE-NG to v10.1.0"
```

---

### Nexus Mods Page

The `<!-- nexus:start/end -->` block at the top of `README.md` is the **source of truth** for Requirements, Installation, and Compatibility. `docs/nexus-page.md` holds the rest of the BBCode page (overview, tagline, credits).

To update the Nexus page:

1. Edit Requirements/Installation/Compatibility inside the `<!-- nexus:start/end -->` block in `README.md`.
2. Edit overview, tagline, and credits directly in `docs/nexus-page.md`.
   > **Do not edit** the `<!-- generated:start/end -->` block in `docs/nexus-page.md` â€” it is overwritten every time the script runs.
3. Generate the combined BBCode output:

```bash
python3 scripts/generate-nexus-page.py

# Or copy straight to the clipboard (wl-copy, xclip, xsel or pbcopy):
python3 scripts/generate-nexus-page.py --copy
```

4. Paste the output into the Nexus Mods page editor.

---

### CI

| Workflow           | Trigger                                                                                               | What it does                                                                                                                              |
| ------------------ | ----------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `setup.yml`        | First push in a repo created from this template                                                       | Renames placeholders using the repo name, then self-deletes                                                                               |
| `ci.yml`           | PRs to `main` touching `src/`, `test/`, `cmake/`, `vcpkg.json`, `CMakeLists.txt`, `CMakePresets.json` | `clang-format` (ubuntu) â†’ `test-linux` and `build-linux` (ubuntu) and `test-windows` + `build-windows` + `clang-tidy` (windows, parallel) |
| `release.yml`      | Push of a `v*` tag                                                                                    | Builds, packages via `scripts/package.sh`, publishes a GitHub Release with zip + PDB                                                      |
| `nexus-upload.yml` | Auto-triggered via `workflow_run` once `release.yml` completes, or manual `workflow_dispatch`         | Downloads release zip, generates cliff release notes, uploads to Nexus Mods                                                               |
| `lint.yml`         | PRs touching `scripts/`, `README.md`, or `docs/`                                                      | Runs shellcheck on shell scripts, plus dprint formatting and Vale prose checks on Markdown                                                |
| `pr-title.yml`     | PR opened/edited/reopened/synchronize                                                                 | Checks PR title follows Conventional Commits (`feat`, `fix`, `chore`, `refactor`)                                                         |

#### Nexus Mods Upload

`nexus-upload.yml` runs once `release.yml` completes, after approval in the `nexus` environment. It can also be run manually via **workflow_dispatch** with a version input. The one-time setup (environment, secrets, and variables) is in [docs/RELEASING.md](docs/RELEASING.md#nexus-mods-upload).

---

## License

Copyright (c) 2026 skydoomskse Contributors. Licensed under the GNU GPL v3.0 or later; see [LICENSE](LICENSE). CommonLibSSE-NG is also GPL-3.0-or-later, so mods built from this template are distributed under the same terms.
