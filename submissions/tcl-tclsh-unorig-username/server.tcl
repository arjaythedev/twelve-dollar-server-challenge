# The feed API in Tcl, the language SQLite was born in. Run by start.sh: tclsh8.7 server.tcl NOFILE
#
# Every socket is in binary mode, so a Tcl string here is a string of bytes (one char per byte).
# Text goes to SQLite as a blob (@var) and is CAST to TEXT there; JSON comes back as a blob,
# built by SQLite. That keeps UTF-8 exact and leaves Tcl almost nothing to do per row.

# First in auto_path, so build.sh's sha256c wins over tcllib's own sha256c.tcl, which would run
# critcl at startup and compile into ~/.critcl.
set auto_path [linsert $auto_path 0 [file join [file dirname [file normalize [info script]]] lib]]
# The Debian packaged SQLite binding (an 8.6 stubs extension, which 8.7 loads).
lappend auto_path /usr/lib/tcltk
package require sqlite3
package require sha256c
if {![string match *__critcl_load__* [package ifneeded sha256c [package present sha256c]]]} {
    error "sha256c did not load from ./lib: run build.sh"
}

sqlite3 db $env(SQLITE_PATH)
db eval {
    PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;
    PRAGMA mmap_size=268435456; PRAGMA cache_size=-32000
}
db cache size 32
set t0 [clock seconds]

# HMAC-SHA256 (RFC 2104): hash contexts that have already absorbed the key pads, built once.
set key [encoding convertto utf-8 $env(JWT_SECRET)]
if {[string length $key] > 64} {set key [sha2::sha256c_update $key [sha2::sha256c_init256]]}
binary scan [binary format a64 $key] c* kb
set ipad {}; set opad {}
foreach k $kb {append ipad [format %c [expr {($k ^ 0x36) & 0xff}]]; append opad [format %c [expr {($k ^ 0x5c) & 0xff}]]}
set ictx [sha2::sha256c_update $ipad [sha2::sha256c_init256]]
set octx [sha2::sha256c_update $opad [sha2::sha256c_init256]]
unset key kb ipad opad k

proc head {status} {return "HTTP/1.1 $status\r\nContent-Type: application/json\r\nContent-Length: "}
proc err {status msg} {set b "{\"error\":\"$msg\"}"; return "[head $status][string length $b]\r\n\r\n$b"}
set H200 [head "200 OK"]; set H201 [head "201 Created"]
array set E [list \
    404 [err "404 Not Found" "not found"] id [err "400 Bad Request" "invalid post id"] \
    post [err "404 Not Found" "post not found"] bearer [err "401 Unauthorized" "missing bearer token"] \
    token [err "401 Unauthorized" "invalid or expired token"] payload [err "401 Unauthorized" "invalid token payload"] \
    json [err "400 Bad Request" "malformed JSON body"] body [err "400 Bad Request" "body is required"] \
    long [err "400 Bad Request" "body must be at most 500 characters"] \
    500 [err "500 Internal Server Error" "internal server error"]]

