# SRT Live Server

## Introduction

srt-live-server (SLS) is an open source live streaming server for low latency based on Secure Reliable Transport (SRT).
Normally, the latency of transport by SLS is less than 1 second on the internet.

This repository is the IRL focused fork of SLS. It adds SRTLA (bonded cellular) support, player key authentication, per stream bitrate limiting, audio gap filling, webhook driven push destinations, an extended HTTP stats / control API, and a number of stability fixes.

## Requirements

SLS builds and runs on Linux and on macOS. It is not supported on Windows.

System prerequisites:

- A C++17 capable compiler (GCC or Clang).
- CMake 3.10 or newer.
- OpenSSL development headers (`openssl-dev` on Alpine, `libssl-dev` on Debian or Ubuntu).
- zlib development headers (`zlib-dev` on Alpine, `zlib1g-dev` on Debian or Ubuntu).
- A libsrt install — the BELABOX-patched `irlserver/srt` (`belabox`) **or** stock
  Haivision/srt. The patched fork is optional (ADR-002); see the libsrt section
  immediately below for how the CMake probe selects the path.
- Git submodules in this repository (`git submodule update --init`).

This server links against libsrt to drive SRTLA bonded connections. It builds
against **either** libsrt fork — the patched fork is **optional**:

- **BELABOX-patched [`irlserver/srt`](https://github.com/irlserver/srt)** (`belabox`
  branch) provides the `SRTO_SRTLAPATCHES` socket option. When present, SLS uses it
  — the original, unchanged behavior.
- **Stock [Haivision/srt](https://github.com/Haivision/srt)** lacks that option. When
  building against stock libsrt, SLS uses the standard equivalents
  (`SRTO_NAKREPORT=0` + `SRTO_LOSSMAXTTL=40`) on the SRTLA publisher listener.

A CMake probe (`SLS_HAVE_SRTO_SRTLAPATCHES`) detects which libsrt is on the include
path and compiles the matching branch automatically — no flags needed. The startup
log states the active mode (`SRT compat mode: srtlapatches` vs `standard-options`).
The stock substitution is authorized by ADR-002 ("SRT patch necessity"), which found
it a SAFE replacement for the custom patch under reorder stress.

To build against the patched belabox fork:

```bash
git clone https://github.com/irlserver/srt.git
cd srt && git checkout belabox && ./configure && make -j$(nproc) && sudo make install && sudo ldconfig
```

To build against stock libsrt, install your distro's `libsrt-dev` (or build
Haivision/srt) instead — no patched fork required.

The canonical, reproducible build is the [`Dockerfile`](Dockerfile), which uses
`CERALIVE/srt@reorderfreeze-1.5.5`; the CI build check
(`.github/workflows/build-check.yml`) runs `docker build` so it can never drift
from how the image is produced.

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

Three libFuzzer targets exercise the network- and operator-boundary input parsers
under AddressSanitizer + UndefinedBehaviorSanitizer:

| Target | Drives | Seed corpus |
|--------|--------|-------------|
| `fuzz_ts_parser` | the length-driven MPEG-TS / PAT / PMT / PES parser | `tests/fuzz/corpus/ts/` |
| `fuzz_streamid` | the SRT `streamid` parse + handshake-time safety gate | `tests/fuzz/corpus/streamid/` |
| `fuzz_conf` | the `sls.conf` port-list / tokenizer / value setters | `tests/fuzz/corpus/conf/` |

Fuzzing is a dedicated, **clang-only** build flavor (libFuzzer is a Clang feature).
`SLS_FUZZ` is mutually exclusive with `SLS_SANITIZE` / `SLS_TSAN`, so use a separate
build directory. Build all three targets once:

```bash
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Release -DSLS_FUZZ=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target fuzz_ts_parser fuzz_streamid fuzz_conf -j
```

**Local 60-second smoke run (mirrors CI).** This is the exact invocation the `fuzz`
CI job runs on every push / PR — a fixed 60 s budget per target against the committed
seed corpus, failing on any crash. A writable work directory is passed **first** so
the committed corpus stays pristine and any `crash-*` unit lands there; `-close_fd_mask=1`
silences the parser's own stdout logging so libFuzzer's progress stays readable; the
UBSan suppressions file mutes one benign, documented signed-shift finding without
affecting crash detection.

