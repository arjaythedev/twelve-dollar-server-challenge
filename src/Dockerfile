FROM ubuntu:24.04@sha256:78d4744ead1f5b19b04d0df558ab182a5300903900755e515b2ca4d10eb2044d

ENV UV_PYTHON_INSTALL_DIR=/opt/python \
    UV_CACHE_DIR=/tmp/uv-cache \
    UV_PYTHON_PREFERENCE=only-managed \
    HOST=0.0.0.0 PORT=3000 SQLITE_PATH=/data/feed.db
WORKDIR /app
COPY install.sh build.sh start.sh server.py pyproject.toml uv.lock .python-version ./
COPY api ./api
# Normalize Windows checkout line endings without changing upstream files.
RUN sed -i 's/\r$//' ./*.sh \
    && bash install.sh \
    && apt-get install -y --no-install-recommends jq openssl \
    && useradd --uid 10001 --create-home benchmark \
    && mkdir -p /opt/python \
    && chown -R 10001:10001 /app /opt/python
USER 10001:10001
RUN bash build.sh
CMD ["bash", "start.sh"]
