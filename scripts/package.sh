#!/bin/sh
# package.sh — compiles (if Makefile exists) and packages each integration in integrations/<id>
# into multi-arch (Toon 1 + Toon 2) packages/<id>.tgz
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INTEGRATIONS_DIR="$REPO_ROOT/integrations"
PACKAGES_DIR="$REPO_ROOT/packages"

mkdir -p "$PACKAGES_DIR"

for d in "$INTEGRATIONS_DIR"/*; do
    if [ -d "$d" ]; then
        id="$(basename "$d")"
        echo "Processing $id ..."

        if [ -f "$d/manifest.json" ]; then
            BIN=$(sed -n 's/.*"binary":[ ]*"\([^"]*\)".*/\1/p' "$d/manifest.json" | head -n1)
            if [ -n "$BIN" ] && [ -f "$d/Makefile" ]; then
                echo "  -> Compiling $BIN for Toon 2 (ARMv7)..."
                make -C "$d" clean || true
                make -C "$d" TARGET=toon2 OUT="$BIN.toon2"
                echo "  -> Compiling $BIN for Toon 1 (ARMv5)..."
                make -C "$d" TARGET=toon1 OUT="$BIN.toon1"

                echo "  -> Generating multi-arch launcher wrapper $BIN..."
                cat > "$d/$BIN" <<EOF
#!/bin/sh
DIR="\$(dirname "\$0")"
ARCH="\$(uname -m)"
case "\$ARCH" in
    armv5*)
        exec "\$DIR/$BIN.toon1" "\$@"
        ;;
    *)
        exec "\$DIR/$BIN.toon2" "\$@"
        ;;
esac
EOF
                chmod +x "$d/$BIN" "$d/$BIN.toon1" "$d/$BIN.toon2"
            fi
        fi

        echo "Packaging $id -> $PACKAGES_DIR/$id.tgz ..."
        (cd "$d" && tar czf "$PACKAGES_DIR/$id.tgz" .)
    fi
done

echo "Done packaging integrations."


