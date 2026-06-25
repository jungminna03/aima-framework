#!/usr/bin/env bash
# =============================================================================
# aima_framework — macOS one-click project setup (scaffolds the PARENT project).
#
# aima_framework is a PURE, drop-in dependency you copy into your game folder:
#     MyGame/aima_framework/         <- you copied this whole folder here
#     MyGame/aima_framework/tools/setup-mac.command   <- you ran this
# Running this turns the PARENT folder (MyGame) into a buildable game project:
# it stamps a project skeleton (game/, CMakeLists, presets, IDE wiring) into
# MyGame — your GAME LOGIC lives there, in  MyGame/game/, OUTSIDE aima_framework
# (which you never touch). Then it installs the toolchain and builds the native
# arm64 `aima_game` exe + its hot-reloadable game module. An empty game = a BLACK
# window.
#
# What it does, in order (each step is skipped if already satisfied):
#   1. Verify macOS + Xcode Command Line Tools (xcode-select --install if missing).
#   2. brew install cmake + ninja + git if missing.
#   3. Clone + bootstrap vcpkg to ~/vcpkg if needed; persist VCPKG_ROOT.
#   4. SCAFFOLD: stamp tools/template/ into the parent (MyGame) on first run
#      (game/, CMakeLists.txt, presets, .vscode/.run, aima.project.json). Never
#      overwrites an already-scaffolded project (your game/ is safe).
#   5. IDE picker [1 VS Code / 2 CLion / 3 Xcode / 4 none] — opens MyGame pre-wired.
#   6. Unless --skip-build: configure + build aima_game (cmake --preset mac-arm64-debug).
#
# Re-runnable: safe to run again; finished steps are skipped, game/ untouched.
#
# Usage (double-click tools/setup-mac.command, or):
#   tools/setup-mac.sh                 # scaffold parent, pick IDE, build
#   tools/setup-mac.sh --ide xcode     # non-interactive IDE choice (vscode|clion|xcode|none)
#   tools/setup-mac.sh --no-open       # don't open any IDE
#   tools/setup-mac.sh --skip-build    # scaffold + open IDE but don't build
#   tools/setup-mac.sh --skip-install  # don't brew-install / clone vcpkg (assume present)
# =============================================================================
set -euo pipefail

IDE=""            # vscode | clion | xcode | none ; empty => ask
OPEN_IDE=1
SKIP_BUILD=0
SKIP_INSTALL=0
PRESET="mac-arm64-debug"

while [ $# -gt 0 ]; do
  case "$1" in
    --ide)          IDE="${2:?--ide needs an argument}"; shift ;;
    --no-open)      OPEN_IDE=0 ;;
    --skip-build)   SKIP_BUILD=1 ;;
    --skip-install) SKIP_INSTALL=1 ;;
    --preset)       PRESET="${2:?--preset needs an argument}"; shift ;;
    -h|--help)      sed -n '2,38p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

# tools/ lives inside the dropped aima_framework/ ; the PROJECT is its PARENT (MyGame).
FRAMEWORK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../MyGame/aima_framework
REPO_ROOT="$(cd "$FRAMEWORK_DIR/.." && pwd)"                        # .../MyGame  (the project)
TEMPLATE_DIR="$FRAMEWORK_DIR/tools/template"
VCPKG_DIR="$HOME/vcpkg"

info(){ printf '\033[36m==> %s\033[0m\n' "$*"; }
ok(){   printf '\033[32m    %s\033[0m\n' "$*"; }
warn(){ printf '\033[33m    %s\033[0m\n' "$*"; }

[ -d /opt/homebrew/bin ] && export PATH="/opt/homebrew/bin:$PATH"
[ -d /usr/local/bin ] && export PATH="/usr/local/bin:$PATH"

# --- 0. Platform ------------------------------------------------------------
[ "$(uname)" = "Darwin" ] || { echo "This script only runs on macOS." >&2; exit 1; }
if [ "$(uname -m)" != "arm64" ]; then
  warn "This Mac is not Apple Silicon (arm64). The mac-arm64-debug preset targets arm64-osx;"
  warn "for an Intel Mac, pass --preset with an x64-osx preset (none is shipped by default)."
fi

# Guard against running from the wrong place: tools/ MUST sit inside aima_framework/.
if [ "$(basename "$FRAMEWORK_DIR")" != "aima_framework" ]; then
  warn "Expected this script under  <YourGame>/aima_framework/tools/  but its parent is"
  warn "'$(basename "$FRAMEWORK_DIR")'. Copy the whole aima_framework/ folder into your game"
  warn "folder and run  aima_framework/tools/setup-mac.command  from there."
fi

