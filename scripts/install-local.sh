#!/bin/sh
# install-local.sh - install an integration package on a FreeToon Toon without
# the Marketplace (for testing a package before it is in catalog/index.json).
#
# Run ON the Toon:   sh install-local.sh /tmp/<id>.tgz
#
# Does what /mnt/data/integrations-install.sh does after its download step:
# unpack into /mnt/data/integrations/<id>/ (settings files already there are
# kept), make it root-owned, write the inittab respawn row the installer
# itself would use (id 'i' + first 3 hex of md5(<id>)), so
# `/mnt/data/integrations-install.sh remove <id>` cleans it up too), reload
# init, and restart toonui so it reads the new manifest.json. toonui itself is
# not replaced.
set -eu
TGZ="${1:?usage: install-local.sh /tmp/<id>.tgz}"
[ -f "$TGZ" ] || { echo "no $TGZ" >&2; exit 1; }

STAGE=/tmp/install-local.$$
mkdir -p "$STAGE"
trap 'rm -rf "$STAGE"' EXIT
tar xzf "$TGZ" -C "$STAGE" manifest.json
ID=$(sed -n 's/^ *"id": *"\([a-z0-9-]*\)".*/\1/p' "$STAGE/manifest.json" | head -n 1)
BIN=$(sed -n 's/^ *"binary": *"\([^"]*\)".*/\1/p' "$STAGE/manifest.json" | head -n 1)
[ -n "$ID" ] && [ -n "$BIN" ] || { echo "manifest.json without id/binary" >&2; exit 1; }
case "$BIN" in */*|.*|*..*) echo "manifest binary '$BIN' must be a plain file name" >&2; exit 1 ;; esac

DIR=/mnt/data/integrations/$ID
LOG=/var/volatile/tmp/integration-$ID.log
ROW_ID=i$(printf "%s" "$ID" | md5sum | cut -c1-3)
ROW="$ROW_ID:2345:respawn:$DIR/$BIN >> $LOG 2>&1"

mkdir -p "$DIR"
tar xzf "$TGZ" -C "$DIR"
chmod +x "$DIR/$BIN" "$DIR/$BIN".toon1 "$DIR/$BIN".toon2 2>/dev/null || true
chown -R 0:0 "$DIR" 2>/dev/null || true
chmod -R go-w "$DIR" 2>/dev/null || true

if grep -q "^$ROW_ID:" /etc/inittab && ! grep -q "^$ROW_ID:.*$DIR/" /etc/inittab; then
    echo "inittab row $ROW_ID belongs to something else - not touching it" >&2; exit 1
fi
if [ -x /mnt/data/inittab_edit.sh ]; then
    /mnt/data/inittab_edit.sh upsert "$ROW_ID" "$ROW"
elif ! grep -q "^$ROW_ID:" /etc/inittab; then
    cp -p /etc/inittab /etc/inittab.bak
    cp /etc/inittab /etc/inittab.new
    echo "$ROW" >> /etc/inittab.new
    if [ "$(wc -l < /etc/inittab.new)" -ge 15 ]; then sync; mv -f /etc/inittab.new /etc/inittab; sync
    else rm -f /etc/inittab.new; echo "inittab check failed, unchanged" >&2; exit 1; fi
fi

# Stop a running daemon so init respawns the new binary; reload inittab.
# Match whole arguments only (the launcher execs $BIN.toon2 / $BIN.toon1), so a
# shell whose command line merely mentions the directory is left alone.
# TERM first so a daemon can clean up (bambuddy removes its firewall rule), then KILL.
pids=""
for p in /proc/[0-9]*; do
    [ "${p#/proc/}" = "$$" ] && continue
    case " $(tr '\0' ' ' 2>/dev/null < "$p/cmdline") " in
        *" $DIR/$BIN "*|*" $DIR/$BIN.toon1 "*|*" $DIR/$BIN.toon2 "*) pids="$pids ${p#/proc/}" ;;
    esac
done
if [ -n "$pids" ]; then
    kill $pids 2>/dev/null || true
    sleep 2
    kill -9 $pids 2>/dev/null || true
fi
kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true

# toonui scans /mnt/data/integrations/*/manifest.json only at start. The
# marker tells ui_launcher.sh this restart is deliberate (no crash-loop count).
if [ "${NO_TOONUI_RESTART:-0}" != 1 ]; then
    touch /var/volatile/tmp/toonui_restart
    killall toonui 2>/dev/null || pkill -x toonui 2>/dev/null || true
fi
echo "installed $ID in $DIR (inittab row $ROW_ID). Bind the tile: Settings > Tiles."
echo "logs: $LOG and /var/volatile/tmp/toonui.log"
