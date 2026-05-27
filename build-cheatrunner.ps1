param(
  [switch]$Clean,
  [switch]$ConfigureOnly,
  [switch]$SkipJsCheck,
  [switch]$Sqlite,
  [switch]$NoSqlite
)

$ErrorActionPreference = "Stop"

function Fail($Message) {
  Write-Error $Message
  exit 1
}

$payloadRoot = Resolve-Path $PSScriptRoot
$projectRoot = $payloadRoot
$parentRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildDir    = Join-Path $payloadRoot "build"
$cacheFile   = Join-Path $buildDir "CMakeCache.txt"
$jsCheckScript = Join-Path $projectRoot "tools\check_dashboard_js.py"

# --- SDK resolution (search in priority order) ---
$sdkCandidates = @()
if ($env:PS5_PAYLOAD_SDK) {
  $sdkCandidates += $env:PS5_PAYLOAD_SDK
}
$sdkCandidates += (Join-Path $payloadRoot "PS5-Payload-dev\sdk-master")
$sdkCandidates += (Join-Path $payloadRoot "ps5-payload-sdk")
$sdkCandidates += (Join-Path $projectRoot "PS5-Payload-dev\sdk-master")
$sdkCandidates += (Join-Path $projectRoot "ps5-payload-sdk")
$sdkCandidates += (Join-Path $parentRoot "PS5-Payload-dev\sdk-master")
$sdkCandidates += (Join-Path $parentRoot "ps5-payload-sdk")

$sdkRoot = $null
foreach ($candidate in $sdkCandidates) {
  $tc = Join-Path $candidate "toolchain\prospero.cmake"
  if (Test-Path $tc) {
    $sdkRoot = $candidate
    break
  }
}

if (-not $sdkRoot) {
  Write-Error @"
PS5 payload SDK not found.
Expected one of:
  - environment variable PS5_PAYLOAD_SDK
  - $payloadRoot\PS5-Payload-dev\sdk-master
  - $payloadRoot\ps5-payload-sdk
  - $projectRoot\PS5-Payload-dev\sdk-master
  - $projectRoot\ps5-payload-sdk
  - $parentRoot\PS5-Payload-dev\sdk-master
  - $parentRoot\ps5-payload-sdk
"@
  exit 1
}

Write-Host "Using PS5 payload SDK: $sdkRoot"

$toolchainPath = Join-Path $sdkRoot "toolchain\prospero.cmake"

# --- SDK lib checks ---
$sdkLibDir = Join-Path $sdkRoot "target\lib"

$requiredLibs = @(
  "libkernel_sys.so",
  "libSceSystemService.so",
  "libSceUserService.so"
)

foreach ($lib in $requiredLibs) {
  $libPath = Join-Path $sdkLibDir $lib
  if (-not (Test-Path $libPath)) {
    Fail "Required SDK library not found: $libPath`nVerify that the PS5 payload SDK is fully populated."
  }
}

$sqlitePath = Join-Path $sdkLibDir "libsqlite3.so"
if (Test-Path $sqlitePath) {
  Write-Warning "libsqlite3.so found in SDK lib dir -- not needed; use CHEATRUNNER_SQLITE_BUNDLED=ON to bundle the amalgamation instead."
}

$lncUtilPath = Join-Path $sdkLibDir "libSceLncUtil.so"
if (Test-Path $lncUtilPath) {
  Write-Warning "libSceLncUtil.so found -- sceLncUtil* stubs are already in libSceSystemService; linking both may cause duplicate symbol errors."
}

# --- cmake ---
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCmd) {
  Fail "cmake is not installed or not in PATH."
}

# --- Ninja ---
$sdkNinja = Join-Path $sdkRoot "win\ninja.exe"
$ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue

if ($ninjaCmd) {
  $ninjaPath = $ninjaCmd.Source
} elseif (Test-Path $sdkNinja) {
  $ninjaPath = $sdkNinja
} else {
  Fail "Ninja was not found. Install Ninja or ensure the SDK win\ninja.exe exists."
}

Write-Host "Using Ninja: $ninjaPath"

# --- Dashboard JS check ---
if (-not $SkipJsCheck) {
  if (Test-Path $jsCheckScript) {
    $pyCmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $pyCmd) { $pyCmd = Get-Command python3 -ErrorAction SilentlyContinue }
    if ($pyCmd) {
      Write-Host "Running dashboard JS syntax check..."
      & $pyCmd.Source $jsCheckScript
      if ($LASTEXITCODE -ne 0) {
        Fail "Dashboard JS syntax check failed. Fix the error or pass -SkipJsCheck to skip."
      }
    } else {
      Write-Host "python not found -- skipping JS syntax check. Install Python to enable."
    }
  } else {
    Write-Host "JS check script not found at $jsCheckScript -- skipping."
  }
}

