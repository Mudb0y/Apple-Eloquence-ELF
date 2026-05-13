#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
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

MODULEBINDIR_CANDIDATES=()
if command -v pkg-config >/dev/null 2>&1; then
    pc="$(pkg-config --variable=modulebindir speech-dispatcher 2>/dev/null || true)"
    [ -n "$pc" ] && MODULEBINDIR_CANDIDATES+=("$pc")
fi
MOD_LIB="$(ldconfig -p 2>/dev/null \
    | awk '/libspeechd_module\.so/ {print $NF; exit}')" || true
[ -n "$MOD_LIB" ] && MODULEBINDIR_CANDIDATES+=("$(dirname "$MOD_LIB")/speech-dispatcher-modules")
MODULEBINDIR_CANDIDATES+=(\
    "${PREFIX}/lib/$(uname -m)-linux-gnu/speech-dispatcher-modules" \
    "${PREFIX}/lib/speech-dispatcher-modules" \
    "${PREFIX}/lib64/speech-dispatcher-modules" )

run rm -rf "${DESTDIR}${PREFIX}/lib/eloquence"
for cand in "${MODULEBINDIR_CANDIDATES[@]}"; do
    run rm -f "${DESTDIR}${cand}/sd_eloquence"
    # libspeechd_module bundled by install.sh for tarball-based installs
    # since 1.0.1.  Glob removes .so.0 and the .so.0.0.0 alias if present.
    run rm -f "${DESTDIR}${cand}"/libspeechd_module.so.*
done

# Resampler previews dropped by install.sh since 1.1.0.  Remove the
# whole preview dir, then try to remove the parent /usr/share/eloquence
# if it's empty -- which it will be for tarball installs (cmake source
# installs additionally drop requirements.txt there and keep it).
run rm -rf "${DESTDIR}${PREFIX}/share/eloquence/resampler-previews"
rmdir --ignore-fail-on-non-empty "${DESTDIR}${PREFIX}/share/eloquence" 2>/dev/null || true

if [ "$PURGE" -eq 1 ]; then
    run rm -f "${DESTDIR}${SYSCONFDIR}/speech-dispatcher/modules/eloquence.conf"
fi

echo "Removed apple-eloquence-elf."
