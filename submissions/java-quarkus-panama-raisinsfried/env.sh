# Sourced by build.sh and start.sh: /opt/temurin-27, else $JAVA_HOME, else the javac on PATH; plus /opt/maven.
if [ -x /opt/temurin-27/bin/java ]; then
  export JAVA_HOME=/opt/temurin-27
elif [ -z "${JAVA_HOME:-}" ] || [ ! -x "$JAVA_HOME/bin/javac" ]; then
  if command -v javac >/dev/null 2>&1; then
    JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
    export JAVA_HOME
  fi
fi
if [ -n "${JAVA_HOME:-}" ]; then
  export PATH="$JAVA_HOME/bin:$PATH"
fi
if [ -x /opt/maven/bin/mvn ]; then
  export PATH="/opt/maven/bin:$PATH"
fi
