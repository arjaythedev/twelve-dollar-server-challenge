#!/usr/bin/env bash
# worker.sh: persistent HTTP/1.1 keep-alive worker with persistent SQLite coprocess
set -u
shopt -s lastpipe
export LC_ALL=C

# Clean up SQLite coprocess when worker exits
cleanup() {
    if [[ -n "${SQLITE_PID:-}" ]]; then
        if [[ -n "${SQLITE[1]:-}" ]]; then
            printf ".exit\n" >&"${SQLITE[1]}" 2>/dev/null || true
        fi
        kill "$SQLITE_PID" 2>/dev/null || true
        wait "$SQLITE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Initialize persistent SQLite coprocess
coproc SQLITE { exec sqlite3 -batch "$SQLITE_PATH" 2>&1; }

# Configure SQLite for high-concurrency WAL mode and low memory footprint
printf "PRAGMA synchronous=NORMAL; PRAGMA cache_size=-2000; PRAGMA temp_store=MEMORY; PRAGMA busy_timeout=5000;\nSELECT '__READY__';\n" >&"${SQLITE[1]}"
while IFS= read -r line <&"${SQLITE[0]}"; do
    [[ "$line" == "__READY__" ]] && break
done

# Query SQLite coprocess and return compact output via SQL_OUT (zero subshells)
query_sqlite() {
    printf "%s;\nSELECT '__DONE__';\n" "$1" >&"${SQLITE[1]}"
    local line
    IFS= read -r SQL_OUT <&"${SQLITE[0]}"
    if [[ "$SQL_OUT" == "__DONE__" ]]; then
        SQL_OUT=""
        return
    fi
    while IFS= read -r line <&"${SQLITE[0]}"; do
        [[ "$line" == "__DONE__" ]] && break
        SQL_OUT+=$'\n'"$line"
    done
}

# Constant feed SQL query (pre-compiled string)
FEED_SQL="SELECT json_object('posts', json_group_array(json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)))) FROM (SELECT * FROM posts ORDER BY created_at DESC, id DESC LIMIT 20) p JOIN users u ON u.id = p.user_id"

# HTTP response helpers (zero local allocations on hot path)
send_resp() {
    local status="$1"
    local body="$2"
    local conn="${3:-keep-alive}"

    printf 'HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n%s' \
        "$status" "${#body}" "$conn" "$body"
}

send_err() {
    local code="$1"
    local msg="$2"
    local conn="${3:-keep-alive}"
    local body="{\"error\":\"$msg\"}"
    send_resp "$code" "$body" "$conn"
}

# Base64url character map
b64chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

# Convert 64-character hex string (SHA256) to 43-character base64url string
# Sets B64_OUT
hex2b64url() {
    local hex="$1"
    B64_OUT=""
    local i val
    for (( i=0; i<60; i+=6 )); do
        val=$(( 0x${hex:i:6} ))
        B64_OUT+="${b64chars:(val>>18&63):1}${b64chars:(val>>12&63):1}${b64chars:(val>>6&63):1}${b64chars:(val&63):1}"
    done
    val=$(( (0x${hex:60:4} << 2) ))
    B64_OUT+="${b64chars:(val>>12&63):1}${b64chars:(val>>6&63):1}${b64chars:(val&63):1}"
}

# Validate HS256 JWT
# Sets AUTH_OUT ("OK" or "ERR:<code>:<message>")
# Sets AUTH_UID and AUTH_USER when valid
validate_jwt() {
    local auth_header="$1"
    if [[ "$auth_header" != "Bearer "* ]]; then
        AUTH_OUT="ERR:401:missing bearer token"
        return
    fi
    local jwt="${auth_header#Bearer }"
    local h="${jwt%%.*}"
    local rest="${jwt#*.}"
    local p="${rest%%.*}"
    local s="${rest#*.}"

    if [[ "$h" == "$jwt" || "$rest" == "$jwt" || "$p" == "$rest" || -z "$s" ]]; then
        AUTH_OUT="ERR:401:invalid or expired token"
        return
    fi

    # Verify HMAC-SHA256 signature (openssl dgst + lastpipe + pure bash b64 conversion)
    local hex_val
    printf "%s" "$h.$p" | openssl dgst -sha256 -hmac "$JWT_SECRET" | read -r _ hex_val
    hex2b64url "$hex_val"
    if [[ "$s" != "$B64_OUT" ]]; then
        AUTH_OUT="ERR:401:invalid or expired token"
        return
    fi

    local now
    printf -v now "%(%s)T" -1

    # Decode and validate payload in SQLite coprocess (0 forks, no external base64)
    local check_sql="WITH header AS (SELECT CAST(base64(replace(replace('$h', '-', '+'), '_', '/')) AS TEXT) AS jh), payload AS (SELECT CAST(base64(replace(replace('$p', '-', '+'), '_', '/')) AS TEXT) AS jp) SELECT CASE WHEN json_valid(jh) = 0 OR json_extract(jh, '$.alg') IS NOT 'HS256' THEN 'ERR:401:invalid or expired token' WHEN json_valid(jp) = 0 THEN 'ERR:401:invalid token payload' WHEN json_type(jp, '$.exp') IS NOT 'integer' OR json_extract(jp, '$.exp') < $now THEN 'ERR:401:invalid or expired token' WHEN (json_type(jp, '$.sub') IS NOT 'text' AND json_type(jp, '$.sub') IS NOT 'integer') THEN 'ERR:401:invalid token payload' WHEN json_extract(jp, '$.sub') NOT GLOB '[1-9]*' OR json_extract(jp, '$.sub') GLOB '*[^0-9]*' THEN 'ERR:401:invalid token payload' WHEN json_type(jp, '$.username') IS NOT 'text' OR length(json_extract(jp, '$.username')) = 0 THEN 'ERR:401:invalid token payload' ELSE 'OK:' || json_extract(jp, '$.sub') || ':' || json_extract(jp, '$.username') END FROM header, payload"

    query_sqlite "$check_sql"
    if [[ "$SQL_OUT" == OK:* ]]; then
        AUTH_OUT="OK"
        AUTH_UID="${SQL_OUT#OK:}"
        AUTH_USER="${AUTH_UID#*:}"
        AUTH_UID="${AUTH_UID%%:*}"
    else
        AUTH_OUT="$SQL_OUT"
    fi
}

# Main Keep-Alive Request Loop
while IFS= read -r -t 70 request_line; do
    request_line="${request_line%$'\r'}"
    [[ -z "$request_line" ]] && continue

    method="${request_line%% *}"
    rest="${request_line#* }"
    uri="${rest%% *}"
    proto="${rest#* }"

    content_length=0
    auth_header=""
    conn_header=""

    while IFS= read -r -t 10 header_line; do
        header_line="${header_line%$'\r'}"
        [[ -z "$header_line" ]] && break

        case "$header_line" in
            [Cc][Oo][Nn][Tt][Ee][Nn][Tt]-[Ll][Ee][Nn][Gg][Tt][Hh]:*)
                val="${header_line#*:}"
                val="${val#"${val%%[! ]*}"}"
                content_length="${val%"${val##*[! ]}"}"
                ;;
            [Aa][Uu][Tt][Hh][Oo][Rr][Ii][Zz][Aa][Tt][Ii][Oo][Nn]:*)
                val="${header_line#*:}"
                val="${val#"${val%%[! ]*}"}"
                auth_header="${val%"${val##*[! ]}"}"
                ;;
            [Cc][Oo][Nn][Nn][Ee][Cc][Tt][Ii][Oo][Nn]:*)
                val="${header_line#*:}"
                val="${val#"${val%%[! ]*}"}"
                conn_header="${val%"${val##*[! ]}"}"
                ;;
        esac
    done

    # Read body if any
    body=""
    if (( content_length > 0 )); then
        IFS= read -r -N "$content_length" -t 10 body
    fi

    # Keep-Alive negotiation
    resp_conn="keep-alive"
    should_close=0
    if [[ "$conn_header" == *[Cc]lose* || "$proto" == "HTTP/1.0" ]]; then
        resp_conn="close"
        should_close=1
    fi

    # Endpoints Router (prioritized by frequency in benchmark)
    if [[ "$method" == "GET" && "$uri" == "/feed" ]]; then
        query_sqlite "$FEED_SQL"
        send_resp "200 OK" "$SQL_OUT" "$resp_conn"

    elif [[ "$method" == "GET" && "$uri" == /posts/* ]]; then
        post_id="${uri#/posts/}"
        if [[ "$post_id" == */* || "$post_id" != [1-9]* || "$post_id" == *[!0-9]* ]]; then
            send_err "400 Bad Request" "invalid post id" "$resp_conn"
        else
            sql="SELECT json_object('post', json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $post_id"
            query_sqlite "$sql"
            if [[ -z "$SQL_OUT" ]]; then
                send_err "404 Not Found" "post not found" "$resp_conn"
            else
                send_resp "200 OK" "$SQL_OUT" "$resp_conn"
            fi
        fi

    elif [[ "$method" == "POST" && "$uri" == /posts/*/like ]]; then
        # Rule: auth is verified before route id
        validate_jwt "$auth_header"
        if [[ "$AUTH_OUT" != "OK" ]]; then
            err_code="${AUTH_OUT#ERR:}"
            err_msg="${err_code#*:}"
            err_status="${err_code%%:*}"
            send_err "$err_status" "$err_msg" "$resp_conn"
        else
            post_id="${uri#/posts/}"
            post_id="${post_id%/like}"
            if [[ "$post_id" == */* || "$post_id" != [1-9]* || "$post_id" == *[!0-9]* ]]; then
                send_err "400 Bad Request" "invalid post id" "$resp_conn"
            else
                query_sqlite "SELECT 1 FROM posts WHERE id = $post_id;"
                if [[ "$SQL_OUT" != "1" ]]; then
                    send_err "404 Not Found" "post not found" "$resp_conn"
                else
                    query_sqlite "INSERT INTO likes (user_id, post_id) VALUES ($AUTH_UID, $post_id) ON CONFLICT DO NOTHING RETURNING 201;"
                    if [[ "$SQL_OUT" == "201" ]]; then
                        send_resp "201 Created" "{\"liked\":true,\"already_liked\":false,\"post_id\":$post_id}" "$resp_conn"
                    else
                        send_resp "200 OK" "{\"liked\":true,\"already_liked\":true,\"post_id\":$post_id}" "$resp_conn"
                    fi
                fi
            fi
        fi

    elif [[ "$method" == "POST" && "$uri" == "/posts" ]]; then
        validate_jwt "$auth_header"
        if [[ "$AUTH_OUT" != "OK" ]]; then
            err_code="${AUTH_OUT#ERR:}"
            err_msg="${err_code#*:}"
            err_status="${err_code%%:*}"
            send_err "$err_status" "$err_msg" "$resp_conn"
        else
            escaped_username="${AUTH_USER//\'/\'\'}"
            escaped_body="${body//\'/\'\'}"
            val_sql="SELECT CASE WHEN NOT json_valid('$escaped_body') THEN 'ERR:400:malformed JSON body' WHEN typeof(json_extract('$escaped_body', '$.body')) != 'text' OR length(trim(json_extract('$escaped_body', '$.body'))) = 0 THEN 'ERR:400:body is required' WHEN length(trim(json_extract('$escaped_body', '$.body'))) > 500 THEN 'ERR:400:body must be at most 500 characters' ELSE 'OK:' || trim(json_extract('$escaped_body', '$.body')) END;"
            query_sqlite "$val_sql"

            if [[ "$SQL_OUT" == ERR:* ]]; then
                err_code="${SQL_OUT#ERR:}"
                err_msg="${err_code#*:}"
                err_status="${err_code%%:*}"
                send_err "$err_status" "$err_msg" "$resp_conn"
            elif [[ "$SQL_OUT" == OK:* ]]; then
                trimmed_body="${SQL_OUT#OK:}"
                escaped_trimmed_body="${trimmed_body//\'/\'\'}"
                query_sqlite "INSERT INTO posts (user_id, body) VALUES ($AUTH_UID, '$escaped_trimmed_body') RETURNING json_object('post', json_object('id', id, 'body', body, 'created_at', created_at, 'author', '$escaped_username', 'like_count', 0));"
                send_resp "201 Created" "$SQL_OUT" "$resp_conn"
            fi
        fi

    elif [[ "$method" == "GET" && "$uri" == "/health" ]]; then
        query_sqlite "SELECT 1"
        if [[ "$SQL_OUT" == "1" ]]; then
            now_s=""
            printf -v now_s "%(%s)T" -1
            uptime_s=$(( now_s - SERVER_START_TIME ))
            send_resp "200 OK" "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":$uptime_s}" "$resp_conn"
        else
            send_resp "503 Service Unavailable" "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"sqlite error\"}" "$resp_conn"
        fi

    else
        send_err "404 Not Found" "not found" "$resp_conn"
    fi

    if (( should_close )); then
        break
    fi
done
