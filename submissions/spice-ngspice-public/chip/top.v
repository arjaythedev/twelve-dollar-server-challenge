/* verilator lint_off WIDTH */
// The chip: its pins, two FIFOs, and the core (chip.v) behind them.
//
// The board clock (clk, 100 MHz) is multiplied 8x on chip: shim.cpp runs cclk for 8 cycles after each
// rising edge of clk, the way a PLL would. The core moves one byte per cclk cycle, so the pins move up
// to 8 bytes per board clock, and each pin changes at most once per board clock (ngspice keeps only one
// value per pin per timestep).
//
// In:  wi carries wn bytes (first byte in bits 63:56) each time wt toggles; ie (end of input) for one
//      board clock after the last word.
// Out: wo carries won bytes each time ot toggles; done goes high once the core is done and the output
//      FIFO and the body buffer are empty. The core writes a response body into the body buffer
//      before its header (it counts the body for Content-Length), so the buffer goes out last.
// The comparator pins (ca, cb, cclr, cend) come straight from the core, which changes them on tick.
module chip(input clk, input cclk,
            input [63:0] wi, input [3:0] wn, input wt, input ie,
            input cbad, input cack,
            input rst,  // not used in here: the shim power-cycles the model on its rising edge
            output reg [63:0] wo, output reg [3:0] won, output reg ot, output reg done,
            output [7:0] ca, output [7:0] cb, output cclr, output cend);
  reg [2:0] ph = 0;                           // the cclk cycle within the board clock
  wire tick = ph == 0;

  reg [7:0] ifo [0:31]; reg [5:0] ih = 0, it = 0;   // input FIFO
  reg [7:0] ofo [0:31]; reg [5:0] oh = 0, ot_ = 0;  // output FIFO
  reg [7:0] bbuf [0:65535]; reg [16:0] bh = 0, bt = 0;  // body buffer
  reg wt_seen = 0, end_pend = 0, cot_seen = 0;

  wire c_iv = ih != it;
  wire c_ie = !c_iv && end_pend;
  wire c_ot, c_osel, c_done; wire [7:0] c_ob;
  core u(.clk(cclk), .tick(tick), .iv(c_iv), .ie(c_ie), .ib(ifo[ih[4:0]]), .cbad(cbad), .cack(cack),
         .ot(c_ot), .ob(c_ob), .osel(c_osel), .done(c_done), .ca(ca), .cb(cb), .cclr(cclr), .cend(cend));

  integer k;
  reg [5:0] n, cnt; reg [16:0] bn;
  always @(posedge cclk) begin
    ph <= ph + 1;
    if (c_iv) ih <= ih + 1;
    if (c_ie) end_pend <= 0;
    // the board's inputs are steady for the whole burst: take this clock's word on the first cycle
    if (tick) begin
      if (wt != wt_seen) begin
        wt_seen <= wt;
        for (k = 0; k < 8; k = k + 1) if (k < wn) ifo[(it + k) & 31] = wi[63 - 8 * k -: 8];
        it <= it + wn;
      end
      if (ie) end_pend <= 1;
    end
    // the core's output bytes queue up ...
    if (c_ot != cot_seen) begin
      cot_seen <= c_ot;
      if (c_osel) begin bbuf[bt[15:0]] <= c_ob; bt <= bt + 1; end
      else begin ofo[ot_[4:0]] <= c_ob; ot_ <= ot_ + 1; end
    end
    // ... and leave on the pins, up to 8 per board clock, on the last cycle
    if (ph == 7) begin
      cnt = ot_ - oh; n = cnt > 8 ? 8 : cnt;
      if (n != 0) begin
        for (k = 0; k < 8; k = k + 1) wo[63 - 8 * k -: 8] <= k < n ? ofo[(oh + k) & 31] : 8'd0;
        won <= n; ot <= ~ot; oh <= oh + n;
      end else if (c_done && bh != bt) begin
        bn = bt - bh > 8 ? 8 : bt - bh;
        for (k = 0; k < 8; k = k + 1) wo[63 - 8 * k -: 8] <= k < bn ? bbuf[(bh + k) & 16'hffff] : 8'd0;
        won <= bn; ot <= ~ot; bh <= bh + bn;
      end else if (c_done) done <= 1;
    end
  end
  initial begin wo = 0; won = 0; ot = 0; done = 0; end
endmodule