# --- Clean ---
if ($Clean -and (Test-Path $buildDir)) {
  $resolvedBuild   = (Resolve-Path $buildDir).Path
  $resolvedPayload = (Resolve-Path $payloadRoot).Path
  if (-not $resolvedBuild.StartsWith($resolvedPayload, [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail "Refusing to remove build dir outside payload root: $resolvedBuild"
  }
  Write-Host "Cleaning build directory: $resolvedBuild"
  Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

# --- Stale artifact check ---
$ninjaFile = Join-Path $buildDir "build.ninja"
if (-not $Clean -and (Test-Path $ninjaFile)) {
  $srcDir = (Join-Path $payloadRoot "src").Replace('\', '/')
  $ninjaContent = Get-Content -LiteralPath $ninjaFile -Raw -ErrorAction SilentlyContinue
  if ($ninjaContent) {
    # Files that must never appear in a clean build (removed subsystems)
    $removedSources = @('cr_hotkey.c', 'cr_scepad_compat.h')
    $staleRemoved = $removedSources | Where-Object { $ninjaContent -match [regex]::Escape($_) }
    if ($staleRemoved) {
      $names = $staleRemoved -join ', '
      Write-Warning "Stale build cache references removed subsystem source(s): $names"
      Write-Host "Auto-cleaning build directory to ensure removed code is not linked."
      Remove-Item -LiteralPath $buildDir -Recurse -Force
    } else {
      # Also check for files referenced in cache that no longer exist on disk
      $cPathPattern = [regex]::Escape($srcDir) + '/\S+\.c'
      $missing = [regex]::Matches($ninjaContent, $cPathPattern, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase) |
        ForEach-Object { $_.Value.Replace('/', '\') } |
        Select-Object -Unique |
        Where-Object { -not (Test-Path $_) }
      if ($missing) {
        $names = ($missing | ForEach-Object { Split-Path $_ -Leaf }) -join ', '
        Write-Warning "Stale build cache references deleted source file(s): $names"
        Write-Host "Auto-cleaning build directory to avoid linker errors."
        Remove-Item -LiteralPath $buildDir -Recurse -Force
      }
    }
  }
}

# --- SQLite amalgamation check ---
$sqliteC = Join-Path $payloadRoot "src\third_party\sqlite3.c"
$sqliteH = Join-Path $payloadRoot "src\third_party\sqlite3.h"
$enableSqlite = $false

if ($NoSqlite) {
  Write-Host "SQLite disabled (-NoSqlite)."
} elseif ($Sqlite) {
  if (-not (Test-Path $sqliteC) -or -not (Test-Path $sqliteH)) {
    Write-Error @"
-Sqlite was requested but the SQLite amalgamation is missing.

Download it from https://sqlite.org/download.html
  (sqlite-amalgamation-*.zip  ->  sqlite3.c + sqlite3.h)

Then place both files in:
  $payloadRoot\src\third_party\sqlite3.c
  $payloadRoot\src\third_party\sqlite3.h

Then re-run:  .\build-cheatrunner.ps1 -Sqlite
"@
    exit 1
  }
  Write-Host "SQLite amalgamation found. Enabling CHEATRUNNER_SQLITE_BUNDLED."
  $enableSqlite = $true
} elseif ((Test-Path $sqliteC) -and (Test-Path $sqliteH)) {
  Write-Host "SQLite amalgamation found (auto-detected). Enabling CHEATRUNNER_SQLITE_BUNDLED. Pass -NoSqlite to disable."
  $enableSqlite = $true
} else {
  Write-Host "SQLite amalgamation not found in src\third_party -- building without SQLite."
}

# --- Configure ---
Write-Host "Configuring CheatRunnerPayload..."
$configureArgs = @(
  "-S", $payloadRoot,
  "-B", $buildDir,
  "-G", "Ninja",
  "-DCMAKE_MAKE_PROGRAM=$ninjaPath"
)

$configureArgs += ("-DCHEATRUNNER_SQLITE_BUNDLED=" + $(if ($enableSqlite) { "ON" } else { "OFF" }))

if (Test-Path $cacheFile) {
  $cacheLine = Select-String -Path $cacheFile -Pattern "^CMAKE_TOOLCHAIN_FILE:FILEPATH=" -ErrorAction SilentlyContinue
  if ($cacheLine) {
    $cachedToolchain = ($cacheLine.Line -split "=", 2)[1]
    if ($cachedToolchain -notlike "*prospero.cmake") {
      Fail "Existing build cache is not configured for PS5 toolchain. Run with -Clean once."
    }
  } else {
    Write-Host "Toolchain entry not found in existing cache, forcing toolchain argument."
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchainPath"
  }
} else {
  $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchainPath"
}

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
  if (Test-Path $cacheFile) {
    Fail "CMake configure failed. An existing build cache may be stale -- try running with -Clean."
  }
  Fail "CMake configure failed."
}

if ($ConfigureOnly) {
  Write-Host "Configure complete. Build skipped because -ConfigureOnly was used."
  exit 0
}

# --- Build ---
Write-Host "Building CheatRunner.elf..."
& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

$elfPath = Join-Path $buildDir "CheatRunner.elf"
if (Test-Path $elfPath) {
  $elfInfo = Get-Item $elfPath
  Write-Host ("Build complete: {0} ({1} bytes)" -f $elfInfo.FullName, $elfInfo.Length)

  # --- Post-build ELF string sanity check (FAIL on banned strings from removed subsystems) ---
  $bannedStrings = @(
    'ScePad support compiled',
    'pad open',
    'cr_hotkey',
    'illusionyy'
  )
  $elfText = [System.IO.File]::ReadAllText($elfPath, [System.Text.Encoding]::GetEncoding(28591))
  $found = $bannedStrings | Where-Object { $elfText.Contains($_) }
  if ($found) {
    Fail ("ELF contains string(s) from removed subsystems: " + ($found -join ', ') + "`nRun with -Clean to rebuild from scratch.")
  } else {
    Write-Host "ELF string sanity check passed -- no banned strings found."
  }
} else {
  Write-Host "Build finished, but CheatRunner.elf was not found in build directory."
}
