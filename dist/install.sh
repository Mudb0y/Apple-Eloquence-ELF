#!/usr/bin/env bash
# Installer for apple-eloquence-elf release tarballs.
#
# Drops the converted ECI runtime into /usr/lib/eloquence, the
# sd_eloquence speech-dispatcher module into the daemon's modulebindir,
# and the eloquence.conf template into /etc/speech-dispatcher/modules.
#
# Resolves runtime dependencies through the host's package manager
# (with a confirmation prompt) before installing files.  Run as root.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="/usr"
SYSCONFDIR="/etc"
DESTDIR="${DESTDIR:-}"
DRY_RUN=0
ASSUME_YES=0
SKIP_DEPS=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  --prefix=DIR       Install root for libraries and modules (default: /usr)
  --sysconfdir=DIR   Config root (default: /etc)
  --dry-run          Show what would be installed; do not touch the system
  -y, --yes          Don't prompt before installing missing packages
  --skip-deps        Don't check or install runtime dependencies
  -h, --help         Show this message

Honors DESTDIR for staged installs (e.g. distro packagers).
EOF
}

for arg in "$@"; do
    case "$arg" in
        --prefix=*)     PREFIX="${arg#*=}" ;;
        --sysconfdir=*) SYSCONFDIR="${arg#*=}" ;;
        --dry-run)      DRY_RUN=1 ;;
        -y|--yes)       ASSUME_YES=1 ;;
        --skip-deps)    SKIP_DEPS=1 ;;
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

# ------------------------------------------------------------------------
# Dependency resolution
# ------------------------------------------------------------------------
# Detect the host's package family from /etc/os-release.  The full set
# of runtime deps is installed via the family's package manager after
# user confirmation; we don't try to install per-library, because every
# distro packages this stuff slightly differently and a "what's missing"
# inventory adds opacity without saving the user anything (the package
# manager is idempotent).

FAMILY=""
detect_family() {
    [ -r /etc/os-release ] || return
    # shellcheck disable=SC1091
    . /etc/os-release
    local id_list="${ID:-} ${ID_LIKE:-}"
    for tok in $id_list; do
        case "$tok" in
            debian|ubuntu|raspbian|linuxmint|pop|elementary|kali)
                FAMILY=debian; return ;;
            fedora|rhel|centos|rocky|almalinux|ol)
                FAMILY=fedora; return ;;
            arch|cachyos|endeavouros|manjaro|garuda|artix)
                FAMILY=arch; return ;;
            opensuse-tumbleweed|opensuse-leap|opensuse|sles|suse)
                FAMILY=suse; return ;;
        esac
    done
}

PKG_MGR=""
PKG_INSTALL_CMD=""
PKG_LIST=""
plan_packages() {
    case "$FAMILY" in
        debian)
            PKG_MGR="apt-get"
            PKG_INSTALL_CMD="apt-get install -y --no-install-recommends"
            PKG_LIST="speech-dispatcher libspeechd2 libc++1 libc++abi1 libsoxr0"
            ;;
        fedora)
            PKG_MGR="dnf"
            PKG_INSTALL_CMD="dnf install -y"
            PKG_LIST="speech-dispatcher libcxx libcxxabi soxr"
            ;;
        arch)
            PKG_MGR="pacman"
            PKG_INSTALL_CMD="pacman -S --noconfirm --needed"
            PKG_LIST="speech-dispatcher libc++ libc++abi libsoxr"
            ;;
        suse)
            PKG_MGR="zypper"
            PKG_INSTALL_CMD="zypper install -y --no-confirm"
            PKG_LIST="speech-dispatcher libc++1 libc++abi1 libsoxr0"
            ;;
    esac
}

