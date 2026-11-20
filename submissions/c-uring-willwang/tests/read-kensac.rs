//! Adapter for the original compiled db.rs, json.rs and C shim, for the
//! sequential comparison driver. No request code is replaced.
#![allow(dead_code)]
mod db;
mod json;
use std::ffi::{c_char, c_int, CStr};

pub struct State { db: db::Db, out: Vec<u8> }
#[repr(C)]
pub struct Bytes { data: *const u8, len: usize }

#[no_mangle]
pub unsafe extern "C" fn adapter_open(path: *const c_char) -> *mut State {
    let Ok(path) = CStr::from_ptr(path).to_str() else { return std::ptr::null_mut() };
    let Ok(db) = db::Db::open(path) else { return std::ptr::null_mut() };
    Box::into_raw(Box::new(State { db, out: Vec::new() }))
}
#[no_mangle]
pub extern "C" fn adapter_commit(s: &mut State) -> c_int {
    if !s.db.commit() { return 0 }
    // Advance checkpoint bookkeeping; the shared replay controls admission.
    s.db.tick();
    1
}
#[no_mangle]
pub extern "C" fn adapter_read(s: &mut State, point: c_int, id: i64, out: &mut Bytes) -> c_int {
    s.out.clear();
    let ok = if point == 0 { s.db.feed(&mut s.out).is_ok() }
             else { matches!(s.db.post(id, &mut s.out), Ok(db::PostLookup::Found)) };
    *out = Bytes { data: s.out.as_ptr(), len: s.out.len() };
    ok as c_int
}
#[no_mangle]
pub unsafe extern "C" fn adapter_create(s: &mut State, uid: i64, body: *const c_char, id: &mut i64) -> c_int {
    let Ok((new, _, _)) = s.db.insert_post(uid, CStr::from_ptr(body).to_bytes()) else { return 0 };
    *id = new;
    1
}
#[no_mangle]
pub extern "C" fn adapter_like(s: &mut State, uid: i64, id: i64) -> c_int {
    s.db.insert_like(uid, id).is_ok() as c_int
}
