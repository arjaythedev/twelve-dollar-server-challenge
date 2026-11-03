#!/usr/bin/env bash
# Shared by build.sh/start.sh. Prefer an explicitly configured Java 25, then PATH.
java25() { [[ -x "$1/bin/java" ]] && "$1/bin/java" -XshowSettings:properties -version 2>&1 | grep -Eq 'java.specification.version = 25$'; }
if [[ -n "${JAVA_HOME:-}" ]] && java25 "$JAVA_HOME"; then
  :
elif command -v java >/dev/null && java25 "$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"; then
  JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"
elif java25 /opt/java-jdkhttpserver/jdk-25.0.4.1+1; then
  JAVA_HOME=/opt/java-jdkhttpserver/jdk-25.0.4.1+1
else
  echo 'Java 25 is required. Set JAVA_HOME or run install.sh.' >&2
  exit 1
fi
export JAVA_HOME
export PATH="$JAVA_HOME/bin:$PATH"
