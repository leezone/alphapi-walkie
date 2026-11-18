#!/bin/sh
cd "$(dirname "$0")"
python3 -m http.server 8000 --bind 127.0.0.1 &
SERVER=$!
trap "kill $SERVER 2>/dev/null" EXIT INT TERM
sleep 0.5
if command -v xdg-open >/dev/null 2>&1; then xdg-open "http://localhost:8000/index.html"; elif command -v open >/dev/null 2>&1; then open "http://localhost:8000/index.html"; fi
wait $SERVER
