# SRT Live Server

## Introduction

srt-live-server (SLS) is an open source live streaming server for low latency based on Secure Reliable Tranport (SRT).
Normally, the latency of transport by SLS is less than 1 second in internet.

## Requirements

SLS can only run on Unix-based operating systems.

This server links against libsrt to drive SRTLA bonded connections. It builds
against **either** libsrt fork — the patched fork is **optional**:

- **BELABOX-patched [`irlserver/srt`](https://github.com/irlserver/srt)** (`belabox`
  branch) provides the `SRTO_SRTLAPATCHES` socket option. When present, SLS uses it
  — the original, unchanged behavior.
- **Stock [Haivision/srt](https://github.com/Haivision/srt)** lacks that option. When
  building against stock libsrt, SLS uses the standard equivalents
  (`SRTO_NAKREPORT=0` + `SRTO_LOSSMAXTTL=30`) on the SRTLA publisher listener.

A CMake probe (`SLS_HAVE_SRTO_SRTLAPATCHES`) detects which libsrt is on the include
path and compiles the matching branch automatically — no flags needed. The startup
log states the active mode (`SRT compat mode: srtlapatches` vs `standard-options`).
The stock substitution is authorized by ADR-002 ("SRT patch necessity"), which found
it a SAFE replacement for the custom patch under reorder stress.

To build against the patched fork (still used by the Docker image):

```bash
git clone https://github.com/irlserver/srt.git
cd srt && git checkout belabox && ./configure && make -j$(nproc) && sudo make install && sudo ldconfig
```

To build against stock libsrt, install your distro's `libsrt-dev` (or build
Haivision/srt) instead — no patched fork required.

The canonical, reproducible build is the [`Dockerfile`](Dockerfile), which uses the
patched fork; the CI build check (`.github/workflows/build-check.yml`) runs
`docker build` so it can never drift from how the image is produced.

## Compilation

```bash
git submodule update --init
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Release
make -j
```

Binaries are created in `build/bin/` directory.

## Usage

`cd build`

### Help information

```bash
./srt_server -h
```

### Run with default configuration file

```bash
./srt_server -c ../sls.conf
```

## Configuration

Configuration directives are documented on the [wiki page](https://github.com/rstular/srt-live-server/wiki/Directives).

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
SRTLA bonded connections require special SRT patches that disable dynamic reorder tolerance and periodic NAK reports. Using the wrong setting causes glitching:
- Direct SRT with SRTLA patches = dropped packets
- SRTLA without patches = spurious retransmissions

## Testing

srt-live-server only supports the MPEG-TS format streaming.

### Test with FFmpeg

You can push camera live stream using FFmpeg. FFmpeg must be compiled with `--enable-libsrt` flag - to obtain appropriate binaries, download FFmpeg sourcecode from https://github.com/FFmpeg/FFmpeg, then compile FFmpeg with `--enable-libsrt`.

`srt` library is installed in folder `/usr/local/lib64`.

If `ERROR: srt >= 1.3.0 not found using pkg-config` occurs during the compilation of FFmpeg, please check the `ffbuild/config.log` file and follow its instruction to resolve this issue. In most cases it can be resolved by executing the following command:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig
```

If `error while loading shared libraries: libsrt.so.1` occurs, please add `srt` library path to the runtime linker configuration file, `/etc/ld.so.conf`, then refresh the cache by running the comand `/sbin/ldconfig` as root.

#### Push stream from webcam to SRT

```bash
./ffmpeg -f avfoundation -framerate 30 -i "0:0" -vcodec libx264  -preset ultrafast -tune zerolatency -flags2 local_header  -acodec libmp3lame -g  30 -pkt_size 1316 -flush_packets 0 -f mpegts "srt://[your.sls.ip]:8080?streamid=uplive.sls/live/test"
```

#### Play a SRT stream using FFplay

```bash
./ffplay -fflags nobuffer -i "srt://[your.sls.ip]:8080?streamid=live.sls/live/test"
```

### Test with OBS

OBS supports SRT protocol to publish streams from version `v25.0` onwards. To publish SRT stream from OBS to SRT Live Server you can use the following url:

```
srt://[your.sls.ip]:8080?streamid=uplive.sls/live/test
```

You can also add a SRT stream as an input source. To do this, add a `Media source` to OBS, enter `mpegts` as input format and set the following input URL:

```
srt://[your.sls.ip]:8080?streamid=live.sls/live/test
```

### Test with SRT Live Client

There is a test tool in SLS which can be used as a performance test - it has no codec overhead, only network overhead. The SRT Live Client can play a SRT stream to a TS file, or push a TS file to a SRT stream.

#### Push a TS file via SRT

```bash
./srt_client -r srt://[your.sls.ip]:8080?streamid=uplive.sls/live/test -i [the full file name of exist ts file]
```

#### Play a SRT stream

```bash
./srt_client -r srt://[your.sls.ip]:8080?streamid=live.sls/live/test -o [the full file name of ts file to save]
```

## Use SLS with docker

Please refer to: https://hub.docker.com/r/ravenium/srt-live-server

## Development

To build a debug build of the SRT Live Server, run the following commands:

```bash
git submodule update --init
mkdir build && cd build
cmake ../ -DCMAKE_BUILD_TYPE=Debug
make -j
```

## Note:

- SLS refers to the RTMP url format (domain/app/stream_name), example: www.sls.com/live/test. The URL must be set in streamid parameter of SRT, which will be the unique identification a stream.

- How to distinguish the publisher and player of the same stream? In the configuration file file, you can set parameters of domain_player/domain_publisher and app_player/app_publisher to resolve it. Importantly, the two combination strings of domain_publisher/app_publisher and domain_player/app_player must not be equal in the same server block.

- I supplied a simple android app for testing SLS, which can be downloaded from https://github.com/Edward-Wu/liteplayer-srt
