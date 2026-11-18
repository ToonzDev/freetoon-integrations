#!/bin/sh
# Homey launcher (manifest.json "binary"): runs the build for this Toon.
DIR="$(dirname "$0")"
case "$(uname -m)" in
    armv5*) B="$DIR/homey.toon1" ;;
    *)      B="$DIR/homey.toon2" ;;
esac
if [ -x "$B" ]; then exec "$B" "$@"; fi
echo "homey: no build for $(uname -m) in this package" >&2
while true; do sleep 3600; done      # idle instead of respawn-storming init
