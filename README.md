# SRT Live Server

## Introduction

srt-live-server (SLS) is an open source live streaming server for low latency based on Secure Reliable Transport (SRT).
Normally, the latency of transport by SLS is less than 1 second on the internet.

This repository is the IRL focused fork of SLS. It adds SRTLA (bonded cellular) support, player key authentication, per stream bitrate limiting, audio gap filling, webhook driven push destinations, an extended HTTP stats / control API, and a number of stability fixes documented in the feature docs and `CONFIGURATION.md`.

## CERALIVE fork

This is CERALIVE's hard fork of [`irlserver/irl-srt-server`](https://github.com/irlserver/irl-srt-server). The server source under `src/` is upstream's, byte for byte. What CERALIVE changes is the libsrt the server is built against, the CI/CD layer, and how the production image is released. `AGENTS.md` has the full contract; the short version:

**libsrt.** Upstream builds against `irlserver/srt` (branch `belabox`, SRT 1.5.5 era). This fork builds against [`CERALIVE/srt`](https://github.com/CERALIVE/srt): Haivision SRT **1.5.7** plus three CERALIVE socket options. The pin is the release tag `srt-v1.5.7+ceralive.2` (Debian package `libsrt1.5-ceralive 1.5.7+ceralive.2`), resolved to commit `d487b13365205b6cd5da9d9b50868c323e255b7c`. `scripts/check-srt-pin.sh` asserts the `Dockerfile` and every CI job pin the same commit.

| Option | Enumerator | What it does |
|---|---|---|
| `118` | `SRTO_SRTLAPATCHES` | Compatibility name for what upstream's srt calls `SRTLAPATCHES`. Setting it turns on `120` and sets `119` to the compat default. This is the only one SLS itself sets (`listen_publisher_srtla`). |
| `119` | `SRTO_PERIODICNAKGATE` | Periodic NAK report gate, tri-state: `0` always send (stock SRT), `1` send only for genuine loss (reorderable ranges filtered out), `2` never send (upstream `SRTLAPATCHES` behaviour). The released compat default is `2`, selected by the D10 simulation A/B (24 valid runs, four cells, N=3 per arm); this is not hardware validation. |
| `120` | `SRTO_REORDERFREEZE` | Freeze the reorder tolerance at its maximum instead of letting it decay on ordered runs. |

Stock Haivision libsrt does not declare `SRTO_SRTLAPATCHES`, so this source does not compile against it. That is intended: one CI leg builds against apt libsrt and passes only when the compile fails.

**Image.** The production image is `ghcr.io/ceralive/irl-srt-server:<PROJECT_VERSION>`, where PROJECT_VERSION is upstream's `CMakeLists.txt` version (currently `3.1.0`, so the target tag is `ghcr.io/ceralive/irl-srt-server:3.1.0`). Tags are semver and immutable; the same manifest is also tagged `sha-<commit>`. A successful publication run and both live tags resolving to its signed digest are the release receipt; `docs/IMAGE-RELEASE.md` is the procedure. The SRTLA listener's effective CERALIVE options are **118 on, 119 = 2, 120 on**.

**Branches.** `main` is the canonical and GitHub default branch; push and PR CI target it, and image publication validates `origin/main` and the `refs/heads/main` signing identity. The previous canonical history is preserved at `legacy` (`ae229f9`); it must not be force-pushed or deleted.

**CI.** `ci.yml` runs the repository contract scripts, a `debug` / `asan-ubsan` / `tsan` matrix on the pinned CERALIVE/srt (full ctest plus the publisher-authorization E2E), the stock-libsrt negative leg, clang-tidy, clang-format, a 60 s libFuzzer smoke per target, and a report-only coverage job. `build-check.yml` builds the production `Dockerfile` for `amd64` and `arm64`, verifies the shipped binary links the pin, and runs Trivy, SBOM, and CodeQL. `publish-image.yml` is manual and is the only thing that publishes.

## Requirements

SLS depends on the IRL maintained SRT fork at `https://github.com/irlserver/srt` (branch `belabox`). This fork carries the SRTLA patches the server requires. (CERALIVE builds against `CERALIVE/srt` instead; see the "CERALIVE fork" section above. The paragraph below is upstream's and still describes why stock SRT is not a runtime option.) Building against upstream Haivision SRT will compile but produces the dropped packet / glitching behavior the SRTLA notes in this README warn about; only use upstream SRT as a reference for the base SRT API, not as the runtime dependency.

System prerequisites:

- A C++17 capable compiler (GCC or Clang).
- CMake 3.10 or newer.
- OpenSSL development headers (`openssl-dev` on Alpine, `libssl-dev` on Debian or Ubuntu).
- zlib development headers (`zlib-dev` on Alpine, `zlib1g-dev` on Debian or Ubuntu).
- The IRL SRT fork (`irlserver/srt`, branch `belabox`) built and installed on the host. See the Dockerfile in this repository for the exact build steps used in CI.
- Git submodules in this repository (`git submodule update --init`).

SLS builds and runs on Linux and on macOS. It is not supported on Windows.

## Compilation

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Binaries are created in `build/bin/`.

## Running the tests

The repository ships a doctest based unit test suite wired into CTest.

```bash
cmake -S . -B build -DSLS_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Two sanitizer build flavors are available for catching memory and threading bugs on the manual ring buffer and the cross thread role / listener / manager state. These options are mutually exclusive.

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON -DSLS_SANITIZE=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON -DSLS_TSAN=ON
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure
```

### Fuzzing the parsers

Four libFuzzer targets exercise the network- and operator-boundary input parsers
under AddressSanitizer + UndefinedBehaviorSanitizer:

| Target | Drives | Seed corpus |
|--------|--------|-------------|
| `fuzz_ts_parser` | the length-driven MPEG-TS / PAT / PMT / PES parser | `tests/fuzz/corpus/ts/` |
| `fuzz_timecode` | the in-band SMPTE timecode scanner (TS -> PES -> NAL -> SEI) | `tests/fuzz/corpus/timecode/` |
| `fuzz_streamid` | the SRT `streamid` parse + handshake-time safety gate | `tests/fuzz/corpus/streamid/` |
| `fuzz_conf` | the `sls.conf` port-list / tokenizer / value setters | `tests/fuzz/corpus/conf/` |

Fuzzing is a dedicated, **clang-only** build flavor (libFuzzer is a Clang feature).
`SLS_FUZZ` is mutually exclusive with `SLS_SANITIZE` / `SLS_TSAN`, so use a separate
build directory. Build all four targets once:

```bash
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Release -DSLS_FUZZ=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_ts_parser fuzz_timecode fuzz_streamid fuzz_conf -j
```

**Local 60-second smoke run (mirrors CI).** This is the exact invocation the `fuzz`
CI job runs on every push / PR — a fixed 60 s budget per target against the committed
seed corpus, failing on any crash. A writable work directory is passed **first** so
the committed corpus stays pristine and any `crash-*` unit lands there; `-close_fd_mask=1`
silences the parser's own stdout logging so libFuzzer's progress stays readable; the
UBSan suppressions file mutes one benign, documented signed-shift finding without
affecting crash detection.

```bash
for t in ts_parser:ts timecode:timecode streamid:streamid conf:conf; do
  tgt="fuzz_${t%%:*}"; corpus="tests/fuzz/corpus/${t##*:}"
  work="$(mktemp -d)"
  UBSAN_OPTIONS=suppressions=tests/fuzz/ubsan_suppressions.txt \
    ./build-fuzz/bin/"$tgt" -max_total_time=60 -close_fd_mask=1 "$work" "$corpus"
done
```

**Extended / nightly campaign.** For a deeper, longer-running campaign, raise the time
budget and let libFuzzer grow the corpus in a writable directory. Seed it from the
committed corpus and keep the new finds. Set `-max_total_time` to one hour below, or
drop the flag entirely for an unbounded run that stops only on a crash:

```bash
mkdir -p fuzz-runs/ts && cp tests/fuzz/corpus/ts/* fuzz-runs/ts/
UBSAN_OPTIONS=suppressions=tests/fuzz/ubsan_suppressions.txt \
  ./build-fuzz/bin/fuzz_ts_parser \
    -max_total_time=3600 -print_final_stats=1 \
    -jobs=$(nproc) -workers=$(nproc) \
    fuzz-runs/ts tests/fuzz/corpus/ts
```

`-jobs` / `-workers` fan the campaign across cores; libFuzzer writes any new coverage
units into the first directory (`fuzz-runs/ts`). Promote genuinely useful new inputs
back into `tests/fuzz/corpus/ts/` to strengthen the committed seed set.

**Reproduce a crash.** When a run finds a bug, libFuzzer writes the offending bytes to
a `crash-<sha1>` file in the writable work directory (and the CI job uploads it as the
`fuzz-findings` artifact). Replay it deterministically by passing that single file:

```bash
UBSAN_OPTIONS=suppressions=tests/fuzz/ubsan_suppressions.txt \
  ./build-fuzz/bin/fuzz_ts_parser crash-<sha1>
```

The target runs that one input once and prints the ASan / UBSan report; minimize it
further with `-minimize_crash=1 -runs=100000 crash-<sha1>`.

## Usage

`cd build`

### Help information

```bash
./bin/srt_server -h
```

### Run with default configuration file

```bash
./bin/srt_server -c ../src/sls.conf
```

## Configuration

The full list of IRL specific configuration directives lives in [`CONFIGURATION.md`](CONFIGURATION.md). The upstream `rstular/srt-live-server` [wiki](https://github.com/rstular/srt-live-server/wiki/Directives) remains a useful reference for the base SLS directives this fork inherited, but every directive added by this fork is documented in `CONFIGURATION.md`.

### SRTLA / Bonded Connection Support

SRT Live Server supports both SRTLA (bonded cellular) and direct SRT connections on the same server using separate publisher ports:

```
server {
    listen_player 4000;               # All streams playable here
    listen_publisher 4001;            # Direct SRT (OBS, FFmpeg)
    listen_publisher_srtla 4002;      # SRTLA/bonded (via srtla_rec)
    ...
}
```

- `listen_publisher` (for direct SRT connections, standard behavior)
- `listen_publisher_srtla` (for SRTLA/bonded connections, enables SRTLA patches automatically)
- `listen_player` (playback for streams from both publisher types)

**Multiple ports per role**
`listen_player`, `listen_publisher`, and `listen_publisher_srtla` each accept more than one port. Provide a comma separated list, inclusive ranges (`a-b`), or a mix. One listener is created per port, so a client may connect on any of them.

```
server {
    listen_player 4000,4010,5000-5005;   # players may connect on any of these
    listen_publisher 4001;
    ...
}
```

**Why separate ports?**
SRTLA bonded connections require special SRT patches that disable dynamic reorder tolerance and periodic NAK reports. Using the wrong setting causes glitching. Direct SRT served with SRTLA patches drops packets, while SRTLA served without the patches produces spurious retransmissions.

## Testing

srt-live-server only supports the MPEG-TS format streaming.

### Test with FFmpeg

You can push a camera live stream using FFmpeg. FFmpeg must be compiled with `--enable-libsrt`. To obtain appropriate binaries, download FFmpeg sourcecode from https://github.com/FFmpeg/FFmpeg, then compile FFmpeg with `--enable-libsrt`.

The `srt` library is installed in folder `/usr/local/lib64`.

If `ERROR: srt >= 1.3.0 not found using pkg-config` occurs during the compilation of FFmpeg, please check the `ffbuild/config.log` file and follow its instruction to resolve this issue. In most cases it can be resolved by executing the following command:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig
```

If `error while loading shared libraries: libsrt.so.1` occurs, please add the `srt` library path to the runtime linker configuration file, `/etc/ld.so.conf`, then refresh the cache by running the command `/sbin/ldconfig` as root.

#### Push stream from webcam to SRT

```bash
./ffmpeg -f avfoundation -framerate 30 -i "0:0" -vcodec libx264  -preset ultrafast -tune zerolatency -flags2 local_header  -acodec libmp3lame -g  30 -pkt_size 1316 -flush_packets 0 -f mpegts "srt://[your.sls.ip]:8080?streamid=uplive.sls/live/test"
```

#### Play a SRT stream using FFplay

```bash
./ffplay -fflags nobuffer -i "srt://[your.sls.ip]:8080?streamid=live.sls/live/test"
```

### Test with OBS

OBS supports the SRT protocol to publish streams from version `v25.0` onwards. To publish an SRT stream from OBS to SRT Live Server you can use the following url:

```
srt://[your.sls.ip]:8080?streamid=uplive.sls/live/test
```

You can also add an SRT stream as an input source. To do this, add a `Media source` to OBS, enter `mpegts` as input format and set the following input URL:

```
srt://[your.sls.ip]:8080?streamid=live.sls/live/test
```

### Test with SRT Live Client

There is a test tool in SLS which can be used as a performance test. It has no codec overhead, only network overhead. The SRT Live Client can play an SRT stream to a TS file, or push a TS file to an SRT stream.

#### Push a TS file via SRT

```bash
./srt_client -r srt://[your.sls.ip]:8080?streamid=uplive.sls/live/test -i [the full file name of exist ts file]
```

#### Play a SRT stream

```bash
./srt_client -r srt://[your.sls.ip]:8080?streamid=live.sls/live/test -o [the full file name of ts file to save]
```

## Use SLS with docker

The repository's `Dockerfile` builds a minimal Alpine based image that pins the SRT fork to a known good commit. In this fork that is `CERALIVE/srt` (see "CERALIVE fork" above). To bump the pin, change `ARG SRT_COMMIT=...` in the `Dockerfile` and the matching `SRT_COMMIT:` sites in `.github/workflows/ci.yml`, then run `scripts/check-srt-pin.sh`. A community maintained image of the upstream project is also published at `https://hub.docker.com/r/ravenium/srt-live-server`.

## Development

To build a debug build of the SRT Live Server, run the following commands:

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON
cmake --build build -j
```

For sanitizer flavored debug builds see the "Running the tests" section above. For agent and contributor orientation (build layout, where the live SRT boundary is, commit conventions) see [`CLAUDE.md`](CLAUDE.md); for the CERALIVE-specific contract (libsrt pin, socket options, release procedure, anti-patterns) see [`AGENTS.md`](AGENTS.md).

### Bumping vendored submodules

Vendored libraries under `lib/` are pinned via git submodules. `lib/cpp-httplib` is pinned to release tag `v0.48.0`, `lib/json` to `v3.12.0`, and `lib/spdlog` tracks the `irlserver/spdlog` fork (which does not publish release tags, so it is pinned by commit). To bump one:

```bash
cd lib/<name>
git fetch --tags
git checkout <new-tag-or-commit>
cd ../..
git add lib/<name>
git commit -m "chore(deps): bump <name> to <new-tag-or-commit>"
```

libsrt is not a submodule; it is pinned by commit hash via the `SRT_COMMIT` build argument in `Dockerfile` and checked by `scripts/check-srt-pin.sh`.

## Notes

- SLS refers to the RTMP url format (domain/app/stream_name), example: www.sls.com/live/test. The URL must be set in the streamid parameter of SRT, which will be the unique identification of a stream.

- How to distinguish the publisher and player of the same stream? In the configuration file, you can set parameters of `domain_player` / `domain_publisher` and `app_player` / `app_publisher` to resolve it. Importantly, the two combination strings of `domain_publisher` / `app_publisher` and `domain_player` / `app_player` must not be equal in the same server block.

- A simple Android app for testing SLS can be downloaded from https://github.com/Edward-Wu/liteplayer-srt.
