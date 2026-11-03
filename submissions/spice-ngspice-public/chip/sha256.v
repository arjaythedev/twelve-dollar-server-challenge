/* verilator lint_off WIDTH */
/* verilator lint_off CASEINCOMPLETE */
// SHA-256 compression core, one round per clock (66 clocks per 64-byte block).
// Pulse start with blk (the 16 message words, big-endian) and hin (the chaining value);
// vld pulses when hout = hin + the compressed state is ready.
module sha256c(input clk, input start, input [511:0] blk, input [255:0] hin,
               output reg [255:0] hout, output reg vld);
  reg busy = 0, fin = 0;
  reg [31:0] a, b, c, d, e, f, g, h;
  reg [31:0] w [0:15];
  reg [5:0] t;
  function [31:0] K(input [5:0] i); case (i)
    0:K=32'h428a2f98;1:K=32'h71374491;2:K=32'hb5c0fbcf;3:K=32'he9b5dba5;4:K=32'h3956c25b;5:K=32'h59f111f1;6:K=32'h923f82a4;7:K=32'hab1c5ed5;
    8:K=32'hd807aa98;9:K=32'h12835b01;10:K=32'h243185be;11:K=32'h550c7dc3;12:K=32'h72be5d74;13:K=32'h80deb1fe;14:K=32'h9bdc06a7;15:K=32'hc19bf174;
    16:K=32'he49b69c1;17:K=32'hefbe4786;18:K=32'h0fc19dc6;19:K=32'h240ca1cc;20:K=32'h2de92c6f;21:K=32'h4a7484aa;22:K=32'h5cb0a9dc;23:K=32'h76f988da;
    24:K=32'h983e5152;25:K=32'ha831c66d;26:K=32'hb00327c8;27:K=32'hbf597fc7;28:K=32'hc6e00bf3;29:K=32'hd5a79147;30:K=32'h06ca6351;31:K=32'h14292967;
    32:K=32'h27b70a85;33:K=32'h2e1b2138;34:K=32'h4d2c6dfc;35:K=32'h53380d13;36:K=32'h650a7354;37:K=32'h766a0abb;38:K=32'h81c2c92e;39:K=32'h92722c85;
    40:K=32'ha2bfe8a1;41:K=32'ha81a664b;42:K=32'hc24b8b70;43:K=32'hc76c51a3;44:K=32'hd192e819;45:K=32'hd6990624;46:K=32'hf40e3585;47:K=32'h106aa070;
    48:K=32'h19a4c116;49:K=32'h1e376c08;50:K=32'h2748774c;51:K=32'h34b0bcb5;52:K=32'h391c0cb3;53:K=32'h4ed8aa4a;54:K=32'h5b9cca4f;55:K=32'h682e6ff3;
    56:K=32'h748f82ee;57:K=32'h78a5636f;58:K=32'h84c87814;59:K=32'h8cc70208;60:K=32'h90befffa;61:K=32'ha4506ceb;62:K=32'hbef9a3f7;default:K=32'hc67178f2;
  endcase endfunction
  wire [31:0] w0 = w[0], w1 = w[1], w9 = w[9], w14 = w[14];
  wire [31:0] S1 = {e[5:0], e[31:6]} ^ {e[10:0], e[31:11]} ^ {e[24:0], e[31:25]};
  wire [31:0] ch = (e & f) ^ (~e & g);
  wire [31:0] t1 = h + S1 + ch + K(t) + w0;
  wire [31:0] S0 = {a[1:0], a[31:2]} ^ {a[12:0], a[31:13]} ^ {a[21:0], a[31:22]};
  wire [31:0] mj = (a & b) ^ (a & c) ^ (b & c);
  wire [31:0] t2 = S0 + mj;
  wire [31:0] s0 = {w1[6:0], w1[31:7]} ^ {w1[17:0], w1[31:18]} ^ (w1 >> 3);
  wire [31:0] s1 = {w14[16:0], w14[31:17]} ^ {w14[18:0], w14[31:19]} ^ (w14 >> 10);
  integer i;
  always @(posedge clk) begin
    vld <= 0;
    if (start) begin
      {a, b, c, d, e, f, g, h} <= hin; hout <= hin; t <= 0; busy <= 1;
      for (i = 0; i < 16; i = i + 1) w[i] <= blk[511 - 32 * i -: 32];
    end else if (busy) begin
      h <= g; g <= f; f <= e; e <= d + t1; d <= c; c <= b; b <= a; a <= t1 + t2;
      for (i = 0; i < 15; i = i + 1) w[i] <= w[i + 1];
      w[15] <= w0 + s0 + w9 + s1; t <= t + 1;
      if (t == 63) begin busy <= 0; fin <= 1; end
    end else if (fin) begin
      fin <= 0; vld <= 1;
      hout <= {hout[255:224] + a, hout[223:192] + b, hout[191:160] + c, hout[159:128] + d,
               hout[127:96] + e, hout[95:64] + f, hout[63:32] + g, hout[31:0] + h};
    end
  end
