#!/usr/bin/env bash
set -euo pipefail

image_ref="${1:?image reference is required}"
release_tag="${2:?release tag is required}"
expected_sha="${3:?expected SHA is required}"

inspect_ref() {
	local ref="$1"
	if [[ -n "${IMAGE_INSPECT_COMMAND:-}" ]]; then
		"${IMAGE_INSPECT_COMMAND}" "${ref}"
	else
		docker buildx imagetools inspect "${ref}"
	fi
}

for tag in "${release_tag}" "sha-${expected_sha}"; do
	ref="${image_ref}:${tag}"
	if output="$(inspect_ref "${ref}" 2>&1)"; then
		echo "${ref} already exists; published tags are never overwritten" >&2
		exit 1
	else
		status=$?
	fi

	if [[ "${status}" -eq 1 ]]; then
		case "${output}" in
			"manifest unknown" | "no such manifest" | "ERROR: ${ref}: not found")
				continue
				;;
		esac
	fi

	echo "registry lookup for ${ref} failed closed (status ${status})" >&2
	echo "${output}" >&2
	exit 1
done

printf 'image tag collision check: PASS\n'
