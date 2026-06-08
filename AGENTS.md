# irl-srt-server

Fork of [irlserver/irl-srt-server](https://github.com/irlserver/irl-srt-server). C++17/CMake. Receives the bonded SRT stream from the device side and re-serves it for downstream consumers.

Parent manifest: [`../AGENTS.md`](../AGENTS.md)

---

## ROLE IN THE GROUP

```
srtla (device, bond) ──▶ irl-srt-server ──▶ ceralive-platform (ingest)
```

- Sits at the cloud edge, accepting the SRTLA-bonded stream from the device.
- Re-serves the stream over SRT so ceralive-platform can pull it.
- No encoding, no transcoding — pure SRT relay with MPEG-TS payload.

---

## STACK

| Layer | Detail |
|-------|--------|
| Language | C++17 |
| Build | CMake 3.5+, outputs to `build/bin/` |
| SRT transport | System-installed libsrt (`-lsrt`); **must be the BELABOX-patched [`irlserver/srt`](https://github.com/irlserver/srt) `belabox` branch** (defines `SRTO_SRTLAPATCHES`); no srt submodule |
| Submodules | `lib/spdlog` (irlserver/spdlog 1.9.2), `lib/json` (nlohmann/json) |
| Config | `sls.conf` — domain/app/stream routing, publisher vs player separation |

---

## SRT DEPENDENCY

`irl-srt-server` has no `srt` submodule. `.gitmodules` contains only `lib/spdlog` and `lib/json`. `src/CMakeLists.txt` links with `-lsrt` directly, so system-installed libsrt must be present before building.

**The libsrt must be the BELABOX-patched fork**, not upstream Haivision. `src/core/SLSSrt.cpp` sets the `SRTO_SRTLAPATCHES` socket option, which only exists on [`irlserver/srt`](https://github.com/irlserver/srt) (default `belabox` branch). Building against stock libsrt fails to compile. Install it before building (see README Requirements section).

The canonical, reproducible build is the [`Dockerfile`](Dockerfile) — Alpine + `irlserver/srt@belabox` + submodules — and CI (`.github/workflows/build-check.yml`) runs `docker build` so the build check never drifts from the production image.

---

## BUILD

```bash
git submodule update --init        # pulls spdlog + json only
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release
make -j
# binaries: build/bin/srt_server, build/bin/srt_client
```

---

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Server entry point | `src/srt-live-server.cpp` |
| Client test tool | `src/srt-live-client.cpp` |
| Stream routing config | `sls.conf` |
| Build wiring | `CMakeLists.txt`, `src/CMakeLists.txt` |
| Logging | via `lib/spdlog` |

---

## STREAM ID FORMAT

SLS uses RTMP-style stream IDs in the SRT `streamid` parameter:

- Publisher: `srt://host:8080?streamid=uplive.sls/live/<name>`
- Player: `srt://host:8080?streamid=live.sls/live/<name>`

Publisher and player domain/app combos must differ in `sls.conf`.

---

## NOTES

- Only MPEG-TS format is supported.
- Remote: `origin https://github.com/CERALIVE/irl-srt-server`
- Upstream catch-up: add `irlserver https://github.com/irlserver/irl-srt-server` and merge `irlserver/main` (default branch) into `master`. Last sync absorbed the multi-listen-port feature; our `master` carries `.clang-format`, `AGENTS.md`, the `.omo/` gitignore, the explicit `util.hpp`/`strlcpy` include, and the Docker-based build-check on top.
- CI: `.github/workflows/build-check.yml` runs `docker build` on amd64 + arm64.
- Not part of the device image — cloud deployment only.
