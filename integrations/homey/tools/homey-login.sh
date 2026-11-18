#!/bin/sh
# homey-login.sh - one-time Athom login for a Homey without API keys
# (Homey Early 2016/2018). After this the Homey app on the Toon renews its own
# 24-hour session token every day.
#
#   sh homey-login.sh                      prints the two values to type in the
#                                          app (Homey > settings > Oudere Homey)
#   sh homey-login.sh root@<toon>          writes them to the Toon over ssh
#   TOON_SSH="ssh -p 2222 root@nas" sh homey-login.sh -   same, with your own ssh command
#
# Needs the official Homey CLI (npm i -g homey) and curl. It logs in under a
# throwaway CLI profile, so your own `homey login` is left alone, and it never
# runs the CLI again after reading the login (the refresh token would rotate).
set -eu
command -v homey >/dev/null 2>&1 || { echo "Install the Homey CLI first: npm i -g homey" >&2; exit 1; }
command -v node >/dev/null 2>&1 || { echo "node is missing (it comes with the Homey CLI)" >&2; exit 1; }

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT INT TERM
chmod 700 "$D"
umask 077

echo "A browser window opens: log in with the Athom account of your Homey."
# The CLI can exit non-zero after a good login (its code prompt is still open),
# so judge by the stored login, not by the exit code.
HOME="$D" homey login || true

F="$D/.athom-cli/settings.json"
[ -f "$F" ] || { echo "No login found, try again." >&2; exit 1; }
REFRESH=$(node -e 'const s=require(process.argv[1]); process.stdout.write(s.homeyApi.token.refresh_token||"")' "$F")
ACCESS=$(node -e 'const s=require(process.argv[1]); process.stdout.write(s.homeyApi.token.access_token||"")' "$F")
[ -n "$REFRESH" ] && [ -n "$ACCESS" ] || { echo "The login did not give a refresh token." >&2; exit 1; }

# Which Homey: from the account (a call with the access token does not rotate anything)
printf 'Authorization: Bearer %s\n' "$ACCESS" > "$D/h"
curl -s -m 20 -H @"$D/h" https://api.athom.com/user/me > "$D/me.json"
node -e '
const me = require(process.argv[1]);
for (const h of (me.homeys || [])) console.log([h._id || h.id, h.name || "", h.modelName || h.model || ""].join("\t"));
' "$D/me.json" > "$D/homeys"
N=$(wc -l < "$D/homeys" | tr -d " ")
if [ "$N" -eq 0 ]; then echo "This account has no Homey." >&2; exit 1; fi
if [ "$N" -eq 1 ]; then HOMEY_ID=$(cut -f1 "$D/homeys")
else
    echo "Which Homey?"; nl -w2 -s") " "$D/homeys"
    printf "Number: "; read -r k
    HOMEY_ID=$(sed -n "${k}p" "$D/homeys" | cut -f1)
fi
case "$HOMEY_ID" in *[!0-9a-f]*|"") echo "Unexpected Homey id" >&2; exit 1 ;; esac

TARGET="${1:-}"
if [ -z "$TARGET" ]; then
    echo
    echo "Type these in the app (Homey > settings > Oudere Homey):"
    echo "  Homey-id:        $HOMEY_ID"
    echo "  Vernieuwsleutel: $REFRESH"
    exit 0
fi
if [ "$TARGET" = "-" ]; then SSH="${TOON_SSH:?set TOON_SSH}"; else SSH="ssh $TARGET"; fi

# Values go over stdin, not on a command line.
printf 'cloud_refresh=%s\nhomey_id=%s\ntoken_at=0\n' "$REFRESH" "$HOMEY_ID" | $SSH '
f=/mnt/data/integrations/homey/homey.conf
[ -f "$f" ] || { echo "Homey app not installed on this Toon" >&2; exit 1; }
umask 077
while IFS= read -r line; do
    k=${line%%=*}
    grep -v "^$k=" "$f" > "$f.new" || true
    printf "%s\n" "$line" >> "$f.new"
    mv "$f.new" "$f"
done
chmod 600 "$f"
echo "Saved on the Toon; the app renews its Homey session within a minute."'
