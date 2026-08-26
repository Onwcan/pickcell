# Multi-stage: the build tree is large -- protobuf, abseil and three fetched
# repositories -- and none of it belongs in the image that runs.

FROM ubuntu:24.04 AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      g++ cmake ninja-build git ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/

# Tests are the top-level project's business, not the image's.
RUN cmake -S . -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DPICKCELL_BUILD_TESTS=OFF && \
    cmake --build /build -j "$(nproc)"

FROM ubuntu:24.04 AS runtime

# Not `scratch` here, unlike safeedge. protobuf and abseil are linked
# dynamically against the system C++ runtime, and statically linking the whole
# set to reach a scratch image would be a fight with no prize -- this container
# is a demonstration harness, not a safety-rated artefact.
RUN apt-get update && \
    apt-get install -y --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/* && \
    useradd --system --no-create-home --uid 10001 pickcell

COPY --from=build /build/src/pickcelld /usr/local/bin/pickcelld
COPY --from=build /build/src/pickcell-reaction /usr/local/bin/pickcell-reaction

USER 10001
EXPOSE 9200

# No shell in the healthcheck: pickcelld serves /healthz and the orchestrator
# can probe it directly.
ENTRYPOINT ["/usr/local/bin/pickcelld"]
