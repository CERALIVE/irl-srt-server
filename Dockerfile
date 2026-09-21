# build stage
# Base image digest-pinned for reproducible, supply-chain-verifiable builds.
# Bump the tag+digest together: docker inspect alpine:<tag> --format '{{index .RepoDigests 0}}'.
FROM alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d as build
RUN apk update &&\
    apk add --no-cache linux-headers alpine-sdk cmake tcl openssl-dev zlib-dev
WORKDIR /tmp
COPY . /tmp/srt-live-server/
# Pin SRT to the CERALIVE/srt release tag srt-v1.5.7+ceralive.2: Haivision v1.5.7
# carrying the CERALIVE SRTO_PERIODICNAKGATE and SRTO_SRTLAPATCHES socket options —
# the same libsrt the device runs (Debian libsrt1.5-ceralive 1.5.7+ceralive.2).
# The SHA below is the TAG's resolved commit, not the merge commit that preceded it.
# Bump source: https://github.com/CERALIVE/srt (tag srt-v<version>).
ARG SRT_COMMIT=d487b13365205b6cd5da9d9b50868c323e255b7c
RUN git clone https://github.com/CERALIVE/srt.git
WORKDIR /tmp/srt
RUN git checkout ${SRT_COMMIT} && ./configure && make -j$(nproc) && make install
WORKDIR /tmp/srt-live-server
RUN git submodule update --init
RUN cmake . -DCMAKE_BUILD_TYPE=Release
RUN make -j$(nproc)

# final stage
FROM alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d
ENV LD_LIBRARY_PATH /lib:/usr/lib:/usr/local/lib64
RUN apk update &&\
    apk add --no-cache openssl libstdc++ &&\
    adduser -D srt &&\
    mkdir /etc/sls /logs &&\
    chown srt /logs
COPY --from=build /usr/local/bin/srt-* /usr/local/bin/
COPY --from=build /usr/local/lib/libsrt* /usr/local/lib/
COPY --from=build /tmp/srt-live-server/bin/* /usr/local/bin/
COPY src/sls.conf /etc/sls/
VOLUME /logs
EXPOSE 8181 1936/udp
USER srt
WORKDIR /home/srt
ENTRYPOINT [ "srt_server", "-c", "/etc/sls/sls.conf"]
