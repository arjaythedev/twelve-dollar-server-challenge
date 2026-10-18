-- Cryptography utilities: HMAC-SHA256 and constant-time comparison via
-- libsodium FFI; base64url via lunet.sodium (the linked library). Nothing
-- here hashes or generates secrets.

local ffi = require("ffi")
local sodium_mod = require("lunet.sodium")
local crypto = {}

ffi.cdef[[
    int sodium_init(void);

    // HMAC-SHA256
    int crypto_auth_hmacsha256(
        unsigned char *out,
        const unsigned char *in,
        unsigned long long inlen,
        const unsigned char *k
    );

    // Constant time comparison
    int sodium_memcmp(const void * const b1_, const void * const b2_, size_t len);
]]

local sodium = ffi.load("sodium")
local initialized = false

local function init()
    if not initialized then
        if sodium.sodium_init() < 0 then
            error("libsodium initialization failed")
        end
        initialized = true
    end
end

local HMAC_BYTES = 32
local HMAC_KEYBYTES = 32

function crypto.hmac_sha256(message, key)
    init()
    if #key < HMAC_KEYBYTES then
        local padded = ffi.new("unsigned char[?]", HMAC_KEYBYTES)
        ffi.copy(padded, key, #key)
        key = ffi.string(padded, HMAC_KEYBYTES)
    elseif #key > HMAC_KEYBYTES then
        key = key:sub(1, HMAC_KEYBYTES)
    end
    local out = ffi.new("unsigned char[?]", HMAC_BYTES)
    sodium.crypto_auth_hmacsha256(out, message, #message, key)
    return ffi.string(out, HMAC_BYTES)
end

function crypto.constant_time_compare(a, b)
    init()
    if #a ~= #b then return false end
    return sodium.sodium_memcmp(a, b, #a) == 0
end

-- Unpadded base64url, the JWT profile, via the linked library. The library
-- raises its diagnostic on malformed input; decode reports failure as nil so
-- callers keep deciding the response status.

function crypto.base64_encode(data)
    return sodium_mod.base64_encode(data, sodium_mod.BASE64_VARIANT_URLSAFE_NO_PADDING)
end

function crypto.base64_decode(data)
    local ok, decoded = pcall(sodium_mod.base64_decode, data, sodium_mod.BASE64_VARIANT_URLSAFE_NO_PADDING)
    if not ok then
        return nil
    end
    return decoded
end

return crypto
