#!/usr/bin/env bash
# Builds zsh 5.9 from its release tarball into ./zsh, with one extra module, zsh/sqlite
# (module/sqlite.c), and the two-line ztcp change below. Everything else is stock zsh.
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
[ zsh/bin/zsh -nt module/sqlite.c ] && [ zsh/bin/zsh -nt build.sh ] && exit 0
rm -rf build zsh && mkdir build && cd build
curl -fsSL -o zsh.tar.xz https://www.zsh.org/pub/old/zsh-5.9.tar.xz \
  || curl -fsSL -o zsh.tar.xz https://downloads.sourceforge.net/project/zsh/zsh/5.9/zsh-5.9.tar.xz
echo '9b8d1ecedd5b5e81fbf1918e876752a7dd948e05c1a0dba10ab863842d45acd5  zsh.tar.xz' | sha256sum -c -
tar xf zsh.tar.xz && cd zsh-5.9
cp "$D"/module/sqlite.c "$D"/module/sqlite.mdd Src/Modules/
# Three changes to ztcp -l for serving: a listen backlog of 4096 instead of 1, SO_REUSEADDR so a
# restart can bind while old connections sit in TIME_WAIT, and binding to $HOST (stock ztcp always binds 0.0.0.0).
sed -i -e 's/listen(sess->fd, 1)/listen(sess->fd, 4096)/' \
  -e 's/^\(	if (!zsh_inet_aton("0.0.0.0", \&(sess->sock.in.sin_addr)))\)$/	len = 1;\n	setsockopt(sess->fd, SOL_SOCKET, SO_REUSEADDR, (char *)\&len, sizeof(len));\n\1/' \
  -e 's/zsh_inet_aton("0.0.0.0", &(sess/zsh_inet_aton(getenv("HOST") ? getenv("HOST") : "0.0.0.0", \&(sess/' \
  Src/Modules/tcp.c
grep -q SO_REUSEADDR Src/Modules/tcp.c && grep -q 'getenv("HOST")' Src/Modules/tcp.c && grep -q 'listen(sess->fd, 4096)' Src/Modules/tcp.c
./configure -q --prefix="$D/zsh" --disable-gdbm --disable-pcre --without-tcsetpgrp LIBS=-lsqlite3 >/dev/null
make -s -j"$(nproc)" >/dev/null
make -s install.bin install.modules >/dev/null
cd "$D" && rm -rf build
zsh/bin/zsh -fc 'zmodload zsh/sqlite zsh/net/tcp zsh/zselect zsh/system && echo built: $ZSH_VERSION'