# --- 1. Xcode Command Line Tools --------------------------------------------
if ! xcode-select -p >/dev/null 2>&1; then
  info "Installing Xcode Command Line Tools (a GUI dialog will appear)..."
  xcode-select --install || true
  echo "    Finish the CLT install, then re-run this script." ; exit 1
fi
ok "Xcode Command Line Tools present ($(xcode-select -p))."
if [ ! -d "/Applications/Xcode.app" ]; then
  warn "Full Xcode.app not found — install it from the App Store to use the Xcode IDE option."
  warn "(CLI/VS Code/CLion builds still work via the Command Line Tools.)"
fi

# --- 2. Homebrew + cmake + ninja + git --------------------------------------
if [ "$SKIP_INSTALL" = "1" ]; then
  ok "--skip-install: assuming cmake / ninja / git are already present."
else
  command -v brew >/dev/null 2>&1 || { echo "Homebrew not found. Install from https://brew.sh then re-run." >&2; exit 1; }
  for pkg in cmake ninja git; do
    if command -v "$pkg" >/dev/null 2>&1; then ok "$pkg already present ($(command -v "$pkg"))."
    else info "brew install $pkg"; brew install "$pkg"; fi
  done
fi
for t in cmake ninja git; do
  command -v "$t" >/dev/null 2>&1 || { echo "$t still not found on PATH; cannot continue." >&2; exit 1; }
done

# --- 3. vcpkg (manifest mode) -----------------------------------------------
ensure_vcpkg(){
  if [ -n "${VCPKG_ROOT:-}" ] && [ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    ok "VCPKG_ROOT already set: $VCPKG_ROOT"; return
  fi
  if [ -f "$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake" ]; then
    export VCPKG_ROOT="$VCPKG_DIR"
    ok "Found existing vcpkg at $VCPKG_DIR (not re-cloning / not re-bootstrapping)."
  else
    if [ "$SKIP_INSTALL" = "1" ]; then
      echo "--skip-install but no vcpkg at \$VCPKG_ROOT or $VCPKG_DIR. Aborting." >&2; exit 1
    fi
    info "Cloning vcpkg to $VCPKG_DIR ..."
    git clone https://github.com/microsoft/vcpkg "$VCPKG_DIR"
    info "Bootstrapping vcpkg ..."
    "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics
    export VCPKG_ROOT="$VCPKG_DIR"
    ok "vcpkg ready: $VCPKG_DIR"
  fi
  case "$(basename "${SHELL:-/bin/zsh}")" in
    bash) profile="$HOME/.bash_profile" ;;
    *)    profile="$HOME/.zshrc" ;;
  esac
  if ! grep -qs 'VCPKG_ROOT' "$profile" 2>/dev/null; then
    printf '\n# vcpkg (aima_framework)\nexport VCPKG_ROOT="%s"\n' "$VCPKG_ROOT" >> "$profile"
    ok "Added VCPKG_ROOT to $profile"
  else
    ok "VCPKG_ROOT already in $profile"
  fi
}
ensure_vcpkg

# --- 4. Scaffold the PARENT project (first run only) ------------------------
# aima_framework stays pristine. Stamp the project skeleton into the parent
# (MyGame) so your game logic lives OUTSIDE aima_framework, in MyGame/game/.
scaffold_project(){
  if [ -f "$REPO_ROOT/CMakeLists.txt" ]; then
    ok "Project already scaffolded ($(basename "$REPO_ROOT")/CMakeLists.txt exists) — game/ left untouched."
    return
  fi
  if [ ! -d "$TEMPLATE_DIR" ]; then
    echo "Template missing ($TEMPLATE_DIR). Is aima_framework/ intact?" >&2; exit 1
  fi
  info "Scaffolding the game project into  $(basename "$REPO_ROOT")/  (outside aima_framework)..."
  cp -R "$TEMPLATE_DIR/." "$REPO_ROOT/"
  ok "Stamped: game/ + CMakeLists.txt + presets + .vscode/.run + aima.project.json"
  ok "Your game logic = $(basename "$REPO_ROOT")/game/  (edit there; aima_framework/ stays untouched)."
}
scaffold_project

# --- 5. IDE picker (opens MyGame) -------------------------------------------
# IDE wiring (.vscode/, .run/) is intentionally git-ignored — so it's regenerated
# per-machine here from the template whenever it's missing, even on an already-
# scaffolded project (where step 4's full scaffold is skipped). This is what lets
# a fresh clone get working IDE files without ever committing them. Existing
# wiring is left untouched (your local tweaks are safe). Xcode needs none of this:
# its project is generated fresh by `cmake -G Xcode` below.
stamp_wiring(){   # $1 = wiring subdir to ensure exists (e.g. .vscode)
  local sub="$1"
  if [ -e "$REPO_ROOT/$sub" ]; then
    ok "$sub/ already present — left untouched."
  elif [ -d "$TEMPLATE_DIR/$sub" ]; then
    cp -R "$TEMPLATE_DIR/$sub" "$REPO_ROOT/$sub"
    ok "Regenerated $sub/ from template (git-ignored; per-machine, not committed)."
  else
    warn "Template has no $sub/ to regenerate."
  fi
}

