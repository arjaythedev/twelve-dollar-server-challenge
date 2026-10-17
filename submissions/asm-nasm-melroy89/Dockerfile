# Local qualification image. The organizer builds through install/build/start.sh.
FROM ubuntu:24.04
WORKDIR /app
COPY install.sh /tmp/install.sh
RUN bash /tmp/install.sh \
    && useradd -m runner && chown runner:runner /app
COPY --chown=runner:runner src /app/src
COPY --chown=runner:runner tools /app/tools
COPY --chown=runner:runner build.sh start.sh /app/
USER runner
RUN bash build.sh
CMD ["bash", "start.sh"]
