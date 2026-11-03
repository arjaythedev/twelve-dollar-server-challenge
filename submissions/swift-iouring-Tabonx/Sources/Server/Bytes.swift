// Byte ranges and the output buffer every response is written into.

import CShim

/// A borrowed byte range. The owner of the memory keeps it alive while the view is used.
public struct ByteView {
    public var base: UnsafePointer<UInt8>?
    public var count: Int

    @inline(__always)
    public init(base: UnsafePointer<UInt8>?, count: Int) {
        self.base = base
        self.count = count
    }

    public static var empty: ByteView { ByteView(base: nil, count: 0) }

    @inline(__always)
    public subscript(i: Int) -> UInt8 { base.unsafelyUnwrapped[i] }

    @inline(__always)
    public func slice(_ from: Int, _ to: Int) -> ByteView {
        ByteView(base: base.map { $0 + from }, count: to - from)
    }

    @inline(__always)
    public func equals(_ s: StaticString) -> Bool {
        guard count == s.utf8CodeUnitCount else { return false }
        if count == 0 { return true }
        return memeq(base.unsafelyUnwrapped, s.utf8Start, count)
    }

    @inline(__always)
    public func hasPrefix(_ s: StaticString) -> Bool {
        let n = s.utf8CodeUnitCount
        guard count >= n else { return false }
        if n == 0 { return true }
        return memeq(base.unsafelyUnwrapped, s.utf8Start, n)
    }
}

@inline(__always)
func memeq(_ a: UnsafePointer<UInt8>, _ b: UnsafePointer<UInt8>, _ n: Int) -> Bool {
    var i = 0
    while i < n {
        if a[i] != b[i] { return false }
        i &+= 1
    }
    return true
}

/// A growable byte buffer that owns its memory. Not copyable, so a response buffer has exactly one owner.
public struct OutBuffer: ~Copyable {
    public private(set) var base: UnsafeMutablePointer<UInt8>
    public private(set) var capacity: Int
    public var count: Int

    public init(capacity: Int) {
        self.capacity = max(capacity, 64)
        self.base = UnsafeMutablePointer<UInt8>.allocate(capacity: self.capacity)
        self.count = 0
    }

    deinit { base.deallocate() }

    public var view: ByteView { ByteView(base: UnsafePointer(base), count: count) }

    @inline(__always)
    public mutating func reserve(_ extra: Int) {
        if count &+ extra > capacity { grow(count &+ extra) }
    }

    @inline(never)
    mutating func grow(_ needed: Int) {
        var newCapacity = capacity &* 2
        while newCapacity < needed { newCapacity &*= 2 }
        let newBase = UnsafeMutablePointer<UInt8>.allocate(capacity: newCapacity)
        newBase.update(from: base, count: count)
        base.deallocate()
        base = newBase
        capacity = newCapacity
    }

    @inline(__always)
    public mutating func append(_ byte: UInt8) {
        reserve(1)
        base[count] = byte
        count &+= 1
    }

    @inline(__always)
    public mutating func append(_ s: StaticString) {
        append(s.utf8Start, s.utf8CodeUnitCount)
    }

    @inline(__always)
    public mutating func append(_ p: UnsafePointer<UInt8>, _ n: Int) {
        reserve(n)
        (base + count).update(from: p, count: n)
        count &+= n
    }

    @inline(__always)
    public mutating func append(_ v: ByteView) {
        if v.count > 0 { append(v.base.unsafelyUnwrapped, v.count) }
    }

    /// Decimal digits of a non-negative or negative integer.
    public mutating func appendInt(_ value: Int64) {
        reserve(20)
        var v = value
        if v < 0 {
            base[count] = UInt8(ascii: "-")
            count &+= 1
            if v == Int64.min {
                append("9223372036854775808")
                return
            }
            v = -v
        }
        if v < 10 {
            base[count] = UInt8(truncatingIfNeeded: v) &+ 48
            count &+= 1
            return
        }
        var digits = 0
        var t = v
        while t > 0 { digits &+= 1; t /= 10 }
        var i = count &+ digits &- 1
        while v > 0 {
            base[i] = UInt8(truncatingIfNeeded: v % 10) &+ 48
            v /= 10
            i &-= 1
        }
        count &+= digits
    }

    /// Drops everything after `newCount`.
    @inline(__always)
    public mutating func truncate(_ newCount: Int) { count = newCount }

    /// Moves `length` bytes from `from` to `to` inside the buffer (ranges may overlap).
    public mutating func move(from: Int, to: Int, length: Int) {
        if length > 0 && from != to {
            memmove(base + to, base + from, length)
        }
    }

    /// Removes the first `n` bytes, keeping the rest.
    public mutating func consume(_ n: Int) {
        if n >= count {
            count = 0
        } else if n > 0 {
            move(from: n, to: 0, length: count &- n)
            count &-= n
        }
    }

    @inline(__always)
    public mutating func set(_ index: Int, _ byte: UInt8) { base[index] = byte }
}
