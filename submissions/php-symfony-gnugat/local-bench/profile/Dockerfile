# The benchmark image plus Excimer, a sampling profiler. Not part of the submission.
# BASE: to profile another phase's benchmark image, e.g. --build-arg BASE=twelve-dollar-php-symfony:p4
# Excimer's module comes from the php-zts deb packages, unpacked: FrankenPHP's release binary (PHP 8.5 ZTS) loads it,
# with the profiling Caddyfiles' `php_ini extension`. Installing the package would pull PHP itself.
ARG BASE=twelve-dollar-php-symfony
FROM $BASE
USER root
RUN mkdir -p /etc/apt/keyrings /tmp/excimer /usr/local/lib/php-excimer \
    && curl -fsSL https://pkg.henderkes.com/api/packages/85/debian/repository.key -o /etc/apt/keyrings/static-php85.asc \
    && echo "deb [signed-by=/etc/apt/keyrings/static-php85.asc] https://pkg.henderkes.com/api/packages/85/debian php-zts main" \
      > /etc/apt/sources.list.d/static-php85.list \
    && apt-get update \
    && cd /tmp/excimer \
    && apt-get download php-zts-excimer=1.2.7+php85-1 \
    && dpkg -x php-zts-excimer_*.deb . \
    && mv usr/lib/php-zts/modules/excimer-zts-85.so /usr/local/lib/php-excimer/excimer.so \
    && cd / && rm -rf /tmp/excimer /var/lib/apt/lists/* \
    && mkdir -m 777 /tmp/profile
USER app
