#!/bin/sh
# install-local.sh - install matter.tgz on a FreeToon Toon without the
# Marketplace (for testing before the package is in the catalog).
# Run ON the Toon:   sh install-local.sh /tmp/matter.tgz
#
# Does what /mnt/data/integrations-install.sh does after its download step:
# unpack into /mnt/data/integrations/matter/, root-owned, inittab respawn row
# "ibf7" (= 'i' + first 3 hex chars of md5("matter"), the installer's own row
# id, so `integrations-install.sh remove matter` cleans this up too), HUP init.
# Then restarts toonui so it reads the new manifest.json. It never touches
# toonui itself nor the Matter Bridge installation in /mnt/data/tsc.
set -eu
TGZ="${1:-/tmp/matter.tgz}"
ID=matter
DIR=/mnt/data/integrations/$ID
BIN=freetoon-app-matter
LOG=/var/volatile/tmp/integration-$ID.log
ROW_ID=i$(printf "%s" "$ID" | md5sum | cut -c1-3)
ROW="$ROW_ID:2345:respawn:$DIR/$BIN >> $LOG 2>&1"

[ -f "$TGZ" ] || { echo "no $TGZ" >&2; exit 1; }
mkdir -p "$DIR"
tar xzf "$TGZ" -C "$DIR"
[ -f "$DIR/manifest.json" ] && [ -x "$DIR/$BIN" ] || { echo "package incomplete" >&2; exit 1; }
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

# Stop a running daemon so init respawns the new binary, reload inittab.
for p in /proc/[0-9]*; do
    case "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" in
        *"$DIR/$BIN"*) kill -9 "${p#/proc/}" 2>/dev/null || true ;;
    esac
done
kill -HUP 1 2>/dev/null || telinit q 2>/dev/null || true

# toonui scans /mnt/data/integrations/*/manifest.json only at start. The
# marker tells ui_launcher.sh this restart is deliberate (no crash-loop count).
touch /var/volatile/tmp/toonui_restart
killall toonui 2>/dev/null || pkill -x toonui 2>/dev/null || true
echo "installed in $DIR (inittab row $ROW_ID). Bind the tile: Settings > Tiles."
echo "logs: $LOG and /var/volatile/tmp/toonui.log"
