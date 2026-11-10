// SQLite's B-tree layer, which isn't part of its API. The amalgamation is compiled with `SQLITE_PRIVATE` defined
// empty (it would be `static`), so these functions, internal to it, link like any other; they're declared here with
// opaque pointers, as 3.54.1 has them (build.sh pins it by sha256). The one-column seek key, built in the cursor's
// own memory, follows `Layout`'s offsets, checked against SQLite at start.
import CSQLite

@usableFromInline @_extern(c, "sqlite3DbNameToBtree")
func sqlite3DbNameToBtree(_ db: OpaquePointer, _ name: UnsafePointer<CChar>) -> OpaquePointer?
@usableFromInline @_extern(c, "sqlite3BtreeCursorSize")
func sqlite3BtreeCursorSize() -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeCursorZero")
func sqlite3BtreeCursorZero(_ cursor: UnsafeMutableRawPointer)
@usableFromInline @_extern(c, "sqlite3BtreeBeginTrans")
func sqlite3BtreeBeginTrans(_ btree: OpaquePointer, _ write: Int32, _ schemaVersion: UnsafeMutablePointer<Int32>?) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeCursor")
func sqlite3BtreeCursor(_ btree: OpaquePointer, _ root: UInt32, _ write: Int32, _ keys: OpaquePointer?,
                        _ cursor: UnsafeMutableRawPointer) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeCloseCursor")
func sqlite3BtreeCloseCursor(_ cursor: UnsafeMutableRawPointer) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeTableMoveto")
func sqlite3BtreeTableMoveto(_ cursor: UnsafeMutableRawPointer, _ rowid: Int64, _ bias: Int32,
                             _ result: UnsafeMutablePointer<Int32>) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeIndexMoveto")
func sqlite3BtreeIndexMoveto(_ cursor: UnsafeMutableRawPointer, _ key: OpaquePointer,
                             _ result: UnsafeMutablePointer<Int32>) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeFirst")
func sqlite3BtreeFirst(_ cursor: UnsafeMutableRawPointer, _ empty: UnsafeMutablePointer<Int32>) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeNext")
func sqlite3BtreeNext(_ cursor: UnsafeMutableRawPointer, _ flags: Int32) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeLast")
func sqlite3BtreeLast(_ cursor: UnsafeMutableRawPointer, _ empty: UnsafeMutablePointer<Int32>) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreePrevious")
func sqlite3BtreePrevious(_ cursor: UnsafeMutableRawPointer, _ flags: Int32) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreeIntegerKey")
func sqlite3BtreeIntegerKey(_ cursor: UnsafeMutableRawPointer) -> Int64
@usableFromInline @_extern(c, "sqlite3BtreeEof")
func sqlite3BtreeEof(_ cursor: UnsafeMutableRawPointer) -> Int32
@usableFromInline @_extern(c, "sqlite3BtreePayloadFetch")
func sqlite3BtreePayloadFetch(_ cursor: UnsafeMutableRawPointer, _ local: UnsafeMutablePointer<UInt32>) -> UnsafeRawPointer?
@usableFromInline @_extern(c, "sqlite3BtreePayloadSize")
func sqlite3BtreePayloadSize(_ cursor: UnsafeMutableRawPointer) -> UInt32
@usableFromInline @_extern(c, "sqlite3KeyInfoAlloc")
func sqlite3KeyInfoAlloc(_ db: OpaquePointer, _ keyColumns: Int32, _ extra: Int32) -> OpaquePointer?
@usableFromInline @_extern(c, "sqlite3KeyInfoUnref")
func sqlite3KeyInfoUnref(_ keys: OpaquePointer)
@usableFromInline @_extern(c, "sqlite3VdbeAllocUnpackedRecord")
func sqlite3VdbeAllocUnpackedRecord(_ keys: OpaquePointer) -> OpaquePointer?
@usableFromInline @_extern(c, "sqlite3VdbeRecordUnpack")
func sqlite3VdbeRecordUnpack(_ length: Int32, _ record: UnsafeRawPointer, _ unpacked: OpaquePointer)
@usableFromInline @_extern(c, "sqlite3DbFreeNN")
func sqlite3DbFreeNN(_ db: OpaquePointer, _ allocation: UnsafeMutableRawPointer)
