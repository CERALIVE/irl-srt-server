#!/usr/bin/env bash
# Assert every srt build-input pin agrees on one commit.
#
# irl-srt-server builds libsrt from source at a pinned CERALIVE/srt commit. That
# commit is duplicated across four build inputs that MUST stay in lockstep, or a
# CI leg would link a different libsrt than the production image:
#   1. Dockerfile                 ARG SRT_COMMIT=...   (the production image)
#   2. .github/workflows/ci.yml   build-and-test env   SRT_COMMIT: ...
#   3. .github/workflows/ci.yml   fuzz env             SRT_COMMIT: ...
#   4. .github/workflows/ci.yml   coverage env         SRT_COMMIT: ...
#
# This gate fails if the four disagree, or if any still references a known-retired
# pin. Historical/doc mentions (ADRs, evidence, changelogs) are out of scope — only
# the build inputs above are checked.
set -euo pipefail

cd "$(dirname "$0")/.."

EXPECTED_PIN="ca14c8bd06c89d2fd7b69bb3d8eea48dd47c2e3e"
RETIRED_PINS=("66b3609cc004e6a4c485e0adc11149025e782083" "b06fdb6b85937f3f5cf5452b150a6bb7e35b0226")

fail=0
declare -a pins=()

collect() {
  local label="$1" value="$2"
  if [[ -z "$value" ]]; then
    echo "MISSING pin: $label"
    fail=1
    return
  fi
  echo "  $label = $value"
  pins+=("$value")
  for retired in "${RETIRED_PINS[@]}"; do
    if [[ "$value" == "$retired" ]]; then
      echo "    ^ ERROR: $label still pins the retired commit $retired"
      fail=1
    fi
  done
  if [[ "$value" != "$EXPECTED_PIN" ]]; then
    echo "    ^ ERROR: $label != expected $EXPECTED_PIN"
    fail=1
  fi
}

echo "srt build-input pins:"

dockerfile_pin="$(sed -n 's/^ARG SRT_COMMIT=\([0-9a-f]\{40\}\).*/\1/p' Dockerfile)"
collect "Dockerfile:ARG SRT_COMMIT" "$dockerfile_pin"

mapfile -t ci_pins < <(sed -n 's/^[[:space:]]*SRT_COMMIT:[[:space:]]*\([0-9a-f]\{40\}\).*/\1/p' .github/workflows/ci.yml)
if [[ ${#ci_pins[@]} -ne 3 ]]; then
  echo "MISSING pin: expected 3 SRT_COMMIT env sites in ci.yml, found ${#ci_pins[@]}"
  fail=1
fi
idx=0
for p in "${ci_pins[@]}"; do
  idx=$((idx + 1))
  collect "ci.yml:SRT_COMMIT#${idx}" "$p"
done

uniq_count="$(printf '%s\n' "${pins[@]}" | sort -u | wc -l | tr -d ' ')"
if [[ "$uniq_count" != "1" ]]; then
  echo "ERROR: build-input pins disagree ($uniq_count distinct values)"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "srt pin check FAILED"
  exit 1
fi

echo "srt pin check OK: all ${#pins[@]} build inputs pin $EXPECTED_PIN"
