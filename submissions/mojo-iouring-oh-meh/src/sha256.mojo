# SHA-256 and HMAC-SHA256, written here so JWT verification is Mojo end to end.
# The HMAC keeps the inner and outer pad states hashed once, so verifying a
# token costs the message blocks plus one outer block. No allocation per call.
from bytes import Bytes, balloc, bcopy, bzero, calloc_t

comptime W32 = Pointer[UInt32, MutUntrackedOrigin]


def make_k() -> W32:
    var k = calloc_t[UInt32](64)
    var vals: List[UInt32] = [
        0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
        0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
        0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
        0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
        0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
        0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
        0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
        0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
    ]
    for i in range(64):
        k[unsafe_offset=i] = vals[i]
    return k


@always_inline
def rotr(x: UInt32, n: UInt32) -> UInt32:
    return (x >> n) | (x << (UInt32(32) - n))


@always_inline
def load_be32(p: Bytes, i: Int) -> UInt32:
    return (
        (UInt32(p[unsafe_offset=i]) << 24)
        | (UInt32(p[unsafe_offset=i + 1]) << 16)
        | (UInt32(p[unsafe_offset=i + 2]) << 8)
        | UInt32(p[unsafe_offset=i + 3])
    )


@always_inline
def store_be32(p: Bytes, i: Int, v: UInt32):
    p[unsafe_offset=i] = UInt8((v >> 24) & 0xFF)
    p[unsafe_offset=i + 1] = UInt8((v >> 16) & 0xFF)
    p[unsafe_offset=i + 2] = UInt8((v >> 8) & 0xFF)
    p[unsafe_offset=i + 3] = UInt8(v & 0xFF)


# One compression round: state (8 words) ^= block (64 bytes). w is 64 words of scratch.
def compress(state: W32, block: Bytes, k: W32, w: W32):
    for i in range(16):
        w[unsafe_offset=i] = load_be32(block, i * 4)
    for i in range(16, 64):
        var w15 = w[unsafe_offset=i - 15]
        var w2 = w[unsafe_offset=i - 2]
        var s0 = rotr(w15, 7) ^ rotr(w15, 18) ^ (w15 >> 3)
        var s1 = rotr(w2, 17) ^ rotr(w2, 19) ^ (w2 >> 10)
        w[unsafe_offset=i] = w[unsafe_offset=i - 16] + s0 + w[unsafe_offset=i - 7] + s1
    var a = state[unsafe_offset=0]
    var b = state[unsafe_offset=1]
    var c = state[unsafe_offset=2]
    var d = state[unsafe_offset=3]
    var e = state[unsafe_offset=4]
    var f = state[unsafe_offset=5]
    var g = state[unsafe_offset=6]
    var h = state[unsafe_offset=7]
    for i in range(64):
        var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
        var ch = (e & f) ^ ((~e) & g)
        var t1 = h + S1 + ch + k[unsafe_offset=i] + w[unsafe_offset=i]
        var S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
        var maj = (a & b) ^ (a & c) ^ (b & c)
        var t2 = S0 + maj
        h = g
        g = f
        f = e
        e = d + t1
        d = c
        c = b
        b = a
        a = t1 + t2
    state[unsafe_offset=0] += a
    state[unsafe_offset=1] += b
    state[unsafe_offset=2] += c
    state[unsafe_offset=3] += d
    state[unsafe_offset=4] += e
    state[unsafe_offset=5] += f
    state[unsafe_offset=6] += g
    state[unsafe_offset=7] += h


def init_state(state: W32):
    state[unsafe_offset=0] = 0x6A09E667
    state[unsafe_offset=1] = 0xBB67AE85
    state[unsafe_offset=2] = 0x3C6EF372
    state[unsafe_offset=3] = 0xA54FF53A
    state[unsafe_offset=4] = 0x510E527F
    state[unsafe_offset=5] = 0x9B05688C
    state[unsafe_offset=6] = 0x1F83D9AB
    state[unsafe_offset=7] = 0x5BE0CD19


struct Sha256:
    var k: W32
    var w: W32
    var state: W32
    var fresh: W32      # the initial state, for plain hashes
    var block: Bytes    # 128 bytes: the padded tail of a message
    var ih: Bytes       # 32 bytes: HMAC inner hash
    # HMAC: states after absorbing key^ipad and key^opad (one block each).
    var inner: W32
    var outer: W32

    def __init__(out self):
        self.k = make_k()
        self.w = calloc_t[UInt32](64)
        self.state = calloc_t[UInt32](8)
        self.fresh = calloc_t[UInt32](8)
        init_state(self.fresh)
        self.block = balloc(128)
        self.ih = balloc(32)
        self.inner = calloc_t[UInt32](8)
        self.outer = calloc_t[UInt32](8)

    # Hash `msg` continuing from `start` (already absorbed `prefix_blocks` blocks) into dst (32 bytes).
    def _finish(mut self, start: W32, prefix_blocks: Int, msg: Bytes, n: Int, dst: Bytes):
        for i in range(8):
            self.state[unsafe_offset=i] = start[unsafe_offset=i]
        var off = 0
        while n - off >= 64:
            compress(self.state, msg.unsafe_offset(off), self.k, self.w)
            off += 64
        var rem = n - off
        bzero(self.block, 128)
        bcopy(self.block, msg.unsafe_offset(off), rem)
        self.block[unsafe_offset=rem] = 0x80
        var total_bits = UInt64((prefix_blocks * 64 + n) * 8)
        var last = 64 if rem < 56 else 128
        for i in range(8):
            self.block[unsafe_offset=last - 1 - i] = UInt8((total_bits >> UInt64(i * 8)) & 0xFF)
        compress(self.state, self.block, self.k, self.w)
        if last == 128:
            compress(self.state, self.block.unsafe_offset(64), self.k, self.w)
        for i in range(8):
            store_be32(dst, i * 4, self.state[unsafe_offset=i])

    def hash(mut self, msg: Bytes, n: Int, dst: Bytes):
        self._finish(self.fresh, 0, msg, n, dst)

    # The HMAC pad block: key (zero-padded, or hashed if longer than a block) xor `x`.
    def _pad_state(mut self, key: Bytes, n: Int, x: UInt8, state: W32):
        var k = balloc(64)
        bzero(k, 64)
        if n > 64:
            self.hash(key, n, k)
        else:
            bcopy(k, key, n)
        for i in range(64):
            k[unsafe_offset=i] ^= x
        init_state(state)
        compress(state, k, self.k, self.w)

    def set_key(mut self, key: Bytes, n: Int):
        self._pad_state(key, n, 0x36, self.inner)
        self._pad_state(key, n, 0x5C, self.outer)

    # HMAC-SHA256(key, msg) -> dst (32 bytes).
    def hmac(mut self, msg: Bytes, n: Int, dst: Bytes):
        self._finish(self.inner, 1, msg, n, self.ih)
        self._finish(self.outer, 1, self.ih, 32, dst)