# Resolve this project's runnable CMake target / exe name. Priority:
#   1. aima.project.json "run" basename (the framework convention; .app-aware —
#      the inner Mach-O is already the target name).
#   2. first add_executable(<name> ...) in the project's CMakeLists.txt — lets
#      pre-existing repos (e.g. hd2d_engine) work without an aima.project.json.
#   3. fallback: aima_game (the scaffold default).
project_exe_name(){
  local name=""
  [ -f "$REPO_ROOT/aima.project.json" ] && name="$(python3 -c "import json,os;print(os.path.basename((json.load(open('$REPO_ROOT/aima.project.json')).get('run') or '')))" 2>/dev/null || true)"
  if [ -z "$name" ] && [ -f "$REPO_ROOT/CMakeLists.txt" ]; then
    name="$(grep -hoE 'add_executable\([[:space:]]*[A-Za-z0-9_]+' "$REPO_ROOT/CMakeLists.txt" | head -1 | sed -E 's/.*add_executable\([[:space:]]*//')"
  fi
  printf '%s' "${name:-aima_game}"
}

# CLion's shared run config (.run/) hardcodes the target name, so a plain copy
# would point at a non-existent 'aima_game' target in projects whose exe differs.
# Regenerate it with the real target substituted in (and the file named after it).
stamp_run_config(){
  if [ -d "$REPO_ROOT/.run" ]; then
    ok ".run/ already present — left untouched."; return
  fi
  local src="$TEMPLATE_DIR/.run/aima_game.run.xml"
  if [ ! -f "$src" ]; then
    warn "Template has no .run/aima_game.run.xml to regenerate."; return
  fi
  local target; target="$(project_exe_name)"
  mkdir -p "$REPO_ROOT/.run"
  sed "s/aima_game/$target/g" "$src" > "$REPO_ROOT/.run/$target.run.xml"
  ok "Regenerated .run/$target.run.xml (CMake target '$target'; git-ignored, per-machine)."
}

