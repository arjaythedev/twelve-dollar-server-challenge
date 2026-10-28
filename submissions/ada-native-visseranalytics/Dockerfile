ARG BASE_IMAGE=ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac
FROM ${BASE_IMAGE} AS build
WORKDIR /app
COPY versions.env install.sh build.sh ./
RUN bash install.sh && groupadd --gid 10001 ada && \
    useradd --uid 10001 --gid 10001 --no-create-home ada && \
    install -d -o ada -g ada /app/bin /app/obj
COPY src ./src
# Copied/iCloud checkouts may have owner-only modes. The build runs as ada.
RUN chown -R ada:ada /app
USER 10001:10001
RUN bash build.sh && sha256sum src/* build.sh versions.env > /app/build-sha256.txt

FROM build AS validation
USER root
ARG NGINX_PACKAGE_VERSION=1.24.0-2ubuntu7.19
RUN . ./versions.env && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      sqlite3 curl jq "openssl=$OPENSSL_VERSION" python3 procps util-linux "nginx=$NGINX_PACKAGE_VERSION" && \
    rm -rf /var/lib/apt/lists/*

FROM ${BASE_IMAGE}
ARG REVISION=unspecified
ARG NGINX_PACKAGE_VERSION=1.24.0-2ubuntu7.19
LABEL org.opencontainers.image.revision="${REVISION}"
WORKDIR /app
COPY versions.env /tmp/ada-versions.env
RUN . /tmp/ada-versions.env && apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      "libgnat-13=$GNAT_VERSION" \
      "libssl3t64=$OPENSSL_VERSION" "nginx=$NGINX_PACKAGE_VERSION" \
      python3 curl util-linux && \
    groupadd --gid 10001 ada && \
    useradd --uid 10001 --gid 10001 --no-create-home ada && \
    install -d -o ada -g ada /run/ada && \
    chown -R ada:ada /var/lib/nginx /var/log/nginx && \
    rm -rf /var/lib/apt/lists/* /tmp/ada-versions.env
COPY --from=build /app/bin/server /app/bin/server
COPY --from=build /app/build-sha256.txt /app/build-sha256.txt
COPY start.sh /app/start.sh
COPY bench/entrypoint.sh bench/monitor.py /app/bench/
COPY bench/nginx-main.conf /etc/nginx/ada-benchmark.conf
RUN chmod 755 /app/start.sh /app/bench/entrypoint.sh && \
    chmod 644 /app/bench/monitor.py /etc/nginx/ada-benchmark.conf
ENV HOST=127.0.0.1 PORT=3000 SQLITE_PATH=/data/feed.db
EXPOSE 80
STOPSIGNAL SIGTERM
ENTRYPOINT ["/app/bench/entrypoint.sh"]
