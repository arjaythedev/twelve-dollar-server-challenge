# The benchmark box, approximately: Ubuntu 24.04, install.sh as root, build.sh as a normal user.
# Build context: the submission folder (see bench.sh).
FROM ubuntu:24.04 AS runtime

COPY install.sh /tmp/install.sh
RUN bash /tmp/install.sh \
    && apt-get install -y --no-install-recommends jq openssl procps \
    && rm -rf /var/lib/apt/lists/*
RUN useradd --create-home app

FROM runtime
COPY --chown=app:app . /app
USER app
RUN bash /app/build.sh

COPY --chmod=755 local-bench/serve.sh /usr/local/bin/serve
CMD ["serve"]