# The post object, in key order, as JSON text made by SQLite.
set POST {json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username,
    'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))}
set QFEED [string map [list %POST% $POST] {SELECT CAST('{"posts":[' || coalesce(group_concat(o, ','), '') || ']}' AS BLOB)
    FROM (SELECT %POST% AS o FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20)}]
set QPOST [string map [list %POST% $POST] {SELECT CAST('{"post":' || %POST% || '}' AS BLOB)
    FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $id}]

# A positive decimal integer as a number, "" if it is not one, 0 if it is too long for a row id.
proc posint {s} {
    if {![regexp {^[0-9]+$} $s]} {return ""}
    set s [string trimleft $s 0]
    if {$s eq ""} {return ""}
    if {[string length $s] > 18} {return 0}
    scan $s %lld n
    return $n
}

proc b64u {s} {
    if {[string length $s] % 4 == 1} {error "bad base64"}
    binary decode base64 -strict [string map {- + _ /} $s][string repeat = [expr {-[string length $s] % 4}]]
}

# Returns {uid username} or {"" error-key}. The username stays raw UTF-8 bytes.
proc auth {buf pos lh} {
    set i [string first "\nauthorization:" $lh]
    if {$i < 0} {return {"" bearer}}
    set s [expr {$pos + $i + 15}]
    set v [string trimleft [string range $buf $s [string first "\r" $buf $s]-1] " \t"]
    if {[string range $v 0 6] ne "Bearer "} {return {"" bearer}}
    if {![regexp {^([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)$} [string range $v 7 end] -> h p sig]} {
        return {"" token}
    }
    set mac [sha2::sha256c_update [sha2::sha256c_update $h.$p $::ictx] $::octx]
    # Compares keyed hashes of the two signatures, so the time `ne` takes says nothing about $sig.
    set want [string map {+ - / _ = {}} [binary encode base64 $mac]]
    if {"[sha2::sha256c_update $want $::ictx]" ne "[sha2::sha256c_update $sig $::ictx]"} {return {"" token}}
    if {[catch {
        set hj [b64u $h]; set pj [b64u $p]
        db eval {
            SELECT json_type(p, '$.exp') AS et, json_extract(p, '$.exp') AS ev,
                   json_type(p, '$.nbf') AS nt, json_extract(p, '$.nbf') AS nv,
                   json_type(p, '$.sub') AS st, json_extract(p, '$.sub') AS sv,
                   json_type(p, '$.username') AS ut, CAST(json_extract(p, '$.username') AS BLOB) AS uv
              FROM (SELECT CAST(@hj AS TEXT) AS h, CAST(@pj AS TEXT) AS p)
             WHERE json_valid(h) AND json_type(h) = 'object' AND json_extract(h, '$.alg') IS 'HS256'
               AND json_valid(p) AND json_type(p) = 'object'
        } r {}
    }] || ![info exists r(et)]} {return {"" token}}
    set now [clock seconds]
    if {$r(et) ni {integer real} || $r(ev) <= $now} {return {"" token}}
    if {$r(nt) ne "" && ($r(nt) ni {integer real} || $r(nv) > $now)} {return {"" token}}
    if {$r(st) ne "text" || $r(ut) ne "text" || [set uid [posint $r(sv)]] eq "" || $uid == 0} {return {"" payload}}
    return [list $uid $r(uv)]
}

proc create {c uid name j} {
    if {[catch {db onecolumn {SELECT json_valid(CAST(@j AS TEXT))}} ok] || !$ok} {return $::E(json)}
    set t ""; set n 0
    db eval {
        SELECT type AS t, CAST(trim(value, char(32, 9, 10, 13, 11, 12)) AS BLOB) AS v,
               length(trim(value, char(32, 9, 10, 13, 11, 12))) AS n
          FROM json_each(CAST(@j AS TEXT)) WHERE key = 'body' AND json_type(CAST(@j AS TEXT)) = 'object'
         ORDER BY id DESC LIMIT 1
    } {}
    if {$t ne "text" || $n == 0} {return $::E(body)}
    if {$n > 500} {return $::E(long)}
    # Autocommit: db eval returns after the INSERT has committed (WAL, synchronous=NORMAL).
    set b [lindex [db eval {
        INSERT INTO posts (user_id, body) VALUES ($uid, CAST(@v AS TEXT))
        RETURNING CAST(json_object('post', json_object('id', id, 'body', body, 'created_at', created_at,
                                   'author', CAST(@name AS TEXT), 'like_count', 0)) AS BLOB)
    }] 0]
    puts -nonewline $c $::H201[string length $b]\r\n\r\n
    puts -nonewline $c $b
    return ""
}

proc like {uid id} {
    db eval {
        INSERT INTO likes (user_id, post_id) SELECT $uid, id FROM posts WHERE id = $id
        ON CONFLICT (user_id, post_id) DO NOTHING
    }
    if {[db changes]} {
        set b "{\"liked\":true,\"already_liked\":false,\"post_id\":$id}"
        return $::H201[string length $b]\r\n\r\n$b
    }
    if {![db exists {SELECT 1 FROM posts WHERE id = $id}]} {return $::E(post)}
    set b "{\"liked\":true,\"already_liked\":true,\"post_id\":$id}"
    return $::H200[string length $b]\r\n\r\n$b
}

# Answers one request: writes the response into the channel's buffer.
proc handle {c buf pos he next lh} {
    set sp [string first " " $buf $pos]
    set m [string range $buf $pos $sp-1]
    set path [string range $buf $sp+1 [string first " " $buf $sp+1]-1]
    if {[set q [string first ? $path]] >= 0} {set path [string range $path 0 $q-1]}
    if {$m eq "GET"} {
        if {$path eq "/feed"} {
            set b [db onecolumn $::QFEED]
        } elseif {[string match /posts/* $path] && [string first / $path 7] < 0 && [string length $path] > 7} {
            set id [posint [string range $path 7 end]]
            if {$id eq ""} {return $::E(id)}
            set b [db onecolumn $::QPOST]
            if {$b eq ""} {return $::E(post)}
        } elseif {$path eq "/health"} {
            if {[catch {db onecolumn {SELECT 1}} e]} {
                set b "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"[string map {\" \\\" \\ \\\\} $e]\"}"
                return [head "503 Service Unavailable"][string length $b]\r\n\r\n$b
            }
            set b "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":[expr {[clock seconds] - $::t0}]}"
        } else {
            return $::E(404)
        }
        puts -nonewline $c $::H200[string length $b]\r\n\r\n
        puts -nonewline $c $b
        return ""
    }
    if {$m ne "POST"} {return $::E(404)}
    if {$path eq "/posts"} {
        set id ""
    } elseif {[regexp {^/posts/([^/]+)/like$} $path -> id]} {
    } else {
        return $::E(404)
    }
    lassign [auth $buf $pos $lh] uid name
    if {$uid eq ""} {return $::E($name)}
    if {$id eq ""} {return [create $c $uid $name [string range $buf $he+4 $next-1]]}
    set id [posint $id]
    if {$id eq ""} {return $::E(id)}
    like $uid $id
}

set nconn 0
set MAXCONN [expr {[lindex $argv 0] - 64}]
proc drop {c} {
    incr ::nconn -1
    unset -nocomplain ::B($c)
    catch {close $c}
}

# Reads what has arrived, answers every complete request in it (pipelining), keeps a partial one.
proc rd {c} {
    if {[catch {read $c} d]} {return [drop $c]}
    if {$d eq "" && [eof $c]} {return [drop $c]}
    upvar #0 B($c) b
    append b $d
    set pos 0; set len [string length $b]; set close 0; set full 0
    while {[set he [string first "\r\n\r\n" $b $pos]] >= 0} {
        set lh [string tolower [string range $b $pos $he]]
        set next [expr {$he + 4}]; set cl 0
        if {[set i [string first "\ncontent-length:" $lh]] >= 0} {
            scan [string range $lh $i+16 $i+40] %d cl
            if {$cl > 0} {incr next $cl}
            if {$next > $len} break
        }
        if {[string first "\nconnection: close" $lh] >= 0} {set close 1}
        if {[catch {handle $c $b $pos $he $next $lh} r]} {
            # The client is gone (broken pipe, reset): skip the rest of its pipelined requests.
            if {[lindex $::errorCode 0] eq "POSIX"} {return [drop $c]}
            puts stderr $::errorInfo; set r $::E(500)
        }
        if {[catch {puts -nonewline $c $r}]} {return [drop $c]}
        set pos $next
        if {$close} break
        # A client that does not read its responses: stop until they drain, so its output stays bounded.
        if {[chan pending output $c] > 1048576} {set full 1; break}
    }
    set b [string range $b $pos end]
    if {[catch {flush $c}] || $close || [string length $b] > 1048576} {return [drop $c]}
    if {$full} {fileevent $c readable {}; fileevent $c writable [list resume $c]}
}

# Tcl fires this once the queued output has been written: answer the rest, then read again.
proc resume {c} {
    fileevent $c writable {}
    fileevent $c readable [list rd $c]
    rd $c
}

proc accept {c addr port} {
    # Below start.sh's descriptor limit (argv), so accept never fails with EMFILE and spins.
    if {$::nconn >= $::MAXCONN} {return [close $c]}
    incr ::nconn
    fconfigure $c -blocking 0 -translation binary -buffering full
    fileevent $c readable [list rd $c]
}

socket -server accept -myaddr $env(HOST) $env(PORT)
vwait forever
