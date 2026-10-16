#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
# Serial GC for 1 vCPU. Young gen is fixed and large so that only minor GCs run in steady state;
# metaspace is sized up front so it never needs a Full GC to grow; old gen is 384m (heap 768m).
exec java --enable-native-access=ALL-UNNAMED -XX:ActiveProcessorCount=1 -XX:+UseSerialGC \
  -Xms768m -Xmx768m -Xmn384m -XX:MaxTenuringThreshold=3 \
  -XX:MetaspaceSize=64m -XX:MaxMetaspaceSize=64m -XX:+AlwaysPreTouch \
  -jar build/libs/twelve-dollar-micronaut.jar "$@"
