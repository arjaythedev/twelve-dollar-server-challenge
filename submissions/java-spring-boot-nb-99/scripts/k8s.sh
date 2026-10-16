#!/usr/bin/env bash
# Run the challenge suite or benchmark in the current kubectl context.
#   scripts/k8s.sh test
#   scripts/k8s.sh benchmark [benchmark.py options]
# Builds the app and seed images for the cluster's node architectures, pushes them
# to $K8S_REGISTRY, and applies k8s/ pinned to those digests in $K8S_NAMESPACE.
# Its pods are deleted afterwards; the namespace and the optional image pull secret
# $K8S_PULL_SECRET are provided by the user and stay.
set -euo pipefail
action=${1:?usage: k8s.sh test|benchmark [benchmark.py options]}
shift
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
: "${K8S_REGISTRY:?Set K8S_REGISTRY to the image repository prefix, e.g. registry.example.com/owner}"
ns=${K8S_NAMESPACE:-twelve-dollar-challenge}
pull_secret=${K8S_PULL_SECRET:-}
selector=app.kubernetes.io/part-of=twelve-dollar-java
# Pin the context so switching contexts during a run cannot redirect it.
context=$(kubectl config current-context)
kube=(kubectl --context "$context" -n "$ns")
work=$(mktemp -d "${TMPDIR:-/tmp}/twelve-dollar-java.XXXXXX")
pids=()
child=
deployed=false
# shellcheck source-path=SCRIPTDIR source=lib.sh
. "$here/scripts/lib.sh"

cleanup() {
  local status=$?
  trap - EXIT
  for pid in ${pids[@]+"${pids[@]}"}; do kill "$pid" 2>/dev/null || true; done
  if [ "$deployed" = true ] &&
    ! "${kube[@]}" delete pod,service -l "$selector" --wait >"$work/cleanup.log" 2>&1; then
    cat "$work/cleanup.log" >&2
    status=2
  fi
  rm -rf "$work"
  exit "$status"
}
trap cleanup EXIT
# Bash runs traps only after a foreground command exits, so the benchmark runs in
# the background. Ctrl-C already reaches it; SIGTERM sent to make must be forwarded.
stop() {
  if [ -n "$child" ]; then
    [ "$1" = INT ] || kill -TERM "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  exit "$2"
}
trap 'stop INT 130' INT
trap 'stop TERM 143' TERM

need curl
need docker
need python3
echo "Context $context, namespace $ns"
if [ -n "$pull_secret" ] && ! "${kube[@]}" get secret "$pull_secret" -o name >/dev/null 2>&1; then
  echo "Namespace $ns has no image pull secret $pull_secret." >&2
  exit 1
fi

platforms=$(kubectl --context "$context" get nodes \
  -o jsonpath='{range .items[*]}linux/{.status.nodeInfo.architecture}{"\n"}{end}' | sort -u | paste -sd, -)
mkdir "$work/k8s"
cp -R "$here/k8s" "$work/k8s/base"
{
  echo "resources: [base]"
  echo "namespace: $ns"
  echo "images:"
} >"$work/k8s/kustomization.yaml"
for image in runtime:app checks:seed; do
  name=twelve-dollar-java-${image#*:}
  echo "Building $name for $platforms and pushing it to $K8S_REGISTRY"
  docker buildx build --platform "$platforms" --target "${image%%:*}" \
    --build-arg MAVEN_PROXY_ENABLED="${MAVEN_PROXY_ENABLED:-false}" \
    --build-arg MAVEN_PROXY_HOST="${MAVEN_PROXY_HOST:-localhost}" \
    --build-arg MAVEN_PROXY_PORT="${MAVEN_PROXY_PORT:-8080}" \
    -f "$here/Dockerfile" -t "$K8S_REGISTRY/$name:latest" --push \
    --metadata-file "$work/$name.json" "$root"
  digest=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["containerimage.digest"])' \
    "$work/$name.json")
  printf '  - name: %s\n    newName: %s\n    digest: %s\n' "$name" "$K8S_REGISTRY/$name" "$digest" \
    >>"$work/k8s/kustomization.yaml"
done
echo "patches:" >>"$work/k8s/kustomization.yaml"
if [ -n "$pull_secret" ]; then
  printf '%s\n' '  - target: {kind: Pod}' '    patch: |-' '      - op: add' \
    '        path: /spec/imagePullSecrets' "        value: [{name: $pull_secret}]" \
    >>"$work/k8s/kustomization.yaml"
fi
if [ "$action" = test ]; then
  # The challenge suite runs on this host and needs no load generator.
  # shellcheck disable=SC2016 # $patch is a literal kustomize directive.
  printf '%s\n' '  - patch: |-' '      $patch: delete' '      apiVersion: v1' \
    '      kind: Pod' '      metadata:' '        name: k6' >>"$work/k8s/kustomization.yaml"
  pods=(pod/server)
else
  pods=(pod/server pod/k6)
fi

# Every run starts from fresh pods, so the database is a fresh seed copy.
deployed=true
"${kube[@]}" delete pod,service -l "$selector" --wait
"${kube[@]}" apply -k "$work/k8s"
echo "Waiting for image pulls, seeding, and readiness"
deadline=$((SECONDS + 600))
until "${kube[@]}" wait --for=condition=Ready "${pods[@]}" --timeout=5s >/dev/null 2>&1; do
  # The pod has no restarting container, so look for failed containers.
  states=$("${kube[@]}" get pods -l "$selector" -o jsonpath='{range .items[*]}{range .status.initContainerStatuses[*]}{.state.terminated.exitCode} {.state.waiting.reason} {end}{range .status.containerStatuses[*]}{.state.terminated.exitCode} {.state.waiting.reason} {end}{end}')
  if [[ $states =~ [1-9]|Err|BackOff ]] || [ "$SECONDS" -ge "$deadline" ]; then
    echo "The pods did not become ready." >&2
    "${kube[@]}" get pods -o wide >&2
    for pod in "${pods[@]}"; do
      "${kube[@]}" logs "$pod" --all-containers --tail=40 >&2 || true
    done
    exit 1
  fi
done

# The port-forward only carries health checks and the challenge suite; k6 runs in the cluster.
port=$(free_port)
"${kube[@]}" port-forward pod/server "$port:80" >"$work/port-forward.log" 2>&1 &
pids+=("$!")
url=http://127.0.0.1:$port
if ! wait_healthy "$url" "$!"; then
  cat "$work/port-forward.log" >&2
  exit 1
fi

case "$action" in
  test)
    need jq
    need openssl
    NO_PROXY='*' no_proxy='*' bash "$root/test/test.sh" "$url"
    ;;
  benchmark)
    printf -v exec_prefix 'kubectl --context %q -n %q exec' "$context" "$ns"
    options=(--mode k8s --url "$url"
      --k6 exec --k6-exec "$exec_prefix k6 -c k6 --" --k6-url http://server
      --app-exec "$exec_prefix server -c app --"
      --app-limits "$("${kube[@]}" get pod server -o jsonpath='{.spec.resources.limits}')")
    if [ "${PROFILE:-false}" = true ]; then
      options+=(--jfr-exec "$exec_prefix server -c app --")
    fi
    python3 "$here/benchmark.py" "${options[@]}" "$@" &
    child=$!
    wait "$child"
    ;;
  *)
    echo "Unknown action: $action" >&2
    exit 2
    ;;
esac
