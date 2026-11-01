#!/usr/bin/env bash
# sync-libs.sh <repo-dir-or-url> [commit]: re-vendor one of our libraries into third_party/<repo>/ from a pinned commit,
# then rewrite its line in third_party/PINS (repo, commit, sha256 over the vendored files; build.sh checks it).
# Only the files the build needs are copied, unchanged, plus the repo's licences. Default commit: HEAD.
#   tools/sync-libs.sh ../cursed-http-parser 3be7fd3
#   tools/sync-libs.sh https://github.com/public/cursed-jwt.git 6f92f71
#   tools/sync-libs.sh ../cursed-sqlite-jit f748f0b          (vendored as third_party/cursed-sqlite-turbo)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=${1:?usage: sync-libs.sh <repo-dir-or-url> [commit]}
rev=${2:-HEAD}
repo=$(basename "${src%.git}")
[ "$repo" = cursed-sqlite-jit ] && repo=cursed-sqlite-turbo # the repo's old directory name
case $repo in
  cursed-http-parser) paths="LICENSE include/fastparse.h include/batchparse.h include/fbparse.h gen/httpgen.py
    vendor/pico/picohttpparser.c vendor/pico/picohttpparser.h vendor/pico/COMMIT vendor/pico/README.md" ;;
  cursed-jwt) paths="LICENSE licenses/OpenSSL-LICENSE.txt include/jwt.h src/jwt.c src/jwt_mb.inc
    src/sha256_x86_64_ossl.S" ;;
  cursed-sqlite-turbo) paths="LICENSE qc/qc.c qc/qcg.c qc/qvm.c qc/qvm_run.h qc/gen_decoders.py" ;; # DB_READS=turbo
  *) echo "sync-libs.sh: unknown library '$repo'" >&2; exit 2 ;;
esac
sha() { if sha256sum --version > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -d "$src/.git" ] || git -C "$src" rev-parse --git-dir > /dev/null 2>&1; then
  git_dir=$src
else
  git clone --quiet --no-checkout "$src" "$tmp/clone"
  git_dir=$tmp/clone
fi
commit=$(git -C "$git_dir" rev-parse "$rev^{commit}")
# shellcheck disable=SC2086 # $paths is a word list
git -C "$git_dir" archive --format=tar "$commit" $paths | tar -x -C "$tmp" -f - 2> /dev/null ||
  { mkdir -p "$tmp/x"; git -C "$git_dir" archive --format=tar "$commit" $paths | tar -x -C "$tmp/x" -f -; }
dst=$here/third_party/$repo
rm -rf "$dst"
mkdir -p "$dst"
for p in $paths; do
  mkdir -p "$dst/$(dirname "$p")"
  cp "$tmp/$p" "$dst/$p"
done
sum=$(cd "$dst" && find . -type f | LC_ALL=C sort | while read -r f; do sha "$f"; done | sha | cut -d' ' -f1)
touch "$here/third_party/PINS"
grep -v "^$repo " "$here/third_party/PINS" > "$tmp/pins" || true
echo "$repo $commit $sum" >> "$tmp/pins"
LC_ALL=C sort "$tmp/pins" > "$here/third_party/PINS"
echo "third_party/$repo: $commit ($(echo $paths | wc -w | tr -d ' ') files, sha256 $sum)"
