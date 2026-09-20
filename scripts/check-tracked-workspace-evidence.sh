#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
cd "$repo_root"

needle=$(printf '%s%s' '.' 'omo')
if matches=$(git grep --no-color -n -I -F "$needle" -- . ':(exclude).gitignore'); then
  printf 'Tracked workspace-local evidence references found:\n%s\n' "$matches" >&2
  exit 1
fi

printf 'No tracked workspace-local evidence references outside .gitignore.\n'
