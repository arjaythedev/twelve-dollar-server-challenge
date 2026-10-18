-- JWT module (HS256) built on lib/crypto (libsodium FFI)
--
-- lunet.jsonic decode returns nil + err on invalid JSON (it does not raise),
-- so the pcall results below must also be type-checked before indexing.

local crypto = require("crypto")
local json = require("lunet.jsonic")

local jwt = {}

-- Verify a JWT token and return its payload
-- @param token: string JWT token
-- @param secret: string secret key used to verify the signature
-- @return payload: table with claims, or nil
-- @return err: string error message, or nil
function jwt.decode(token, secret)
    local header_b64, payload_b64, signature_b64 = token:match("^([^.]+)%.([^.]+)%.([^.]+)$")
    if not header_b64 then
        return nil, "Invalid token format"
    end

    local message = header_b64 .. "." .. payload_b64
    local expected_signature_b64 = crypto.base64_encode(crypto.hmac_sha256(message, secret))
    if not crypto.constant_time_compare(signature_b64, expected_signature_b64) then
        return nil, "Invalid signature"
    end

    local header_json = crypto.base64_decode(header_b64)
    local header_ok, header = pcall(json.decode, header_json)
    if not header_ok or type(header) ~= "table" or header.alg ~= "HS256" then
        return nil, "Invalid header"
    end

    local payload_json = crypto.base64_decode(payload_b64)
    local payload_ok, payload = pcall(json.decode, payload_json)
    if not payload_ok or type(payload) ~= "table" then
        return nil, "Invalid payload"
    end

    if payload.exp and payload.exp < os.time() then
        return nil, "Token expired"
    end

    return payload, nil
end

return jwt
