use std::{fmt, os::fd::RawFd};

#[derive(Clone, Copy, Debug)]
pub enum Token {
    Accept,
    Recv(u32),
    Send(u32),
    Close(u32),
    Wake(RawFd),
    Cancel(u32),
}

const ACCEPT: u64 = 0;
const RECV: u64 = 1;
const SEND: u64 = 2;
const CLOSE: u64 = 3;
const WAKE: u64 = 4;
const CANCEL: u64 = 5;

fn tagged(tag: u64, payload: u32) -> u64 {
    tag << 32 | payload as u64
}

impl Token {
    pub fn pack(self) -> u64 {
        match self {
            Self::Accept => tagged(ACCEPT, 0),
            Self::Recv(slot) => tagged(RECV, slot),
            Self::Send(slot) => tagged(SEND, slot),
            Self::Close(slot) => tagged(CLOSE, slot),
            Self::Wake(fd) => tagged(WAKE, fd as u32),
            Self::Cancel(slot) => tagged(CANCEL, slot),
        }
    }

    pub fn unpack(user_data: u64) -> Self {
        let payload = user_data as u32;
        match user_data >> 32 {
            ACCEPT => Self::Accept,
            RECV => Self::Recv(payload),
            SEND => Self::Send(payload),
            CLOSE => Self::Close(payload),
            WAKE => Self::Wake(payload as RawFd),
            CANCEL => Self::Cancel(payload),
            tag => unreachable!("unknown token tag {tag}"),
        }
    }
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Accept => f.write_str("accept"),
            Self::Recv(slot) => write!(f, "recv[{slot}]"),
            Self::Send(slot) => write!(f, "send[{slot}]"),
            Self::Close(slot) => write!(f, "close[{slot}]"),
            Self::Wake(fd) => write!(f, "wake[{fd}]"),
            Self::Cancel(slot) => write!(f, "cancel[{slot}]"),
        }
    }
}
