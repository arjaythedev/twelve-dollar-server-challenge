use std::os::fd::RawFd;

use crate::{
    server::{Shard, token::Token},
    uring::{Entry, Op, SqFull, Tagged},
};

impl Shard {
    pub(super) fn submit_accept(&mut self, listen_fd: RawFd) {
        let op = Op::Accept { fd: listen_fd };
        let entry = op.encode().user_data(Token::Accept.pack());
        self.push(entry)
    }

    pub(super) fn submit_recv(&mut self, slot: u32) {
        let conn = self.conns.get_mut(slot);
        conn.io().recv_armed = true;
        let op = Op::Recv { file: conn.file() };
        let entry = op.encode().user_data(Token::Recv(slot).pack());
        self.push(entry)
    }

    pub(super) fn submit_cancel(&mut self, slot: u32) {
        let op = Op::AsyncCancel {
            user_data: Token::Recv(slot).pack(),
        };
        let entry = op.encode().user_data(Token::Cancel(slot).pack());
        self.push(entry)
    }

    pub(super) fn submit_send(&mut self, slot: u32) {
        let conn = self.conns.get_mut(slot);
        conn.io().sending = true;
        let file = conn.file();
        let buf = conn.unsent();
        let op = Op::Send {
            file,
            buf: buf.as_ptr(),
            len: buf.len() as u32,
        };
        let entry = op.encode().user_data(Token::Send(slot).pack());
        self.push(entry)
    }

    pub(super) fn submit_close(&mut self, slot: u32) {
        let conn = self.conns.get_mut(slot);
        let file = conn.file();
        let op = Op::Close { file };
        let entry = op.encode().user_data(Token::Close(slot).pack());
        self.push(entry)
    }

    pub(super) fn submit_wake(&mut self, fd: RawFd) {
        let entry = Op::PollAdd { fd }
            .encode()
            .user_data(Token::Wake(fd).pack());
        self.push(entry);
    }

    fn push(&mut self, mut entry: Entry<Tagged>) {
        loop {
            match unsafe { self.ring.push(entry) } {
                Ok(_) => return,
                Err(SqFull(back)) => {
                    if let Err(e) = self.ring.submit_and_wait(0) {
                        eprintln!("submit: {e}");
                    }
                    entry = back;
                }
            }
        }
    }
}
