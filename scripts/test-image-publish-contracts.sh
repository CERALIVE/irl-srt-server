#!/usr/bin/env bash
# Mutation expressions intentionally contain literal workflow variables.
# shellcheck disable=SC2016
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
validate_script="${repo_root}/scripts/validate-image-release.sh"
collision_script="${repo_root}/scripts/check-image-tags-unused.sh"
workflow_checker="${repo_root}/scripts/check-image-publish-workflow.rb"
action_ref_checker="${repo_root}/scripts/check-action-refs.sh"
workflow="${repo_root}/.github/workflows/publish-image.yml"

fail() {
	printf 'image publish behavioral contracts: %s\n' "$*" >&2
	exit 1
}

expect_failure() {
	local name="$1"
	shift
	if "$@" >/dev/null 2>&1; then
		fail "${name} unexpectedly passed"
	fi
	printf 'expected failure: %s\n' "${name}"
}

[[ -x "${validate_script}" ]] || fail "missing executable ${validate_script}"
[[ -x "${collision_script}" ]] || fail "missing executable ${collision_script}"
[[ -x "${workflow_checker}" ]] || fail "missing executable ${workflow_checker}"
[[ -x "${action_ref_checker}" ]] || fail "missing executable ${action_ref_checker}"

fixture_root="$(mktemp -d)"
trap 'rm -rf "${fixture_root}"' EXIT

git_fixture="${fixture_root}/git"
git init -q -b master "${git_fixture}"
git -C "${git_fixture}" config user.name "Contract Fixture"
git -C "${git_fixture}" config user.email "fixture@example.invalid"
printf 'allowed\n' > "${git_fixture}/state"
git -C "${git_fixture}" add state
git -C "${git_fixture}" commit -q -m allowed
allowed_sha="$(git -C "${git_fixture}" rev-parse HEAD)"
git -C "${git_fixture}" switch -q -c side
printf 'side\n' > "${git_fixture}/state"
git -C "${git_fixture}" commit -qam side
side_sha="$(git -C "${git_fixture}" rev-parse HEAD)"

git -C "${git_fixture}" checkout -q "${allowed_sha}"
(
	cd "${git_fixture}"
	bash "${validate_script}" "2026.7.1" "${allowed_sha}" "refs/heads/master" "refs/heads/master"
)
expect_failure "malformed release tag" \
	bash -c "cd '${git_fixture}' && bash '${validate_script}' 'bad/tag' '${allowed_sha}' refs/heads/master refs/heads/master"
expect_failure "reserved latest tag" \
	bash -c "cd '${git_fixture}' && bash '${validate_script}' latest '${allowed_sha}' refs/heads/master refs/heads/master"
expect_failure "short expected SHA" \
	bash -c "cd '${git_fixture}' && bash '${validate_script}' 2026.7.1 ade3c29 refs/heads/master refs/heads/master"
expect_failure "nonhex expected SHA" \
	bash -c "cd '${git_fixture}' && bash '${validate_script}' 2026.7.1 zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz refs/heads/master refs/heads/master"
expect_failure "checkout SHA mismatch" \
	bash -c "cd '${git_fixture}' && bash '${validate_script}' 2026.7.1 0000000000000000000000000000000000000000 refs/heads/master refs/heads/master"

git -C "${git_fixture}" checkout -q "${side_sha}"
expect_failure "commit outside master ancestry" \
	bash -c "cd '${git_fixture}' && bash '${validate_script}' 2026.7.1 '${side_sha}' refs/heads/master refs/heads/master"

mock_inspect="${fixture_root}/mock-inspect"
cat > "${mock_inspect}" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
ref="$1"
for existing in ${MOCK_EXISTING_REFS:-}; do
	if [[ "${ref}" == "${existing}" ]]; then
		echo '{"digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}'
		exit 0
	fi
done
case "${MOCK_REGISTRY_RESPONSE:-manifest-unknown}" in
	manifest-unknown)
		echo "manifest unknown" >&2
		exit 1
		;;
	no-such-manifest)
		echo "no such manifest" >&2
		exit 1
		;;
	buildx-not-found)
		echo "ERROR: ${ref}: not found" >&2
		exit 1
		;;
	unauthorized)
		echo "unauthorized: authentication required" >&2
		exit 2
		;;
	denied)
		echo "denied: permission_denied" >&2
		exit 2
		;;
	dns)
		echo "failed to resolve source metadata: dial tcp: lookup ghcr.io: no such host" >&2
		exit 1
		;;
	timeout)
		echo "failed to do request: Head https://ghcr.io/v2/: i/o timeout" >&2
		exit 124
		;;
	malformed)
		exit 1
		;;
	ambiguous)
		printf 'manifest unknown\nunauthorized: authentication required\n' >&2
		exit 1
		;;
	*)
		echo "unexpected mock registry response" >&2
		exit 64
		;;
esac
MOCK
chmod +x "${mock_inspect}"

