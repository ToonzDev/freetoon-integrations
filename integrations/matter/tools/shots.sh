#!/bin/sh
# Render preview/*.png: the screen in each bridge state, from the fixtures in
# tools/fixtures (copied into preview/build/tmp/root per shot).
set -eu
cd "$(dirname "$0")/.."
R=preview/build/render
F=tools/fixtures
mkdir -p preview/build/tmp preview

shot() {   # name, state, extra render args...
    name=$1; state=$2; shift 2
    rm -rf preview/build/tmp/root
    mkdir -p preview/build/tmp/root
    D=preview/build/tmp/root
    cp "$F/prefs.json" "$F/comfort.json" "$D/"
    printf '#!/bin/sh\n' > "$D/service.sh"; chmod 755 "$D/service.sh"
    # this shell stands in for the Node process and for the daemon
    case "$state" in
        waiting) cp "$F/status-waiting.json" "$D/status.json"; echo $$ > "$D/service.pid" ;;
        paired)  cp "$F/status-paired.json" "$D/status.json"; echo $$ > "$D/service.pid" ;;
        stopped) cp "$F/status-waiting.json" "$D/status.json"; cp "$F/service.log" "$D/" ;;
        missing) rm -f "$D/service.sh" ;;
        reset)   cp "$F/status-waiting.json" "$D/status.json" ;;
    esac
    act=idle
    [ "$state" = reset ] && act=reset
    printf '{"daemon_pid":%d,"action":"%s","last_action":"idle"}' $$ "$act" > "$D/action.json"
    MATTER_ROOT="$D" "$R" "preview/build/tmp/$name.ppm" "$@"
    python3 -I tools/ppm2png.py "preview/build/tmp/$name.ppm" "preview/$name.png"
    echo "preview/$name.png"
}
shot waiting-dark waiting
shot waiting-light waiting --light
shot waiting-warm waiting --theme 2
shot waiting-black waiting --theme 3
shot waiting-en waiting --en
shot paired-dark paired
shot stopped-dark stopped
shot missing-dark missing
shot reset-busy-dark reset
shot confirm-reset-dark waiting 882,548
