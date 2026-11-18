#!/bin/sh
# bambuddy-relay.sh - push BamBuddy printer status to a FreeToon Toon.
#
# For networks where the Toon can't reach BamBuddy (e.g. Toon in an IoT VLAN)
# but a machine next to BamBuddy can reach the Toon. Runs anywhere with sh +
# curl: a tiny Alpine container, a NAS task, a Raspberry Pi.
#
#   BAMBUDDY_URL=http://127.0.0.1:8000 BAMBUDDY_KEY=... TOON=192.168.1.50 sh bambuddy-relay.sh
#
# Environment:
#   BAMBUDDY_URL   BamBuddy base URL as seen from this machine (required)
#   BAMBUDDY_KEY   BamBuddy API key (header X-API-Key), if BamBuddy needs one
#   PRINTER_ID     printer id in BamBuddy (default 1)
#   TOON           Toon address (required); TOON_PORT default 8766
#   TOON_TOKEN     shared secret, same as push_token in bambuddy.conf on the Toon
#   INTERVAL       seconds between pushes (default 15)
set -u
: "${BAMBUDDY_URL:?set BAMBUDDY_URL}" "${TOON:?set TOON}"
BAMBUDDY_KEY="${BAMBUDDY_KEY:-}"
PRINTER_ID="${PRINTER_ID:-1}"
TOON_PORT="${TOON_PORT:-8766}"
TOON_TOKEN="${TOON_TOKEN:-}"
INTERVAL="${INTERVAL:-15}"
TMP="${TMPDIR:-/tmp}/bambuddy-relay.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"; exit 0' INT TERM

bb() {   # GET from BamBuddy into a file; prints the HTTP code
    curl -s -m 10 -o "$2" -w '%{http_code}' -H "X-API-Key: $BAMBUDDY_KEY" "$BAMBUDDY_URL$1"
}
toon() { # POST a file to the Toon; prints the reply
    curl -s -m 10 -X POST -H "Content-Type: $3" -H "X-Toon-Token: $TOON_TOKEN" \
         --data-binary "@$2" "http://$TOON:$TOON_PORT$1"
}

last_err=""
while :; do
    code=$(bb "/api/v1/printers/$PRINTER_ID/status" "$TMP/status.json")
    if [ "$code" = "200" ]; then
        reply=$(toon /status "$TMP/status.json" application/json)
        case "$reply" in
            *'"need_cover":true'*)
                cover=$(sed -n 's/.*"cover_url":"\([^"]*\)".*/\1/p' "$TMP/status.json")
                # cover_url is null once a print is done; /cover still has the last plate
                [ -n "$cover" ] || cover="/api/v1/printers/$PRINTER_ID/cover"
                if [ "$(bb "$cover" "$TMP/cover.png")" = "200" ]; then
                    toon /cover "$TMP/cover.png" image/png >/dev/null
                fi ;;
            *'"ok":true'*) ;;
            *) [ "$last_err" = "toon" ] || echo "$(date '+%F %T') Toon $TOON:$TOON_PORT did not accept the status: $reply" >&2
               last_err=toon; sleep "$INTERVAL"; continue ;;
        esac
        [ -z "$last_err" ] || echo "$(date '+%F %T') ok again" >&2
        last_err=""
    else
        [ "$last_err" = "bb" ] || echo "$(date '+%F %T') BamBuddy returned HTTP $code" >&2
        last_err=bb
    fi
    sleep "$INTERVAL"
done
