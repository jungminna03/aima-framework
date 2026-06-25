#!/usr/bin/env python3
# =============================================================================
# aima — TOKEN ISSUER (generic, manifest-driven, cross-platform core).
#
# Called by issue-token.sh / .command (macOS/Linux) and issue-token.bat (Windows).
# Pure stdlib. ONE script for every layout — it auto-detects the PROJECT root:
#   * if tools/ lives inside an  aima_framework/  folder (the drop-in model:
#     MyGame/aima_framework/tools/...), the project is the PARENT of the
#     framework  →  MyGame.   ← game logic lives OUTSIDE aima_framework.
#   * otherwise tools/ sits at the project root (e.g. an existing game whose
#     tools/ is top-level)      →  tools/..
# Per-project data lives in an optional  aima.project.json  at the project root
# (so the SCRIPT never diverges between projects — only the data does). It:
#   1. Resolves the project (above) + name/desc/build/run/shot from the manifest.
#   2. Generates a unique token  <slug>-<8 hex>  (slug = name lowercased),
#      IDEMPOTENT: reuses this project's existing token if already registered.
#   3. Registers/updates the project in the SHARED registry the live-dev Telegram
#      brain reads:  ~/.aima/registry.json  (key = project name).
#   4. Prints the token + the  /bind <token>  instruction.
# =============================================================================
import json
import os
import secrets
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
_parent = os.path.dirname(TOOLS_DIR)                       # tools/..
if os.path.basename(_parent).lower() in ("aima_framework", "aima-framework"):
    PROJECT_ROOT = os.path.dirname(_parent)               # parent of framework = MyGame
else:
    PROJECT_ROOT = _parent                                # tools/ at project root
FOLDER_NAME = os.path.basename(PROJECT_ROOT)


def load_manifest():
    """Optional aima.project.json at the project root: per-project overrides."""
    path = os.path.join(PROJECT_ROOT, "aima.project.json")
    try:
        with open(path, "r", encoding="utf-8") as f:
            m = json.load(f)
        return (m if isinstance(m, dict) else {}), True
    except (FileNotFoundError, json.JSONDecodeError):
        return {}, False


def main():
    manifest, had_manifest = load_manifest()
    name = manifest.get("name") or FOLDER_NAME
    slug = name.lower()
    desc = manifest.get("desc") or f"{name} — a game built on aima_framework."
    build = manifest.get("build") or (
        "VCPKG_ROOT=$HOME/vcpkg cmake --preset mac-arm64-debug && "
        "cmake --build --preset mac-arm64-debug")
    # exe name is unknowable generically; the scaffolded template ships an
    # aima.project.json with the right "run", so this default is only a fallback.
    run = manifest.get("run") or f"build/mac-arm64-debug/bin/{name}"
    shot = manifest.get("shot") or "AIMA_SHOT={png} AIMA_SHOTFRAME=30"

    aima_dir = os.path.join(os.path.expanduser("~"), ".aima")
    os.makedirs(aima_dir, exist_ok=True)
    registry_path = os.path.join(aima_dir, "registry.json")

    try:
        with open(registry_path, "r", encoding="utf-8") as f:
            registry = json.load(f)
        if not isinstance(registry, dict):
            registry = {}
    except (FileNotFoundError, json.JSONDecodeError):
        registry = {}

    existing = registry.get(name) or {}                   # IDEMPOTENT: reuse token
    token = existing.get("bind_token") or f"{slug}-{secrets.token_hex(4)}"

    registry[name] = {
        "repo": PROJECT_ROOT,
        "desc": desc,
        "build": build,
        "run": run,
        "shot": shot,
        "bind_token": token,
    }

    tmp = registry_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(registry, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, registry_path)

    # ---- report ----
    use_color = sys.stdout.isatty() and os.name != "nt"
    g = "\033[32m" if use_color else ""
    c = "\033[36m" if use_color else ""
    b = "\033[1m" if use_color else ""
    y = "\033[33m" if use_color else ""
    r = "\033[0m" if use_color else ""

    print("")
    print(f"{c}==> Token issued for project '{name}'{r}")
    print(f"    repo:     {PROJECT_ROOT}")
    print(f"    registry: {registry_path}")
    print(f"    run:      {run}")
    if not had_manifest:
        print(f"{y}    (no aima.project.json — using defaults; run tools/setup first){r}")
    print("")
    print(f"{b}    토큰: {g}{token}{r}")
    print("")
    print(f"    텔레그램 그룹에서 이걸 쳐: {b}/bind {token}{r}")
    print(f"    (In the Telegram group, type:  /bind {token} )")
    print("")


if __name__ == "__main__":
    main()
