# Optional local qualification image; the organiser uses install/build/start.sh.
FROM ubuntu:24.04
WORKDIR /app
ENV PATH="/opt/twelve-fastify-node-v26.11.1/bin:$PATH"
COPY install.sh /tmp/install.sh
RUN bash /tmp/install.sh \
    && apt-get install -y --no-install-recommends curl jq openssl sqlite3 \
    && useradd -m runner && chown runner:runner /app
COPY --chown=runner:runner package.json package-lock.json tsconfig.json server.ts build.sh start.sh verify.mjs /app/
USER runner
RUN bash build.sh
CMD ["bash", "start.sh"]
