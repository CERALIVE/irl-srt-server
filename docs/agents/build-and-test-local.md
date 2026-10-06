<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## BUILD AND TEST (local)

```bash
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSLS_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
bash scripts/check-srt-pin.sh
bash scripts/test-image-publish-contracts.sh
```

The host must have `CERALIVE/srt` at the pinned commit installed (`./configure && make &&
make install`, exactly as the `Dockerfile` does). A stock `libsrt-openssl-dev` on the include
path wins over `/usr/local` on some distros and turns every build into the negative leg;
`docker build .` is the safe isolation. Before scripting an edit, check line endings:
several `src/core/*.cpp` files and `src/sls.conf` are CRLF.

