# build stage
# Base image is digest-pinned for reproducible, supply-chain-verifiable builds;
# bump the tag+digest together (docker inspect alpine:<tag> --format
# '{{index .RepoDigests 0}}'). `apk upgrade` is intentionally omitted so the
# pinned digest fully determines the package set — the Trivy gate in CI flags
# any HIGH/CRITICAL CVE that the pin would otherwise carry.
FROM alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d AS build
# ffmpeg is pulled in for the SRT loopback e2e below (it only synthesises the
# TS payload); it lives in the build stage and never reaches the final image.
ENV LD_LIBRARY_PATH=/usr/local/lib64:/usr/local/lib
# iproute2 (tc/netem), curl + jq are used by the SRT loopback e2e's loss-matrix
# leg (tests/e2e/srt_loopback.sh phase 4): netem injects loss/reorder and the
# HTTP /stats endpoint is read for the per-profile NAK differential. Under a
# plain `docker build` (no NET_ADMIN) that leg self-SKIPs; they are present so a
# `docker run --cap-add=NET_ADMIN` of this stage can exercise the full matrix.
RUN apk add --no-cache linux-headers alpine-sdk cmake tcl openssl-dev zlib-dev ffmpeg iproute2 curl jq
WORKDIR /tmp
COPY . /tmp/srt-live-server/
# Pin SRT to the CERALIVE/srt 1.5.6+ceralive.1 release (tag srt-v1.5.6+ceralive.1,
# HEAD of master): Haivision v1.5.6 with the KMREQ heap-overflow CVE fix
# (CVE-2026-55869), carrying the CeraLive SRTO_REORDERFREEZE and socket-teardown
# patches. Same libsrt the device board runs (libsrt1.5-ceralive 1.5.6+ceralive.1).
# Bump source: https://github.com/CERALIVE/srt/releases (tag srt-v<version>).
ARG SRT_COMMIT=b06fdb6b85937f3f5cf5452b150a6bb7e35b0226
RUN git clone https://github.com/CERALIVE/srt.git
WORKDIR /tmp/srt
RUN git checkout ${SRT_COMMIT} && ./configure && make -j$(nproc) && make install
WORKDIR /tmp/srt-live-server
RUN git submodule update --init
RUN cmake . -DCMAKE_BUILD_TYPE=Release -DSLS_BUILD_TESTS=ON
RUN make -j$(nproc)

# Hard build gates: the full unit suite (ctest — config validator + the doctest
# tests, including the SRT receive-profile checks that read the freeze/NAK
# sockopts back off real listeners on this CERALIVE/srt build) and a real SRT
# loopback e2e must both pass, or `docker build` fails. The e2e runs srt_server
# and pushes/pulls an MPEG-TS stream over libsrt on 127.0.0.1 — no skip path.
RUN cp tests/e2e/sls-loopback.conf /etc/sls-loopback.conf && \
    ctest --output-on-failure && \
    PATH="/tmp/srt-live-server/bin:${PATH}" sh tests/e2e/srt_loopback.sh

# final stage
FROM alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d
ENV LD_LIBRARY_PATH /lib:/usr/lib:/usr/local/lib64
RUN apk add --no-cache openssl libstdc++ &&\
    adduser -D srt &&\
    mkdir /etc/sls /logs &&\
    chown srt /logs
COPY --from=build /usr/local/bin/srt-* /usr/local/bin/
COPY --from=build /usr/local/lib/libsrt* /usr/local/lib/
# Ship only the server. srt_client is a load/test tool built in the build stage
# (and used by the loopback e2e there); it has no place in the production image.
COPY --from=build /tmp/srt-live-server/bin/srt_server /usr/local/bin/
COPY src/sls.conf /etc/sls/
VOLUME /logs
EXPOSE 8181 1936/udp
USER srt
WORKDIR /home/srt
ENTRYPOINT [ "srt_server", "-c", "/etc/sls/sls.conf"]
