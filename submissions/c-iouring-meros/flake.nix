{
  description = "An io_uring server (with an epoll fallback) for the $12 server challenge";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # The challenge repository: seed generator, conformance suite and load test. flake.lock pins the revision.
    # Inside a checkout of that repository use: --override-input challenge path:../..
    challenge = {
      url = "github:arjaythedev/twelve-dollar-server-challenge";
      flake = false;
    };
  };

  outputs =
    { self, nixpkgs, challenge }:
    let
      lib = nixpkgs.lib;
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # deps.env is the single source of truth for pinned dependencies; build.sh reads it too.
      deps = lib.pipe (builtins.readFile ./deps.env) [
        (lib.splitString "\n")
        (builtins.filter (line: builtins.match "[A-Z0-9_]+=.*" line != null))
        (map (line:
          let kv = builtins.match "([A-Z0-9_]+)=(.*)" line;
          in lib.nameValuePair (builtins.elemAt kv 0) (builtins.elemAt kv 1)))
        builtins.listToAttrs
      ];

      source = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions [ ./Makefile ./deps.env ./src ./tests ];
      };

      perSystem =
        pkgs:
        let
          sqlite-amalgamation = pkgs.stdenvNoCC.mkDerivation {
            pname = "sqlite-amalgamation";
            version = deps.SQLITE_VERSION;
            src = pkgs.fetchurl {
              url = "https://www.sqlite.org/${deps.SQLITE_YEAR}/sqlite-amalgamation-${deps.SQLITE_VERSION}.zip";
              sha256 = deps.SQLITE_SHA256;
            };
            nativeBuildInputs = [ pkgs.unzip ];
            installPhase = ''
              mkdir -p $out
              cp sqlite3.c sqlite3.h $out/
            '';
          };

          # liburing's static library, from the same pinned source build.sh uses. Laid out as the Makefile expects:
          # <dir>/src/include and <dir>/src/liburing.a.
          liburing = pkgs.stdenv.mkDerivation {
            pname = "liburing-static";
            version = deps.LIBURING_VERSION;
            src = pkgs.fetchurl {
              url = "https://github.com/axboe/liburing/archive/refs/tags/liburing-${deps.LIBURING_VERSION}.tar.gz";
              sha256 = deps.LIBURING_SHA256;
            };
            hardeningDisable = [ "all" ];
            configurePhase = "./configure --cc=$CC > /dev/null";
            buildPhase = "make -s -C src -j$NIX_BUILD_CORES liburing.a";
            installPhase = ''
              mkdir -p $out/src
              cp -r src/include $out/src/include
              cp src/liburing.a $out/src/liburing.a
            '';
          };

          server = pkgs.stdenv.mkDerivation {
            pname = "twelve-dollar-server";
            version = "1.0";
            src = source;
            enableParallelBuilding = true;
            hardeningDisable = [ "all" ]; # the Makefile decides (see NO_HARDENING)
            makeFlags = [ "SQLITE_DIR=${sqlite-amalgamation}" "URING_DIR=${liburing}" ];
            installPhase = "install -Dm755 bin/server $out/bin/server";
            meta.mainProgram = "server";
          };

          # Nanoseconds per call of the read queries, without networking (tests/bench/query_bench.c).
          query-bench = server.overrideAttrs (_: {
            pname = "twelve-dollar-query-bench";
            buildFlags = [ "build/query_bench" ];
            installPhase = "install -Dm755 build/query_bench $out/bin/query_bench";
            meta.mainProgram = "query_bench";
          });

          # The same build with AddressSanitizer and UBSan, for the end-to-end tests.
          server-sanitized = server.overrideAttrs (old: {
            pname = "${old.pname}-sanitized";
            makeFlags = old.makeFlags ++ [ "SANITIZE=1" ];
            dontStrip = true;
          });

          # The challenge's seed database (50k users, 500k posts, 2M likes). make-seed.sh checks its own checksum.
          seed = pkgs.runCommand "twelve-dollar-seed" { nativeBuildInputs = [ pkgs.nodejs pkgs.sqlite ]; } ''
            cp -r ${challenge}/seed ${challenge}/schema.sql .
            chmod -R u+w .
            bash seed/make-seed.sh
            mkdir -p $out
            cp seed/feed.db seed/tokens.json $out/
          '';

          # The load generator for the scale tests (tests/loadgen/loadgen.c, see its header for provenance).
          loadgen = pkgs.stdenv.mkDerivation {
            pname = "loadgen";
            version = "1.0";
            src = lib.fileset.toSource { root = ./tests/loadgen; fileset = ./tests/loadgen/loadgen.c; };
            buildPhase = "$CC -O2 -pthread -o loadgen loadgen.c -lm";
            installPhase = "install -Dm755 loadgen $out/bin/loadgen";
            meta.mainProgram = "loadgen";
          };

          scaleTest = import ./nix/scale-test.nix { inherit pkgs server seed loadgen challenge; };

          e2eTools = [ pkgs.curl pkgs.jq pkgs.openssl pkgs.python3 ];

          runE2e = binary: ''
            export SERVER_BIN=${binary}/bin/server SEED_DB=${seed}/feed.db CHALLENGE_ROOT=${challenge}
            bash ${source}/tests/e2e/run.sh
          '';

          # `nix run .#loadtest -- <users>`: the challenge's k6 test against this server, pinned to one CPU when possible.
          withServer = name: runtimeInputs: command:
            pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = runtimeInputs ++ [ pkgs.coreutils pkgs.util-linux pkgs.curl ];
              text = ''
                work="$(mktemp -d)"
                server_pid=""
                trap '[ -z "$server_pid" ] || kill "$server_pid" 2>/dev/null; rm -rf "$work"' EXIT
                cp ${seed}/feed.db "$work/feed.db"; chmod u+w "$work/feed.db"
                export JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000 SQLITE_PATH="$work/feed.db"
                taskset -c 0 ${server}/bin/server &
                server_pid=$!
                for _ in $(seq 100); do curl -sf http://127.0.0.1:3000/health >/dev/null && break; sleep 0.2; done
                ${command}
              '';
            };
        in
        {
          packages = {
            default = server;
            inherit server server-sanitized liburing query-bench seed sqlite-amalgamation loadgen;

            # Virtual machines shaped like the challenge's box: `nix build .#vm-scale-100k -L`.
            vm-scale-60k = scaleTest { users = 60000; rampSeconds = 40; holdSeconds = 40; };
            vm-scale-100k = scaleTest { users = 100000; rampSeconds = 60; holdSeconds = 60; };
            vm-scale-150k = scaleTest { users = 150000; rampSeconds = 60; holdSeconds = 60; };
            vm-scale-250k = scaleTest { users = 250000; rampSeconds = 90; holdSeconds = 60; };
            # Real k6 against a server with a tiny descriptor limit: 4,000 users on 1,500 descriptors is the same
            # situation as 100,000 users on 65,535, small enough for k6 to drive.
            vm-k6-fallback = scaleTest { users = 4000; rampSeconds = 30; holdSeconds = 60; fdLimit = 1500; useK6 = true; };
          };

          checks = {
            # Compiles with warnings as errors and runs the unit tests.
            unit = server.overrideAttrs (old: {
              pname = "twelve-dollar-unit";
              makeFlags = old.makeFlags ++ [ "EXTRA_CFLAGS=-Werror" ];
              doCheck = true;
              checkTarget = "test";
              installPhase = "touch $out";
            });

            # The challenge's 42 checks, then the protocol tests, against the optimized build.
            e2e = pkgs.runCommand "twelve-dollar-e2e" { nativeBuildInputs = e2eTools; } ''
              ${runE2e server}
              touch $out
            '';

            # The same under AddressSanitizer and UBSan: any memory error or undefined behaviour fails the run.
            e2e-sanitized = pkgs.runCommand "twelve-dollar-e2e-sanitized" { nativeBuildInputs = e2eTools; } ''
              export ASAN_OPTIONS=detect_leaks=0
              ${runE2e server-sanitized}
              touch $out
            '';
          };

          apps = {
            e2e = {
              type = "app";
              program = lib.getExe (pkgs.writeShellApplication {
                name = "e2e";
                runtimeInputs = e2eTools;
                text = runE2e server;
              });
            };
            bench = {
              type = "app";
              program = lib.getExe (withServer "bench" [ pkgs.wrk ] ''
                for path in /feed /posts/250000 /health; do
                  echo "== GET $path"
                  wrk -t4 -c512 -d8s --latency "http://127.0.0.1:3000$path" | grep -E 'Requests/sec|50%|99%|Non-2xx'
                done
              '');
            };
            # `nix run .#query-bench`: the read queries on a fresh database copy, with an empty and a filled write-ahead log.
            query-bench = {
              type = "app";
              program = lib.getExe (pkgs.writeShellApplication {
                name = "query-bench";
                runtimeInputs = [ pkgs.coreutils ];
                text = ''
                  work="$(mktemp -d)"
                  trap 'rm -rf "$work"' EXIT
                  for wal_writes in 0 2000; do
                    cp ${seed}/feed.db "$work/feed.db"; chmod u+w "$work/feed.db"; rm -f "$work"/feed.db-*
                    ${query-bench}/bin/query_bench "$work/feed.db" "$wal_writes"
                  done
                '';
              });
            };
            loadtest = {
              type = "app";
              program = lib.getExe (withServer "loadtest" [ pkgs.k6 ] ''
                k6 run -e VUS="''${1:-2500}" -e TOKENS=${seed}/tokens.json ${challenge}/bench/load.js
              '');
            };
          };

          # For comparison runs with another server binary: the same virtual machines, any `serverPackage`.
          lib = { inherit scaleTest; };

          devShells.default = pkgs.mkShell {
            inputsFrom = [ server ];
            packages = e2eTools ++ [
              pkgs.gdb pkgs.valgrind pkgs.strace pkgs.perf pkgs.wrk pkgs.k6 pkgs.sqlite
              pkgs.clang-tools pkgs.shellcheck
            ];
            SQLITE_DIR = "${sqlite-amalgamation}";
            URING_DIR = "${liburing}";
            shellHook = ''
              echo "make          build bin/server"
              echo "make test     unit tests"
              echo "SEED_DB=${seed}/feed.db SERVER_BIN=bin/server CHALLENGE_ROOT=${challenge} bash tests/e2e/run.sh"
            '';
          };
        };

      outputs = forAllSystems perSystem;
    in
    {
      packages = lib.mapAttrs (_: o: o.packages) outputs;
      checks = lib.mapAttrs (_: o: o.checks) outputs;
      apps = lib.mapAttrs (_: o: o.apps) outputs;
      devShells = lib.mapAttrs (_: o: o.devShells) outputs;
      lib = lib.mapAttrs (_: o: o.lib) outputs;
    };
}
