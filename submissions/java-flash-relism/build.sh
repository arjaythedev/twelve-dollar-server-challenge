#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
MAVEN_VERSION=3.9.16
MAVEN_SHA512=831a8591fe20c8243b1dbe7d71e3244f31d1665b0804b2e825e38cbbe5ce0cafb8338851f90780735568773e0a6cd07bbec107cda0b896b008b861075358b6f6
if [[ -x /opt/flash-feed/jdk/bin/java ]]; then
    export JAVA_HOME=/opt/flash-feed/jdk
    export PATH="$JAVA_HOME/bin:$PATH"
fi
if [[ ! -x ".tools/apache-maven-$MAVEN_VERSION/bin/mvn" ]]; then
    mkdir -p .tools
    curl --fail --location --retry 3 \
        "https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/$MAVEN_VERSION/apache-maven-$MAVEN_VERSION-bin.tar.gz" \
        -o .tools/maven.tar.gz
    echo "$MAVEN_SHA512  .tools/maven.tar.gz" | sha512sum -c -
    tar -xzf .tools/maven.tar.gz -C .tools
    rm .tools/maven.tar.gz
fi
exec ".tools/apache-maven-$MAVEN_VERSION/bin/mvn" --batch-mode --no-transfer-progress clean package "$@"
