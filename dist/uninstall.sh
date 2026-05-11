#!/usr/bin/env bash
# Reverses what install.sh did.  Leaves eloquence.conf in place so users
# don't lose tuning across upgrades; pass --purge to remove that too.

set -euo pipefail

PREFIX="/usr"
SYSCONFDIR="/etc"
DESTDIR="${DESTDIR:-}"
DRY_RUN=0
PURGE=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--prefix=DIR] [--sysconfdir=DIR] [--purge] [--dry-run]

  --purge   Also remove ${SYSCONFDIR}/speech-dispatcher/modules/eloquence.conf
EOF
}

for arg in "$@"; do
    case "$arg" in
        --prefix=*)     PREFIX="${arg#*=}" ;;
        --sysconfdir=*) SYSCONFDIR="${arg#*=}" ;;
        --dry-run)      DRY_RUN=1 ;;
        --purge)        PURGE=1 ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; usage; exit 2 ;;
    esac
done

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    echo "This uninstaller needs to write under $PREFIX and $SYSCONFDIR." >&2
    exit 1
fi

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  [dry-run] %s\n' "$*"
    else
        "$@"
    fi
}

MODULEBINDIR=""
if command -v pkg-config >/dev/null 2>&1; then
    MODULEBINDIR="$(pkg-config --variable=modulebindir speech-dispatcher 2>/dev/null || true)"
fi
[ -z "$MODULEBINDIR" ] && MODULEBINDIR="${PREFIX}/lib/speech-dispatcher-modules"

run rm -rf "${DESTDIR}${PREFIX}/lib/eloquence"
run rm -f  "${DESTDIR}${MODULEBINDIR}/sd_eloquence"

if [ "$PURGE" -eq 1 ]; then
    run rm -f "${DESTDIR}${SYSCONFDIR}/speech-dispatcher/modules/eloquence.conf"
fi

echo "Removed apple-eloquence-elf."
