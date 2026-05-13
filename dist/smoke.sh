#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# sd_eloquence smoke test -- run after install, before tagging release.
set -euo pipefail

OUT=${OUT:-/tmp/sd_eloquence_smoke.log}
echo "Writing smoke results to $OUT"
echo "sd_eloquence smoke run $(date)" >"$OUT"

run() {
    local label="$1"; shift
    echo >>"$OUT"
    echo "[$label] $*" >>"$OUT"
    spd-say -o eloquence "$@" 2>>"$OUT" || true
    sleep 1.5
}

run "en-US" -l en-US "American English."
run "en-GB" -l en-GB "British English."
run "es-ES" -l es-ES "Hola mundo."
run "es-MX" -l es-MX "Hola desde México."
run "fr-FR" -l fr-FR "Bonjour le monde."
run "fr-CA" -l fr-CA "Bonjour du Québec."
run "de-DE" -l de-DE "Hallo Welt."
run "it-IT" -l it-IT "Ciao mondo."
run "pt-BR" -l pt-BR "Olá mundo."
run "fi-FI" -l fi-FI "Hei maailma."

for v in Reed Shelley Sandy Rocko Flo Grandma Grandpa Eddy; do
    run "variant-$v" -y "$v" "Variant $v"
done

run "ssml-mark" -m '<speak>Before <mark name="here"/> after.</speak>'

( spd-say -o eloquence "This is a very long sentence that should be canceled in the middle." &
  PID=$!; sleep 0.3; kill -INT $PID 2>/dev/null || true ) >>"$OUT" 2>&1 || true

echo "[pause/resume] manual: ensure spd-say survives a pause/resume cycle" >>"$OUT"

echo "Smoke complete." | tee -a "$OUT"