# ldd-based check on the shipped binaries.  Returns the list of
# "not found" sonames (one per line); empty output means deps satisfied.
missing_libs() {
    {
        ldd "$here/sd_eloquence" 2>/dev/null || true
        for so in "$here"/lib/*.so; do
            ldd "$so" 2>/dev/null || true
        done
    } | awk '/=> not found/ {print $1}' | sort -u
}

# Check whether the daemon binary is present (speech-dispatcher itself).
# Without it modulebindir won't exist and the module won't get spawned.
have_speechd() {
    command -v speech-dispatcher >/dev/null 2>&1
}

resolve_dependencies() {
    [ "$SKIP_DEPS" -eq 1 ] && return 0
    detect_family
    plan_packages

    local missing
    missing="$(missing_libs)"
    local need_speechd=0
    if ! have_speechd; then need_speechd=1; fi

    if [ -z "$missing" ] && [ "$need_speechd" -eq 0 ]; then
        # ldconfig says everything resolves AND speechd is present.
        # Still offer to install the package set, because some users
        # have only some of the libs pulled in transitively from other
        # packages and would benefit from explicit installs of every
        # dep listed (e.g. libsoxr might be present but not the package
        # state we want it in).
        return 0
    fi

    echo "Runtime dependencies not fully satisfied:"
    if [ -n "$missing" ]; then
        echo "  Missing libraries (from ldd):"
        printf '    %s\n' $missing
    fi
    if [ "$need_speechd" -eq 1 ]; then
        echo "  Missing: speech-dispatcher daemon (modulebindir lookup will fail)"
    fi
    echo

    if [ -z "$FAMILY" ]; then
        echo "Could not detect distro family from /etc/os-release." >&2
        echo "Install equivalents of these manually, then re-run:" >&2
        echo "    libc++ libc++abi libsoxr speech-dispatcher" >&2
        exit 1
    fi

    echo "Detected ${FAMILY}; would run:"
    echo "    ${PKG_INSTALL_CMD} ${PKG_LIST}"
    echo

    if [ "$ASSUME_YES" -ne 1 ]; then
        local ans=""
        printf 'Install these packages now? [Y/n] '
        if read -r ans </dev/tty 2>/dev/null; then
            :
        else
            # No controlling tty -- treat as "no" unless --yes was passed.
            echo
            echo "No tty available for prompt; re-run with --yes to auto-install" >&2
            echo "or --skip-deps to bypass this check." >&2
            exit 1
        fi
        case "$ans" in
            ""|y|Y|yes|YES|Yes) ;;
            *) echo "Aborted by user."; exit 1 ;;
        esac
    fi

    # Refresh package indexes where the manager needs it.
    case "$PKG_MGR" in
        apt-get) run apt-get update -qq ;;
        pacman)  run pacman -Sy --noconfirm >/dev/null ;;
    esac

    run $PKG_INSTALL_CMD $PKG_LIST

    # Re-verify -- if a soname is still unresolved at this point the
    # package set is wrong for this distro and we shouldn't continue.
    if [ "$DRY_RUN" -eq 0 ]; then
        ldconfig 2>/dev/null || true
        missing="$(missing_libs)"
        if [ -n "$missing" ]; then
            echo "Still missing after install:" >&2
            printf '  %s\n' $missing >&2
            echo "The package list for ${FAMILY} likely needs an update;" >&2
            echo "open an issue at https://github.com/Mudb0y/Apple-Eloquence-ELF/issues" >&2
            exit 1
        fi
        if ! have_speechd; then
            echo "speech-dispatcher binary still missing after install." >&2
            exit 1
        fi
    fi
}

resolve_dependencies

# ------------------------------------------------------------------------
# Layout resolution
# ------------------------------------------------------------------------
# modulebindir: pkg-config first (works on dev systems), then derive
# from the loader's view of libspeechd_module.so.0 (correct on every
# distro that has speech-dispatcher installed), then fall back to
# standard candidate paths.
MODULEBINDIR=""
if command -v pkg-config >/dev/null 2>&1; then
    MODULEBINDIR="$(pkg-config --variable=modulebindir speech-dispatcher 2>/dev/null || true)"
fi
if [ -z "$MODULEBINDIR" ]; then
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

for f in "$here"/lib/*.so; do
    [ -e "$f" ] || continue
    run install -m 0644 "$f" "${DESTDIR}${DATA_DIR}/"
done
if [ -e "$here/eci.ini" ]; then
    run install -m 0644 "$here/eci.ini" "${DESTDIR}${DATA_DIR}/eci.ini"
fi

run install -m 0755 "$here/sd_eloquence" "${DESTDIR}${MODULEBINDIR}/sd_eloquence"

# eloquence.conf -- only drop on a fresh install so we don't clobber
# the user's tuning. Speech-dispatcher auto-discovers the module from
# this directory + the module binary in modulebindir; we do not edit
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

Config:  ${CONF_DIR}/eloquence.conf
Data:    ${DATA_DIR}/
EOF
