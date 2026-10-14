# Simulates the $12 droplet: Ubuntu 24.04, our install.sh as root, build.sh as a normal user.
FROM ubuntu:24.04
COPY install.sh /tmp/install.sh
RUN bash /tmp/install.sh && rm -rf /var/lib/apt/lists/* /tmp/install.sh
RUN useradd -m app && mkdir -p /data /app && chown app /data /app
WORKDIR /app
COPY --chown=app . /app
USER app
RUN bash build.sh
ENV SQLITE_PATH=/data/feed.db JWT_SECRET=twelve-dollar-challenge HOST=0.0.0.0 PORT=80
EXPOSE 80
ENTRYPOINT ["bash", "/app/docker/entrypoint.sh"]
