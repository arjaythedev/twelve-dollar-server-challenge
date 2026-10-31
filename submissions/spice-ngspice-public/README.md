# spice-ngspice-public: the API is a circuit simulated in SPICE

Every request is answered by a circuit simulated in **ngspice**. The circuit is a request chip (Verilog), a
gate-level signature comparator, and a ring-oscillator clock. SQLite stores the data. Bash and socat move
bytes between the socket, the simulator and the `sqlite3` CLI, and decide nothing.

- **Language:** Verilog (the chip), SPICE netlist (the board), Bash (the wire)
- **Simulator:** ngspice 42 (Ubuntu 24.04 package) with XSPICE. The chip runs as a `d_cosim` element,
  compiled by Verilator 5.020.
- **Database:** SQLite through the `sqlite3` CLI (3.45, Ubuntu 24.04)
- **Server:** socat 1.8 forks one `wire/conn.sh` per connection
- **Mode:** **behind Nginx** (`bench/nginx.conf`) on `127.0.0.1:3000`. Upstream connections stay open until
  Nginx closes them.

## What runs where

| Part | Where it runs | What it does |
|---|---|---|
| `chip/*.v` (~750 lines of Verilog) | ngspice, as a `d_cosim` element (the Verilog compiled by Verilator and loaded into the simulator) | Parses the raw HTTP request: request line, path, query, and the `Authorization` header (matched case-insensitively). Does the routing and the post id checks. Checks `Bearer`. Splits the JWT and base64url-decodes it. Validates the JSON of the header and payload. Computes **HMAC-SHA256** (a SHA-256 core at one round per clock, with the key from `JWT_SECRET`). Checks `alg`, `exp` (against the time it is given), `sub` and `username`. Validates the body JSON fully (a streaming parser with a 64-deep stack). Decodes it, including `\u` escapes and surrogate pairs to UTF-8. Trims it and counts characters. Writes the SQL text, quoting `'`. Then turns SQLite's raw rows into the JSON response, escaping `"`, `\` and control characters. Picks every status code and error. Writes the whole HTTP/1.1 response, Content-Length included. |
| `board/board.cir.in` (SPICE netlist) | ngspice, as XSPICE digital primitives | The 100 MHz clock is a ring oscillator: a NAND gate fed back through a delay line. A `d_source` drives the request bytes onto 8 pins. **The signature compare is 12 gates** (8 XOR, an 8-input OR and an accumulate-and-clear flip-flop). The chip streams the HMAC tag and the token's signature through it a byte pair per clock, and its output bit decides whether the token is valid. A DAC bridge onto an analog node ends the run when the chip raises `done`. |
| `wire/conn.sh` (Bash) | one process per connection | Frames each request off the socket: the lines up to the blank line, then `Content-Length` bytes. Turns bytes into `d_source` rows and the chip's output pins back into bytes. Runs the SQL the chip wrote through `sqlite3 -ascii` and gives the chip SQLite's stdout, stderr and exit code. Copies the chip's response to the socket. Its only branch on content is the first byte of the chip's answer: `!` means the chip answered on its own, `?` means run this SQL first. |
| `sqlite3` | one process per database request | Runs the statements. |

The circuit's outputs decide every response. A request that needs the database is two simulations (phase A:
request → SQL; phase B: SQLite's rows → HTTP response) with one `sqlite3` run between them. Errors (401,
400, 404) come from phase A alone.

What isn't gates: the chip is RTL that Verilator turns into C++, which ngspice runs as a `d_cosim` circuit
element on its own clock and pins. I first tried the whole thing as Yosys-synthesised gates (4-LUTs and
flip-flops) in XSPICE. That works, but the SHA-256 core alone (3,856 elements) took ~4 ms per clock in
ngspice, so one HMAC would take ~1.7 s, and parsing the netlist took 5.7 s. Through `d_cosim`, the same core
costs ~2.4 µs per clock. So the gate-level part is the comparator and the clock, and the rest of the chip is
Verilog.

## The board (excerpt of `board/board.cir.in`)

