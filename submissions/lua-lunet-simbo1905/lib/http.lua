-- HTTP parsing and response building helpers for Lunet

local http = {}

-- This demo supports a small, explicit HTTP/1.1 framing subset: origin-form
-- requests, CRLF headers, at most one value for each header field, and an
-- optional decimal Content-Length body. Chunked and every other
-- Transfer-Encoding are rejected. Responses are persistent by default
-- (HTTP/1.1 keeps the connection open); pass persistent=false for a
-- Connection: close response. Pipelining is excluded: a chunk that runs past
-- the end of a request body is a hard 400.
http.MAX_HEADER_BYTES = 16 * 1024
http.MAX_BODY_BYTES = 1024 * 1024

local function request_error(message, status)
    return nil, message, status or 400
end

local function parse_content_length(value)
    if not value:match("^%d+$") then
        return request_error("Invalid Content-Length")
    end

    local length = 0
    for i = 1, #value do
        length = length * 10 + tonumber(value:sub(i, i))
        if length > http.MAX_BODY_BYTES then
            return request_error("Request body too large", 413)
        end
    end
    return length
end

local function parse_request_head(head)
    local first_line, headers_raw = head:match("^([^\r\n]*)\r\n(.*)$")
    first_line = first_line or head
    headers_raw = headers_raw or ""

    local method, path, version = first_line:match("^([!#$%%&'%*%+%-%._%^_`|~%w]+) ([^ %c]+) (HTTP/%d+%.%d+)$")
    if not method or path:sub(1, 1) ~= "/" then
        return request_error("Invalid HTTP request")
    end
    if version ~= "HTTP/1.1" then
        return request_error("Only HTTP/1.1 is supported", 505)
    end

    local headers = {}
    if headers_raw ~= "" then
        for line in (headers_raw .. "\r\n"):gmatch("(.-)\r\n") do
            local name, value = line:match("^([^:]+):(.*)$")
            if not name or not name:match("^[!#$%%&'%*%+%-%._%^_`|~%w]+$") then
                return request_error("Invalid HTTP header")
            end

            local key = name:lower()
            if headers[key] ~= nil then
                return request_error("Duplicate HTTP header")
            end
            headers[key] = value:gsub("^[ \t]+", ""):gsub("[ \t]+$", "")
        end
    end

    if not headers.host or headers.host == "" then
        return request_error("Missing Host header")
    end
    if headers["transfer-encoding"] then
        return request_error("Transfer-Encoding is not supported")
    end

    local content_length = 0
    if headers["content-length"] then
        local length, length_err, length_status = parse_content_length(headers["content-length"])
        if length == nil then return nil, length_err, length_status end
        content_length = length
    end

    -- A query string is allowed but nothing here consumes it: split it off so
    -- it never reaches the router.
    local query_start = path:find("?", 1, true)
    if query_start then
        path = path:sub(1, query_start - 1)
    end

    return {
        method = method,
        path = path,
        version = version,
        headers = headers,
        content_length = content_length,
    }
end

-- Finalize a fully received request: attach the body, drop the framing field.
local function finalize(request, body)
    request.body = body
    request.content_length = nil
    return request
end

-- Incrementally read exactly one supported request. The reader must return a
-- string chunk, or nil plus an optional error on EOF/failure.
function http.read_request(read_chunk)
    local header_chunks = {}
    local header_size = 0
    local request_head
    local body_chunks
    local body_size = 0
    local expected_body_size
    local saw_bytes = false

    while true do
        local chunk, read_err = read_chunk()
        if not chunk then
            if not saw_bytes then return nil, read_err end
            return request_error(read_err or "Incomplete HTTP request")
        end
        if type(chunk) ~= "string" then
            return request_error("Invalid socket read")
        end
        saw_bytes = true

        if not request_head then
            header_chunks[#header_chunks + 1] = chunk
            header_size = header_size + #chunk
            local received = table.concat(header_chunks)
            local header_end = received:find("\r\n\r\n", 1, true)
            if not header_end then
                if header_size > http.MAX_HEADER_BYTES then
                    return request_error("Request headers too large", 431)
                end
            else
                if header_end + 3 > http.MAX_HEADER_BYTES then
                    return request_error("Request headers too large", 431)
                end

                request_head = received:sub(1, header_end - 1)
                local request, parse_err, parse_status = parse_request_head(request_head)
                if not request then return nil, parse_err, parse_status end
                expected_body_size = request.content_length

                body_chunks = { received:sub(header_end + 4) }
                body_size = #body_chunks[1]
                if body_size > http.MAX_BODY_BYTES then
                    return request_error("Request body too large", 413)
                end
                if body_size > expected_body_size then
                    return request_error("Unexpected data after request body")
                end
                if body_size == expected_body_size then
                    return finalize(request, table.concat(body_chunks))
                end
            end
        else
            body_size = body_size + #chunk
            if body_size > http.MAX_BODY_BYTES then
                return request_error("Request body too large", 413)
            end
            body_chunks[#body_chunks + 1] = chunk

            if body_size > expected_body_size then
                return request_error("Unexpected data after request body")
            end
            if body_size == expected_body_size then
                return finalize(request, table.concat(body_chunks))
            end
        end
    end
end

-- Build HTTP response. persistent=false emits Connection: close; a default
-- (nil/true) response leaves the connection persistent per HTTP/1.1.
function http.response(status, headers, body, persistent)
    local status_text = {
        [200] = "OK",
        [201] = "Created",
        [400] = "Bad Request",
        [401] = "Unauthorized",
        [404] = "Not Found",
        [413] = "Payload Too Large",
        [431] = "Request Header Fields Too Large",
        [500] = "Internal Server Error",
        [503] = "Service Unavailable",
        [505] = "HTTP Version Not Supported",
    }
    
    local status_line = string.format("HTTP/1.1 %d %s", status, status_text[status] or "Unknown")
    
    local normalized_headers = {}
    for name, value in pairs(headers or {}) do
        local key = tostring(name):lower()
        -- Framing is owned by this function: no duplicate content length or
        -- conflicting persistent-connection declaration can be emitted.
        if key ~= "content-length" and key ~= "connection" then
            normalized_headers[key] = value
        end
    end
    normalized_headers["content-type"] = normalized_headers["content-type"] or "application/json"
    if not persistent then
        normalized_headers["connection"] = "close"
    end
    
    -- Build header lines
    local header_lines = {status_line}
    for k, v in pairs(normalized_headers) do
        header_lines[#header_lines + 1] = string.format("%s: %s", k, tostring(v))
    end
    
    body = body or ""
    header_lines[#header_lines + 1] = string.format("Content-Length: %d", #body)
    
    return table.concat(header_lines, "\r\n") .. "\r\n\r\n" .. body
end

-- JSON response helper
function http.json_response(status, data, persistent)
    local json = require("lunet.jsonic")
    local body = json.encode(data)
    return http.response(status, { ["Content-Type"] = "application/json" }, body, persistent)
end

return http
