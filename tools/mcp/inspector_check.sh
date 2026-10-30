#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks freetvc-headless's MCP server with the official MCP Inspector CLI (a real third-party client).
# usage: tools/mcp/inspector_check.sh <inspector dir> <work dir>
#   <inspector dir>: where `npm install @modelcontextprotocol/inspector` ran (any folder)
# Needs Node 22 on PATH (or in NODE22_BIN) and the ROMs in FREETVC_ROM_DIR (default testdata/roms).
set -eu
INSPECTOR="$1/node_modules/.bin/mcp-inspector"
WORK="$2"
mkdir -p "$WORK"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HEADLESS="$ROOT/build/gcc-release/src/freetvc-headless"
ROMS="${FREETVC_ROM_DIR:-$ROOT/testdata/roms}"
PORT=6599
export PATH="${NODE22_BIN:+$NODE22_BIN:}$PATH" MCP_CATALOG_PATH="$WORK/catalog.json" MCP_CLIENT_CONFIG_PATH="$WORK/client.json"

echo "== stdio: tools/list --strict in each protocol era"
for era in legacy modern auto; do
    "$INSPECTOR" --cli --protocol-era "$era" --format json -e "FREETVC_ROM_DIR=$ROMS" \
        "$HEADLESS" --preset 64k+-2.2 --mcp-stdio --method tools/list --strict > "$WORK/list-$era.json" 2> "$WORK/list-$era.err"
    python3 -I -c "import json,sys; r=json.load(open(sys.argv[1])); r=r.get('result',r); print(sys.argv[2], len(r['tools']), 'tools')" \
        "$WORK/list-$era.json" "$era"
done

echo "== HTTP: one emulator, BASIC typed over MCP"
FREETVC_ROM_DIR="$ROMS" "$HEADLESS" --preset 64k+-2.2 --mcp-port $PORT --mcp-token check 2> "$WORK/server.err" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
sleep 0.5
call() {
    era="$1"; tool="$2"; args="$3"
    "$INSPECTOR" --cli --protocol-era "$era" --format json "http://127.0.0.1:$PORT/mcp" --header "Authorization: Bearer check" \
        --method tools/call --tool-name "$tool" --tool-args-json "$args"
}
call modern run '{"frames":330}' > /dev/null
call legacy press_keys '{"keys":["SPACE"],"settle_frames":100}' > /dev/null
printf '%s' '{"text":"print 6*7\n"}' > "$WORK/type.json"
call modern type_text "$(cat "$WORK/type.json")" > /dev/null
call auto screen_text '{}' > "$WORK/screen.json"
python3 -I -c "import json,sys; r=json.load(open(sys.argv[1]))['result']; t=r['content'][0]['text']; print(t); sys.exit(0 if 'print 6*7\n 42\nok' in t else 1)" "$WORK/screen.json"
echo "inspector_check: PASS"
