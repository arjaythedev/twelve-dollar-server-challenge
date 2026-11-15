// Bounded native input buffers, with exact-length Goose spans for overflow.
import slice_pool;
import slot_pool;
import src.model;
import src.native;
namespace input_buffers;

// Both handle tables fit below this bit. Keep the connection's existing i32 handle.
let POOLED: i32 = 1 << 30;
struct Span { offset: u32, length: u32 }
var storage = slice_pool<u8> {};
var handles = slot_pool<Span> {};

fn span(handle: i32) -> Span {
    assert(handle >= 0); assert(handle < handles.items.len);
    let value = handles.items[handle];
    assert(value.length > 0);
    return value;
}

fn save_pooled(bytes: const u8[:]) -> i32 {
    if handles.free_slots.len == 0 && handles.items.len >= model::MAX_RINGS * 65535 { return -1; }
    // Reserve worst-case growth before allocation, then account the actual high-water length.
    // This can reject near the shared limit even when a free span would have fitted.
    if !native::input_reserve(storage.items.len + bytes.len) { return -1; }
    let saved = storage.alloc_copy(bytes);
    let accounted = native::input_reserve(storage.items.len);
    assert(accounted);
    let offset = storage.items.range_index(saved);
    let handle = handles.alloc_index(Span { offset: offset as! u32, length: saved.len as! u32 }) as! i32;
    assert(handle < POOLED);
    return handle | POOLED;
}

fn save(bytes: const u8[:]) -> i32 {
    if bytes.len <= 0 || bytes.len > model::MAX_HEAD + model::MAX_BODY { return -1; }
    let handle = native::input_new(bytes.len);
    if handle < 0 { return save_pooled(bytes); }
    assert(handle < POOLED);
    let copied = native::input_append(handle, bytes);
    assert(copied); // The new buffer already has capacity for this payload.
    return handle;
}

fn append(handle: i32&, bytes: const u8[:]) -> bool {
    assert(handle >= 0);
    if (handle & POOLED) == 0 {
        if native::box_len(handle) + bytes.len > model::MAX_HEAD + model::MAX_BODY { return false; }
        if bytes.len == 0 { return true; }
        if native::input_append(handle, bytes) { return true; }
        // Keep the original buffer alive until the entire replacement is owned.
        var moved: u8[>..] = [];
        native::box_read(handle, moved);
        moved.append(bytes);
        let replacement = save_pooled(moved);
        if replacement < 0 { return false; }
        native::input_drop(handle);
        handle = replacement;
        return true;
    }
    let index = handle - POOLED;
    let old = span(index);
    let length = old.length as i64 + bytes.len;
    if length > model::MAX_HEAD + model::MAX_BODY { return false; }
    if bytes.len == 0 { return true; }
    if !native::input_reserve(storage.items.len + length) { return false; }
    let offset = old.offset as i64;
    let grown = storage.realloc_slice(storage.items[offset..offset + old.length], length);
    grown[old.length..].move_from(bytes);
    let accounted = native::input_reserve(storage.items.len);
    assert(accounted);
    handles.items[index] = Span {
        offset: storage.items.range_index(grown) as! u32, length: length as! u32,
    };
    return true;
}

fn read(handle: i32, out: u8[>..]&) {
    assert(handle >= 0);
    if (handle & POOLED) == 0 { native::box_read(handle, out); return; }
    let value = span(handle - POOLED);
    let offset = value.offset as i64;
    out.append(storage.items[offset..offset + value.length]);
}

fn release(handle: i32) {
    assert(handle >= 0);
    if (handle & POOLED) == 0 { native::input_drop(handle); return; }
    let index = handle - POOLED;
    let value = span(index);
    let offset = value.offset as i64;
    storage.free_slice(storage.items[offset..offset + value.length]);
    handles.items[index] = Span { offset: 0, length: 0 };
    handles.free(index);
}
