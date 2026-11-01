// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104). Written here so the HMAC pads are hashed once per
// process, which a library API doesn't expose.

private let roundConstants: [UInt32] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]

/// The eight working words of SHA-256.
public struct SHA256State {
    var h0: UInt32 = 0x6a09e667, h1: UInt32 = 0xbb67ae85, h2: UInt32 = 0x3c6ef372, h3: UInt32 = 0xa54ff53a
    var h4: UInt32 = 0x510e527f, h5: UInt32 = 0x9b05688c, h6: UInt32 = 0x1f83d9ab, h7: UInt32 = 0x5be0cd19

    public init() {}

    @inline(__always)
    func rotr(_ x: UInt32, _ n: UInt32) -> UInt32 { (x >> n) | (x << (32 &- n)) }

    /// Processes one 64-byte block.
    mutating func compress(_ block: UnsafePointer<UInt8>, _ w: UnsafeMutablePointer<UInt32>) {
        for t in 0..<16 {
            let b = block + t &* 4
            w[t] = UInt32(b[0]) << 24 | UInt32(b[1]) << 16 | UInt32(b[2]) << 8 | UInt32(b[3])
        }
        for t in 16..<64 {
            let x = w[t &- 15], y = w[t &- 2]
            let s0 = rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3)
            let s1 = rotr(y, 17) ^ rotr(y, 19) ^ (y >> 10)
            w[t] = w[t &- 16] &+ s0 &+ w[t &- 7] &+ s1
        }
        var a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7
        roundConstants.withUnsafeBufferPointer { k in
            for t in 0..<64 {
                let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
                let ch = (e & f) ^ (~e & g)
                let t1 = h &+ s1 &+ ch &+ k[t] &+ w[t]
                let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
                let maj = (a & b) ^ (a & c) ^ (b & c)
                let t2 = s0 &+ maj
                h = g; g = f; f = e; e = d &+ t1
                d = c; c = b; b = a; a = t1 &+ t2
            }
        }
        h0 &+= a; h1 &+= b; h2 &+= c; h3 &+= d
        h4 &+= e; h5 &+= f; h6 &+= g; h7 &+= h
    }

    func write(_ out: UnsafeMutablePointer<UInt8>) {
        put(h0, out); put(h1, out + 4); put(h2, out + 8); put(h3, out + 12)
        put(h4, out + 16); put(h5, out + 20); put(h6, out + 24); put(h7, out + 28)
    }

    @inline(__always)
    func put(_ word: UInt32, _ out: UnsafeMutablePointer<UInt8>) {
        out[0] = UInt8(truncatingIfNeeded: word >> 24)
        out[1] = UInt8(truncatingIfNeeded: word >> 16)
        out[2] = UInt8(truncatingIfNeeded: word >> 8)
        out[3] = UInt8(truncatingIfNeeded: word)
    }
}

/// HMAC-SHA256 with a fixed key. The inner and outer pad blocks are compressed once in `init`.
public struct HMACSHA256 {
    let inner: SHA256State
    let outer: SHA256State
    let w: UnsafeMutablePointer<UInt32>
    let block: UnsafeMutablePointer<UInt8>

    public init(key: ByteView) {
        w = .allocate(capacity: 64)
        block = .allocate(capacity: 128)
        let keyBlock = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
        keyBlock.initialize(repeating: 0, count: 64)
        if key.count > 64 {
            var s = SHA256State()
            Self.hash(&s, key, prefixLength: 0, w: w, block: block)
            s.write(keyBlock)
        } else if key.count > 0 {
            keyBlock.update(from: key.base.unsafelyUnwrapped, count: key.count)
        }
        var pad = UnsafeMutablePointer<UInt8>.allocate(capacity: 64)
        for i in 0..<64 { pad[i] = keyBlock[i] ^ 0x36 }
        var i = SHA256State()
        i.compress(pad, w)
        for j in 0..<64 { pad[j] = keyBlock[j] ^ 0x5c }
        var o = SHA256State()
        o.compress(pad, w)
        inner = i
        outer = o
        pad.deallocate()
        pad = keyBlock
        pad.deallocate()
    }

    /// Hashes `message` onto `state`, which has already absorbed `prefixLength` bytes (a multiple of 64).
    static func hash(
        _ state: inout SHA256State, _ message: ByteView, prefixLength: Int,
        w: UnsafeMutablePointer<UInt32>, block: UnsafeMutablePointer<UInt8>
    ) {
        let n = message.count
        var offset = 0
        if let p = message.base {
            while n &- offset >= 64 {
                state.compress(p + offset, w)
                offset &+= 64
            }
        }
        let rest = n &- offset
        if rest > 0 { block.update(from: message.base.unsafelyUnwrapped + offset, count: rest) }
        block[rest] = 0x80
        let padded = rest &+ 1 &+ 8 <= 64 ? 64 : 128
        (block + (rest &+ 1)).initialize(repeating: 0, count: padded &- rest &- 1)
        let bits = UInt64(prefixLength &+ n) &* 8
        for k in 0..<8 { block[padded &- 1 &- k] = UInt8(truncatingIfNeeded: bits >> (UInt64(k) &* 8)) }
        state.compress(block, w)
        if padded == 128 { state.compress(block + 64, w) }
    }

    /// Writes the 32-byte MAC of `message` into `out`.
    public func mac(_ message: ByteView, into out: UnsafeMutablePointer<UInt8>) {
        var s = inner
        Self.hash(&s, message, prefixLength: 64, w: w, block: block)
        let digest = block + 64
        s.write(digest)
        var o = outer
        Self.hash(&o, ByteView(base: UnsafePointer(digest), count: 32), prefixLength: 64, w: w, block: block)
        o.write(out)
    }

    /// Plain SHA-256, for tests.
    public func sha256(_ message: ByteView, into out: UnsafeMutablePointer<UInt8>) {
        var s = SHA256State()
        Self.hash(&s, message, prefixLength: 0, w: w, block: block)
        s.write(out)
    }
}
