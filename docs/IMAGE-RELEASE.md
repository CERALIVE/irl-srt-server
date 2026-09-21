# Immutable image release

The production image is published only by `.github/workflows/publish-image.yml`. The
workflow is manual, builds one exact commit from canonical `main`, and publishes a single
Linux `amd64`/`arm64` manifest to `ghcr.io/ceralive/irl-srt-server`.

Publishing is separate from deployment. Running this workflow does not change
`ceralive-platform` variables, does not create a platform release, and does not deploy an
ingest zone.

The first hard-fork release tag is `3.1.0`. Publication is confirmed only by a successful
workflow run and both live tags resolving to its signed digest, not by the branch swap
or by green CI alone. The prior canonical history remains preserved at `legacy`.

## Versioning

The release tag is **semver `MAJOR.MINOR.PATCH`**, and it must equal the `VERSION` in
`project(srt-live-server VERSION ...)` in `CMakeLists.txt`. That version is upstream's; this
fork does not bump it on its own, because the server source is byte-identical to upstream.
A new image therefore means one of two things: upstream released a new version and the fork
synced to it, or the CERALIVE layer (libsrt pin, CI, Dockerfile) changed and PROJECT_VERSION
was bumped in a deliberate PR.

`scripts/validate-image-release.sh` enforces this. It rejects a tag that is not
`MAJOR.MINOR.PATCH`, a tag that differs from PROJECT_VERSION, a dispatch from any ref other
than `refs/heads/main`, a checkout whose HEAD differs from the expected SHA, and a SHA that is
not an ancestor of `origin/main`. The `latest` tag and the reserved `sha-*` namespace are
never used as release tags.

The git tag for a published image is `v<PROJECT_VERSION>`, created after the image is
published and verified.

## Pre-flight

The commit must be on `main`, all PR/CI checks green, `scripts/check-srt-pin.sh` green, and
both output tags unused. The tag check is:

```bash
EXPECTED_SHA="$(gh api repos/CERALIVE/irl-srt-server/commits/main --jq .sha)"
RELEASE_TAG="$(sed -nE 's/^project\([^)]*[[:space:]]VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)"

bash scripts/check-image-tags-unused.sh \
  ghcr.io/ceralive/irl-srt-server "${RELEASE_TAG}" "${EXPECTED_SHA}"
```

The package is private, so the check needs a credential with `read:packages`. Without it
every manifest lookup answers `403`, and the script fails closed on authorization rather
than reporting the tag free. That is correct behaviour: only `manifest unknown`, `no such
manifest`, or Buildx's exact `not found` response count as proof of absence. Refresh the
scope with `gh auth refresh -h github.com -s read:packages` (a browser device flow) before
running it.

## Dispatch

```bash
gh workflow run publish-image.yml \
  --repo CERALIVE/irl-srt-server \
  --ref main \
  -f release_tag="${RELEASE_TAG}" \
  -f expected_sha="${EXPECTED_SHA}"
```

The expected SHA must be exactly 40 lowercase hexadecimal characters. Existing tags are
never overwritten, so a repeated dispatch fails closed.

The workflow calls the same `validate-image-release.sh` and `check-image-tags-unused.sh`
guards exercised by `scripts/test-image-publish-contracts.sh`. That test runs the guards
against isolated Git history and an injected registry client, covering malformed input,
CalVer-shaped and mismatched tags, wrong ancestry, the old canonical branch name as a
dispatch ref, collisions, partial prior publication, and registry errors. It also resolves
every external action reference against GitHub with a bounded timeout. The Cosign installer
is pinned to the immutable commit behind the latest stable tag; its upstream does not expose
a floating major ref.

## Release gate

Before anything is pushed, native `amd64` and `arm64` release-gate jobs build the production
`Dockerfile`, which compiles the pinned `CERALIVE/srt`, links the server against it, and
runs the unit suite. The gate then boots the shipped image and asserts three things from the
startup log:

- `Initialized libsrt v1.5.7` (the pinned libsrt, not a stock one)
- `srtla_patches=on` (the SRTLA listener came up with `SRTO_SRTLAPATCHES` accepted)
- no `SRTO_SRTLAPATCHES failure`

`srtla_patches=on` is only reachable if `srt_setsockopt` accepted the fork-only option, so
it is the runtime proof that the image links the CERALIVE libsrt. The shipped `sls.conf`
carries CRLF line endings that the parser rejects; the gate normalizes it into a scratch
copy first.

The publish job cannot start unless both architecture gates and the repository contract
checks pass.

## Output and verification

A successful run produces two immutable names for one multi-architecture manifest:

```text
ghcr.io/ceralive/irl-srt-server:<release-tag>
ghcr.io/ceralive/irl-srt-server:sha-<full-40-character-commit>
```

The build emits maximum-mode provenance and an SBOM attestation. The workflow captures the
registry's `sha256:` manifest digest, signs
`ghcr.io/ceralive/irl-srt-server@sha256:<digest>` keylessly with GitHub OIDC, verifies that
signature, and reports success. The run summary is the release receipt: both tags, the
digest reference, and the signature verification.

Verify independently:

```bash
IMAGE_REF="ghcr.io/ceralive/irl-srt-server"
DIGEST="sha256:<digest-from-the-run-summary>"

cosign verify \
  --certificate-identity \
  "https://github.com/CERALIVE/irl-srt-server/.github/workflows/publish-image.yml@refs/heads/main" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  "${IMAGE_REF}@${DIGEST}"

docker buildx imagetools inspect "${IMAGE_REF}:${RELEASE_TAG}"
docker buildx imagetools inspect "${IMAGE_REF}:sha-${EXPECTED_SHA}"
```

Both tags must resolve to the same digest before handoff.

## Release notes

The GitHub release for `v<PROJECT_VERSION>` records what the image links, by number:

- the exact `CERALIVE/srt` release (tag and Debian version)
- the three socket options and their effective values on `listen_publisher_srtla`:
  `118` (`SRTO_SRTLAPATCHES`) on, `119` (`SRTO_PERIODICNAKGATE`) at the compat default,
  `120` (`SRTO_REORDERFREEZE`) on

For `3.1.0`, the exact libsrt release is `srt-v1.5.7+ceralive.2` (Debian
`libsrt1.5-ceralive 1.5.7+ceralive.2`), commit
`d487b13365205b6cd5da9d9b50868c323e255b7c`. The effective values are **118 on,
119 = 2, 120 on**. D10 selected suppress (`2`) under its pre-registered rule:
24 valid runs, four netem cells, N=3 per arm; filter (`1`) won loss on one cell
(three required), with the goodput guard passing on all four. This is simulation
evidence, not real bonded-hardware validation.

## Platform handoff and rollback

`ceralive-platform` consumes the release tag through its `IRL_SRT_SERVER_RELEASE_TAG`
repository variable and resolves it to a digest before constructing the signed production
release manifest. Setting that variable happens only after image publication and signature
verification, in the platform repository, under its own release authorization.

No release tag is ever moved. Rollback selects a previously verified platform release by
its `irl-srt-server@sha256:<previous-digest>` reference through the platform's documented
signed rollback path.
