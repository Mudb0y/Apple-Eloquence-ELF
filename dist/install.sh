#!/usr/bin/env bash
# Installer for apple-eloquence-elf release tarballs.
#
# Drops the converted ECI runtime into /usr/lib/eloquence, the
# sd_eloquence speech-dispatcher module into the daemon's modulebindir,
# and the eloquence.conf template into /etc/speech-dispatcher/modules.
#
# Run as root.  Use --prefix to relocate (defaults to /usr).

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="/usr"
SYSCONFDIR="/etc"
DESTDIR="${DESTDIR:-}"
DRY_RUN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--prefix=DIR] [--sysconfdir=DIR] [--dry-run]

  --prefix=DIR       Install root for libraries and modules (default: /usr)
  --sysconfdir=DIR   Config root (default: /etc)
  --dry-run          Show what would be installed, but do not touch the system
  -h, --help         Show this message

Honors DESTDIR for staged installs (e.g. distro packagers).
EOF
}

for arg in "$@"; do
    case "$arg" in
        --prefix=*)     PREFIX="${arg#*=}" ;;
        --sysconfdir=*) SYSCONFDIR="${arg#*=}" ;;
        --dry-run)      DRY_RUN=1 ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; usage; exit 2 ;;
    esac
done

if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    echo "This installer needs to write under $PREFIX and $SYSCONFDIR." >&2
    echo "Re-run with sudo, or pass --dry-run to preview." >&2
    exit 1
fi

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '  [dry-run] %s\n' "$*"
    else
        "$@"
    fi
}

# Resolve speech-dispatcher's module install directory.  End users
# usually don't have libspeechd-dev installed so pkg-config alone is
# not reliable.  Probe in order:
#
#   1. pkg-config (works on dev systems)
#   2. the directory of libspeechd_module.so.0 (works whenever speech-
#      dispatcher itself is installed, picks up multiarch on Debian)
#   3. common candidate paths (multiarch first, then non-multiarch)
#
MODULEBINDIR=""
if command -v pkg-config >/dev/null 2>&1; then
    MODULEBINDIR="$(pkg-config --variable=modulebindir speech-dispatcher 2>/dev/null || true)"
fi
if [ -z "$MODULEBINDIR" ]; then
    # Derive from the loader's view of libspeechd_module.  On Debian /
    # Ubuntu this gives /usr/lib/x86_64-linux-gnu/speech-dispatcher-modules;
    # on Arch / Fedora it gives /usr/lib/speech-dispatcher-modules.
    MOD_LIB="$(ldconfig -p 2>/dev/null \
        | awk '/libspeechd_module\.so/ {print $NF; exit}')"
    if [ -n "$MOD_LIB" ]; then
        cand="$(dirname "$MOD_LIB")/speech-dispatcher-modules"
        [ -d "$cand" ] && MODULEBINDIR="$cand"
    fi
fi
if [ -z "$MODULEBINDIR" ]; then
    for cand in \
        "${PREFIX}/lib/$(uname -m)-linux-gnu/speech-dispatcher-modules" \
        "${PREFIX}/lib/speech-dispatcher-modules" \
        "${PREFIX}/lib64/speech-dispatcher-modules"
    do
        if [ -d "$cand" ]; then
            MODULEBINDIR="$cand"
            break
        fi
    done
fi
if [ -z "$MODULEBINDIR" ]; then
    MODULEBINDIR="${PREFIX}/lib/speech-dispatcher-modules"
    echo "warning: could not detect speech-dispatcher's module directory;" >&2
    echo "         falling back to ${MODULEBINDIR}." >&2
    echo "         Is speech-dispatcher actually installed?  Try:" >&2
    echo "             Debian/Ubuntu: apt install speech-dispatcher" >&2
    echo "             Arch:          pacman -S speech-dispatcher" >&2
fi

DATA_DIR="${PREFIX}/lib/eloquence"
CONF_DIR="${SYSCONFDIR}/speech-dispatcher/modules"

echo "Installing apple-eloquence-elf:"
echo "  ECI runtime         -> ${DESTDIR}${DATA_DIR}"
echo "  sd_eloquence binary -> ${DESTDIR}${MODULEBINDIR}"
echo "  module config       -> ${DESTDIR}${CONF_DIR}/eloquence.conf"
echo

run install -d -m 0755 "${DESTDIR}${DATA_DIR}"
run install -d -m 0755 "${DESTDIR}${MODULEBINDIR}"
run install -d -m 0755 "${DESTDIR}${CONF_DIR}"

# Language modules + eci.so + the auto-generated eci.ini
for f in "$here"/lib/*.so; do
    [ -e "$f" ] || continue
    run install -m 0644 "$f" "${DESTDIR}${DATA_DIR}/"
done
if [ -e "$here/eci.ini" ]; then
    run install -m 0644 "$here/eci.ini" "${DESTDIR}${DATA_DIR}/eci.ini"
fi

# Speech-dispatcher module binary
run install -m 0755 "$here/sd_eloquence" "${DESTDIR}${MODULEBINDIR}/sd_eloquence"

# eloquence.conf -- only drop on a fresh install so we don't clobber
# the user's tuning. Speech-dispatcher auto-discovers the module from
# this directory + the module binary in modulebindir; do not edit
# speechd.conf.
if [ -e "${DESTDIR}${CONF_DIR}/eloquence.conf" ]; then
    echo "note: ${CONF_DIR}/eloquence.conf already present; leaving it alone."
    echo "      The new template is at ${here}/eloquence.conf if you want to diff."
else
    run install -m 0644 "$here/eloquence.conf" "${DESTDIR}${CONF_DIR}/eloquence.conf"
fi

cat <<EOF

Done.

Next steps:
  1. Restart speech-dispatcher:  systemctl --user restart speech-dispatcher
                                  (or: pkill -HUP speech-dispatcher)
  2. Select the module:          spd-say -o eloquence "Hello from Eloquence."
  3. Wire it into your screen reader (e.g. Orca -> Preferences -> Voice).

Runtime deps (install with your distro's package manager):
  - libc++ + libc++abi  (Arch: libc++ libc++abi; Debian: libc++1 libc++abi1)
  - libsoxr             (optional; enables higher sample-rate output)

Config:  ${CONF_DIR}/eloquence.conf
Data:    ${DATA_DIR}/
EOF
