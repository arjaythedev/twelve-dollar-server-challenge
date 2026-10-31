/* verilator lint_off WIDTH */
/* verilator lint_off CASEINCOMPLETE */
// jscan: a streaming JSON validator and decoder, one input byte per clock.
//
// It checks the whole grammar (objects, arrays, strings with escapes, numbers, true/false/null,
// nesting up to 64 deep) and reports events for the members of a top-level object, so the parent
// can pick out the keys it cares about without buffering anything:
//   kb_v/kb  a decoded byte of a depth-1 key       kend  that key ended
//   vstart   a depth-1 member value starts, vtype: 0 string, 1 number, 2 anything else
//   sn/sb    decoded bytes of a depth-1 string value (0-4 bytes per clock, sb[7:0] first)
//   nb_v/nb  a raw character of a depth-1 number    vend  a depth-1 string or number ended
// The event outputs are combinational on (state, input) and valid in the cycle the byte is taken.
// Pulse eof (with v low) once after the last byte; err is then final.
module jscan(input clk, input rst, input v, input [7:0] c, input eof,
             output reg err, output reg top_obj,
             output reg kb_v, output reg [7:0] kb, output reg kend,
             output reg vstart, output reg [1:0] vtype,
             output reg [2:0] sn, output reg [31:0] sb,
             output reg nb_v, output reg [7:0] nb, output reg vend);
  localparam VAL = 0, ARR0 = 1, OBJ0 = 2, OBJK = 3, COLON = 4, AFTER = 5, STR = 6, ESC = 7, UHEX = 8,
             NUM = 9, LIT = 10;
  // number sub-states
  localparam N_MINUS = 0, N_ZERO = 1, N_INT = 2, N_DOT = 3, N_FRAC = 4, N_E = 5, N_ESIGN = 6, N_EXP = 7;

  reg [3:0] st, nst;
  reg [6:0] depth, ndepth;
  reg [63:0] stk, nstk;          // 1 = object, 0 = array; stk[0] is the innermost
  reg is_key, nis_key;           // the string being read is a key
  reg d1, nd1;                   // the string/number being read is a depth-1 member key or value
  reg [2:0] ns, nns;             // number sub-state
  reg [1:0] lit, nlit;           // 0 true, 1 false, 2 null
  reg [2:0] lpos, nlpos;
  reg [1:0] uc, nuc;             // hex digits seen in \uXXXX
  reg [15:0] ux, nux;
  reg [15:0] hi, nhi;            // pending high surrogate (0 = none)
  reg nerr, ntop;

  function ws(input [7:0] x); ws = x == 8'h20 || x == 8'h09 || x == 8'h0a || x == 8'h0d; endfunction
  function digit(input [7:0] x); digit = x >= "0" && x <= "9"; endfunction
  function [4:0] hexv(input [7:0] x);  // bit 4 = invalid
    if (x >= "0" && x <= "9") hexv = {1'b0, x[3:0]};
    else if ((x >= "a" && x <= "f") || (x >= "A" && x <= "F")) hexv = {1'b0, x[3:0] + 4'd9};
    else hexv = 5'h10;
  endfunction
  function [7:0] litc(input [1:0] l, input [2:0] p);
    case (l) 0: litc = "true" >> (8 * (3 - p)); 1: litc = "false" >> (8 * (4 - p)); default: litc = "null" >> (8 * (3 - p)); endcase
  endfunction
  function [2:0] litn(input [1:0] l); litn = l == 1 ? 5 : 4; endfunction
  function numend(input [2:0] s); numend = s == N_ZERO || s == N_INT || s == N_FRAC || s == N_EXP; endfunction
  function numc(input [7:0] x); numc = digit(x) || x == "." || x == "e" || x == "E" || x == "+" || x == "-"; endfunction

  // the member-of-top-level-object test for a value or key about to start at the current depth
  wire member = depth == 1 && stk[0];

  reg [3:0] s;   // effective state for this byte (a number ends on the first non-number byte)
  reg [20:0] cp;
  wire [4:0] hv = hexv(c);
  always @* begin
    nst = st; ndepth = depth; nstk = stk; nis_key = is_key; nd1 = d1; nns = ns; nlit = lit; nlpos = lpos;
    nuc = uc; nux = ux; nhi = hi; nerr = err; ntop = top_obj;
    kb_v = 0; kb = c; kend = 0; vstart = 0; vtype = 0; sn = 0; sb = 0; nb_v = 0; nb = c; vend = 0; cp = 0;
    s = st;
    if ((v || eof) && st == NUM && !(v && numc(c))) begin
      if (!numend(ns)) nerr = 1;
      if (d1) vend = 1;
      s = AFTER; nst = AFTER;
    end
    if (eof) begin
      if (!(s == AFTER && depth == 0)) nerr = 1;
    end else if (v && !err) begin
      case (s)
        VAL, ARR0: begin
          if (ws(c)) ;
          else if (s == ARR0 && c == "]") begin nstk = stk >> 1; ndepth = depth - 1; nst = AFTER; end
          else if (c == "{" || c == "[") begin
            if (depth == 0 && c == "{") ntop = 1;
            if (member) begin vstart = 1; vtype = 2; end
            if (depth == 63) nerr = 1;
            nstk = {stk[62:0], c == "{"}; ndepth = depth + 1; nst = c == "{" ? OBJ0 : ARR0;
          end else if (c == "\"") begin
            if (member) begin vstart = 1; vtype = 0; end
            nd1 = member; nis_key = 0; nhi = 0; nst = STR;
          end else if (c == "-" || digit(c)) begin
            if (member) begin vstart = 1; vtype = 1; nb_v = 1; end
            nd1 = member; nns = c == "-" ? N_MINUS : c == "0" ? N_ZERO : N_INT; nst = NUM;
          end else if (c == "t" || c == "f" || c == "n") begin
            if (member) begin vstart = 1; vtype = 2; end
            nlit = c == "t" ? 0 : c == "f" ? 1 : 2; nlpos = 1; nst = LIT;
          end else nerr = 1;
        end
        OBJ0, OBJK: begin
          if (ws(c)) ;
          else if (s == OBJ0 && c == "}") begin nstk = stk >> 1; ndepth = depth - 1; nst = AFTER; end
          else if (c == "\"") begin nis_key = 1; nd1 = depth == 1; nhi = 0; nst = STR; end
          else nerr = 1;
        end
        COLON: if (ws(c)) ; else if (c == ":") nst = VAL; else nerr = 1;
        AFTER: begin
          if (ws(c)) ;
          else if (depth == 0) nerr = 1;
          else if (c == ",") nst = stk[0] ? OBJK : VAL;
          else if (c == (stk[0] ? "}" : "]")) begin nstk = stk >> 1; ndepth = depth - 1; end
          else nerr = 1;
        end
        STR: begin
          if (c == "\"") begin
            nst = is_key ? COLON : AFTER;
            if (d1) begin if (is_key) kend = 1; else vend = 1; end
          end else if (c == "\\") nst = ESC;
          else if (c < 8'h20) nerr = 1;
          else if (d1) begin
            if (is_key) kb_v = 1; else begin sn = 1; sb = {24'b0, c}; end
          end
          if (c != "\\") nhi = 0;   // a high surrogate not followed by \u is dropped
        end
        ESC: begin
          nst = STR; kb = c;
          case (c)
            "\"", "\\", "/": kb = c;
            "b": kb = 8'h08; "f": kb = 8'h0c; "n": kb = 8'h0a; "r": kb = 8'h0d; "t": kb = 8'h09;
            "u": begin nst = UHEX; nuc = 0; nux = 0; end
            default: nerr = 1;
          endcase
          if (c != "u") begin
            nhi = 0;
            if (d1) begin if (is_key) kb_v = 1; else begin sn = 1; sb = {24'b0, kb}; end end
          end
        end
        UHEX: begin
          if (hv[4]) nerr = 1;
          nux = {ux[11:0], hv[3:0]}; nuc = uc + 1;
          if (uc == 3) begin
            nst = STR;
            if (nux >= 16'hd800 && nux < 16'hdc00) nhi = nux;
            else begin
              nhi = 0;
              if (hi != 0 && nux >= 16'hdc00 && nux < 16'he000)
                cp = 21'h10000 + {1'b0, hi[9:0], nux[9:0]};
              else cp = {5'b0, nux};
              if (d1 && !is_key) begin
                if (cp < 21'h80) begin sn = 1; sb = {24'b0, cp[7:0]}; end
                else if (cp < 21'h800) begin sn = 2; sb = {16'b0, 2'b10, cp[5:0], 3'b110, cp[10:6]}; end
                else if (cp < 21'h10000) begin sn = 3; sb = {8'b0, 2'b10, cp[5:0], 2'b10, cp[11:6], 4'b1110, cp[15:12]}; end
                else begin sn = 4; sb = {2'b10, cp[5:0], 2'b10, cp[11:6], 2'b10, cp[17:12], 5'b11110, cp[20:18]}; end
              end else if (d1 && is_key) begin kb_v = cp < 21'h80; kb = cp[7:0]; end
            end
          end
        end
        NUM: begin
          if (d1) nb_v = 1;
          case (ns)
            N_MINUS: if (c == "0") nns = N_ZERO; else if (digit(c)) nns = N_INT; else nerr = 1;
            N_ZERO, N_INT: if (digit(c) && ns == N_INT) ; else if (c == ".") nns = N_DOT;
                           else if (c == "e" || c == "E") nns = N_E; else nerr = 1;
            N_DOT, N_FRAC: if (digit(c)) nns = N_FRAC; else if (ns == N_FRAC && (c == "e" || c == "E")) nns = N_E; else nerr = 1;
            N_E: if (c == "+" || c == "-") nns = N_ESIGN; else if (digit(c)) nns = N_EXP; else nerr = 1;
            N_ESIGN, N_EXP: if (digit(c)) nns = N_EXP; else nerr = 1;
          endcase
        end
        LIT: begin
          if (c != litc(lit, lpos)) nerr = 1;
          nlpos = lpos + 1;
          if (lpos + 1 == litn(lit)) nst = AFTER;
        end
        default: nerr = 1;
      endcase
    end
  end

  always @(posedge clk) begin
    if (rst) begin
      st <= VAL; depth <= 0; stk <= 0; is_key <= 0; d1 <= 0; ns <= 0; lit <= 0; lpos <= 0;
      uc <= 0; ux <= 0; hi <= 0; err <= 0; top_obj <= 0;
    end else begin
      st <= nst; depth <= ndepth; stk <= nstk; is_key <= nis_key; d1 <= nd1; ns <= nns; lit <= nlit;
      lpos <= nlpos; uc <= nuc; ux <= nux; hi <= nhi; err <= nerr; top_obj <= ntop;
    end
  end
endmodule
