use std::mem;

use crate::{
    app::{App, Handled},
    uring::FixedFile,
};

// a client that never completes a request would otherwise grow partial forever
const MAX_PARTIAL: usize = 4096;

pub struct Io {
    // the multishot recv can still post cqes for this slot
    pub recv_armed: bool,
    // recv hit ENOBUFS; the slot sits in the shard's starved queue until rearmed
    pub starved: bool,
    pub sending: bool,
    pub closing: bool,
    pub closed: bool,
}

impl Io {
    fn new() -> Self {
        Self {
            recv_armed: false,
            starved: false,
            sending: false,
            closing: false,
            closed: false,
        }
    }

    pub fn can_free(&self) -> bool {
        if self.recv_armed {
            return false;
        }
        // the starved queue still holds this slot
        if self.starved {
            return false;
        }
        if self.sending {
            return false;
        }
        self.closed
    }

    // returns false if the conn was already closing
    pub fn start_closing(&mut self) -> bool {
        !std::mem::replace(&mut self.closing, true)
    }
}

pub struct Conn {
    file: FixedFile,
    // unprocessed input: the tail of an incomplete request, or everything that arrived while a
    // send was in flight; empty and unallocated between requests
    partial: Vec<u8>,
    // the kernel reads this until the send completes, so never touch it while one is in flight
    out: Vec<u8>,
    sent: usize,
    io: Io,
}

enum Drained {
    Consumed(usize),
    Close,
}

impl Conn {
    pub fn new(file: FixedFile) -> Self {
        Self {
            file,
            partial: Vec::new(),
            out: Vec::new(),
            sent: 0,
            io: Io::new(),
        }
    }

    pub fn file(&self) -> FixedFile {
        self.file
    }

    pub fn io(&mut self) -> &mut Io {
        &mut self.io
    }

    pub fn unsent(&self) -> &[u8] {
        &self.out[self.sent..]
    }

    pub fn after_recv(&mut self, data: &[u8], app: &mut App) -> Next {
        if self.io.sending {
            // handling it now would write into out while the kernel reads it; after_send picks it up
            self.partial.extend_from_slice(data);
            if self.partial.len() > MAX_PARTIAL {
                return Next::Close;
            }
            return Next::Wait;
        }

        if self.partial.is_empty() {
            // use data from pbuf as input and store the remaining on partial
            let drained = drain(data, &mut self.out, app);
            let Drained::Consumed(n) = drained else {
                return Next::Close;
            };
            self.partial.extend_from_slice(&data[n..]);
        } else {
            // use partial as input; extend what's new from pbuf
            self.partial.extend_from_slice(data);
            let drained = drain(&self.partial, &mut self.out, app);
            let Drained::Consumed(n) = drained else {
                return Next::Close;
            };
            self.partial.drain(..n);
        }
        self.settle()
    }

    pub fn after_send(&mut self, n: usize, app: &mut App) -> Next {
        self.sent += n;
        if self.sent < self.out.len() {
            return Next::Send;
        }
        self.out = Vec::new();
        self.sent = 0;
        if self.partial.is_empty() {
            return Next::Wait;
        }

        // requests that arrived while the send was in flight
        let drained = drain(&self.partial, &mut self.out, app);
        let Drained::Consumed(n) = drained else {
            return Next::Close;
        };
        self.partial.drain(..n);
        self.settle()
    }

    fn settle(&mut self) -> Next {
        if self.partial.len() > MAX_PARTIAL {
            return Next::Close;
        }
        if self.partial.is_empty() {
            self.partial = Vec::new();
        }
        if !self.out.is_empty() {
            return Next::Send;
        }
        Next::Wait
    }
}

fn drain(data: &[u8], out: &mut Vec<u8>, app: &mut App) -> Drained {
    let mut start = 0;
    // one recv can carry several pipelined requests
    loop {
        match app.handle(&data[start..], out) {
            Handled::Responded { consumed } => start += consumed,
            Handled::NeedMore => return Drained::Consumed(start),
            Handled::Close => return Drained::Close,
        }
    }
}

pub enum Next {
    // nothing to submit; the armed recv delivers whatever comes next
    Wait,
    Send,
    Close,
}

const NONE: u32 = u32::MAX;

enum Entry {
    Vacant(u32),
    Occupied(Conn),
}

pub struct Slab {
    entries: Vec<Entry>,
    curr_top: u32,
}

impl Slab {
    pub fn new() -> Self {
        Self {
            entries: Vec::new(),
            curr_top: NONE,
        }
    }

    pub fn insert(&mut self, conn: Conn) -> u32 {
        if self.curr_top == NONE {
            self.entries.push(Entry::Occupied(conn));
            return (self.entries.len() - 1) as u32;
        }

        let slot = self.curr_top;
        let prev = mem::replace(&mut self.entries[slot as usize], Entry::Occupied(conn));
        self.curr_top = match prev {
            Entry::Vacant(next) => next,
            Entry::Occupied(_) => unreachable!("free list points at an occupied slot"),
        };
        slot
    }

    pub fn get_mut(&mut self, slot: u32) -> &mut Conn {
        match &mut self.entries[slot as usize] {
            Entry::Occupied(conn) => conn,
            Entry::Vacant(_) => panic!("slot {slot} is empty"),
        }
    }

    pub fn remove(&mut self, slot: u32) {
        let entry = &mut self.entries[slot as usize];
        if let Entry::Vacant(_) = entry {
            panic!("slot {slot} removed twice");
        }
        *entry = Entry::Vacant(self.curr_top);
        self.curr_top = slot;
    }
}
