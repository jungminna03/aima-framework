#!/usr/bin/env bash
# Double-click launcher (Finder) for the aima game starter's macOS setup.
# A .command file opens in Terminal and runs; this just calls setup-mac.sh.
cd "$(dirname "$0")" || exit 1
echo "aima game starter — macOS setup (vcpkg + aima)"
echo "=============================================="
./setup-mac.sh "$@"
status=$?
echo ""
echo "(Done. You can close this window. Re-run any time — it skips finished steps.)"
exit $status
