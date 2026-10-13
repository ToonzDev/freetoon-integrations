#!/bin/sh
# package.sh — packages each folder in integrations/<id> into packages/<id>.tgz
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INTEGRATIONS_DIR="$REPO_ROOT/integrations"
PACKAGES_DIR="$REPO_ROOT/packages"

mkdir -p "$PACKAGES_DIR"

for d in "$INTEGRATIONS_DIR"/*; do
    if [ -d "$d" ]; then
        id="$(basename "$d")"
        echo "Packaging $id -> $PACKAGES_DIR/$id.tgz ..."
        (cd "$d" && tar czf "$PACKAGES_DIR/$id.tgz" .)
    fi
done

echo "Done packaging integrations."
