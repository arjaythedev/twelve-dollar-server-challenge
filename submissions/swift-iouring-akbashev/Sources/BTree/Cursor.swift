// A Swift view of SQLite's B-trees, for reading them directly: cursors over the tables and indexes, moved inside the
// connection's open transaction, and the records decoded here (Record.swift), in place. Pages, cache, WAL and
// transaction stay SQLite's. Inlinable, so the server's calls compile as if they were its own.
public import CSQLite

/// A cursor over one of SQLite's B-trees. Its moves are `mutating`, and a record is a `Span` borrowed from it, so the
/// compiler rules out reading a record after its cursor has moved on.
public struct Cursor: ~Copyable, ~Sendable {
    @usableFromInline let db: OpaquePointer
    @usableFromInline let btree: OpaquePointer
    /// SQLite's `BtCursor`, `sqlite3BtreeCursorSize()` bytes.
    @usableFromInline let raw: UnsafeMutableRawPointer
    @usableFromInline let root: UInt32
    /// An index's key description (its key columns, then the rowid); nil for a table.
    @usableFromInline var keys: OpaquePointer?
    /// The key an index seek compares with, unpacked into SQLite's form.
    @usableFromInline var probe: OpaquePointer?
    /// A one-column key (an `UnpackedRecord` and the `Mem` it points to, at `Layout`'s offsets, in the cursor's own
    /// memory), used once `oneColumnSeeks` is on: the integer alone, with entries equal to it sorting after it.
    @usableFromInline let probeRecord: UnsafeMutableRawPointer?
    @usableFromInline let probeValue: UnsafeMutableRawPointer?
    /// Seeks use the one-column key. Off until the reads have checked that it finds what the two-column one does
    /// (`Layout`'s offsets are SQLite's).
    public var oneColumnSeeks = false
    @usableFromInline var isOpen = false
    private var released = false

    /// Over the table (`keyColumns` 0) or index whose root is page `root` of `db`'s main database, its key columns
    /// ascending (descending ones are only walked, never sought); nil when out of memory.
    public init?(_ db: OpaquePointer, root: UInt32, keyColumns: Int32 = 0) {
        guard let btree = sqlite3DbNameToBtree(db, "main"),
              let raw = sqlite3_malloc(sqlite3BtreeCursorSize()) else { return nil }
        var keys: OpaquePointer?
        var probe: OpaquePointer?
        if keyColumns > 0 {
            keys = sqlite3KeyInfoAlloc(db, keyColumns, 1)
            probe = keys.flatMap { sqlite3VdbeAllocUnpackedRecord($0) }
            guard probe != nil else {
                if let keys { sqlite3KeyInfoUnref(keys) }
                sqlite3_free(raw)
                return nil
            }
        }
        self.db = db
        self.btree = btree
        self.raw = raw
        self.root = root
        self.keys = keys
        self.probe = probe
        if let keys {
            let record = UnsafeMutableRawPointer.allocate(byteCount: Layout.recordSize, alignment: 8)
            let value = UnsafeMutableRawPointer.allocate(byteCount: Layout.valueSize, alignment: 8)
            record.initializeMemory(as: UInt8.self, repeating: 0, count: Layout.recordSize)
            value.initializeMemory(as: UInt8.self, repeating: 0, count: Layout.valueSize)
            record.storeBytes(of: UnsafeRawPointer(keys), toByteOffset: Layout.recordKeyInfo, as: UnsafeRawPointer.self)
            record.storeBytes(of: UnsafeRawPointer(value), toByteOffset: Layout.recordValues, as: UnsafeRawPointer.self)
            probeRecord = record
            probeValue = value
        } else {
            probeRecord = nil
            probeValue = nil
        }
    }

    deinit {
        if !released { Self.free(db, raw, keys, probe, open: isOpen) }
        probeRecord?.deallocate()
        probeValue?.deallocate()
    }

    /// Frees the cursor. `keys` and `probe` came from the connection's allocator, so this comes before it closes.
    public mutating func release() {
        guard !released else { return }
        Self.free(db, raw, keys, probe, open: isOpen)
        isOpen = false
        released = true
    }

    private static func free(_ db: OpaquePointer, _ raw: UnsafeMutableRawPointer, _ keys: OpaquePointer?,
                             _ probe: OpaquePointer?, open: Bool) {
        if open { _ = sqlite3BtreeCloseCursor(raw) }
        if let probe { sqlite3DbFreeNN(db, UnsafeMutableRawPointer(probe)) }
        if let keys { sqlite3KeyInfoUnref(keys) }
        sqlite3_free(raw)
    }

    /// Inside the connection's open transaction (starting the B-tree's read transaction if no statement has yet);
    /// closed before it ends.
    @inlinable public mutating func open() -> Bool {
        if isOpen { return true }
        guard sqlite3BtreeBeginTrans(btree, 0, nil) == SQLITE_OK else { return false }
        sqlite3BtreeCursorZero(raw)
        isOpen = sqlite3BtreeCursor(btree, root, 0, keys, raw) == SQLITE_OK
        return isOpen
    }

    @inlinable public mutating func close() {
        guard isOpen else { return }
        _ = sqlite3BtreeCloseCursor(raw)
        isOpen = false
    }

    /// The table row `rowid`. From the row just before it (rows read in ascending order), SQLite's seek takes one step
    /// along the leaf instead of a descent from the root.
    @inlinable public mutating func seek(_ rowid: Int64) -> Move {
        var res: Int32 = 0
        guard sqlite3BtreeTableMoveto(raw, rowid, 0, &res) == SQLITE_OK else { return .failed }
        // An exact match leaves the cursor on it.
        return res == 0 ? .entry : .none
    }

