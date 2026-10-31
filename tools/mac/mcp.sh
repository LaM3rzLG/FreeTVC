#!/bin/bash
# mcp.sh TOOL ['JSON-ARGS'] [image.png]: calls one freeTVC MCP tool over HTTP and prints the text result
ARGS="${2:-}"; [ -z "$ARGS" ] && ARGS='{}'
curl -s -X POST "http://127.0.0.1:${FREETVC_MCP_PORT:-6584}/mcp" -H "Authorization: Bearer ${FREETVC_MCP_TOKEN:-claudeteszt}" \
  -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$ARGS}}" |
python3 -c '
import json,sys,base64
r=json.load(sys.stdin)
if "error" in r: print("HIBA:", r["error"]); sys.exit(1)
for c in r["result"]["content"]:
    if c["type"]=="text": print(c["text"])
    elif c["type"]=="image":
        open(sys.argv[1],"wb").write(base64.b64decode(c["data"])); print("[kep ->", sys.argv[1], "]")
' "${3:-/dev/null}"
