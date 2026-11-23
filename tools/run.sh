#!/usr/bin/env bash
# Starts the Solder simulator and opens the wallet and the virtual board side by side.
#   tools/run.sh            first start: factory WiFi password, no wallet
#   tools/run.sh --ready    skip setup (PIN 123456)
set -euo pipefail
cd "$(dirname "$0")/.."
PORT="${PORT:-8080}"
python3 tools/simulator.py --port "$PORT" "$@" &
SIM=$!
trap 'kill $SIM 2>/dev/null' EXIT INT TERM
sleep 1
URL="http://localhost:$PORT"
if command -v open >/dev/null; then open "$URL/device"; open "$URL"
elif command -v xdg-open >/dev/null; then xdg-open "$URL/device"; xdg-open "$URL"
else echo "Open $URL and $URL/device"; fi
wait $SIM
