# spice-ngspice-public: the API is a circuit simulated in SPICE

Every request is answered by a circuit simulated in **ngspice**. One ngspice process runs one board for
the whole time the server is up. The board has a request chip (Verilog), a gate-level signature
comparator, a ring-oscillator clock and a network card / storage controller (C). SQLite stores the data.

- **Language:** Verilog (the chip), SPICE netlist (the board), C (the network card / storage controller)
- **Simulator:** ngspice 42 (Ubuntu 24.04 package) with XSPICE. The chip and the network card run as
  `d_cosim` elements; the chip is compiled by Verilator 5.020.
- **Database:** SQLite through the library (3.45, Ubuntu 24.04), one connection
- **Server:** the network card inside ngspice listens on the socket (epoll); no other process
- **Mode:** **behind Nginx** (`bench/nginx.conf`) on `127.0.0.1:3000`. Upstream connections stay open until
  Nginx closes them.

## What runs where

| Part | Where it runs | What it does |
|---|---|---|
| `chip/*.v` (~800 lines of Verilog) | ngspice, as a `d_cosim` element (the Verilog compiled by Verilator and loaded into the simulator) | Parses the raw HTTP request: request line, path, query, and the `Authorization` header (matched case-insensitively). Does the routing and the post id checks. Checks `Bearer`. Splits the JWT and base64url-decodes it. Validates the JSON of the header and payload. Computes **HMAC-SHA256** (a SHA-256 core at one round per clock, with the key from `JWT_SECRET`). Checks `alg`, `exp` (required, against the time it is given), `sub` and `username`. Validates the body JSON fully (a streaming parser with a 64-deep stack). Decodes it, including `\u` escapes and surrogate pairs to UTF-8. Trims it and counts characters. Writes the SQL text, quoting `'`. Then turns SQLite's raw rows into the JSON response, escaping `"`, `\` and control characters. Picks every status code and error. Writes the whole HTTP/1.1 response, Content-Length included. |
| `board/board.cir.in` (SPICE netlist) | ngspice, as XSPICE digital primitives | The 100 MHz clock is a ring oscillator: one NAND gate with a 5 ns delay fed back to itself. **The signature compare is 12 gates** (8 XOR, an 8-input OR and an accumulate-and-clear flip-flop). The chip streams the HMAC tag and the token's signature through it a byte pair per board clock, and its output bit decides whether the token is valid. A voltage source ramping up at 5 ns, through an ADC bridge, is the power-on that starts the clock: the only analog part. |
| `nic/nic.c` (C) | ngspice, as a second `d_cosim` element on the board | The network card and storage controller, in place of the Bash wire of the first version. It owns the listening socket. It frames each request: the lines up to the blank line, then `Content-Length` bytes. It clocks the bytes into the chip's pins, 8 a clock, and reads the chip's output pins back into bytes. It runs the SQL the chip wrote on SQLite and gives the chip the rows as `sqlite3 -ascii` prints them, plus the error text and an exit code. It writes the chip's response to the socket. Its only branch on content is the first byte of the chip's answer: `!` means the chip answered on its own, `?` means run this SQL first. Between jobs it pulses the chip's `rst` pin. |
| SQLite | inside the same process (the library) | Runs the statements. |

