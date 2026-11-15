#!/usr/bin/env zsh
# The whole web server, in one Z shell process: ztcp listens and accepts, zselect waits on every
# socket, sysread/syswrite move the bytes, $(( )) does HMAC-SHA256, and zsh/sqlite (module/) talks
# to the database in-process.
emulate -R zsh
setopt extendedglob nomultibyte cprecedences
zmodload zsh/net/tcp zsh/zselect zsh/system zsh/datetime zsh/sqlite || exit 1

typeset -gi START=$EPOCHSECONDS
typeset -g SQLITE_PATH=${SQLITE_PATH:?} JWT_SECRET=${JWT_SECRET:?}
zsqlite -o $SQLITE_PATH || { print -u2 "cannot open $SQLITE_PATH: $REPLY"; exit 1 }
for p in 'journal_mode=WAL' 'synchronous=NORMAL' 'busy_timeout=5000' 'mmap_size=268435456' 'cache_size=-16000'; do
  zsqlite "PRAGMA $p" || { print -u2 "PRAGMA $p: $REPLY"; exit 1 }
done

# ---- SQL. Every request reads SQLite while it is served; statements are prepared once and reused.
POST_JSON="json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))"
Q_FEED="SELECT '{\"posts\":[' || coalesce(group_concat(j, ','), '') || ']}' FROM (SELECT $POST_JSON j FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20)"
Q_POST="SELECT json_object('post',$POST_JSON) FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=CAST(?1 AS INTEGER)"
WS="' '||char(9,10,11,12,13)"
# body check: malformed | required | long | ok, then the trimmed body
Q_BODY="SELECT CASE WHEN NOT json_valid(?1) THEN 'malformed' ELSE coalesce((SELECT CASE WHEN type<>'text' OR trim(value,$WS)='' THEN 'required' WHEN length(trim(value,$WS))>500 THEN 'long' ELSE 'ok' END FROM json_each(CASE WHEN json_valid(?1) AND json_type(?1)='object' THEN ?1 ELSE '{}' END) WHERE key='body' ORDER BY id DESC LIMIT 1),'required') END, coalesce((SELECT trim(value,$WS) FROM json_each(CASE WHEN json_valid(?1) AND json_type(?1)='object' THEN ?1 ELSE '{}' END) WHERE key='body' AND type='text' ORDER BY id DESC LIMIT 1),'')"
Q_INSERT="INSERT INTO posts (user_id, body) VALUES (CAST(?1 AS INTEGER), ?2) RETURNING json_object('post',json_object('id',id,'body',body,'created_at',created_at,'author',?3,'like_count',0))"
Q_LIKE="INSERT INTO likes (user_id, post_id) SELECT CAST(?1 AS INTEGER), id FROM posts WHERE id=CAST(?2 AS INTEGER) ON CONFLICT (user_id, post_id) DO NOTHING RETURNING post_id"
Q_EXISTS="SELECT id FROM posts WHERE id=CAST(?1 AS INTEGER)"
# token check after the signature: bad | payload | ok, then sub and username. ?1 header, ?2 payload, ?3 now
Q_CLAIMS="SELECT CASE
  WHEN NOT json_valid(?1) OR NOT json_valid(?2) THEN 'bad'
  WHEN json_type(?1)<>'object' OR json_type(?1,'\$.alg') IS NOT 'text' OR json_extract(?1,'\$.alg')<>'HS256' THEN 'bad'
  WHEN json_type(?2)<>'object' OR coalesce(json_type(?2,'\$.exp'),'') NOT IN ('integer','real') OR json_extract(?2,'\$.exp')<=CAST(?3 AS INTEGER) THEN 'bad'
  WHEN json_type(?2,'\$.nbf') IS NOT NULL AND (json_type(?2,'\$.nbf') NOT IN ('integer','real') OR json_extract(?2,'\$.nbf')>CAST(?3 AS INTEGER)) THEN 'bad'
  WHEN json_type(?2,'\$.sub') IS NOT 'text' OR json_extract(?2,'\$.sub') NOT GLOB '[0-9]*' OR json_extract(?2,'\$.sub') GLOB '*[^0-9]*'
    OR length(json_extract(?2,'\$.sub'))>18 OR CAST(json_extract(?2,'\$.sub') AS INTEGER)=0 OR json_type(?2,'\$.username') IS NOT 'text' THEN 'payload'
  ELSE 'ok' END,
  CASE WHEN json_valid(?2) AND json_type(?2)='object' THEN CAST(json_extract(?2,'\$.sub') AS INTEGER) END,
  CASE WHEN json_valid(?2) AND json_type(?2)='object' THEN json_extract(?2,'\$.username') END"

