//! Small end-to-end check for the candidate FFI boundary; run only in Linux.

#[path = "l2_uring.rs"]
mod l2_uring;

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::fd::AsRawFd;
use std::time::Duration;

fn main() -> std::io::Result<()> {
    const TABLE: u32 = 32;
    let listener = TcpListener::bind(("127.0.0.1", 0))?;
    let address = listener.local_addr()?;
    let mut rings = [
        l2_uring::Ring::new(64, 256, TABLE, 17, 16)?,
        l2_uring::Ring::new(64, 256, TABLE, 18, 16)?,
    ];
    assert!(rings[0].event_fd() >= 0 && rings[1].event_fd() >= 0);

    unsafe { rings[1].accept(listener.as_raw_fd(), 0, 1)?; }
    assert_eq!(rings[1].submit()?, 1);
    let mut client = TcpStream::connect(address)?;
    client.set_read_timeout(Some(Duration::from_secs(5)))?;
    client.set_write_timeout(Some(Duration::from_secs(5)))?;
    let ready_mask = l2_uring::Ring::wait_any(&mut rings, 5_000)?;
    assert_eq!(ready_mask, 0b10, "only the second ring should wake the owner");
    let accepted = rings[1].wait()?;
    assert_eq!((accepted.user_data, accepted.result), (1, 0));
    assert_eq!(accepted.buffer_id, None);

    unsafe { rings[1].recv_select(0, 2)?; }
    assert_eq!(rings[1].submit()?, 1);
    client.write_all(b"l2-buffer-ownership")?;
    let received = rings[1].wait()?;
    assert_eq!(received.user_data, 2);
    let buffer_id = received.buffer_id.expect("selected receive must return a buffer");
    assert_eq!(received.result, 19);
    assert_eq!(&rings[1].provided_buffer(buffer_id)?[..19], b"l2-buffer-ownership");

    rings[1].return_buffer(buffer_id)?;
    assert_eq!(rings[1].available_buffer_count(), 16);

    unsafe { rings[1].send(0, b"committed-reply".as_ptr(), 15, 4)?; }
    assert_eq!(rings[1].submit()?, 1);
    assert_eq!(rings[1].wait()?.user_data, 4);
    let mut reply = [0u8; 15];
    client.read_exact(&mut reply)?;
    assert_eq!(&reply, b"committed-reply");

    unsafe { rings[1].close(0, 5)?; }
    assert_eq!(rings[1].submit()?, 1);
    let closed = rings[1].wait()?;
    assert_eq!((closed.user_data, closed.result), (5, 0));
    drop(client);
    drop(rings);
    drop(listener);
    println!("L2 FFI probe passed: cross-ring eventfd wake, direct accept, selected recv, buffer reprovide, send, direct close");
    Ok(())
}
