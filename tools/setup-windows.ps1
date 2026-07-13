<#
=============================================================================
aima_framework — Windows one-click project setup (scaffolds the PARENT project).

aima_framework is a PURE, drop-in dependency you copy into your game folder:
    MyGame\aima_framework\                 <- you copied this whole folder here
    MyGame\aima_framework\tools\setup.bat  <- you ran this
Running this turns the PARENT folder (MyGame) into a buildable game project: it
stamps a project skeleton (game\, CMakeLists, presets, IDE wiring) into MyGame —
your GAME LOGIC lives there, in  MyGame\game\, OUTSIDE aima_framework (which you
never touch). Then it installs the toolchain and builds the `aima_game.exe` +
its hot-reloadable game module. An empty game = a BLACK window.

What it does, in order (each step is skipped if already satisfied):
  1. Verify Windows + winget.
  2. winget-install (if missing): CMake, Ninja, Git, Visual Studio 2022 Build Tools.
  3. Clone + bootstrap vcpkg to %USERPROFILE%\vcpkg if needed; set VCPKG_ROOT.
  4. SCAFFOLD: stamp tools\template\ into the parent (MyGame) on first run
     (game\, CMakeLists, presets, .vscode\.run, aima.project.json). Never
     overwrites an already-scaffolded project (your game\ is safe).
  5. IDE picker [1 VS Code / 2 CLion / 3 Visual Studio / 4 none] — opens MyGame.
  6. Unless -SkipBuild: import vcvars + configure + build aima_game.

Re-runnable: safe to run again; finished steps are skipped, game\ untouched.

Usage (from a normal or admin PowerShell):
    powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1
Or just double-click  tools\setup.bat .

Options:
    -Config release             build the windows-release preset instead of windows-debug
    -Ide vscode|clion|vs|none   pick the IDE non-interactively
    -SkipBuild                  do everything except the final cmake configure/build
