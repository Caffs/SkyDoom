# init.ps1 — run once after cloning this template to set up a new mod project.
#
# Usage: .\scripts\init.ps1 "AuthorName" "ModName"
#   AuthorName  Your name (e.g. "YourName")
#   ModName     PascalCase name for your mod (e.g. "BetterJumping")
#
# Run without arguments to be prompted interactively.

param(
    [string]$AuthorName,
    [string]$ModName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path "CMakeLists.txt")) {
    Write-Error "Error: must be run from the project root (e.g. .\scripts\init.ps1)"
    exit 1
}

if (-not $AuthorName) {
    $AuthorName = Read-Host "Author name (e.g. YourName)"
}
if (-not $ModName) {
    $ModName = Read-Host "Mod name — PascalCase (e.g. BetterJumping)"
}

$ModNameLower = $ModName.ToLower() -replace '_', '-'

if ($ModNameLower -notmatch '^[a-z0-9]+(-[a-z0-9]+)*$') {
    Write-Error "Error: mod name '$ModName' is not valid.`n  After lowercasing it must match ^[a-z0-9]+(-[a-z0-9]+)*$`n  Use only letters, digits, and hyphens (e.g. BetterJumping)"
    exit 1
}

$Placeholder      = "ExampleMod"
$PlaceholderLower = "examplemod"
$PlaceholderAuthor = "Author"

$Files = @(
    "CMakeLists.txt",
    "src\Plugin.cpp",
    "vcpkg.json",
    "README.md",
    ".github\workflows\build.yml"
)

Write-Host "Renaming placeholders..."
foreach ($File in $Files) {
    if (Test-Path $File) {
        $Content = Get-Content $File -Raw
        $Content = $Content `
            -replace [regex]::Escape($PlaceholderLower), $ModNameLower `
            -replace [regex]::Escape($Placeholder), $ModName `
            -replace [regex]::Escape($PlaceholderAuthor), $AuthorName
        Set-Content $File -Value $Content -NoNewline -Encoding utf8
    } else {
        Write-Warning "  warning: $File not found, skipping"
    }
}

Write-Host "Initialising submodules..."
git submodule update --init --recursive
if ($LASTEXITCODE -ne 0) {
    Write-Error "git submodule update failed"
    exit 1
}

Write-Host "Bootstrapping vcpkg..."
if (Test-Path "lib\vcpkg\bootstrap-vcpkg.bat") {
    & "lib\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
    if ($LASTEXITCODE -ne 0) {
        Write-Error "vcpkg bootstrap failed"
        exit 1
    }
} else {
    Write-Error "lib\vcpkg\bootstrap-vcpkg.bat not found — did submodules init correctly?"
    exit 1
}

Write-Host "Setting up .env..."
if (-not (Test-Path ".env")) {
    Copy-Item ".env.example" ".env"
    Write-Host "Copied .env.example to .env"
    Write-Host ""
    Write-Host "Edit .env and set your Skyrim paths before building:"
    Write-Host "  SKYRIM_MODS_FOLDER — path to your mod manager's staging folder"
    Write-Host "  SKYRIM_FOLDER      — path to your Skyrim installation (fallback)"
} else {
    Write-Host ".env already exists, skipping"
}

Write-Host ""
Write-Host "Done. Project set up as '$ModName' by '$AuthorName'"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Edit .env with your Skyrim paths"
Write-Host "  2. Run: cmake --preset release-windows"

