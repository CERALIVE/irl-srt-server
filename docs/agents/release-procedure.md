<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## RELEASE PROCEDURE

1. The commit is on canonical `main`, CI green, the
   `check-srt-pin.sh` gate green, `CMakeLists.txt` PROJECT_VERSION is the intended tag.
2. Confirm `ghcr.io/ceralive/irl-srt-server:<PROJECT_VERSION>` is unused with
   `scripts/check-image-tags-unused.sh <image> <tag> <sha>`. This needs a credential with
   `read:packages`; the package is private and an anonymous or under-scoped token gets `403`,
   which the script correctly treats as blocking, not as "tag free".
3. Dispatch `publish-image.yml` from `main` with `release_tag=<PROJECT_VERSION>` and
   `expected_sha=<40-hex>`. `validate-image-release.sh` rejects anything that is not semver
   `MAJOR.MINOR.PATCH`, not equal to PROJECT_VERSION, not the checked-out SHA, or not an
   ancestor of `main`.
4. Both architecture release gates build the Dockerfile and boot the image, asserting
   `Initialized libsrt v1.5.7`, `srtla_patches=on`, and no `SRTO_SRTLAPATCHES failure`.
5. The publish job pushes `:<tag>` and `:sha-<sha>`, attaches provenance and SBOM, signs the
   digest keylessly, verifies the signature, and prints the receipt in the run summary.
6. Tag the commit `v<PROJECT_VERSION>` and write the release notes: **pin the exact libsrt
   release and the three option numbers with their effective values** (118 on, 119 = the
   compat default, 120 on).

The first hard-fork release is `3.1.0`. Its publication receipt is a successful
`publish-image.yml` run plus both live registry tags resolving to the signed digest;
the git tag is created only after that verification. A branch swap or green CI alone
is not evidence of publication.

