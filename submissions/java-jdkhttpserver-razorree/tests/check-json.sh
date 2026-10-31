#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source ./java-env.sh
output="$1"
mkdir -p "$output/classes"
"$JAVA_HOME/bin/javac" -cp "target/server.jar:target/lib/*" -d "$output/classes" tests/JsonContractCheck.java tests/AuthContractCheck.java tests/DatabaseContractCheck.java
"$JAVA_HOME/bin/java" -cp "$output/classes:target/server.jar:target/lib/*" challenge.JsonContractCheck

"$JAVA_HOME/bin/java" -cp "$output/classes:target/server.jar:target/lib/*" challenge.AuthContractCheck

"$JAVA_HOME/bin/java" --enable-native-access=ALL-UNNAMED -cp "$output/classes:target/server.jar:target/lib/*" challenge.DatabaseContractCheck "$PWD/../../seed/feed.db" "$output/database-contract.db"