    /// The first index entry whose first column, an integer, is at least `key`.
    @inlinable public mutating func seek(atLeast key: Int64) -> Move {
        if oneColumnSeeks, let probeRecord, let probeValue {
            // The integer alone, as a `Mem`; with `default_rc` +1 every entry equal to it sorts after it, so the cursor
            // lands on the first of them or just before.
            probeValue.storeBytes(of: key, toByteOffset: Layout.valueInteger, as: Int64.self)
            probeValue.storeBytes(of: Layout.memInt, toByteOffset: Layout.valueFlags, as: UInt16.self)
            probeRecord.storeBytes(of: 1, toByteOffset: Layout.recordFieldCount, as: UInt16.self)
            probeRecord.storeBytes(of: 1, toByteOffset: Layout.recordDefaultResult, as: Int8.self)
            probeRecord.storeBytes(of: 0, toByteOffset: Layout.recordErrorCode, as: UInt8.self)
            probeRecord.storeBytes(of: 0, toByteOffset: Layout.recordEqualSeen, as: UInt8.self)
            var res: Int32 = 0
            guard sqlite3BtreeIndexMoveto(raw, OpaquePointer(probeRecord), &res) == SQLITE_OK else { return .failed }
            if sqlite3BtreeEof(raw) != 0 { return .none }
            return res < 0 ? next() : .entry
        }
        // It seeks to (key, the smallest rowid), which every entry of `key` sorts after, so the cursor lands just before
        // the first of them.
        guard let probe else { return .failed }
        // A record of two 8-byte integers (serial type 6): header [3, 6, 6], then each value big-endian.
        var bytes = InlineArray<19, UInt8>(repeating: 0)
        bytes[0] = 3
        bytes[1] = 6
        bytes[2] = 6
        for i in 0..<8 {
            bytes[3 + i] = UInt8(truncatingIfNeeded: UInt64(bitPattern: key) >> (56 - 8 * i))
            bytes[11 + i] = UInt8(truncatingIfNeeded: UInt64(bitPattern: Int64.min) >> (56 - 8 * i))
        }
        bytes.span.withUnsafeBytes { sqlite3VdbeRecordUnpack(Int32($0.count), $0.baseAddress!, probe) }
        var res: Int32 = 0
        guard sqlite3BtreeIndexMoveto(raw, probe, &res) == SQLITE_OK else { return .failed }
        if sqlite3BtreeEof(raw) != 0 { return .none }
        return res < 0 ? next() : .entry
    }

    @inlinable public mutating func first() -> Move {
        var empty: Int32 = 1
        guard sqlite3BtreeFirst(raw, &empty) == SQLITE_OK else { return .failed }
        return empty == 0 ? .entry : .none
    }

    @inlinable public mutating func next() -> Move {
        let rc = sqlite3BtreeNext(raw, 0)
        return rc == SQLITE_OK ? .entry : rc == SQLITE_DONE ? .none : .failed
    }

    /// `counts[key - lo]` += the entries of this index (on one integer column) with each key in `lo...hi`.
    @inlinable public mutating func count(_ lo: Int64, _ hi: Int64, into counts: inout MutableSpan<UInt32>) -> Count {
        precondition(lo <= hi && hi - lo < Int64(counts.count))
        var move = seek(atLeast: lo)
        while move == .entry {
            guard let key = firstInteger() else { return .unexpected }
            if key > hi { return .done }
            if key >= lo { counts[Int(key - lo)] += 1 }
            move = next()
        }
        return move == .none ? .done : .failed
    }

    /// The current entry's first column, an integer; nil if it isn't one.
    @inlinable borrowing func firstInteger() -> Int64? {
        let entry = record
        guard let column = Record.columns(entry) as InlineArray<1, Record.Column>? else { return nil }
        return Record.integer(entry, column[0])
    }

    /// The current entry's record; empty when it spills onto overflow pages.
    @inlinable public var record: Span<UInt8> {
        @_lifetime(borrow self) borrowing get {
            // PayloadSize parses the entry's cell; PayloadFetch only reads what that left (in this order, as SQLite's
            // own column reads do).
            let size = sqlite3BtreePayloadSize(raw)
            var local: UInt32 = 0
            let base = sqlite3BtreePayloadFetch(raw, &local)
            let length = local == size ? Int(local) : 0
            let span = Span(_unsafeElements: UnsafeBufferPointer(start: base?.assumingMemoryBound(to: UInt8.self), count: length))
            return _overrideLifetime(span, borrowing: self)
        }
    }

    public enum Move {
        case entry, none, failed
    }

    public enum Count {
        case done, unexpected, failed
    }
}

/// Byte offsets into SQLite 3.54.1's structs (build.sh pins it), the same on x86-64 and arm64: `UnpackedRecord` and
/// `Mem` (`sqlite3_value`), which the one-column seek key fills in the cursor's own memory. Only used once the reads
/// have checked seeks with it against the two-column key.
@usableFromInline enum Layout {
    // UnpackedRecord (40 bytes): pKeyInfo, aMem, nField, default_rc, errCode, eqSeen.
    @usableFromInline static let recordSize = 40, recordKeyInfo = 0, recordValues = 8, recordFieldCount = 28
    @usableFromInline static let recordDefaultResult = 30, recordErrorCode = 31, recordEqualSeen = 34
    // Mem (56 bytes): u.i and flags; MEM_Int.
    @usableFromInline static let valueSize = 64, valueInteger = 0, valueFlags = 20
    @usableFromInline static let memInt: UInt16 = 0x0004
}
