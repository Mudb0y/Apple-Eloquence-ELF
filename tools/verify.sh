#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# verify.sh -- check that all shipped binaries match tools/checksums.txt
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f tools/checksums.txt ]; then
    echo "ERROR: tools/checksums.txt not found" >&2
    exit 1
fi

grep -v '^#' tools/checksums.txt | grep -v '^$' | sha256sum -c --strict --quiet
echo "All binaries match expected checksums."
