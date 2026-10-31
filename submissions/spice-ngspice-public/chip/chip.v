/* verilator lint_off WIDTH */
/* verilator lint_off CASEINCOMPLETE */
// The request chip. ngspice clocks it as a d_cosim element (Verilator); see ../board/board.cir.
//
// Input: one byte per clock on i7..i0 while iv is high, then ie (end of input) for one clock.
// Two kinds of job, picked by the first line:
//   phase A  "A\n<now>\n<boot>\n<secret>\n<the HTTP request as received: request line, headers, body>"
//            -> "!<HTTP response>"                (an error decided without the database), or
//            -> "?<kind line>\n<SQL>"             (the statements SQLite should run)
//   phase B  "B\n<kind line>\n<sqlite3 exit code>\n<sqlite3 -ascii stdout>\x1d<stderr>"
//            -> "<HTTP response>"
// An HTTP response is emitted twice over: a dry run of the body program counts its bytes for
// Content-Length, then the header goes out, then the body program runs again for real.
// Output: one byte per change of ot, on ob; done goes high when the job is finished.
// The HMAC tag and the token's signature leave the chip on ca/cb (cclr marks the first byte) for
// the gate-level comparator on the board, whose answer comes back on cbad. ngspice delivers the
// chip's outputs a clock late (d_cosim's delay), so the chip raises cend after the last pair and
// waits for the board to clock it back on cack before it reads cbad.
module chip(input clk, input iv, input ie,
            input i7, input i6, input i5, input i4, input i3, input i2, input i1, input i0,
            input cbad, input cack,
            output reg ot, output reg [7:0] ob, output reg done,
            output reg [7:0] ca, output reg [7:0] cb, output reg cclr, output reg cend);
  wire [7:0] ib = {i7, i6, i5, i4, i3, i2, i1, i0};

  // ---------------------------------------------------------------- strings and programs
  // Program bytes: printable text and CR/LF are emitted as is; 0x00 ends; others are ops:
  //   01 the arg digits (phase A: post id, phase B: the kind line's argument)   02 the token's user id
  //   03 the post body, SQL-quoted   04 the token's username, SQL-quoted      05 the uptime in seconds
  //   06 sqlite3's stderr, JSON-escaped   07 the result rows, each through ROW
  //   ROW only: 11 the next field as is, 12 the next field JSON-escaped
  localparam CTCL = "\015\012Content-Type: application/json\015\012Content-Length: \005\015\012\015\012";
  localparam POSTSQL = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes l WHERE l.post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id";
  // response bodies
  localparam E_NF  = "{\"error\":\"not found\"}";
  localparam E_ID  = "{\"error\":\"invalid post id\"}";
  localparam E_MIS = "{\"error\":\"missing bearer token\"}";
  localparam E_INV = "{\"error\":\"invalid or expired token\"}";
  localparam E_PAY = "{\"error\":\"invalid token payload\"}";
  localparam E_MAL = "{\"error\":\"malformed JSON body\"}";
  localparam E_REQ = "{\"error\":\"body is required\"}";
  localparam E_LNG = "{\"error\":\"body must be at most 500 characters\"}";
  localparam B_H200 = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":\001}";
  localparam B_H503 = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"\006\"}";
  localparam B_F    = "{\"posts\":[\007]}";
  localparam B_P    = "{\"post\":\007}";
  localparam B_L201 = "{\"liked\":true,\"already_liked\":false,\"post_id\":\001}";
  localparam B_L200 = "{\"liked\":true,\"already_liked\":true,\"post_id\":\001}";
  localparam B_PNF  = "{\"error\":\"post not found\"}";
  localparam B_500  = "{\"error\":\"internal server error\"}";
  localparam ROW    = "{\"id\":\021,\"body\":\"\022\",\"created_at\":\"\022\",\"author\":\"\022\",\"like_count\":\021}";
  // SQL jobs (phase A, no HTTP header)
  localparam A_H = "?H\005\012SELECT 1;";
  localparam A_F = {"?F\012", POSTSQL, " ORDER BY p.created_at DESC,p.id DESC LIMIT 20;"};
  localparam A_P = {"?P\001\012", POSTSQL, " WHERE p.id=\001;"};
  localparam A_C = "?C\012INSERT INTO posts(user_id,body) VALUES(\002,'\003') RETURNING id,body,created_at,'\004',0;";
  localparam A_L = {"?L\001\012INSERT INTO likes(user_id,post_id) SELECT \002,\001 WHERE EXISTS(SELECT 1 FROM posts WHERE id=\001)",
                    " ON CONFLICT DO NOTHING;SELECT EXISTS(SELECT 1 FROM posts WHERE id=\001),changes();"};
  // HTTP headers ('!' marks a phase A answer)
  localparam HA_400 = {"!HTTP/1.1 400 Bad Request", CTCL}, HA_401 = {"!HTTP/1.1 401 Unauthorized", CTCL},
             HA_404 = {"!HTTP/1.1 404 Not Found", CTCL}, HB_200 = {"HTTP/1.1 200 OK", CTCL},
             HB_201 = {"HTTP/1.1 201 Created", CTCL}, HB_404 = {"HTTP/1.1 404 Not Found", CTCL},
             HB_500 = {"HTTP/1.1 500 Internal Server Error", CTCL}, HB_503 = {"HTTP/1.1 503 Service Unavailable", CTCL};
  localparam PNF = 0, PID = 1, PMIS = 2, PINV = 3, PPAY = 4, PMAL = 5, PREQ = 6, PLNG = 7,
             PH = 8, PF = 9, PP = 10, PC = 11, PL = 12, BH200 = 13, BH503 = 14, BF = 15, BP = 16,
             BL201 = 17, BL200 = 18, BPNF = 19, B500 = 20, PROW = 21,
             H400 = 22, H401 = 23, H404 = 24, H200 = 25, H201 = 26, HB404 = 27, H500 = 28, H503 = 29;
  `define R(n, s) n: rom = i < $bits(s) / 8 ? s[$bits(s) - 1 - 8 * i -: 8] : 8'd0;
  function [7:0] rom(input [4:0] p, input [10:0] i);
    case (p)
      `R(PNF, E_NF) `R(PID, E_ID) `R(PMIS, E_MIS) `R(PINV, E_INV) `R(PPAY, E_PAY) `R(PMAL, E_MAL) `R(PREQ, E_REQ)
      `R(PLNG, E_LNG) `R(PH, A_H) `R(PF, A_F) `R(PP, A_P) `R(PC, A_C) `R(PL, A_L) `R(BH200, B_H200)
      `R(BH503, B_H503) `R(BF, B_F) `R(BP, B_P) `R(BL201, B_L201) `R(BL200, B_L200) `R(BPNF, B_PNF) `R(B500, B_500)
      `R(PROW, ROW) `R(H400, HA_400) `R(H401, HA_401) `R(H404, HA_404) `R(H200, HB_200) `R(H201, HB_201)
      `R(HB404, HB_404) `R(H500, HB_500) `R(H503, HB_503)
      default: rom = 0;
    endcase
  endfunction
  // literal compare helpers: char i of a short string
  function [7:0] chr(input [127:0] s, input [4:0] n, input [13:0] i); chr = i < n ? s[8 * (n - 1 - i) +: 8] : 8'hff; endfunction
  function [7:0] lc(input [7:0] x); lc = x >= "A" && x <= "Z" ? x + 8'd32 : x; endfunction
  function isdig(input [7:0] x); isdig = x >= "0" && x <= "9"; endfunction
  function isws(input [7:0] x); isws = x == 8'h20 || (x >= 8'h09 && x <= 8'h0d); endfunction
  function [6:0] b64v(input [7:0] x);  // bit 6 = invalid
    if (x >= "A" && x <= "Z") b64v = x - "A";
    else if (x >= "a" && x <= "z") b64v = x - "a" + 26;
    else if (x >= "0" && x <= "9") b64v = x - "0" + 52;
    else if (x == "-") b64v = 62; else if (x == "_") b64v = 63; else b64v = 7'h40;
  endfunction

  // ---------------------------------------------------------------- storage
  reg [7:0] mem [0:16383];      // the whole input
  reg [7:0] bmem [0:4095];      // the decoded post body
  reg [7:0] umem [0:63];        // the decoded username
  reg [3:0] subd [0:19];        // the user id digits
  reg [3:0] ud [0:15];          // uptime digits, least significant first
  reg [13:0] len = 0, fpos = 0;
  reg [3:0] fi = 0;
  reg phb = 0;

  // phase A fields
  reg [31:0] mth = 0; reg is_get = 0, is_post = 0;
  reg h_ok = 1, f_ok = 1, p_ok = 1, seg = 0, tail = 0, t_ok = 0, id_any = 0, id_nz = 0, id_bad = 0;
  reg [3:0] tpos = 0; reg [4:0] idn = 0; reg [13:0] ulen = 0;
  reg [13:0] a_s = 0, a_e = 0;  // arg digits range in mem
  reg [39:0] now = 0, boot = 0;
  reg [511:0] key = 0; reg [6:0] klen = 0;
  reg br_ok = 0, has_auth = 0, is_auth = 0, an_ok = 1, pcr = 0; reg [3:0] hs = 0; reg [13:0] hpos = 0, apos = 0;
  reg [13:0] tok_s = 0, tok_e = 0, body_s = 0, body_e = 0;
  // phase B fields
  reg [7:0] kind = 0; reg rc0 = 0; reg [13:0] rcn = 0, out_s = 0, out_e = 0, err_s = 0;

  // ---------------------------------------------------------------- the scanners and the MAC
  localparam LOAD = 0, ROUTE = 1, TOK = 2, TEND = 3, TCMP = 4, TVERD = 5, BODY = 6, BEOF = 7, BVERD = 8,
             UPT = 9, EMIT = 10, FIN = 11, BJOB = 12, TACK = 13;
  reg [3:0] st = LOAD;
  reg [2:0] route = 0;  // 0 404, 1 health, 2 feed, 3 get post, 4 create, 5 like
  wire jrst = st == LOAD;

  reg hv = 0, he = 0, pv = 0, pe = 0, yv = 0, ye = 0; reg [7:0] hc = 0, pc_ = 0, yc = 0;
  wire h_err, h_top, h_kbv, h_kend, h_vs, h_nbv, h_ve; wire [7:0] h_kb, h_nb; wire [1:0] h_vt; wire [2:0] h_sn; wire [31:0] h_sb;
  wire p_err, p_top, p_kbv, p_kend, p_vs, p_nbv, p_ve; wire [7:0] p_kb, p_nb; wire [1:0] p_vt; wire [2:0] p_sn; wire [31:0] p_sb;
  wire y_err, y_top, y_kbv, y_kend, y_vs, y_nbv, y_ve; wire [7:0] y_kb, y_nb; wire [1:0] y_vt; wire [2:0] y_sn; wire [31:0] y_sb;
  jscan jh(.clk(clk), .rst(jrst), .v(hv), .c(hc), .eof(he), .err(h_err), .top_obj(h_top), .kb_v(h_kbv), .kb(h_kb), .kend(h_kend),
           .vstart(h_vs), .vtype(h_vt), .sn(h_sn), .sb(h_sb), .nb_v(h_nbv), .nb(h_nb), .vend(h_ve));
  jscan jp(.clk(clk), .rst(jrst), .v(pv), .c(pc_), .eof(pe), .err(p_err), .top_obj(p_top), .kb_v(p_kbv), .kb(p_kb), .kend(p_kend),
           .vstart(p_vs), .vtype(p_vt), .sn(p_sn), .sb(p_sb), .nb_v(p_nbv), .nb(p_nb), .vend(p_ve));
  jscan jy(.clk(clk), .rst(jrst), .v(yv), .c(yc), .eof(ye), .err(y_err), .top_obj(y_top), .kb_v(y_kbv), .kb(y_kb), .kend(y_kend),
           .vstart(y_vs), .vtype(y_vt), .sn(y_sn), .sb(y_sb), .nb_v(y_nbv), .nb(y_nb), .vend(y_ve));

  reg mgo = 0, mv = 0, mend = 0; reg [7:0] mb = 0;
  wire mready, mdone; wire [255:0] mac;
  reg [255:0] macr = 0;
  reg mac_ok = 0;
  hmac hm(.clk(clk), .rst(jrst), .key(key), .go(mgo), .mv(mv), .mb(mb), .mend(mend), .mready(mready), .done(mdone), .mac(mac));

  // token pass
  reg [13:0] ptr = 0; reg [1:0] part = 0; reg tbad = 0;
  reg [11:0] bacc = 0; reg [3:0] bn = 0;
  reg [255:0] sig = 0; reg [5:0] sign = 0; reg [6:0] sigc = 0;
  reg [5:0] cmi = 0;
  // header: "alg" == "HS256"
  reg [2:0] hk = 0; reg hk_ok = 1, alg_key = 0, alg_cap = 0, alg_ok = 0; reg [2:0] av = 0; reg av_ok = 1;
  // payload keys: 1 sub, 2 username, 3 exp
  reg [3:0] pk = 0; reg pk_s = 1, pk_u = 1, pk_e = 1; reg [1:0] pkey = 0, pcap = 0;
  reg sub_has = 0, sub_str = 0, sub_bad = 0, sub_nz = 0; reg [4:0] subn = 0;
  reg u_has = 0, u_str = 0, u_long = 0; reg [6:0] un = 0;
  reg e_has = 0, e_num = 0, e_bad = 0; reg [39:0] expv = 0;
  // body
  reg [2:0] yk = 0; reg yk_ok = 1, body_key = 0, bcap = 0;
  reg b_has = 0, b_str = 0, b_started = 0, b_over = 0;
  reg [12:0] bw = 0, b_s = 0, b_e = 0; reg [12:0] bcnt = 0, becnt = 0;

  // emitter
  reg [4:0] prog = 0, sprog = 0; reg [10:0] pc = 0, spc = 0; reg in_row = 0;
  reg [3:0] op = 0;               // the op being run (0: none)
  reg [13:0] rp = 0, rq = 0;      // op source pointer and end
  reg [47:0] pb = 0; reg [2:0] pn = 0;   // pending escaped bytes
  reg [39:0] upt = 0; reg [4:0] udn = 0;
  reg [4:0] bprog = 0, hprog = 0; reg [1:0] pass = 2; reg [15:0] clen = 0; reg conv_hdr = 0;
  // start a response: body program b, HTTP header program h (0: none, a phase A SQL job)
  task resp(input [4:0] b, input [4:0] h);
    begin bprog <= b; hprog <= h; prog <= b; pc <= 0; pass <= h == 0 ? 2 : 0; clen <= 0; st <= EMIT; end
  endtask

  integer k;
  reg [7:0] x, oc; reg [12:0] w; reg st_, emit_;
  reg [6:0] bv; reg [11:0] t12; reg [12:0] cnt;
  wire tok_inv = tbad || part != 2 || cbad || sign != 32 || sigc != 43 || h_err || !h_top || !alg_ok || p_err || !p_top ||
                 (!e_has || !e_num || e_bad || expv <= now);
  wire pay_inv = !(sub_has && sub_str && !sub_bad && sub_nz && u_has && u_str && !u_long);

  always @(posedge clk) begin
    hv <= 0; he <= 0; pv <= 0; pe <= 0; yv <= 0; ye <= 0; mgo <= 0; mv <= 0; mend <= 0; cclr <= 0; cend <= 0;
    case (st)
    // ------------------------------------------------------------ load the input, parse fields on the fly
    LOAD: begin
      if (iv) begin
        mem[len] <= ib; len <= len + 1; fpos <= fpos + 1;
        if (fi == 0) begin
          if (ib == "\n") begin fi <= 1; fpos <= 0; end else phb <= ib == "B";
        end else if (!phb) begin
          if (fi < 4 && ib == "\n") begin fi <= fi + 1; fpos <= 0; end
          else case (fi)
            1: if (isdig(ib)) now <= now * 10 + {36'b0, ib[3:0]};
            2: if (isdig(ib)) boot <= boot * 10 + {36'b0, ib[3:0]};
            3: if (klen < 64) begin key[511 - 8 * klen -: 8] <= ib; klen <= klen + 1; end
            4: begin   // the HTTP request
              pcr <= ib == "\r";
              case (hs)
                0: if (ib == " ") begin   // method
                     is_get <= hpos == 3 && mth[23:0] == "GET"; is_post <= hpos == 4 && mth == "POST"; hs <= 1; hpos <= 0;
                   end else begin mth <= {mth[23:0], ib}; hpos <= hpos + 1; end
                1: if (ib == " " || ib == "?") begin ulen <= hpos; hs <= ib == " " ? 3 : 2; end   // path
                   else begin
                     hpos <= hpos + 1;
                     h_ok <= h_ok && ib == chr("/health", 7, hpos);
                     f_ok <= f_ok && ib == chr("/feed", 5, hpos);
                     if (hpos < 6) p_ok <= p_ok && ib == chr("/posts", 6, hpos);
                     else if (hpos == 6) begin if (ib == "/") begin seg <= 1; a_s <= len + 1; a_e <= len + 1; end else p_ok <= 0; end
                     else if (seg && !tail) begin
                       if (ib == "/") begin tail <= 1; tpos <= 1; t_ok <= 1; end
                       else begin
                         id_any <= 1;
                         if (!isdig(ib)) id_bad <= 1;
                         else if (ib == "0" && !id_nz) a_s <= len + 1;
                         else begin id_nz <= 1; idn <= idn + 1; if (idn == 18) id_bad <= 1; end
                         a_e <= len + 1;
                       end
                     end else if (tail) begin t_ok <= t_ok && ib == chr("/like", 5, {10'b0, tpos}); tpos <= tpos + 1; end
                   end
                2: if (ib == " ") hs <= 3;                                   // query string
                3: if (ib == "\n") begin hs <= 4; hpos <= 0; an_ok <= 1; end   // "HTTP/1.1"
                4: if (hpos == 0 && ib == "\r") hs <= 7;                     // a header name, or the blank line
                   else if (hpos == 0 && ib == "\n") begin hs <= 8; body_s <= len + 1; end
                   else if (ib == ":") begin
                     hs <= 5; is_auth <= an_ok && hpos == 13;
                     if (an_ok && hpos == 13) begin has_auth <= 1; br_ok <= 1; apos <= 0; tok_s <= 14'h3fff; end
                   end else begin an_ok <= an_ok && lc(ib) == chr("authorization", 13, hpos); hpos <= hpos + 1; end
                5, 6: if (hs == 5 && (ib == " " || ib == "\t")) ;             // a header value
                   else if (ib == "\n") begin
                     if (is_auth) tok_e <= pcr ? len - 1 : len;
                     hs <= 4; hpos <= 0; an_ok <= 1; is_auth <= 0;
                   end else begin
                     hs <= 6;
                     if (is_auth) begin
                       if (apos < 7) br_ok <= br_ok && ib == chr("Bearer ", 7, apos);
                       if (apos == 0) tok_s <= len + 7;
                       apos <= apos + 1;
                     end
                   end
                7: if (ib == "\n") begin hs <= 8; body_s <= len + 1; end
                default: ;                                                  // the body
              endcase
            end
            default: ;
          endcase
        end else begin
          if ((fi < 3 && ib == "\n") || (fi == 3 && ib == 8'h1d)) begin
            fi <= fi + 1; fpos <= 0;
            case (fi)
              1: a_e <= len;
              2: begin rcn <= fpos; out_s <= len + 1; end
              3: begin out_e <= len; err_s <= len + 1; end
              default: ;
            endcase
          end else if (fi == 1) begin if (fpos == 0) begin kind <= ib; a_s <= len + 1; end end
          else if (fi == 2) begin if (fpos == 0) rc0 <= ib == "0"; end
        end
      end
      if (ie) begin
        if (!phb && hs != 8) body_s <= len;
        if (phb && fi < 4) begin out_e <= fi == 3 ? len : out_s; err_s <= len; end
        body_e <= len;
        st <= phb ? BJOB : ROUTE;
      end
    end
    // ------------------------------------------------------------ phase A: route
    ROUTE: begin
      if (is_get && h_ok && ulen == 7) begin upt <= now > boot ? now - boot : 0; udn <= 0; st <= UPT; end
      else if (is_get && f_ok && ulen == 5) resp(PF, 0);
      else if (is_post && p_ok && (ulen == 6 || (tail && t_ok && tpos == 5))) begin
        route <= ulen == 6 ? 4 : 5;
        if (has_auth && br_ok && tok_s <= tok_e) st <= TOK; else resp(PMIS, H401);
      end
      else if (is_get && p_ok && seg && !tail) begin if (id_any && !id_bad && id_nz) resp(PP, 0); else resp(PID, H400); end
      else resp(PNF, H404);
      ptr <= tok_s; part <= 0; bn <= 0; mgo <= 1;
    end
    // binary to decimal (uptime, Content-Length), one digit per clock
    UPT: begin
      ud[udn[3:0]] <= upt % 10; upt <= upt / 10; udn <= udn + 1;
      if (upt < 10) begin
        if (conv_hdr) begin prog <= hprog; pass <= 1; pc <= 0; st <= EMIT; end
        else resp(PH, 0);
      end
    end
    // ------------------------------------------------------------ phase A: the token, one byte per clock
    TOK: begin
      if (ptr == tok_e) st <= TEND;
      else begin
        x = mem[ptr];
        if (part == 2) begin
          if (x == ".") tbad <= 1;
          bv = b64v(x); if (bv[6]) tbad <= 1;
          sigc <= sigc + 1;
          t12 = {bacc[5:0], bv[5:0]} >> (bn - 2);
          if (bn >= 2) begin
            if (sign < 32) sig <= {sig[247:0], t12[7:0]};
            sign <= sign + 1; bn <= bn - 2;
          end else bn <= bn + 6;
          bacc <= {bacc[5:0], bv[5:0]};
          ptr <= ptr + 1;
        end else if (mready) begin
          if (x == "." && part == 1) begin
            mend <= 1; pe <= 1; part <= 2; bn <= 0;
          end else begin
            mv <= 1; mb <= x;
            if (x == ".") begin he <= 1; part <= 1; bn <= 0; end
            else begin
              bv = b64v(x); if (bv[6]) tbad <= 1;
              t12 = {bacc[5:0], bv[5:0]} >> (bn - 2);
              if (bn >= 2) begin
                oc = t12[7:0];
                if (part == 0) begin hv <= 1; hc <= oc; end else begin pv <= 1; pc_ <= oc; end
                bn <= bn - 2;
              end else bn <= bn + 6;
              bacc <= {bacc[5:0], bv[5:0]};
            end
          end
          ptr <= ptr + 1;
        end
      end
    end
    TEND: begin
      if (part != 2) begin tbad <= 1; st <= TVERD; end
      else if (mdone) begin macr <= mac; cmi <= 0; st <= TCMP; end
    end
    // the gate-level comparator sees one (tag, signature) byte pair per clock
    TCMP: begin
      if (cmi < 32) begin
        ca <= macr[255 - 8 * cmi -: 8]; cb <= sig[255 - 8 * cmi -: 8]; cclr <= cmi == 0;
      end
      cmi <= cmi + 1;
      if (cmi == 32) begin cend <= 1; st <= TACK; end
    end
    TACK: if (cack) st <= TVERD;
    TVERD: begin
      if (tok_inv) resp(PINV, H401);
      else if (pay_inv) resp(PPAY, H401);
      else if (route == 5) begin if (id_any && !id_bad && id_nz) resp(PL, 0); else resp(PID, H400); end
      else begin ptr <= body_s; st <= BODY; end
    end
    // ------------------------------------------------------------ phase A: the request body
    BODY: begin
      if (ptr == body_e) st <= BEOF;
      else begin yv <= 1; yc <= mem[ptr]; ptr <= ptr + 1; end
    end
    BEOF: begin ye <= 1; st <= BVERD; end
    BVERD: if (!ye) begin
      if (y_err) resp(PMAL, H400);
      else if (!(b_has && b_str && b_started)) resp(PREQ, H400);
      else if (b_over || becnt > 500) resp(PLNG, H400);
      else resp(PC, 0);
    end
    // ------------------------------------------------------------ phase B: pick the response
    BJOB: begin
      if (!(rc0 && rcn == 1) && kind != "H") resp(B500, H500);
      else case (kind)
        "H": if (rc0 && rcn == 1 && out_e > out_s && mem[out_s] == "1") resp(BH200, H200); else resp(BH503, H503);
        "F": resp(BF, H200);
        "P": if (out_e > out_s) resp(BP, H200); else resp(BPNF, HB404);
        "C": if (out_e > out_s) resp(BP, H201); else resp(B500, H500);
        "L": if (mem[out_s] != "1") resp(BPNF, HB404); else if (mem[out_s + 2] == "1") resp(BL201, H201); else resp(BL200, H200);
        default: resp(B500, H500);
      endcase
    end
    // ------------------------------------------------------------ the emitter: one output byte per clock
    EMIT: begin
      emit_ = 0; oc = 0;
      if (pn != 0) begin oc = pb[47:40]; pb <= pb << 8; pn <= pn - 1; emit_ = 1; end
      else if (op != 0) begin
        // fetch the next source byte of the running op
        st_ = 0;
        case (op)
          1, 6: if (rp >= rq) st_ = 1; else x = mem[rp];
          2: if (rp >= {9'b0, subn}) st_ = 1; else x = {4'h3, subd[rp[4:0]]};
          3: if (rp >= {1'b0, b_e}) st_ = 1; else x = bmem[rp[11:0]];
          4: if (rp >= {7'b0, un}) st_ = 1; else x = umem[rp[5:0]];
          5: if (rp >= {9'b0, udn}) st_ = 1; else x = {4'h3, ud[udn[3:0] - 1 - rp[3:0]]};
          11, 12: begin x = mem[rp]; if (rp >= out_e || x == 8'h1f || x == 8'h1e) begin st_ = 1; rp <= rp + 1; end end
          default: st_ = 1;
        endcase
        if (st_) begin op <= 0; pc <= pc + 1; end
        else begin
          rp <= rp + 1; emit_ = 1; oc = x;
          if ((op == 3 || op == 4) && x == "'") begin pb <= {8'h27, 40'b0}; pn <= 1; end
          if (op == 6 || op == 12) begin
            if (x == "\"" || x == "\\") begin oc = "\\"; pb <= {x, 40'b0}; pn <= 1; end
            else if (x < 8'h20) begin
              oc = "\\"; pn <= 5;
              pb <= {"u00", (x[7:4] < 10 ? 8'h30 + {4'b0, x[7:4]} : 8'h57 + {4'b0, x[7:4]}),
                            (x[3:0] < 10 ? 8'h30 + {4'b0, x[3:0]} : 8'h57 + {4'b0, x[3:0]}), 8'b0};
            end
          end
        end
      end else begin
        x = rom(prog, pc);
        case (x)
          0: if (in_row) begin
               if (rp < out_e) begin emit_ = 1; oc = ","; pc <= 0; end
               else begin in_row <= 0; prog <= sprog; pc <= spc + 1; end
             end else case (pass)   // end of a program
               0: begin upt <= {24'b0, clen}; udn <= 0; conv_hdr <= 1; st <= UPT; end   // dry run done: Content-Length
               1: begin pass <= 2; prog <= bprog; pc <= 0; end                         // header done: the body
               default: st <= FIN;
             endcase
          1: begin op <= 1; rp <= a_s; rq <= a_e; end
          2: begin op <= 2; rp <= 0; end
          3: begin op <= 3; rp <= {1'b0, b_s}; end
          4: begin op <= 4; rp <= 0; end
          5: begin op <= 5; rp <= 0; end
          6: begin op <= 6; rp <= err_s; rq <= len > err_s && mem[len - 1] == "\n" ? len - 1 : len; end
          7: begin
               rp <= out_s;
               if (out_e > out_s) begin in_row <= 1; sprog <= prog; spc <= pc; prog <= PROW; pc <= 0; end
               else pc <= pc + 1;
             end
          8'h11: op <= 11;
          8'h12: op <= 12;
          default: begin emit_ = 1; oc = x; pc <= pc + 1; end
        endcase
      end
      if (emit_) begin if (pass == 0) clen <= clen + 1; else begin ob <= oc; ot <= ~ot; end end
    end
    FIN: done <= 1;
    default: ;
    endcase

    // ---------------------------------------------------------------- scanner events (any state)
    // header: "alg"
    if (h_kbv) begin hk_ok <= hk_ok && h_kb == chr("alg", 3, {11'b0, hk}); hk <= hk + 1; end
    if (h_kend) begin alg_key <= hk_ok && hk == 3; hk <= 0; hk_ok <= 1; end
    if (h_vs && alg_key) begin alg_cap <= h_vt == 0; av <= 0; av_ok <= 1; alg_ok <= 0; end
    else if (h_vs) alg_cap <= 0;
    if (alg_cap && h_sn != 0) begin av_ok <= av_ok && h_sn == 1 && h_sb[7:0] == chr("HS256", 5, {11'b0, av}); av <= av + 1; end
    if (alg_cap && h_ve) begin alg_ok <= av_ok && av == 5; alg_cap <= 0; end
    // payload: "sub", "username", "exp"
    if (p_kbv) begin
      pk_s <= pk_s && p_kb == chr("sub", 3, {10'b0, pk});
      pk_u <= pk_u && p_kb == chr("username", 8, {10'b0, pk});
      pk_e <= pk_e && p_kb == chr("exp", 3, {10'b0, pk});
      pk <= pk + 1;
    end
    if (p_kend) begin
      pkey <= pk_s && pk == 3 ? 1 : pk_u && pk == 8 ? 2 : pk_e && pk == 3 ? 3 : 0;
      pk <= 0; pk_s <= 1; pk_u <= 1; pk_e <= 1;
    end
    if (p_vs) begin
      pcap <= pkey;
      case (pkey)
        1: begin sub_has <= 1; sub_str <= p_vt == 0; sub_bad <= 0; sub_nz <= 0; subn <= 0; end
        2: begin u_has <= 1; u_str <= p_vt == 0; u_long <= 0; un <= 0; end
        3: begin e_has <= 1; e_num <= p_vt == 1; e_bad <= 0; expv <= 0; end
        default: ;
      endcase
      if (pkey == 3 && p_nbv) begin if (isdig(p_nb)) expv <= {36'b0, p_nb[3:0]}; else e_bad <= 1; end
    end
    if (!p_vs && pcap == 1 && p_sn != 0) begin
      if (p_sn != 1 || !isdig(p_sb[7:0])) sub_bad <= 1;
      else if (p_sb[7:0] != "0" || sub_nz) begin
        sub_nz <= 1; if (subn < 19) begin subd[subn] <= p_sb[3:0]; subn <= subn + 1; end else sub_bad <= 1;
      end
    end
    if (!p_vs && pcap == 2 && p_sn != 0) begin
      for (k = 0; k < 4; k = k + 1) if (k < p_sn && un + k < 64) umem[un[5:0] + k[5:0]] <= p_sb[8 * k +: 8];
      if (un + {4'b0, p_sn} > 64) u_long <= 1; else un <= un + {4'b0, p_sn};
    end
    if (!p_vs && pcap == 3 && p_nbv) begin if (isdig(p_nb)) expv <= expv * 10 + {36'b0, p_nb[3:0]}; else e_bad <= 1; end
    if (p_ve) pcap <= 0;
    // body: "body"
    if (y_kbv) begin yk_ok <= yk_ok && y_kb == chr("body", 4, {11'b0, yk}); yk <= yk + 1; end
    if (y_kend) begin body_key <= yk_ok && yk == 4; yk <= 0; yk_ok <= 1; end
    if (y_vs) begin
      bcap <= body_key && y_vt == 0;
      if (body_key) begin b_has <= 1; b_str <= y_vt == 0; b_started <= 0; b_over <= 0; bw <= 0; b_s <= 0; b_e <= 0; bcnt <= 0; becnt <= 0; end
    end
    if (!y_vs && bcap && y_sn != 0) begin
      w = bw; st_ = b_started; cnt = bcnt;
      for (k = 0; k < 4; k = k + 1) if (k < y_sn) begin
        x = y_sb[8 * k +: 8];
        if (w < 4095) bmem[w[11:0]] <= x;
        if (!st_ && !isws(x)) begin st_ = 1; b_s <= w; cnt = 0; end
        if (st_) begin
          if (x[7:6] != 2'b10) cnt = cnt + 1;
          if (!isws(x)) begin b_e <= w + 1; becnt <= cnt; end
        end
        w = w + 1;
      end
      b_started <= st_; bw <= w; bcnt <= cnt; if (w >= 4095) b_over <= 1;
    end
    if (y_ve) bcap <= 0;
  end
endmodule