IMAGE_INSPECT_COMMAND="${mock_inspect}" \
	bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=no-such-manifest \
	bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "existing release tag" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" \
		MOCK_EXISTING_REFS="ghcr.io/ceralive/irl-srt-server:2026.7.1" \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "existing full-SHA tag" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" \
		MOCK_EXISTING_REFS="ghcr.io/ceralive/irl-srt-server:sha-${allowed_sha}" \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "partial prior publication" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" \
		MOCK_EXISTING_REFS="ghcr.io/ceralive/irl-srt-server:2026.7.1 ghcr.io/ceralive/irl-srt-server:sha-${allowed_sha}" \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "unauthorized registry lookup" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=unauthorized \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "denied registry lookup" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=denied \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "registry DNS failure" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=dns \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "registry timeout" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=timeout \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "registry executable missing" \
	env IMAGE_INSPECT_COMMAND="${fixture_root}/missing-imagetools" \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "malformed empty registry output" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=malformed \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
expect_failure "ambiguous absent and unauthorized output" \
	env IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=ambiguous \
		bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"
IMAGE_INSPECT_COMMAND="${mock_inspect}" MOCK_REGISTRY_RESPONSE=buildx-not-found \
	bash "${collision_script}" "ghcr.io/ceralive/irl-srt-server" "2026.7.1" "${allowed_sha}"

mutation_dir="${fixture_root}/mutations"
mkdir -p "${mutation_dir}"

mutate_and_expect_failure() {
	local name="$1"
	local expression="$2"
	local candidate="${mutation_dir}/${name}.yml"
	cp "${workflow}" "${candidate}"
	sed -i "${expression}" "${candidate}"
	{
		printf '\n# Inert original workflow text must not satisfy parsed assertions.\n'
		sed 's/^/# /' "${workflow}"
	} >> "${candidate}"
	expect_failure "workflow mutation ${name}" ruby "${workflow_checker}" "${candidate}"
}

ruby "${workflow_checker}" "${workflow}"
bash "${action_ref_checker}" "${workflow}"
mutate_and_expect_failure "test-dependency" 's/needs: release-gate/needs: []/'
mutate_and_expect_failure "source-guard" 's|bash scripts/validate-image-release.sh|bash scripts/missing-release-guard.sh|'
mutate_and_expect_failure "packages-permission" '/packages: write/d'
mutate_and_expect_failure "oidc-permission" '/id-token: write/d'
mutate_and_expect_failure "concurrency" 's/cancel-in-progress: false/cancel-in-progress: true/'
mutate_and_expect_failure "sign-subject" 's/cosign sign --yes "${IMAGE_REF}@${DIGEST}"/cosign sign --yes "${IMAGE_REF}:${RELEASE_TAG}"/'
mutate_and_expect_failure "verify-subject" '/certificate-oidc-issuer/{n;s/@${DIGEST}/:${RELEASE_TAG}/;}'
mutate_and_expect_failure "arm64-platform" 's|platforms: linux/amd64,linux/arm64|platforms: linux/amd64|'
mutate_and_expect_failure "arm64-gate" '/          - arch: arm64/,+1d'
mutate_and_expect_failure "cosign-immutable-ref" \
	's|sigstore/cosign-installer@[0-9a-f]\{40\}|sigstore/cosign-installer@v4|'

unresolvable_workflow="${mutation_dir}/unresolvable-action-ref.yml"
sed 's|sigstore/cosign-installer@[0-9a-f]\{40\}|sigstore/cosign-installer@does-not-exist|' \
	"${workflow}" > "${unresolvable_workflow}"
expect_failure "unresolvable external action reference" \
	bash "${action_ref_checker}" "${unresolvable_workflow}"

malformed_workflow="${mutation_dir}/malformed-action-ref.yml"
sed 's|sigstore/cosign-installer@[0-9a-f]\{40\}|sigstore/cosign-installer|' \
	"${workflow}" > "${malformed_workflow}"
expect_failure "malformed external action reference" \
	bash "${action_ref_checker}" "${malformed_workflow}"

hung_resolver="${fixture_root}/hung-action-ref-resolver"
cat > "${hung_resolver}" <<'HUNG'
#!/usr/bin/env bash
sleep 5
HUNG
chmod +x "${hung_resolver}"
expect_failure "hung external action reference resolver" \
	env ACTION_REF_RESOLVER="${hung_resolver}" ACTION_REF_TIMEOUT_SECONDS=1 \
	bash "${action_ref_checker}" "${workflow}"

non_sha_resolver="${fixture_root}/non-sha-action-ref-resolver"
cat > "${non_sha_resolver}" <<'NON_SHA'
#!/usr/bin/env bash
printf 'resolved-but-not-a-sha\n'
NON_SHA
chmod +x "${non_sha_resolver}"
expect_failure "non-SHA external action resolution" \
	env ACTION_REF_RESOLVER="${non_sha_resolver}" \
	bash "${action_ref_checker}" "${workflow}"

printf 'image publish behavioral contracts: PASS\n'