The circuit's outputs decide every response. A request that needs the database is two jobs for the chip
(phase A: request → SQL; phase B: SQLite's rows → HTTP response). Errors (401, 400, 404) come from phase A
alone. The chip is power-cycled between jobs, so it keeps nothing from one request to the next.

What isn't gates: the chip is RTL that Verilator turns into C++, which ngspice runs as a `d_cosim` circuit
element on its own pins. I first tried the whole thing as Yosys-synthesised gates (4-LUTs and flip-flops) in
XSPICE. That works, but the SHA-256 core alone (3,856 elements) took ~4 ms per clock in ngspice, so one HMAC
would take ~1.7 s, and parsing the netlist took 5.7 s. So the gate-level part is the comparator and the
clock, and the rest of the chip is Verilog.

What runs faster than the board: the chip has a clock multiplier. After each rising edge of the board's
100 MHz clock, the shim (`chip/shim.cpp`) runs the chip's core clock for 8 cycles, the way an on-chip PLL
would. The core still handles one byte per cycle. The pins carry 64-bit words, up to 8 bytes per board
clock, and change at most once per board clock (ngspice keeps one value per pin per timestep). The
comparator gates are clocked by the board, so the core hands them one byte pair per board clock.

## The board (excerpt of `board/board.cir.in`)

```spice
* 100 MHz clock: a ring oscillator, one NAND gate with a 5 ns delay feeding itself back
aosc [run clk] clk nand5
.model nand5 d_nand(rise_delay=5e-9 fall_delay=5e-9)
vdd vdd 0 pwl(0 0 5n 0 6n 1)
apwr [vdd] [run] pwradc

* the network card and storage controller (nic.so): 8 request bytes a clock in, 8 response bytes out
anic [clk ot @won[3:0]@ @wo[63:0]@ done] [wt @wn[3:0]@ @wi[63:0]@ ie rst] NULL nicm
.model nicm d_cosim(simulation="@NIC@" delay=2e-9 queue_size=1024 irreversible=2)

* the chip (Verilog, 8 core cycles per board clock)
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
esave none
repeat
  tran 1n 2m 0 9.9n
  destroy all
  reset
  esave none
end
.endc
```

`build.sh` fills in the chip's pins in the order Verilator generates them, and `@bus[msb:lsb]@` with one
node per bit.

## Cost per request

CPU time of the ngspice process per request (`/proc/<pid>/stat`), 50 serial requests each, straight to
`127.0.0.1:3000` in a linux/arm64 container on an M5 Mac:

| Request | Chip jobs | CPU per request (first version) | CPU per request (now) |
|---|---|---|---|
| `GET /nope` (404) | 1 | 10.9 ms | 1.0 ms |
| `GET /health` | 2 + SQLite | 18.2 ms | 1.8 ms |
| `GET /posts/:id` | 2 + SQLite | 20.6 ms | 3.0 ms |
| `GET /feed` (20 posts, 5.1 KB) | 2 + SQLite | 78.0 ms | 23.2 ms |
| `POST /posts/:id/like` (JWT + HMAC) | 2 + SQLite | 26.4 ms | 3.6 ms |
| `POST /posts` with a bad signature | 1 | 23.3 ms | 2.8 ms |
| `POST /posts` | 2 + SQLite | 25.7 ms | 4.5 ms |

(The first version's numbers are for the whole container, through nginx; they include nginx and curl.)

A feed's phase B is ~1,800 board clocks: 4 KB of rows in at 8 bytes a clock, the body (5.1 KB) built at
one byte per core cycle (counting it for Content-Length), the header, and the body out at 8 bytes a clock.
At that point almost all of the time is ngspice's own event processing per timestep (`EVTload` copying each
`d_cosim` instance's ~140 pins, three calls per clock each); Verilator's code is ~15%.

## How it got fast enough (ngspice findings)

- **One long-running simulation.** The first version started ngspice twice per database request (~8 ms
  each, plus Bash, `od` and `awk`). Now one process runs the board for good. The network card blocks in
  `epoll_wait` inside its `d_cosim` step when it has no request, which stops the simulation (and its clock)
  until one arrives.
- **Saved vectors grow with every timestep** (~20 bytes a clock), and `.save none` refuses to run, so the
  transient runs in 2 ms slices with `destroy all` between them. The network card is linked with
  `-z nodelete`, so its sockets and queue survive `d_cosim` reloading it each slice. Without `reset`
  between slices the ring oscillator stops after one edge in the second slice, and `reset` forgets
  `esave none`: with every event saved, each slice ended in a ~2 s stall.
- **Two `d_cosim` instances with the same `irreversible` value** (the default, 1) crash ngspice 42 in
  `EVTcall_hybrids`; the network card uses 2.
- **Analog loads on fast pins are expensive.** A DAC bridge on a pin that toggles every clock costs ~10 µs
  per clock; the first version's analog "done" node is gone.
- **`d_cosim` defaults redo the timestep.** `d_cosim` runs the model once per accepted analog timestep; if
  an output changes more than `delay` before the end of the step, it cuts the step short and redoes it, and
  a pin can change only once per step. Steps capped below one clock (9.9 ns) and the chip's `delay` set to
  one clock avoid both.
- **Wide pins.** Per byte moved, a 64-bit word per clock costs ~3x less than a byte per clock.

## Results

Local Docker arena on an M5 Mac (linux/arm64). The server container has 1 CPU and 2 GB with no swap, with
the benchmark nginx in front. The load generator is the organizer's mload on other CPUs, at think-scale 1,
with a 60 s ramp. Other sessions' containers were busy on the same host, so treat these as indicative.

| Version | Users | 60 s hold | Requests/s | p50 | p95 | p99 | Errors | Server CPU | Server memory |
|---|---|---|---|---|---|---|---|---|---|
| first (Bash + ngspice per job) | 150 | **pass** | 12.3 | | 363 ms | 633 ms | 0 | 64% | 44 MB |
| first | 250 | fail | 18.8 | | 4.4 s | 5.2 s | 0 | 85% (CPU-bound) | 306 MB |
| now (one long-running board) | 500 | fail | 41.6 | 12 ms | 665 ms | 1.23 s | 0.04% | 40% | 36 MB |

At 500 users the median is 12 ms, and the server is busy well under half the time; the tail comes from rare
stalls of up to ~1.5 s (the server at 100% for a moment). I am still chasing those, and three responses in
that run were malformed (nginx: "upstream sent no valid HTTP/1.0 header"), which looks like the same fault.
An average user costs ~0.6 ms of CPU per second now (a feed is ~22 ms, the rest 1–4 ms).

## Files

- `chip/top.v`, `chip/chip.v`, `chip/jscan.v`, `chip/sha256.v`: the chip. `chip/shim.cpp`: the glue to
  `d_cosim` (pins, the clock multiplier, power-cycle on `rst`). `chip/build-chip.sh` is what ngspice's own
  `vlnggen` does, with optimisation on and our shim: Verilator → `chip.so`.
- `nic/nic.c`: the network card and storage controller.
- `board/board.cir.in`: the board netlist. `board/gen-board.sh` wires the chip's pins.
- `install.sh`, `build.sh`, `start.sh`: as the rules ask. `build.sh` takes ~10–60 s and writes only
  `build/`.
