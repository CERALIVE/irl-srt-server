# Immutable image release

The production image is published only by
`.github/workflows/publish-image.yml`. The workflow is manual, builds the exact
requested commit from canonical `master`, and publishes one Linux
`amd64`/`arm64` manifest to `ghcr.io/ceralive/irl-srt-server`.

Publishing is deliberately separate from deployment. Running this workflow does
not change ceralive-platform variables, create a platform release, or deploy an
ingest zone.

## Dispatch

The selected commit must already be on `master`, all normal PR/CI checks should
be green, and both target tags must be unused. Resolve and retain the full
40-character commit SHA, then dispatch from `master`:

```bash
EXPECTED_SHA="$(gh api repos/CERALIVE/irl-srt-server/commits/master --jq .sha)"
RELEASE_TAG="2026.7.1"

gh workflow run publish-image.yml \
  --repo CERALIVE/irl-srt-server \
  --ref master \
  -f release_tag="${RELEASE_TAG}" \
  -f expected_sha="${EXPECTED_SHA}"
```

The release tag must be a lowercase OCI tag. `latest` and the reserved `sha-*`
namespace are rejected. The expected SHA must be exactly 40 lowercase
hexadecimal characters. The workflow also fails if checkout does not equal that
SHA, the SHA is not on `origin/master`, or either output tag already exists.
Existing tags are never overwritten, so a repeated dispatch fails closed.
The workflow calls the same `validate-image-release.sh` and
`check-image-tags-unused.sh` guards exercised by
`test-image-publish-contracts.sh`; the latter uses isolated Git history and an
injected registry client to cover malformed input, wrong ancestry, collisions,
partial prior publication, and registry errors. The collision guard treats only
`manifest unknown`, `no such manifest`, and Buildx/GHCR's exact
`ERROR: <requested-ref>: not found` response as proof that a tag is absent.
Authorization, network, malformed, ambiguous, executable, and existing-digest
responses remain blocking. The suite also resolves every external action
reference against GitHub with a bounded timeout. The Cosign installer is pinned
to the immutable commit behind the latest stable tag: unlike the other publish
actions, its upstream repository does not expose a floating major ref.
Dependabot retains the release-tag annotation when updating that SHA.

Before publishing, native `amd64` and `arm64` release-gate jobs build the
production Dockerfile. The Dockerfile runs the complete unit suite and SRT
loopback test, and the gate verifies the shipped binary and reorderfreeze
startup mode. The publish job cannot start unless both architecture gates and
the repository contract checks pass.

## Output and verification

A successful run produces two immutable names for the same multi-architecture
manifest:

```text
ghcr.io/ceralive/irl-srt-server:<release-tag>
ghcr.io/ceralive/irl-srt-server:sha-<full-40-character-commit>
```

The build emits maximum-mode provenance and an SBOM attestation. The workflow
captures the registry's `sha256:` manifest digest, signs
`ghcr.io/ceralive/irl-srt-server@sha256:<digest>` keylessly with GitHub OIDC,
and then verifies that signature before reporting success. The run summary is
the release receipt: it records both tags, the digest reference, and successful
signature verification.

Independently verify the receipt with Cosign:

```bash
IMAGE_REF="ghcr.io/ceralive/irl-srt-server"
DIGEST="sha256:<digest-from-the-run-summary>"

cosign verify \
  --certificate-identity \
  "https://github.com/CERALIVE/irl-srt-server/.github/workflows/publish-image.yml@refs/heads/master" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  "${IMAGE_REF}@${DIGEST}"
```

Confirm both tags resolve to that same digest before handoff:

```bash
docker buildx imagetools inspect \
  "ghcr.io/ceralive/irl-srt-server:${RELEASE_TAG}"
docker buildx imagetools inspect \
  "ghcr.io/ceralive/irl-srt-server:sha-${EXPECTED_SHA}"
```

## Platform handoff and rollback

`ceralive-platform` consumes the release tag through its
`IRL_SRT_SERVER_RELEASE_TAG` repository variable and resolves it to a digest
before constructing the signed production release manifest. Setting that
variable happens only **after** image publication and signature verification,
in the platform repository, under its separate release authorization. It is
outside this repository's image-publish workflow and outside the image PR.

No release tag is moved for rollback. Retain the previous platform release
receipt and its `irl-srt-server@sha256:<previous-digest>` reference. A rollback
selects that previously verified platform release through the platform's
documented signed rollback path. Image publication does not deploy a canary or
any other production zone.
