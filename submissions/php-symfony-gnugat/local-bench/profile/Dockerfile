# The benchmark image plus Excimer, a sampling profiler. Not part of the submission.
# BASE: to profile another phase's benchmark image, e.g. --build-arg BASE=twelve-dollar-php-symfony:p4
ARG BASE=twelve-dollar-php-symfony
FROM $BASE
USER root
RUN apt-get update \
    && apt-get install -y --no-install-recommends php-zts-excimer=1.2.7+php85-1 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -m 777 /tmp/profile
USER app
