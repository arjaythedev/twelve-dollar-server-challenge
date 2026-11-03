# Linux perf, to profile the server container from outside (--privileged --pid=container:...). Not part of the submission.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends linux-tools-generic && rm -rf /var/lib/apt/lists/* \
    && ln -s "$(ls -d /usr/lib/linux-tools/*/perf | head -1)" /usr/local/bin/perf