setup_ide(){
  local choice="$IDE"
  if [ -z "$choice" ]; then
    if [ "$OPEN_IDE" != "1" ] || [ ! -t 0 ]; then choice="none"; else
      printf '\n\033[36mWhich IDE do you want to use?\033[0m\n'
      printf '  [1] VS Code   [2] CLion   [3] Xcode   [4] none (skip)\n'
      read -r -p 'Enter 1, 2, 3, or 4: ' ans
      case "$ans" in
        1) choice="vscode" ;; 2) choice="clion" ;; 3) choice="xcode" ;; *) choice="none" ;;
      esac
    fi
  fi
  if [ "$OPEN_IDE" != "1" ] && [ "$choice" != "xcode" ]; then choice="none"; fi

  case "$choice" in
    none)
      ok "Skipping IDE setup." ;;
    vscode)
      stamp_wiring .vscode
      info "VS Code — one-click run is pre-wired (.vscode/ was stamped into the project):"
      echo  "    1. Install the recommended extensions when prompted (CMake Tools + C/C++)."
      echo  "    2. Pick the '$PRESET' preset in the CMake status bar (configures automatically)."
      echo  "    3. Press F5  ->  builds aima_game, then launches it (black window). Done."
      info "Opening VS Code at the project (adds it to File > Open Recent)..."
      if command -v code >/dev/null 2>&1; then code "$REPO_ROOT"
      elif open -a "Visual Studio Code" "$REPO_ROOT" 2>/dev/null; then :   # LaunchServices finds it anywhere
      else warn "VS Code not found. Install it (https://code.visualstudio.com) and open $REPO_ROOT."; fi ;;
    clion)
      local target; target="$(project_exe_name)"
      stamp_run_config
      info "CLion — a shared run config (.run/$target.run.xml) was regenerated for this machine:"
      echo  "    1. File > Open > select  $REPO_ROOT"
      echo  "    2. One time: enable the '$PRESET' CMake preset profile when CLion offers it."
      echo  "    3. Pick the '$target' run config (top-right) and press Run. Done."
      info "Opening CLion at the project (adds it to Recent Projects)..."
      # `open -a CLion` resolves via LaunchServices, so it works for /Applications
      # AND JetBrains Toolbox installs under ~/Applications (the old -d gate missed those).
      if open -a CLion "$REPO_ROOT" 2>/dev/null; then :
      elif command -v clion >/dev/null 2>&1; then clion "$REPO_ROOT"
      else warn "CLion not found. Install it (https://www.jetbrains.com/clion) and open $REPO_ROOT."; fi ;;
    xcode)
      if [ ! -d "/Applications/Xcode.app" ]; then
        warn "Xcode.app not installed — install it from the App Store, then re-run with --ide xcode."
      else
        if [ "$OPEN_IDE" = "1" ]; then info "Generating an Xcode project (cmake -G Xcode) and opening it..."
        else info "Generating an Xcode project (cmake -G Xcode); --no-open, so not launching Xcode..."; fi
        local xbuild="$REPO_ROOT/build/xcode"
        # CMAKE_XCODE_GENERATE_SCHEME=ON makes a per-target .xcscheme — WITHOUT it
        # CMake's Xcode project ships no shared scheme, so Xcode's Run (▶) button is
        # greyed / stuck on the non-runnable ALL_BUILD aggregate.
        cmake -S "$REPO_ROOT" -B "$xbuild" -G Xcode \
          -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
          -DVCPKG_TARGET_TRIPLET=arm64-osx \
          -DCMAKE_CXX_COMPILER=clang++ \
          -DCMAKE_XCODE_GENERATE_SCHEME=ON \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        # The .xcodeproj is named after the top-level project() — ProjectRC,
        # aima_game, etc. Find it instead of hard-coding a name.
        local xcodeproj
        xcodeproj="$(/bin/ls -d "$xbuild"/*.xcodeproj 2>/dev/null | head -1)"
        ok "Xcode project: $xcodeproj"
        local scheme; scheme="$(basename "${xcodeproj%.xcodeproj}")"
        # Pin the game's scheme as the sole shown one so Run (▶) targets IT — not
        # the non-runnable ALL_BUILD aggregate (the cause of "build succeeds but
        # nothing launches") nor the arimu/framework library schemes.
        local us="$xcodeproj/xcuserdata/$(whoami).xcuserdatad/xcschemes"
        mkdir -p "$us"
        cat > "$us/xcschememanagement.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>SchemeUserState</key><dict>
  <key>${scheme}.xcscheme</key><dict><key>isShown</key><true/><key>orderHint</key><integer>0</integer></dict>
  <key>ALL_BUILD.xcscheme</key><dict><key>isShown</key><false/></dict>
  <key>ZERO_CHECK.xcscheme</key><dict><key>isShown</key><false/></dict>
  <key>arimu.xcscheme</key><dict><key>isShown</key><false/></dict>
  <key>aima_framework.xcscheme</key><dict><key>isShown</key><false/></dict>
  <key>game_module.xcscheme</key><dict><key>isShown</key><false/></dict>
</dict></dict></plist>
PLIST
        echo "    Scheme '$scheme' is pinned — just press Run (▶)."
        if [ "$OPEN_IDE" = "1" ]; then
          info "Opening Xcode at the project (adds it to File > Open Recent)..."
          open -a Xcode "$xcodeproj"
        fi
      fi ;;
    *) warn "Unknown IDE '$choice' — skipping." ;;
  esac
}

# --- 6. Configure + build aima_game -----------------------------------------
build_project(){
  cd "$REPO_ROOT"
  info "Configuring (cmake --preset $PRESET)..."
  cmake --preset "$PRESET"
  info "Building (cmake --build --preset $PRESET)..."
  cmake --build --preset "$PRESET"
  # exe name from aima.project.json's "run" / CMakeLists target (see helper).
  local exename; exename="$(project_exe_name)"
  # On macOS the game is a .app bundle, so the binary lives inside it; fall back
  # to the plain path for non-bundle / non-mac builds.
  local exe="$REPO_ROOT/build/$PRESET/bin/$exename.app/Contents/MacOS/$exename"
  [ -x "$exe" ] || exe="$REPO_ROOT/build/$PRESET/bin/$exename"
  if [ -x "$exe" ]; then
    ok "Done. binary: $exe"
    echo ""
    info "Run it (opens a BLACK window — your empty game):"
    echo  "    \"$exe\""
    echo  "    (headless check: AIMA_SHOT=/tmp/shot.png AIMA_SHOTFRAME=30 \"$exe\" -> black PNG, exits 0)"
    echo ""
    info "Next: click  aima_framework/tools/issue-token  for your /bind token, then tell the"
    echo  "      Telegram bot what game to build."
  else
    echo "Build finished but $exe is missing." >&2; exit 1
  fi
}

if [ "$SKIP_BUILD" = "1" ]; then
  setup_ide
  info "Setup complete. (--skip-build given; not building.)"
  echo  "To build later:  cmake --preset $PRESET && cmake --build --preset $PRESET"
else
  build_project
  setup_ide
fi