```bash
for t in ts_parser:ts streamid:streamid conf:conf; do
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

Stream routing and listener directives are configured in `sls.conf` (see the example at `src/sls.conf`). The upstream `rstular/srt-live-server` [wiki](https://github.com/rstular/srt-live-server/wiki/Directives) remains a useful reference for the base SLS directives this fork inherited.

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

## Troubleshooting

**Which SRT compat mode am I running?**
At startup SLS logs the active mode per listener (`info` level, from
`CSLSSrt::libsrt_setup`):

```
SRT compat mode: reorderfreeze (CERALIVE/srt, reorderfreeze + nakreport=1).
SRT compat mode: srtlapatches (patched libsrt).
SRT compat mode: standard-options (stock libsrt, nakreport=0, lossmaxttl=40).
```

`reorderfreeze` means the binary was built against `CERALIVE/srt@reorderfreeze-1.5.5`
(the canonical production libsrt) and is using `SRTO_REORDERFREEZE` per-profile with
NAK set independently. `srtlapatches` means it was built against the BELABOX-patched
`irlserver/srt@belabox` and is using `SRTO_SRTLAPATCHES`. `standard-options` means it
was built against stock Haivision/srt and is using the `SRTO_NAKREPORT=0` +
`SRTO_LOSSMAXTTL=40` equivalents. The mode is fixed at build time by the CMake compat
probe — to switch it, rebuild against the other libsrt.

**A connection is refused with "unsafe characters in host/app/stream".**
The `streamid` resolved to a domain/app/stream component containing a path
separator (`/`, `\`), a control byte, or a bare `.` / `..`. These are rejected
before the name reaches any filesystem path. Fix the `streamid` to use plain
names (letters, digits, `_`, `-`, `.` — but not a leading `.`/`..`).

**The config fails to load with a port error.**
A `listen_player` / `listen_publisher` / `listen_publisher_srtla` spec is
malformed. Each entry must be a single port, a comma-separated list, or an
**ascending** inclusive range `a-b`, all within `1..65535`. A reversed range
such as `5005-5000`, a non-numeric token, or a port outside the range is
rejected at config-parse time with a line number, rather than failing later at
bind. Example of a valid spec: `4000,4010,5000-5005`.

**Build fails with `srt not found` or an `-lsrt` link error.**
System libsrt is missing. Install your distro's `libsrt-dev`, or build a libsrt
fork — see [Requirements](#requirements). After a manual `make install`, run
`sudo ldconfig` so the runtime linker can find `libsrt.so`.

## Use SLS with docker

The repository's `Dockerfile` builds a minimal Alpine based image using `CERALIVE/srt@reorderfreeze-1.5.5` (commit `66b3609`). To bump that pin, change the `ARG SRT_COMMIT=...` line in the `Dockerfile` to the new commit hash from `https://github.com/CERALIVE/srt/tree/reorderfreeze-1.5.5`. A community maintained image is also published at `https://hub.docker.com/r/ravenium/srt-live-server`.

## Development

To build a debug build of the SRT Live Server, run the following commands:

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON
cmake --build build -j
```

For sanitizer flavored debug builds see the "Running the tests" section above. For agent and contributor orientation (build layout, where the live SRT boundary is, commit conventions) see [`AGENTS.md`](AGENTS.md).

### Bumping vendored submodules

Vendored libraries under `lib/` are pinned via git submodules:

| Library | Pin | Notes |
|---------|-----|-------|
| `lib/cpp-httplib` | tag `v0.48.0` (commit `9d159bb`) | yhirose/cpp-httplib release tag |
| `lib/json` | tag `v3.12.0` | nlohmann/json release tag |
| `lib/thread-pool` | tag `v5.1.0` | bshoshany/thread-pool release tag |
| `lib/spdlog` | commit `f1d748e5` | irlserver/spdlog fork — no release tags published; pinned by commit |
| `lib/CxxUrl` | commit `e81b86e` (7 commits past `v0.3`) | No tag covers this commit; `v0.3` is the latest upstream tag but the repo has not tagged subsequent fixes. Pinned by commit until upstream publishes a new tag. |

To bump one:

```bash
cd lib/<name>
git fetch --tags
git checkout <new-tag-or-commit>
cd ../..
git add lib/<name>
git commit -m "chore(deps): bump <name> to <new-tag-or-commit>"
```

The SRT fork (`CERALIVE/srt@reorderfreeze-1.5.5`) is not a submodule; it is pinned by commit hash via the `SRT_COMMIT` build argument in `Dockerfile`.

## Notes

- SLS refers to the RTMP url format (domain/app/stream_name), example: www.sls.com/live/test. The URL must be set in the streamid parameter of SRT, which will be the unique identification of a stream.

- How to distinguish the publisher and player of the same stream? In the configuration file, you can set parameters of `domain_player` / `domain_publisher` and `app_player` / `app_publisher` to resolve it. Importantly, the two combination strings of `domain_publisher` / `app_publisher` and `domain_player` / `app_player` must not be equal in the same server block.

- A simple Android app for testing SLS can be downloaded from https://github.com/Edward-Wu/liteplayer-srt.
