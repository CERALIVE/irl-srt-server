# build stage
FROM alpine:3.21 as build
# ffmpeg is pulled in for the SRT loopback e2e below (it only synthesises the
# TS payload); it lives in the build stage and never reaches the final image.
ENV LD_LIBRARY_PATH=/usr/local/lib64:/usr/local/lib
RUN apk update &&\
    apk upgrade &&\ 
    apk add --no-cache linux-headers alpine-sdk cmake tcl openssl-dev zlib-dev ffmpeg
WORKDIR /tmp
COPY . /tmp/srt-live-server/
# Pin SRT to a known-good commit on the belabox branch for reproducible builds.
# Bump source: https://github.com/irlserver/srt/tree/belabox
ARG SRT_COMMIT=f2297192ce9ab572464e84228efbc46f8c1eabf4
RUN git clone https://github.com/irlserver/srt.git
WORKDIR /tmp/srt
RUN git checkout ${SRT_COMMIT} && ./configure && make -j$(nproc) && make install
WORKDIR /tmp/srt-live-server
RUN git submodule update --init
RUN cmake . -DCMAKE_BUILD_TYPE=Release
RUN make -j$(nproc)

# Hard build gates: the config-validation unit tests (ctest) and a real SRT
# loopback e2e must both pass, or `docker build` fails. The e2e runs srt_server
# and pushes/pulls an MPEG-TS stream over libsrt on 127.0.0.1 — no skip path.
RUN cp tests/e2e/sls-loopback.conf /etc/sls-loopback.conf && \
    ctest --output-on-failure && \
    PATH="/tmp/srt-live-server/bin:${PATH}" sh tests/e2e/srt_loopback.sh

# final stage
FROM alpine:3.21
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
