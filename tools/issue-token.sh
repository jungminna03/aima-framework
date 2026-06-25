#!/usr/bin/env bash
# =============================================================================
# aima game starter — TOKEN ISSUER (macOS / Linux).
#
# Click this (or its .command wrapper). It runs the shared, cross-platform core
# (issue_token.py) which:
#   1. Generates a unique bind token  <slug>-<8 hex>  (slug = this folder's name,
#      lowercased). IDEMPOTENT: if this project is already in the shared registry,
#      it REUSES the existing token (it never reissues).
#   2. Registers/updates this project in the SHARED registry the live-dev Telegram
#      brain reads:  ~/.aima/registry.json  (creating ~/.aima if missing). Key =
#      the project name (this folder's name); value carries repo / desc / build /
#      run / shot / bind_token.
#   3. Prints the token + how to bind it in the Telegram group:  /bind <token> .
#
# Re-runnable + idempotent. Needs python3 (ships with macOS; install from your
# package manager on Linux). Pure stdlib — no pip installs.
# =============================================================================
set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"
command -v "$PY" >/dev/null 2>&1 || { echo "python3 not found on PATH; cannot issue a token." >&2; exit 1; }

exec "$PY" "$TOOLS_DIR/issue_token.py" "$@"