# ---- SHA-256 and HMAC in shell arithmetic. Words are 32-bit values held in 64-bit integers.
typeset -ga K=(
  0x428a2f98 0x71374491 0xb5c0fbcf 0xe9b5dba5 0x3956c25b 0x59f111f1 0x923f82a4 0xab1c5ed5
  0xd807aa98 0x12835b01 0x243185be 0x550c7dc3 0x72be5d74 0x80deb1fe 0x9bdc06a7 0xc19bf174
  0xe49b69c1 0xefbe4786 0x0fc19dc6 0x240ca1cc 0x2de92c6f 0x4a7484aa 0x5cb0a9dc 0x76f988da
  0x983e5152 0xa831c66d 0xb00327c8 0xbf597fc7 0xc6e00bf3 0xd5a79147 0x06ca6351 0x14292967
  0x27b70a85 0x2e1b2138 0x4d2c6dfc 0x53380d13 0x650a7354 0x766a0abb 0x81c2c92e 0x92722c85
  0xa2bfe8a1 0xa81a664b 0xc24b8b70 0xc76c51a3 0xd192e819 0xd6990624 0xf40e3585 0x106aa070
  0x19a4c116 0x1e376c08 0x2748774c 0x34b0bcb5 0x391c0cb3 0x4ed8aa4a 0x5b9cca4f 0x682e6ff3
  0x748f82ee 0x78a5636f 0x84c87814 0x8cc70208 0x90befffa 0xa4506ceb 0xbef9a3f7 0xc67178f2)