endmodule

// HMAC-SHA256 with a key of up to 64 bytes (key is the zero-padded key block, first byte in the top bits).
// Pulse go, then stream the message: mv/mb while mready, then mend (while mready). done pulses with mac.
module hmac(input clk, input rst, input [511:0] key, input go,
            input mv, input [7:0] mb, input mend, output mready,
            output reg done, output reg [255:0] mac);
  localparam [255:0] IV = 256'h6a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19;
  localparam IDLE = 0, WAITC = 1, MSG = 2, PAD = 3, KO = 4, OB = 5, FIN = 6;
  reg [2:0] st = IDLE, ret;
  reg [511:0] blk, cblk;
  reg [255:0] chin, H, inner;
  reg [6:0] bc;
  reg [15:0] mlen;
  reg cstart;
  wire [255:0] cout;
  wire cvld;
  sha256c core(.clk(clk), .start(cstart), .blk(cblk), .hin(chin), .hout(cout), .vld(cvld));
  assign mready = st == MSG && bc + {6'b0, mv} < 64;   // counts a byte already on its way
  wire [63:0] bitlen = {45'b0, mlen + 16'd64, 3'b0};
  always @(posedge clk) begin
    cstart <= 0; done <= 0;
    if (rst) st <= IDLE;
    else case (st)
      IDLE: if (go) begin
        cblk <= key ^ {64{8'h36}}; chin <= IV; cstart <= 1; ret <= MSG; st <= WAITC; mlen <= 0;
      end
      WAITC: if (cvld) begin H <= cout; bc <= 0; st <= ret; end
      MSG:
        if (bc == 64) begin cblk <= blk; chin <= H; cstart <= 1; ret <= MSG; st <= WAITC; end
        else if (mv) begin blk <= {blk[503:0], mb}; bc <= bc + 1; mlen <= mlen + 1; end
        else if (mend) begin blk <= {blk[503:0], 8'h80}; bc <= bc + 1; st <= PAD; end
      PAD:
        if (bc == 64) begin cblk <= blk; chin <= H; cstart <= 1; ret <= PAD; st <= WAITC; end
        else if (bc == 56) begin cblk <= {blk[447:0], bitlen}; chin <= H; cstart <= 1; ret <= KO; st <= WAITC; end
        else begin blk <= {blk[503:0], 8'h00}; bc <= bc + 1; end
      KO: begin inner <= H; cblk <= key ^ {64{8'h5c}}; chin <= IV; cstart <= 1; ret <= OB; st <= WAITC; end
      OB: begin cblk <= {inner, 8'h80, 184'b0, 64'd768}; chin <= H; cstart <= 1; ret <= FIN; st <= WAITC; end
      FIN: begin mac <= H; done <= 1; st <= IDLE; end
      default: st <= IDLE;
    endcase
  end
endmodule
