FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS tools
COPY submissions/java-spring-boot-nb-99/install.sh /tmp/install.sh
RUN bash /tmp/install.sh
ENV JAVA_HOME=/opt/java-challenge-25.0.1
ENV PATH="/opt/java-challenge-25.0.1/bin:/opt/maven-challenge-3.9.11/bin:${PATH}"

FROM tools AS build
ARG MAVEN_PROXY_ENABLED=false
ARG MAVEN_PROXY_HOST=localhost
ARG MAVEN_PROXY_PORT=8080
COPY submissions/java-spring-boot-nb-99/maven-settings.xml.template /root/.m2/settings.xml
RUN sed -i -e "s/@enabled@/${MAVEN_PROXY_ENABLED}/" \
    -e "s/@host@/${MAVEN_PROXY_HOST}/" -e "s/@port@/${MAVEN_PROXY_PORT}/" /root/.m2/settings.xml
WORKDIR /app
COPY submissions/java-spring-boot-nb-99/pom.xml ./
COPY schema.sql /schema.sql
RUN mvn --batch-mode --no-transfer-progress dependency:go-offline
COPY submissions/java-spring-boot-nb-99/src ./src
COPY submissions/java-spring-boot-nb-99/build.sh ./
RUN bash build.sh

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl libstdc++6 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home app
COPY --from=tools /opt/java-challenge-25.0.1 /opt/java-challenge-25.0.1
WORKDIR /app
COPY --from=build /app/target/server.jar ./target/server.jar
COPY submissions/java-spring-boot-nb-99/start.sh ./start.sh
USER 10001:10001
EXPOSE 3000
ENTRYPOINT ["bash", "/app/start.sh"]

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS checks
RUN apt-get update \
    && apt-get install -y --no-install-recommends nodejs sqlite3 curl jq openssl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /challenge
COPY schema.sql ./schema.sql
COPY seed ./seed
COPY test ./test
COPY submissions/java-spring-boot-nb-99/seed-local.sh ./seed-local.sh
ENTRYPOINT ["bash"]
