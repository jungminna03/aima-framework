#!/usr/bin/env bash
# Double-click launcher (Finder) for the aima game starter's TOKEN ISSUER.
# A .command file opens in Terminal and runs; this just calls issue-token.sh.
cd "$(dirname "$0")" || exit 1
echo "aima game starter — TOKEN ISSUER"
echo "================================"
./issue-token.sh "$@"
status=$?
echo "(Copy the token above and type  /bind <token>  in your Telegram group."
echo " You can close this window.)"
exit $status
