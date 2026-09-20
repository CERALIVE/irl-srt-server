#!/usr/bin/env bash
set -euo pipefail

release_tag="${1:?release tag is required}"
expected_sha="${2:?expected SHA is required}"
dispatch_ref="${3:?dispatch ref is required}"
main_ref="${4:-origin/main}"

# SLS image releases are semver PROJECT_VERSION, never CalVer (D18).
[[ "${release_tag}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
	echo "release tag must be semver MAJOR.MINOR.PATCH (for example, 3.1.0)" >&2
	exit 1
}
[[ "${expected_sha}" =~ ^[0-9a-f]{40}$ ]] || {
	echo "expected SHA must be exactly 40 lowercase hexadecimal characters" >&2
	exit 1
}
[[ "${dispatch_ref}" == "refs/heads/main" ]] || {
	echo "release must be dispatched from refs/heads/main" >&2
	exit 1
}

# The release tag must equal the source tree's own CMake PROJECT_VERSION.
project_version="$(sed -nE 's/^project\([^)]*[[:space:]]VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt 2>/dev/null || true)"
[[ -n "${project_version}" ]] || {
	echo "unable to read PROJECT_VERSION from CMakeLists.txt" >&2
	exit 1
}
[[ "${release_tag}" == "${project_version}" ]] || {
	echo "release tag ${release_tag} does not match CMakeLists.txt PROJECT_VERSION ${project_version}" >&2
	exit 1
}

actual_sha="$(git rev-parse HEAD)"
[[ "${actual_sha}" == "${expected_sha}" ]] || {
	echo "checkout HEAD ${actual_sha} does not match expected ${expected_sha}" >&2
	exit 1
}
git rev-parse --verify "${main_ref}^{commit}" >/dev/null
git merge-base --is-ancestor "${expected_sha}" "${main_ref}" || {
	echo "expected SHA is not on canonical main" >&2
	exit 1
}

printf 'release source validation: PASS (%s at %s)\n' "${release_tag}" "${expected_sha}"
