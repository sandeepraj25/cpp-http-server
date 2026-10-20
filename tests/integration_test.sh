#!/usr/bin/env bash
# End-to-end tests: starts the real server and talks to it over HTTP.
#   make integration                    (uses ./server)
#   SERVER=./server_asan tests/integration_test.sh
set -u

SERVER="${SERVER:-./server}"
PORT="${PORT:-18080}"
BASE="http://127.0.0.1:${PORT}"
PASS=0
FAIL=0

mkdir -p logs
PORT="$PORT" RATE_LIMIT_MAX=1000 "$SERVER" > logs/integration_server.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT

# Wait until the server accepts connections
for _ in $(seq 1 50); do
    curl -s -o /dev/null "$BASE/health" && break
    sleep 0.1
done

check() {   # check <description> <expected> <actual>
    if [[ "$3" == "$2" ]]; then
        PASS=$((PASS + 1)); echo "  ok    $1"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL  $1  (expected '$2', got '$3')"
    fi
}

contains() {   # contains <description> <needle> <haystack>
    if [[ "$3" == *"$2"* ]]; then
        PASS=$((PASS + 1)); echo "  ok    $1"
    else
        FAIL=$((FAIL + 1)); echo "  FAIL  $1  (missing '$2')"
    fi
}

code() { curl -s -o /dev/null -w "%{http_code}" "$@"; }

# Sends raw bytes over TCP and prints the reply (for requests curl won't send)
raw() {
    python3 - "$PORT" "$1" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=3)
s.sendall(sys.argv[2].encode().decode("unicode_escape").encode("latin-1"))
data = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk: break
        data += chunk
except socket.timeout:
    pass
print(data.decode("latin-1"))
PY
}

echo "REST API"
check    "GET /health is 200"            200 "$(code "$BASE/health")"
check    "POST /users creates (201)"     201 "$(code -X POST "$BASE/users" -d '{"name":"Alice","email":"alice@example.com"}')"
contains "GET /users lists Alice"        '"name":"Alice"' "$(curl -s "$BASE/users")"
check    "GET /users/1 is 200"           200 "$(code "$BASE/users/1")"
contains "PUT /users/1 updates name"     '"name":"Alicia"' "$(curl -s -X PUT "$BASE/users/1" -d '{"name":"Alicia"}')"
check    "DELETE /users/1 is 204"        204 "$(code -X DELETE "$BASE/users/1")"
check    "GET deleted user is 404"       404 "$(code "$BASE/users/1")"
check    "POST without fields is 400"    400 "$(code -X POST "$BASE/users" -d '{}')"
check    "Unknown route is 404"          404 "$(code "$BASE/nope")"
check    "Wrong method is 405"           405 "$(code -X POST "$BASE/health")"

echo "Static files"
check    "GET /static/index.html is 200" 200 "$(code "$BASE/static/index.html")"
contains "HTML MIME type"                "text/html" "$(curl -s -D - -o /dev/null "$BASE/static/index.html")"
check    "Path traversal is blocked"     403 "$(code --path-as-is "$BASE/static/../Makefile")"

echo "Security"
HEADERS="$(curl -s -D - -o /dev/null "$BASE/health")"
contains "CSP header"                    "Content-Security-Policy" "$HEADERS"
contains "X-Frame-Options header"        "X-Frame-Options: DENY" "$HEADERS"
contains "JSON output is escaped"        'a\",\"admin' "$(curl -s "$BASE/echo?msg=a%22,%22admin%22:%22true")"

echo "Robustness (each of these used to crash the whole server)"
check    "Bad %-escape is handled"       200 "$(code "$BASE/echo?msg=%zz")"
contains "Bad Content-Length -> 400"     "400 Bad Request" "$(raw 'POST /users HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n')"
contains "Huge Content-Length -> 413"    "413 Payload Too Large" "$(raw 'POST /users HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999\r\n\r\n')"
contains "Garbage request -> 400"        "400 Bad Request" "$(raw 'HELLO\r\n\r\n')"
check    "Server still alive"            200 "$(code "$BASE/health")"

echo "HTTP/1.1 behaviour"
PIPE="$(raw 'GET /echo?msg=one HTTP/1.1\r\nHost: x\r\n\r\nGET /echo?msg=two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')"
contains "Pipelined request 1 answered"  '"echo":"one"' "$PIPE"
contains "Pipelined request 2 answered"  '"echo":"two"' "$PIPE"
contains "Keep-Alive by default"         "Connection: keep-alive" "$(curl -s -D - -o /dev/null "$BASE/health")"

echo
echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
