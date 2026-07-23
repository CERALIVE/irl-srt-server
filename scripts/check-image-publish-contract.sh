#!/usr/bin/env bash
# Literal workflow expressions are intentionally single-quoted below.
# shellcheck disable=SC2016
set -euo pipefail

workflow=".github/workflows/publish-image.yml"

fail() {
	printf 'image publish contract: %s\n' "$*" >&2
	exit 1
}

require_literal() {
	local literal="$1"
	local description="$2"
	grep -Fq -- "${literal}" "${workflow}" ||
		fail "missing ${description}: ${literal}"
}

require_regex() {
	local pattern="$1"
	local description="$2"
	grep -Eq -- "${pattern}" "${workflow}" ||
		fail "missing ${description}: ${pattern}"
}

require_count() {
	local literal="$1"
	local minimum="$2"
	local description="$3"
	local count
	count="$(grep -Fc -- "${literal}" "${workflow}" || true)"
	((count >= minimum)) ||
		fail "missing ${description}: expected at least ${minimum} occurrences of ${literal}"
}

[[ -f "${workflow}" ]] || fail "missing ${workflow}"

require_literal "workflow_dispatch:" "manual-only trigger"
if grep -Eq '^  (push|pull_request):' "${workflow}"; then
	fail "publication workflow must not run from push or pull_request events"
fi

require_regex '^[[:space:]]+release_tag:' "release-tag dispatch input"
require_regex '^[[:space:]]+expected_sha:' "expected-SHA dispatch input"
require_count "required: true" 2 "two required dispatch inputs"
require_literal "permissions:" "permissions declaration"
require_count "contents: read" 2 "top-level and publish-job read permission"
require_literal "cancel-in-progress: false" "non-cancelling publish concurrency"

require_literal 'test "${actual_sha}" = "${EXPECTED_SHA}"' "exact checked-out SHA guard"
require_literal 'git merge-base --is-ancestor "${EXPECTED_SHA}" "origin/master"' \
	"canonical-master ancestry guard"
require_literal '[[ "${EXPECTED_SHA}" =~ ^[0-9a-f]{40}$ ]]' "full lowercase SHA validation"
require_literal '[[ "${RELEASE_TAG}" =~ ^[a-z0-9][a-z0-9._-]{0,127}$ ]]' \
	"OCI release-tag validation"
require_literal '[[ "${RELEASE_TAG}" != "latest" && "${RELEASE_TAG}" != sha-* ]]' \
	"mutable/reserved tag rejection"
require_literal '[[ "${GITHUB_REF}" == "refs/heads/master" ]]' \
	"canonical dispatch-ref guard"

require_regex '^[[:space:]]+publish:[[:space:]]*$' "publish job"
require_literal "needs: release-gate" "test-before-publish dependency"
require_literal "packages: write" "GHCR write permission"
require_literal "id-token: write" "keyless-signing OIDC permission"
require_literal "platforms: linux/amd64,linux/arm64" "multi-architecture manifest"
require_count 'ghcr.io/ceralive/irl-srt-server:${{ inputs.release_tag }}' 2 \
	"validated release tag"
require_count 'ghcr.io/ceralive/irl-srt-server:sha-${{ inputs.expected_sha }}' 2 \
	"immutable full-SHA tag"
require_literal "push: true" "registry push"
require_literal "cache-from: type=gha" "Docker GHA cache restore"
require_literal "cache-to: type=gha,mode=max" "Docker GHA cache save"
require_literal "provenance: mode=max" "build provenance attestation"
require_literal "sbom: true" "SBOM attestation"

require_literal "Refuse existing tags" "stale-tag collision guard"
require_literal 'docker buildx imagetools inspect "${IMAGE_REF}:${tag}"' \
	"registry collision lookup"
require_literal "published tags are never overwritten" "collision failure"
require_literal 'steps.build.outputs.digest' "manifest digest capture"
require_literal "Verify published tags resolve to the captured digest" \
	"post-push tag/digest verification"
require_literal 'test "${resolved_digest}" = "${DIGEST}"' "exact published digest check"
require_literal 'cosign sign --yes "${IMAGE_REF}@${DIGEST}"' "digest signing"
require_literal 'cosign verify' "signature verification"
require_literal '--certificate-oidc-issuer "https://token.actions.githubusercontent.com"' \
	"GitHub OIDC issuer verification"

printf 'image publish contract: PASS\n'
