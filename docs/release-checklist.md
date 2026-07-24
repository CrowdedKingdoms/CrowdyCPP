# CrowdyCPP release checklist

Run every required gate from a clean CrowdyCPP checkout. CrowdyJS must be the
exact target declared by `crowdyjsParityTarget` in `package.json`, built with
`npm ci && npm run build`, and exposed through `CROWDYJS_PATH`.

## Content, codegen, and parity

```bash
npm ci
bash scripts/check-content-policy.sh
npm run check:codegen
node tools/parity/parity.mjs \
  --crowdyjs "$CROWDYJS_PATH" \
  --check docs/parity-matrix.md \
  --strict
node tools/parity/agent-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
node tools/parity/layout-fixtures.mjs --crowdyjs "$CROWDYJS_PATH"
npm test
```

The parity command above is the command used by CI. The tools reject a
same-version CrowdyJS checkout at any commit other than the package pin.

## Default build and tests

```bash
cmake -S . -B build-release-default -DCMAKE_BUILD_TYPE=Release
cmake --build build-release-default -j
ctest --test-dir build-release-default --output-on-failure
```

Unconfigured live e2e executables must report skipped through exit 77; they are
not default live release evidence.

## Curl-free build and tests

```bash
cmake -S . -B build-release-no-curl \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_WITH_CURL=OFF \
  -DCROWDY_WITH_CURL_WEBSOCKETS=OFF \
  -DCROWDY_BUILD_E2E=OFF
cmake --build build-release-no-curl -j
ctest --test-dir build-release-no-curl --output-on-failure
```

## Non-throwing compatibility build and tests

```bash
cmake -S . -B build-release-no-exceptions \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_NO_EXCEPTIONS=ON \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF
cmake --build build-release-no-exceptions -j
ctest --test-dir build-release-no-exceptions --output-on-failure
```

Install this profile and verify the reduced package omits Compute authoring,
Crowdy Studio project API/models/controller, Agent/controller, player-host,
Game Kit, and `ContainerMirror` headers while the independent
`crowdy/studio/layout.hpp` surface remains installed and the umbrella consumer
still links.

## OpenSSL-off library build

```bash
cmake -S . -B build-release-no-openssl \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_WITH_OPENSSL=OFF \
  -DCROWDY_BUILD_TESTS=OFF \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCROWDY_BUILD_EXAMPLES=OFF
cmake --build build-release-no-openssl -j
```

This verifies that consumers may supply `ICrypto`; tests that intentionally
link the packaged OpenSSL provider are disabled for this configuration.

## Public headers compile standalone

```bash
git ls-files -z 'include/crowdy/*.hpp' 'include/crowdy/**/*.hpp' |
while IFS= read -r -d '' header; do
  include="${header#include/}"
  printf '#include <%s>\nint main() { return 0; }\n' "$include" |
    c++ -std=c++20 -Iinclude -x c++ -fsyntax-only -
done
```

## Address and undefined-behavior sanitizers

```bash
cmake -S . -B build-release-sanitizers \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-release-sanitizers -j
ctest --test-dir build-release-sanitizers --output-on-failure
```

## Install and external consumer

```bash
cmake -S . -B build-release-install \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDY_BUILD_TESTS=OFF \
  -DCROWDY_BUILD_E2E=OFF \
  -DCROWDY_BUILD_BENCHMARKS=OFF \
  -DCROWDY_BUILD_EXAMPLES=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build-release-prefix"
cmake --build build-release-install -j
cmake --install build-release-install
cmake -S tests/consumer -B build-release-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build-release-prefix"
cmake --build build-release-consumer -j
./build-release-consumer/crowdycpp_consumer
```

The package version file accepts compatible patches within the requested
minor, but must reject a different `0.x` minor.

## Optional live e2e evidence

Only run these against an explicitly selected test deployment. Configure the
variables in [`tests/e2e/README.md`](../tests/e2e/README.md); WebSocket evidence
also requires `CROWDY_E2E_WEBSOCKET=1` and a default or injected transport.

```bash
ctest --test-dir build-release-default -L e2e --output-on-failure
ctest --test-dir build-release-default -L e2e_slow --output-on-failure
ctest --test-dir build-release-default -L e2e_optional --output-on-failure
```

Record passes, failures, and exit-77 skips separately. A skipped live suite is
not evidence that its platform round trip passed.