```spice
* 100 MHz clock: a ring oscillator, one NAND gate (5 ns) feeding itself back through a 5 ns delay line.
aosc [run clkd] clk nand5
adly clk clkd buf5

* the request bytes: run, iv, ie, i7..i0 (the data change 2 ns after each falling edge)
asrc [run iv ie i7 i6 i5 i4 i3 i2 i1 i0] reqsrc
.model reqsrc d_source(input_file="in.txt")

achip [@IN@] [@OUT@] NULL chipm
.model chipm d_cosim(simulation="@SO@" delay=10.005e-9 queue_size=1024)

* signature comparator: cbad <= (cclr ? 0 : cbad) | (ca != cb), one byte pair per clock
ax7 [ca7 cb7] d7 xor1
...
aor [d7 d6 d5 d4 d3 d2 d1 d0] ne or1
ahold [cbad ~cclr] keep and1
anext [ne keep] cnext or1
aff cnext clk NULL NULL cbad NULL dff1
aack cend clk NULL NULL cack NULL dff1

.control
stop when v(adone) > 0.5
tran 1n 2m 0 9.9n
eprint ot ob7 ob6 ob5 ob4 ob3 ob2 ob1 ob0
.endc
```

`build.sh` fills in the chip's pins in the order Verilator generates them. ngspice-42's `d_cosim` shim mishandles
multi-bit *inputs*, so every chip input is a single-bit pin.

## Cost per request

CPU time per request, measured in the arena container (1 CPU, 2 GB): cgroup `cpu.stat` over 40 serial
requests, sent with curl through the benchmark nginx. It includes nginx and curl.

| Request | Simulations | CPU per request |
|---|---|---|
| `GET /nope` (404) | 1 | 10.9 ms |
| `GET /health` | 2 + sqlite3 | 18.2 ms |
| `GET /posts/:id` | 2 + sqlite3 | 20.6 ms |
| `GET /feed` (20 posts, 5.1 KB) | 2 + sqlite3 | 78.0 ms |
| `POST /posts/:id/like` (JWT + HMAC) | 2 + sqlite3 | 26.4 ms |
| `POST /posts` with a bad signature | 1 | 23.3 ms |
| `POST /posts` | 2 + sqlite3 | 25.7 ms |

Where the time goes in a feed request:
- ~5 ms to start ngspice, per simulation;
- about 9,000 simulated clocks in phase B: 4 KB of rows in, 5.1 KB out twice, because the chip runs the
  body once without output to count Content-Length and once for real;
- a few process starts in the wire.

A whole JWT check, HMAC included, is a few hundred clocks.

## How it got fast enough (ngspice findings)

- **`d_cosim` defaults redo the timestep.** `d_cosim` queues input events and runs the model once per
  accepted analog timestep. If an output changes more than `delay` (default 1 ns) before the end of the
  step, it cuts the step short and redoes it, which happens on every output byte. A pin can also change only
  once per step. Steps capped below one clock (9.9 ns) and `delay` set to one clock make each pin change at
  most once per step, with no redo. A feed's simulation went from 228 ms to ~48 ms.
- **Clock source.** An analog PULSE clock through an ADC bridge cost ~45 µs per clock in analog timepoints.
  The digital ring oscillator costs ~2.5 µs.
- **The comparator round trip.** The chip's outputs reach the board one clock late (`delay`), so the chip
  raises `cend` after the last pair and waits for the board to clock it back on `cack` before reading
  `cbad`.
- **`nodelay` on socat.** Bash writes the response in several pieces, and without `TCP_NODELAY`, Nagle plus
  delayed ACK added ~40 ms per request.

## Results

Local Docker arena on an M5 Mac (linux/arm64). The server container has 1 CPU and 2 GB with no swap, with
the benchmark nginx in front. The load generator is the organizer's mload on other CPUs, at think-scale 1,
with a 60 s ramp. Other sessions' containers were busy on the same host, so treat these as indicative.

| Users | 60 s hold | Requests/s | p95 | p99 | Errors | Server CPU | Server memory |
|---|---|---|---|---|---|---|---|
| 60 | **pass** | 4.9 | 142 ms | 283 ms | 0 | 18% | 34 MB |
| 150 | **pass** | 12.3 | 363 ms | 633 ms | 0 | 64% | 44 MB |
| 250 | fail | 18.8 | 4.4 s | 5.2 s | 0 | 85% (CPU-bound) | 306 MB |

So the edge is somewhere between 150 and 250 users on this host. On the benchmark's x86 droplet it could be
lower. An average user costs ~5 ms of CPU per second (two page loads every ~20 s, and a feed is ~78 ms).

## Files

- `chip/chip.v`, `chip/jscan.v`, `chip/sha256.v`: the chip. `chip/build-chip.sh` is what ngspice's own
  `vlnggen` does, with optimisation on: Verilator → `chip.so`.
- `board/board.cir.in`: the board netlist. `board/gen-board.sh` wires the chip's pins.
- `wire/conn.sh`: one connection.
- `install.sh`, `build.sh`, `start.sh`: as the rules ask. `build.sh` takes ~10–60 s and writes only
  `build/`.
