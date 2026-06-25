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
RUN apk add --no-cache linux-headers alpine-sdk cmake tcl openssl-dev zlib-dev ffmpeg
WORKDIR /tmp
COPY . /tmp/srt-live-server/
# Pin SRT to a known-good commit on the CERALIVE/srt reorderfreeze-1.5.5 branch
# (Haivision v1.5.5 + opt-in SRTO_REORDERFREEZE; replaces the retired belabox fork).
# Bump source: https://github.com/CERALIVE/srt/tree/reorderfreeze-1.5.5
ARG SRT_COMMIT=66b3609cc004e6a4c485e0adc11149025e782083
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
