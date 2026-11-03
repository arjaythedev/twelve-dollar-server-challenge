// parser/cmp/hx_httparse.rs (sha256 e6898048ee12), developed separately for this entry.
// httparse (vendor/httparse, v1.10.1) behind picohttpparser's phr_parse_request signature, so the
// benchmark applies the identical routing/header logic to every general parser.
extern crate httparse;
use std::os::raw::{c_char, c_int};

#[repr(C)]
pub struct PhrHeader { name: *const c_char, name_len: usize, value: *const c_char, value_len: usize }

/// returns bytes consumed, -1 invalid, -2 incomplete (as pico)
#[no_mangle]
pub unsafe extern "C" fn hx_parse_request(buf: *const c_char, len: usize, method: *mut *const c_char, method_len: *mut usize,
    path: *mut *const c_char, path_len: *mut usize, minor: *mut c_int, headers: *mut PhrHeader, num_headers: *mut usize,
    _last_len: usize) -> c_int {
    let slice = std::slice::from_raw_parts(buf as *const u8, len);
    let mut h = [httparse::EMPTY_HEADER; 48];
    let max = (*num_headers).min(48);
    let mut req = httparse::Request::new(&mut h[..max]);
    match req.parse(slice) {
        Ok(httparse::Status::Complete(n)) => {
            let m = req.method.unwrap(); *method = m.as_ptr() as *const c_char; *method_len = m.len();
            let p = req.path.unwrap(); *path = p.as_ptr() as *const c_char; *path_len = p.len();
            *minor = req.version.unwrap() as c_int;
            let hs = req.headers;   // trimmed to the parsed headers
            for (i, x) in hs.iter().enumerate() {
                let o = &mut *headers.add(i);
                o.name = x.name.as_ptr() as *const c_char; o.name_len = x.name.len();
                o.value = x.value.as_ptr() as *const c_char; o.value_len = x.value.len();
            }
            *num_headers = hs.len();
            n as c_int
        }
        Ok(httparse::Status::Partial) => -2,
        Err(_) => -1,
    }
}
