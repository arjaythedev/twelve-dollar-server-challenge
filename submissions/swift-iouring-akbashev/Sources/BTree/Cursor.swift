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
    /// A one-column key (an `UnpackedRecord` and the `Mem` it points to, at `Layout`'s offsets), used once `pageReads`
    /// is on: the integer alone, with entries equal to it sorting after it.
    @usableFromInline let probeRecord: UnsafeMutableRawPointer?
    @usableFromInline let probeValue: UnsafeMutableRawPointer?
    /// Seeks use the one-column key, and counts read the leaf pages themselves (`Layout`). Off until the reads have
    /// checked that SQLite's structs are laid out as `Layout` says (`pageLayoutMismatch`, then whole counts compared).
    public var pageReads = false
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
        if pageReads, let probeRecord, let probeValue {
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

    /// `count`, a leaf page at a time: past the seek, every cell of the cursor's page is decoded from the page's bytes
    /// (`Page`), then one step moves to the next page. `.unexpected` (and `counts` partly filled) for a cell not in the
    /// form an entry (post_id, rowid) of small integers takes, which `count` then reads entry by entry.
    @inlinable public mutating func countByPage(_ lo: Int64, _ hi: Int64, into counts: inout MutableSpan<UInt32>) -> Count {
        precondition(pageReads && lo <= hi && hi - lo < Int64(counts.count))
        switch seek(atLeast: lo) {
        case .entry: break
        case .none: return .done
        case .failed: return .failed
        }
        while true {
            var last = 0
            do {
                // The page is borrowed from the cursor until the end of this block, before the cursor moves on.
                let page = self.page
                var i = cellIndex
                // An interior page holds one entry where the walk is; a leaf, a run of them.
                last = page.isLeaf ? page.cellCount : i + 1
                while i < last {
                    guard let key = page.firstInteger(ofCell: i) else { return .unexpected }
                    if key > hi { return .done }
                    if key >= lo { counts[Int(key - lo)] += 1 }
                    i += 1
                }
            }
            cellIndex = last - 1
            let rc = sqlite3BtreeNext(raw, 0)
            if rc == SQLITE_DONE { return .done }
            if rc != SQLITE_OK { return .failed }
        }
    }

    /// The page the cursor is on, borrowed until it moves.
    @inlinable var page: Page {
        @_lifetime(borrow self) borrowing get {
            let memPage = raw.load(fromByteOffset: Layout.cursorPage, as: UnsafeRawPointer.self)
            let fields = RawSpan(_unsafeBytes: UnsafeRawBufferPointer(start: memPage, count: Layout.pageSize))
            let data = fields.unsafeLoad(fromByteOffset: Layout.pageData, as: UnsafeRawPointer.self)
            let dataEnd = fields.unsafeLoad(fromByteOffset: Layout.pageDataEnd, as: UnsafeRawPointer.self)
            let cellPointers = fields.unsafeLoad(fromByteOffset: Layout.pageCellIndex, as: UnsafeRawPointer.self)
            let bytes = Span(_unsafeElements: UnsafeBufferPointer(start: data.assumingMemoryBound(to: UInt8.self),
                                                                  count: data.distance(to: dataEnd)))
            let page = Page(fields: fields, bytes: bytes, cellPointers: data.distance(to: cellPointers))
            return _overrideLifetime(page, borrowing: self)
        }
    }

    /// The entry the cursor is on, on its page (`BtCursor.ix`).
    @inlinable var cellIndex: Int {
        get { Int(raw.loadUnaligned(fromByteOffset: Layout.cursorIndex, as: UInt16.self)) }
        set { raw.storeBytes(of: UInt16(newValue), toByteOffset: Layout.cursorIndex, as: UInt16.self) }
    }

    /// On an entry: nil if the cursor's page, read through `Page`, puts the entry's cell where SQLite's own payload
    /// pointer says it is, and its fields have values a page can have; else what was read. For a record under 128
    /// bytes, the payload follows its one-byte size.
    public mutating func pageLayoutMismatch() -> String? {
        let size = sqlite3BtreePayloadSize(raw)
        var local: UInt32 = 0
        guard size < 0x80, let payload = sqlite3BtreePayloadFetch(raw, &local) else { return "a record of \(size) bytes" }
        let page = self.page
        let (leaf, childPointer, cells, mask, index) = (page.isLeaf, page.childPointerSize, page.cellCount, page.mask, cellIndex)
        let fields = "leaf \(leaf) child pointer \(childPointer) cells \(cells) mask \(mask) index \(index) bytes \(page.bytes.count)"
        guard childPointer == (leaf ? 0 : 4), index < cells, mask >= 511, mask & (mask + 1) == 0, page.bytes.count == mask + 1,
              page.cellPointers + 2 * cells <= page.bytes.count else { return fields }
        let cell = page.cellStart(index) + childPointer + 1
        let found = page.bytes.withUnsafeBufferPointer { UnsafeRawPointer($0.baseAddress!) + cell }
        return found == payload ? nil : fields + ", payload \(found.distance(to: payload)) bytes from the cell"
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

/// The B-tree page a cursor is on: its `MemPage` fields and its bytes, borrowed from the cursor until it moves.
@usableFromInline struct Page: ~Escapable {
    /// SQLite's `MemPage` for it.
    @usableFromInline let fields: RawSpan
    /// The page (`aData` up to `aDataEnd`).
    @usableFromInline let bytes: Span<UInt8>
    /// Where its cell pointer array starts in `bytes`.
    @usableFromInline let cellPointers: Int

    @inlinable @_lifetime(copy fields, copy bytes)
    init(fields: RawSpan, bytes: Span<UInt8>, cellPointers: Int) {
        self.fields = fields
        self.bytes = bytes
        self.cellPointers = cellPointers
    }

    @inlinable var isLeaf: Bool { fields.unsafeLoad(fromByteOffset: Layout.pageLeaf, as: UInt8.self) != 0 }
    /// 4 on an interior page: each cell starts with its child's page number.
    @inlinable var childPointerSize: Int { Int(fields.unsafeLoad(fromByteOffset: Layout.pageChildPointerSize, as: UInt8.self)) }
    @inlinable var cellCount: Int { Int(fields.unsafeLoadUnaligned(fromByteOffset: Layout.pageCellCount, as: UInt16.self)) }
    @inlinable var mask: Int { Int(fields.unsafeLoadUnaligned(fromByteOffset: Layout.pageMask, as: UInt16.self)) }

    /// Where cell `i` starts in `bytes`: its 2-byte big-endian pointer in the cell pointer array.
    @inlinable func cellStart(_ i: Int) -> Int {
        let at = cellPointers + 2 * i
        return (Int(bytes[at]) << 8 | Int(bytes[at + 1])) & mask
    }

    /// The first column of index cell `i`, an integer, when its record has a one-byte size (so it is all on this page),
    /// a header of its size and the column's serial type, and the column in it; nil for any other form. The body may
    /// be empty: types 8 and 9 are the integers 0 and 1, held in the type itself.
    @inlinable func firstInteger(ofCell i: Int) -> Int64? {
        let size = cellStart(i) + childPointerSize
        let record = size + 1
        guard record < bytes.count else { return nil }
        let length = Int(bytes[size])
        guard length >= 3, length < 0x80, record + length <= bytes.count else { return nil }
        let header = Int(bytes[record])
        let type = Int(bytes[record + 1])
        guard header >= 3, header <= length, type < 0x80 else { return nil }
        switch type {
        case 1...6:
            let width = type < 5 ? type : (type == 5 ? 6 : 8)
            guard header + width <= length else { return nil }
            let at = record + header
            var value = Int64(Int8(bitPattern: bytes[at]))
            for j in 1..<width { value = value << 8 | Int64(bytes[at + j]) }
            return value
        case 8, 9:
            return Int64(type - 8)
        default:
            return nil
        }
    }
}

/// Byte offsets into SQLite 3.54.1's structs (build.sh pins it), the same on x86-64 and arm64: `BtCursor`, `MemPage`,
/// `UnpackedRecord` and `Mem` (`sqlite3_value`). Only read once the reads have checked them against SQLite's own
/// answers.
@usableFromInline enum Layout {
    // BtCursor: `ix`, the entry on the current page; `pPage`, that page.
    @usableFromInline static let cursorIndex = 86, cursorPage = 136
    // MemPage (136 bytes)
    @usableFromInline static let pageSize = 136
    @usableFromInline static let pageLeaf = 8, pageChildPointerSize = 10, pageCellCount = 24, pageMask = 26
    @usableFromInline static let pageData = 80, pageDataEnd = 88, pageCellIndex = 96
    // UnpackedRecord (40 bytes): pKeyInfo, aMem, nField, default_rc, errCode, eqSeen.
    @usableFromInline static let recordSize = 40, recordKeyInfo = 0, recordValues = 8, recordFieldCount = 28
    @usableFromInline static let recordDefaultResult = 30, recordErrorCode = 31, recordEqualSeen = 34
    // Mem (56 bytes): u.i and flags; MEM_Int.
    @usableFromInline static let valueSize = 64, valueInteger = 0, valueFlags = 20
    @usableFromInline static let memInt: UInt16 = 0x0004
}