typeset -ga H0=(0x6a09e667 0xbb67ae85 0x3c6ef372 0xa54ff53a 0x510e527f 0x9b05688c 0x1f83d9ab 0x5be0cd19)
typeset -ga H W IH OH
K=( ${K/(#m)*/$(( MATCH ))} ) H0=( ${H0/(#m)*/$(( MATCH ))} )  # hex to decimal once
typeset -gi M=0xFFFFFFFF

sha_block() { # compresses the block in W[1..16] into H
  integer a=H[1] b=H[2] c=H[3] d=H[4] e=H[5] f=H[6] g=H[7] h=H[8] i x s0 s1 t1
  for (( i = 17; i <= 64; i++ )); do
    (( x = W[i-15], s0 = (x>>7 | x<<25) ^ (x>>18 | x<<14) ^ (x>>3),
       x = W[i-2], s1 = (x>>17 | x<<15) ^ (x>>19 | x<<13) ^ (x>>10),
       W[i] = (W[i-16] + s0 + W[i-7] + s1) & M ))
  done
  for (( i = 1; i <= 64; i++ )); do
    (( t1 = (h + ((e>>6 | e<<26) ^ (e>>11 | e<<21) ^ (e>>25 | e<<7)) + ((e & f) ^ (~e & g)) + K[i] + W[i]) & M,
       h = g, g = f, f = e, e = (d + t1) & M, d = c, c = b, b = a,
       a = (t1 + (((a>>2 | a<<30) ^ (a>>13 | a<<19) ^ (a>>22 | a<<10)) & M) + ((a & c) ^ (a & d) ^ (c & d))) & M ))
  done
  (( H[1] = (H[1] + a) & M, H[2] = (H[2] + b) & M, H[3] = (H[3] + c) & M, H[4] = (H[4] + d) & M,
     H[5] = (H[5] + e) & M, H[6] = (H[6] + f) & M, H[7] = (H[7] + g) & M, H[8] = (H[8] + h) & M ))
}
# In the round, a is assigned last, so it still holds the old a; c and d already hold the old b and c.

sha_bytes() { # sha_bytes <data> <bytes hashed before>: feeds data plus padding into H
  local data=$1 c
  integer i j n=$(( ${#data} + $2 ))
  local -a by
  for c in ${(s::)data}; do by+=( $(( #c & 255 )) ); done
  by+=( 128 )
  while (( ($#by + 8) % 64 )); do by+=( 0 ); done
  by+=( 0 0 0 $(( (n >> 29) & 255 )) $(( (n >> 21) & 255 )) $(( (n >> 13) & 255 )) $(( (n >> 5) & 255 )) $(( (n << 3) & 255 )) )
  for (( i = 1; i <= $#by; i += 64 )); do
    for (( j = 0; j < 16; j++ )); do
      (( W[j+1] = by[i+4*j] << 24 | by[i+4*j+1] << 16 | by[i+4*j+2] << 8 | by[i+4*j+3] ))
    done
    sha_block
  done
}

B64='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_'
typeset -gA B64V
integer i
for (( i = 1; i <= 64; i++ )); do B64V[${B64[i]}]=$(( i - 1 )); done

hmac_b64() { # REPLY = base64url(HMAC-SHA256(JWT_SECRET, $1))
  H=( $IH ); sha_bytes "$1" 64
  W=( $H 0x80000000 0 0 0 0 0 0 768 )   # inner digest, padding, length (64 + 32) * 8 bits
  H=( $OH ); sha_block
  local -a by
  integer i v
  for (( i = 1; i <= 8; i++ )); do by+=( $(( H[i] >> 24 )) $(( H[i] >> 16 & 255 )) $(( H[i] >> 8 & 255 )) $(( H[i] & 255 )) ); done
  by+=( 0 )
  REPLY=
  for (( i = 1; i <= 33; i += 3 )); do
    (( v = by[i] << 16 | by[i+1] << 8 | by[i+2] ))
    REPLY+=${B64[(v>>18)+1]}${B64[(v>>12&63)+1]}${B64[(v>>6&63)+1]}${B64[(v&63)+1]}
  done
  REPLY=${REPLY[1,43]}
}

b64d() { # REPLY = base64url-decode($1); status 1 if it is not base64url
  local s=$1 c out=
  integer acc=0 nb=0 x
  (( ${#s} % 4 == 1 )) && return 1
  for c in ${(s::)s}; do
    (( acc = (acc << 6 | B64V[$c]) & 0xFFFF, nb += 6 ))
    if (( nb >= 8 )); then (( nb -= 8, x = acc >> nb & 255 )); out+=${(#)x}; fi
  done
  REPLY=$out
}

# The key's ipad and opad blocks are compressed once at start (a key schedule, not a cached token).
() {
  local c
  local -a k
  integer i
  (( ${#JWT_SECRET} <= 64 )) || { print -u2 "JWT_SECRET over 64 bytes is not supported"; exit 1 }  # shortcut: hash a longer key first if one is ever used
  for c in ${(s::)JWT_SECRET}; do k+=( $(( #c & 255 )) ); done
  while (( $#k < 64 )); do k+=( 0 ); done
  for (( i = 0; i < 16; i++ )); do
    (( W[i+1] = (k[4*i+1] << 24 | k[4*i+2] << 16 | k[4*i+3] << 8 | k[4*i+4]) ^ 0x36363636 ))
  done
  H=( $H0 ); sha_block; IH=( $H )
  for (( i = 0; i < 16; i++ )); do
    (( W[i+1] = (k[4*i+1] << 24 | k[4*i+2] << 16 | k[4*i+3] << 8 | k[4*i+4]) ^ 0x5c5c5c5c ))
  done
  H=( $H0 ); sha_block; OH=( $H )
}

# ---- HTTP
typeset -g ST RB USERID UNAME HEAD
reply() { ST=$1 RB=$2 }
err() { ST=$1 RB="{\"error\":\"$2\"}" }

auth() { # status 0 with USERID and UNAME set, or the error response set
  local v h p s hj
  if [[ $HEAD != (#b)*$'\n'(#i)authorization:[\ $'\t']#([^$'\r']#)* || $match[1] != 'Bearer '* ]]; then
    err '401 Unauthorized' 'missing bearer token'; return 1
  fi
  v=${${match[1]#Bearer }%%[\ $'\t']##}
  if [[ $v != (#b)([A-Za-z0-9_-]##).([A-Za-z0-9_-]##).([A-Za-z0-9_-]##) ]]; then
    err '401 Unauthorized' 'invalid or expired token'; return 1
  fi
  h=$match[1] p=$match[2] s=$match[3]
  hmac_b64 "$h.$p"
  if [[ $REPLY != "$s" ]] || ! b64d $h; then err '401 Unauthorized' 'invalid or expired token'; return 1; fi
  hj=$REPLY
  if ! b64d $p || ! zsqlite $Q_CLAIMS "$hj" "$REPLY" $EPOCHSECONDS; then err '401 Unauthorized' 'invalid or expired token'; return 1; fi
  case $reply[1] in
    (ok) USERID=$reply[2] UNAME=$reply[3] ;;
    (payload) err '401 Unauthorized' 'invalid token payload'; return 1 ;;
    (*) err '401 Unauthorized' 'invalid or expired token'; return 1 ;;
  esac
}

valid_id() { [[ $1 == [0-9]## && $1 == *[1-9]* ]] || { err '400 Bad Request' 'invalid post id'; return 1 } }

route() { # route <method> <path> <body>
  local id
  case "$1 $2" in
    ('GET /health')
      if zsqlite 'SELECT 1'; then reply '200 OK' "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":$(( EPOCHSECONDS - START ))}"
      else reply '503 Service Unavailable' "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"${${REPLY//\\/\\\\}//\"/\\\"}\"}"; fi ;;
    ('GET /feed')
      zsqlite $Q_FEED && reply '200 OK' $reply[1] ;;
    ('GET /posts/'[^/]##)
      id=${2#/posts/}
      valid_id $id || return
      zsqlite $Q_POST $id || return
      if (( $#reply )); then reply '200 OK' $reply[1]; else err '404 Not Found' 'post not found'; fi ;;
    ('POST /posts')
      auth || return
      zsqlite $Q_BODY "$3" || return
      case $reply[1] in
        (malformed) err '400 Bad Request' 'malformed JSON body' ;;
        (required) err '400 Bad Request' 'body is required' ;;
        (long) err '400 Bad Request' 'body must be at most 500 characters' ;;
        (ok) zsqlite $Q_INSERT $USERID "$reply[2]" "$UNAME" && reply '201 Created' $reply[1] ;;
      esac ;;
    ('POST /posts/'[^/]##'/like')
      auth || return
      id=${${2#/posts/}%/like}
      valid_id $id || return
      zsqlite $Q_LIKE $USERID $id || return
      if (( $#reply )); then reply '201 Created' "{\"liked\":true,\"already_liked\":false,\"post_id\":$reply[1]}"; return; fi
      zsqlite $Q_EXISTS $id || return
      if (( $#reply )); then reply '200 OK' "{\"liked\":true,\"already_liked\":true,\"post_id\":$reply[1]}"
      else err '404 Not Found' 'post not found'; fi ;;
    (*) err '404 Not Found' 'not found' ;;
  esac
}

typeset -gA inb  # connection fd -> bytes received and not yet answered

drop() { ztcp -c $1 2>/dev/null; unset "inb[$1]" }

serve() { # answers every complete request in the buffer of connection $1, in order
  local fd=$1 b=$inb[$1] rest body rl target out=
  integer cl close=0 n
  while [[ $b == *$'\r\n\r\n'* ]]; do
    HEAD=$'\n'${b%%$'\r\n\r\n'*} rest=${b#*$'\r\n\r\n'} cl=0
    [[ $HEAD == (#b)*$'\n'(#i)content-length:[\ $'\t']#([0-9]##)(|[^0-9]*) ]] && cl=$match[1]
    (( ${#rest} < cl )) && break
    body=${rest[1,cl]} b=${rest[cl+1,-1]}
    [[ $HEAD == *$'\n'(#i)connection:[\ $'\t']#close* ]] && close=1
    rl=${${HEAD#$'\n'}%%$'\r'*} target=${${rl#* }%% *}
    ST= RB=
    route "${rl%% *}" "${target%%\?*}" "$body"
    [[ -n $ST ]] || err '500 Internal Server Error' 'internal server error'
    out+="HTTP/1.1 $ST"$'\r\n'"Content-Type: application/json"$'\r\n'"Content-Length: ${#RB}"$'\r\n\r\n'$RB
    (( close )) && break
  done
  inb[$fd]=$b
  while (( ${#out} )); do
    syswrite -c n -o $fd $out || { drop $fd; return }
    out=${out[n+1,-1]}
  done
  if (( close || ${#b} > 1048576 )); then drop $fd; fi
}

[[ $1 == selftest ]] && { hmac_b64 'abc'; print -r -- $REPLY; exit }

ztcp -l ${PORT:-3000} || exit 1
typeset -gi LFD=$REPLY
print "listening on ${PORT:-3000}"
typeset -gA rdy
local fd chunk
trap '' PIPE  # a client that hangs up mid-answer costs its connection, not the server
while :; do
  zselect -A rdy -r $LFD ${(k)inb} || continue
  for fd in ${(k)rdy}; do
    (( fd == LFD )) && continue
    if sysread -i $fd -s 65536 chunk; then inb[$fd]+=$chunk; serve $fd; else drop $fd; fi
  done
  # Accept after the reads, so a number freed by drop above cannot be read as if it were ready.
  if (( ${+rdy[$LFD]} )) && ztcp -a $LFD; then
    # zselect is select(2): an fd past FD_SETSIZE would corrupt its fd_set, so refuse those.
    if (( REPLY >= 1000 )); then ztcp -c $REPLY; else inb[$REPLY]=; fi
  fi
done