=============================================================================
#>
[CmdletBinding()]
param(
    [ValidateSet('debug','release')]
    [string]$Config = 'debug',
    [ValidateSet('vscode','clion','vs','none','')]
    [string]$Ide = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
# tools\ lives inside the dropped aima_framework\ ; the PROJECT is its PARENT (MyGame).
$FrameworkDir = Split-Path -Parent $PSScriptRoot          # ...\MyGame\aima_framework
$RepoRoot     = Split-Path -Parent $FrameworkDir          # ...\MyGame  (the project)
$TemplateDir  = Join-Path $FrameworkDir 'tools\template'
$Preset       = "windows-$Config"
# Existing projects may ship their own CMakePresets.json with plain debug/release
# preset names (the template uses windows-*) — follow whatever the project defines.
$PresetFile = Join-Path $RepoRoot 'CMakePresets.json'
if ((Test-Path $PresetFile) -and ((Get-Content $PresetFile -Raw) -notmatch "`"windows-$Config`"")) {
    $Preset = $Config
}

function Info($m){ Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m){ Write-Host "    $m" -ForegroundColor Green }
function Warn($m){ Write-Host "    $m" -ForegroundColor Yellow }

# --- 0. Platform / prerequisites --------------------------------------------
if ($env:OS -ne 'Windows_NT') {
    throw "This setup script only runs on Windows. (Use tools/setup-mac.sh on macOS.)"
}
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install 'App Installer' from the Microsoft Store, then re-run."
}
if ((Split-Path -Leaf $FrameworkDir) -ne 'aima_framework') {
    Warn "Expected this script under  <YourGame>\aima_framework\tools\  but its parent is"
    Warn "'$(Split-Path -Leaf $FrameworkDir)'. Copy the whole aima_framework\ folder into your"
    Warn "game folder and run  aima_framework\tools\setup.bat  from there."
}

function Ensure-WingetPackage {
    param([string]$Id, [string]$ProbeCommand, [string[]]$ExtraArgs)
    if ($ProbeCommand -and (Get-Command $ProbeCommand -ErrorAction SilentlyContinue)) {
        Ok "$Id already present ($ProbeCommand)."; return
    }
    Info "Installing $Id via winget (this can take a while)..."
    $args = @('install','--id',$Id,'-e','--accept-source-agreements','--accept-package-agreements','--disable-interactivity')
    if ($ExtraArgs) { $args += $ExtraArgs }
    winget @args
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
        throw "winget failed to install $Id (exit $LASTEXITCODE)."
    }
}

function Find-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
}

# Stamp the project skeleton into the PARENT (MyGame) on first run.
function Scaffold-Project {
    if (Test-Path (Join-Path $RepoRoot 'CMakeLists.txt')) {
        Ok "Project already scaffolded ($(Split-Path -Leaf $RepoRoot)\CMakeLists.txt exists) — game\ left untouched."
        return
    }
    if (-not (Test-Path $TemplateDir)) { throw "Template missing ($TemplateDir). Is aima_framework\ intact?" }
    Info "Scaffolding the game project into  $(Split-Path -Leaf $RepoRoot)\  (outside aima_framework)..."
    Copy-Item -Path (Join-Path $TemplateDir '*') -Destination $RepoRoot -Recurse -Force
    Ok "Stamped: game\ + CMakeLists.txt + presets + .vscode\.run + aima.project.json"
    Ok "Your game logic = $(Split-Path -Leaf $RepoRoot)\game\  (edit there; aima_framework\ stays untouched)."
}

# Resolve this project's runnable CMake target / exe name. Priority:
#   1. aima.project.json "run" basename (the framework convention).
#   2. first add_executable(<name> ...) in the project's CMakeLists.txt — lets
#      pre-existing repos (e.g. hd2d_engine) work without an aima.project.json.
#   3. fallback: aima_game (the scaffold default).
function Get-ProjectExeName {
    $manifest = Join-Path $RepoRoot 'aima.project.json'
    if (Test-Path $manifest) {
        try {
            $run = (Get-Content $manifest -Raw | ConvertFrom-Json).run
            if ($run) { return (Split-Path -Leaf $run) }
        } catch {}
    }
    $cml = Join-Path $RepoRoot 'CMakeLists.txt'
    if (Test-Path $cml) {
        $m = Select-String -Path $cml -Pattern 'add_executable\(\s*([A-Za-z0-9_]+)' | Select-Object -First 1
        if ($m) { return $m.Matches[0].Groups[1].Value }
    }
    return 'aima_game'
}

# IDE wiring (.vscode\) is git-ignored, so regenerate it per-machine from the
# template whenever it's missing — even on an already-scaffolded project (where
# Scaffold-Project is skipped). Existing wiring is left untouched.
function Stamp-Wiring {
    param([string]$Sub)
    $dest = Join-Path $RepoRoot $Sub
    $src  = Join-Path $TemplateDir $Sub
    if (Test-Path $dest) { Ok "$Sub\ already present — left untouched."; return }
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $dest -Recurse -Force
        Ok "Regenerated $Sub\ from template (git-ignored; per-machine, not committed)."
    } else { Warn "Template has no $Sub\ to regenerate." }
}

# CLion's shared run config (.run\) hardcodes the target name, so a plain copy
# would point at a non-existent 'aima_game' target in projects whose exe differs.
# Regenerate it with the real target substituted in (and the file named after it).
function Stamp-RunConfig {
    $dest = Join-Path $RepoRoot '.run'
    if (Test-Path $dest) { Ok ".run\ already present — left untouched."; return }
    $src = Join-Path $TemplateDir '.run\aima_game.run.xml'
    if (-not (Test-Path $src)) { Warn "Template has no .run\aima_game.run.xml to regenerate."; return }
    $target = Get-ProjectExeName
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    (Get-Content $src -Raw).Replace('aima_game', $target) |
        Set-Content -Path (Join-Path $dest "$target.run.xml") -NoNewline
    Ok "Regenerated .run\$target.run.xml (CMake target '$target'; git-ignored, per-machine)."
}

function Setup-Ide {
    $choice = $Ide
    if (-not $choice) {
        Write-Host "`nWhich IDE do you want to use?" -ForegroundColor Cyan
        Write-Host "  [1] VS Code   [2] CLion   [3] Visual Studio   [4] none (skip)"
        switch ((Read-Host 'Enter 1, 2, 3, or 4').Trim()) {
            '1' { $choice = 'vscode' }
            '2' { $choice = 'clion' }
            '3' { $choice = 'vs' }
            default { $choice = 'none' }
        }
    }
    if ($choice -eq 'none') { Ok "Skipping IDE setup."; return }

    if ($choice -eq 'vscode') {
        Ensure-WingetPackage -Id 'Microsoft.VisualStudioCode' -ProbeCommand 'code'
        Stamp-Wiring '.vscode'
        Info "VS Code — one-click run is pre-wired (.vscode\ was stamped into the project):"
        Write-Host "    1. Install the recommended extensions when prompted (CMake Tools + C/C++)."
        Write-Host "    2. Pick the '$Preset' preset in the CMake status bar (it configures automatically)."
        Write-Host "    3. Press F5  ->  it builds aima_game, then launches it (black window). Done."
        $codeExe = @("$env:LOCALAPPDATA\Programs\Microsoft VS Code\Code.exe",
                     "$env:ProgramFiles\Microsoft VS Code\Code.exe") |
                   Where-Object { Test-Path $_ } | Select-Object -First 1
        Info "Opening VS Code at the project (adds it to File > Open Recent)..."
        if (Get-Command code -ErrorAction SilentlyContinue) { code "$RepoRoot" }
        elseif ($codeExe) { Start-Process -FilePath $codeExe -ArgumentList "`"$RepoRoot`"" }
        else { Write-Host "    (Open VS Code, then File > Open Folder > $RepoRoot.)" }
    }
    elseif ($choice -eq 'clion') {
        $clionExe = Get-ChildItem "$env:ProgramFiles\JetBrains\CLion*\bin\clion64.exe" -ErrorAction SilentlyContinue |
                    Sort-Object FullName -Descending | Select-Object -First 1
        if ($clionExe) { Ok "CLion already present ($($clionExe.FullName))." }
        else {
            Ensure-WingetPackage -Id 'JetBrains.CLion' -ProbeCommand $null
            $clionExe = Get-ChildItem "$env:ProgramFiles\JetBrains\CLion*\bin\clion64.exe" -ErrorAction SilentlyContinue |
                        Sort-Object FullName -Descending | Select-Object -First 1
        }
        $target = Get-ProjectExeName
        Stamp-RunConfig
        Info "CLion — a shared run config (.run\$target.run.xml) was regenerated for this machine:"
        Write-Host "    1. File > Open > select  $RepoRoot"
        Write-Host "    2. One time: enable the '$Preset' CMake preset profile when CLion offers it"
        Write-Host "       (it reads CMakePresets.json + `$env:VCPKG_ROOT), and add a Visual Studio toolchain."
        Write-Host "    3. Pick the '$target' run config (top-right) and press Run (Shift+F10). Done."
        Info "Opening CLion at the project (adds it to Recent Projects)..."
        if ($clionExe) { Start-Process -FilePath $clionExe.FullName -ArgumentList "`"$RepoRoot`"" }
        else { Write-Host "    (Launch CLion from the Start menu and open the folder above.)" }
    }
    elseif ($choice -eq 'vs') {
        $vsInstall = Find-VsInstall
        if (-not $vsInstall) {
            Warn "Visual Studio with the MSVC toolchain wasn't found; the .sln can't be opened/built."
            Warn "(Install Visual Studio 2022 Community from https://visualstudio.microsoft.com, then re-run.)"
            return
        }
        if (-not $env:VCPKG_ROOT) { Warn "VCPKG_ROOT not set; the .sln generate step needs it."; return }
        Info "Generating a Visual Studio solution (cmake -G ""Visual Studio 17 2022"") ..."
        $vsBuild = Join-Path $RepoRoot 'build\vs'
        cmake -S "$RepoRoot" -B "$vsBuild" -G "Visual Studio 17 2022" -A x64 `
            -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
            -DVCPKG_TARGET_TRIPLET=x64-windows
        if ($LASTEXITCODE -ne 0) { throw "cmake -G ""Visual Studio 17 2022"" failed." }
        # The .sln is named after the top-level project() (aima_game, hd2d, ...),
        # not the exe target — glob for it instead of hard-coding a name.
        $sln = Get-ChildItem -Path (Join-Path $vsBuild '*.sln') -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($sln) {
            $target = Get-ProjectExeName
            Ok "Visual Studio solution: $($sln.FullName)"
            Write-Host "    In Visual Studio: right-click '$target' > Set as Startup Project, then press F5."
            Info "Opening Visual Studio at the solution (adds it to Recent)..."
            Start-Process -FilePath $sln.FullName
        } else { Warn "No .sln generated under $vsBuild." }
    }
}

# --- 1. CMake + Ninja + Git -------------------------------------------------
Ensure-WingetPackage -Id 'Kitware.CMake'     -ProbeCommand 'cmake'
Ensure-WingetPackage -Id 'Ninja-build.Ninja' -ProbeCommand 'ninja'
Ensure-WingetPackage -Id 'Git.Git'           -ProbeCommand 'git'

# --- 2. Visual Studio 2022 Build Tools (MSVC toolchain + Windows SDK) --------
$VcComponents = @(
    '--add', 'Microsoft.VisualStudio.Workload.VCTools',
    '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '--add', 'Microsoft.VisualStudio.Component.Windows11SDK.22621',
    '--includeRecommended'
)

# Any VS/Build Tools install, regardless of installed workloads (Find-VsInstall
# filters on the C++ toolchain component, so it misses C++-less installs).
function Find-AnyVsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    & $vswhere -latest -prerelease -products * -property installationPath 2>$null | Select-Object -First 1
}

$vs = Find-VsInstall
if (-not $vs) {
    $anyVs = Find-AnyVsInstall
    if ($anyVs) {
        # VS is registered but the C++ workload is missing (typically an earlier quiet
        # install that died mid-way). winget reports 'already installed' and does
        # nothing, so drive the VS Installer directly to add the workload.
        Info "Found Visual Studio at $anyVs without the MSVC toolchain; adding the C++ workload..."
        Warn "Multi-GB download; a UAC / admin prompt may appear."
        $vsSetup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        $vsArgs = @('modify', '--installPath', "$anyVs") + $VcComponents + @('--quiet', '--norestart')
        $vsProc = Start-Process -FilePath $vsSetup -ArgumentList $vsArgs -Wait -PassThru
        # 3010 = success, reboot required.
        if ($vsProc.ExitCode -ne 0 -and $vsProc.ExitCode -ne 3010) {
            throw "VS Installer modify failed (exit $($vsProc.ExitCode)). Open 'Visual Studio Installer' and add the 'Desktop development with C++' workload manually, then re-run."
        }
    } else {
        Info "Installing Visual Studio 2022 Build Tools (MSVC + Windows SDK)..."
        Warn "Multi-GB download; a UAC / admin prompt may appear."
        $override = "--quiet --wait --norestart $($VcComponents -join ' ')"
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
            --accept-source-agreements --accept-package-agreements `
            --override "$override"
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
            # Exit 1 from the VS bootstrapper usually means: not elevated / UAC declined,
            # another VS Installer instance running, or a pending reboot.
            throw "Build Tools install failed (exit $LASTEXITCODE). Try again from an elevated (admin) PowerShell, close any open Visual Studio Installer, or reboot and re-run."
        }
    }
    $vs = Find-VsInstall
}
if (-not $vs) { throw "MSVC toolchain still not found after install. Re-run this script in a new shell (or reboot if the installer asked for it)." }
Ok "MSVC toolchain: $vs"

# --- 3. vcpkg (manifest mode) -----------------------------------------------
$vcpkgRoot = $env:VCPKG_ROOT
$vcpkgDir  = Join-Path $env:USERPROFILE 'vcpkg'
function Test-VcpkgReady($dir){ $dir -and (Test-Path (Join-Path $dir 'scripts\buildsystems\vcpkg.cmake')) }

if (Test-VcpkgReady $vcpkgRoot) {
    Ok "VCPKG_ROOT already set: $vcpkgRoot"
} elseif (Test-VcpkgReady $vcpkgDir) {
    $vcpkgRoot = $vcpkgDir
    Ok "Found existing vcpkg at $vcpkgDir (not re-cloning / not re-bootstrapping)."
} else {
    Info "Cloning vcpkg to $vcpkgDir ..."
    git clone https://github.com/microsoft/vcpkg "$vcpkgDir"
    if ($LASTEXITCODE -ne 0) { throw "git clone vcpkg failed." }
    Info "Bootstrapping vcpkg ..."
    & (Join-Path $vcpkgDir 'bootstrap-vcpkg.bat') -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed." }
    $vcpkgRoot = $vcpkgDir
    Ok "vcpkg ready: $vcpkgDir"
}
[Environment]::SetEnvironmentVariable('VCPKG_ROOT', $vcpkgRoot, 'User')
$env:VCPKG_ROOT = $vcpkgRoot
Ok "VCPKG_ROOT = $vcpkgRoot  (persisted for your user)"

# --- 4. Scaffold the PARENT project (first run only) ------------------------
Scaffold-Project

# --- 5. Configure + build (or just open the IDE) ----------------------------
if ($SkipBuild) {
    Setup-Ide
    Info "Setup complete. (-SkipBuild given; not building.)"
    Write-Host "`nTo build later, from a Developer PowerShell for VS:" -ForegroundColor Cyan
    Write-Host "    cmake --preset $Preset"
    Write-Host "    cmake --build --preset $Preset"
    return
}

Info "Importing MSVC developer environment..."
$devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (Test-Path $devShell) {
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
} else {
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "Neither DevShell module nor vcvars64.bat found under $vs." }
    cmd /c "`"$vcvars`" && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] }
    }
}

Info "Configuring (cmake --preset $Preset)..."
Push-Location $RepoRoot
try {
    # Explicit overrides make configure robust on fresh machines:
    #  - ninja lives behind a winget Links shim the DevShell PATH may miss -> full path
    #  - a project's presets may hard-code another machine's vcpkg path -> this vcpkg
    $cfgArgs = @('--preset', "$Preset")
    $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninjaCmd) { $cfgArgs += "-DCMAKE_MAKE_PROGRAM=$($ninjaCmd.Source)" }
    $cfgArgs += "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake')"
    cmake @cfgArgs
    if ($LASTEXITCODE -ne 0) {
        # A failed earlier configure leaves a poisoned CMakeCache (e.g. a cached
        # CMAKE_MAKE_PROGRAM-NOTFOUND) -> wipe the build dir and retry once.
        $cacheDir = Join-Path $RepoRoot "build\$Preset"
        if (Test-Path (Join-Path $cacheDir 'CMakeCache.txt')) {
            Warn "Configure failed; clearing stale cache ($cacheDir) and retrying..."
            Remove-Item -Recurse -Force $cacheDir
            cmake @cfgArgs
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed." }
    Info "Building (cmake --build --preset $Preset)..."
    cmake --build --preset "$Preset"
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed." }
} finally {
    Pop-Location
}

# exe name from aima.project.json's "run" / CMakeLists target (see helper).
$exeName = Get-ProjectExeName
$exe = Join-Path $RepoRoot "build\$Preset\bin\$exeName.exe"
Ok "Done. Executable: $exe"
if (Test-Path $exe) {
    Write-Host "`nRun it (opens a BLACK window — your empty game):" -ForegroundColor Cyan
    Write-Host "    & `"$exe`""
    Write-Host "    (headless check: `$env:AIMA_SHOT='C:\Temp\shot.png'; `$env:AIMA_SHOTFRAME='30'; & `"$exe`")"
    Write-Host "`nNext: run  aima_framework\tools\issue-token.bat  for your /bind token, then tell" -ForegroundColor Cyan
    Write-Host "      the Telegram bot what game to build."
}

Setup-Ide
