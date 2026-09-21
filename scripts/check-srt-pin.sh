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
# Historical/doc mentions (ADRs, evidence, changelogs) are out of scope — only
# the build inputs above are checked.
#
# NOTE (plan upstream-rebase-hard-fork): the three ci.yml sites are now pinned
# CERALIVE/srt clones (they replaced upstream's dynamic `git ls-remote ... belabox`
# HEAD resolution). The clang-tidy job deliberately does NOT add a fourth env site:
# it READS the commit out of the Dockerfile, so it cannot drift and this gate keeps
# counting exactly three.
set -euo pipefail

cd "$(dirname "$0")/.."

# srt-v1.5.7+ceralive.2 (the TAG's resolved commit, not the merge commit before it).
EXPECTED_PIN="d487b13365205b6cd5da9d9b50868c323e255b7c"
RETIRED_PINS=(
  "b06fdb6b85937f3f5cf5452b150a6bb7e35b0226"
  "f2297192ce9ab572464e84228efbc46f8c1eabf4"
  "51d500c428c8e618848ee63b16efef77959938c9"
)

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
