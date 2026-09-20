#!/usr/bin/env bash
set -euo pipefail

release_tag="${1:?release tag is required}"
expected_sha="${2:?expected SHA is required}"
dispatch_ref="${3:?dispatch ref is required}"
master_ref="${4:-origin/master}"

[[ "${release_tag}" =~ ^[a-z0-9][a-z0-9._-]{0,127}$ ]] || {
	echo "release tag is not a valid lowercase OCI tag" >&2
	exit 1
}
[[ "${release_tag}" != "latest" && "${release_tag}" != sha-* ]] || {
	echo "release tag must not be latest or use the reserved sha-* namespace" >&2
	exit 1
}
[[ "${expected_sha}" =~ ^[0-9a-f]{40}$ ]] || {
	echo "expected SHA must be exactly 40 lowercase hexadecimal characters" >&2
	exit 1
}
[[ "${dispatch_ref}" == "refs/heads/master" ]] || {
	echo "release must be dispatched from refs/heads/master" >&2
	exit 1
}

actual_sha="$(git rev-parse HEAD)"
[[ "${actual_sha}" == "${expected_sha}" ]] || {
	echo "checkout HEAD ${actual_sha} does not match expected ${expected_sha}" >&2
	exit 1
}
git rev-parse --verify "${master_ref}^{commit}" >/dev/null
git merge-base --is-ancestor "${expected_sha}" "${master_ref}" || {
	echo "expected SHA is not on canonical master" >&2
	exit 1
}

printf 'release source validation: PASS (%s at %s)\n' "${release_tag}" "${expected_sha}"
