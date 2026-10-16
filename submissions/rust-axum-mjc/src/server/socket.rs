//! Register with Tokio only when HTTP needs to wait for socket I/O.

use std::{
    io::{self, IoSlice, Read, Write},
    net::{Shutdown, TcpStream},
    pin::Pin,
    task::{Context, Poll},
};
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};

enum Stream {
    Plain(Option<TcpStream>),
    Registered(tokio::net::TcpStream),
}

pub(super) struct Socket {
    stream: Stream,
    blocked: bool,
}

impl Socket {
    pub(super) fn new(stream: TcpStream) -> Self {
        Self {
            stream: Stream::Plain(Some(stream)),
            blocked: false,
        }
    }

    /// Called after HTTP returns Pending, never after a completed idle session.
    pub(super) fn register_if_blocked(&mut self) -> io::Result<bool> {
        if self.blocked
            && let Stream::Plain(stream) = &mut self.stream
        {
            let stream = stream.take().expect("socket missing");
            self.stream = Stream::Registered(tokio::net::TcpStream::from_std(stream)?);
            self.blocked = false;
            return Ok(true);
        }
        Ok(false)
    }

    pub(super) fn into_std(self) -> io::Result<TcpStream> {
        match self.stream {
            Stream::Plain(stream) => Ok(stream.expect("socket missing")),
            Stream::Registered(stream) => stream.into_std(),
        }
    }
}

impl AsyncRead for Socket {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        match &mut this.stream {
            Stream::Registered(stream) => Pin::new(stream).poll_read(cx, buf),
            Stream::Plain(stream) => match stream
                .as_mut()
                .expect("socket missing")
                .read(buf.initialize_unfilled())
            {
                Ok(n) => {
                    buf.advance(n);
                    Poll::Ready(Ok(()))
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    // The caller arms readiness and polls HTTP again if the
                    // whole session still needs I/O, avoiding a lost wakeup.
                    this.blocked = true;
                    Poll::Pending
                }
                Err(error) => Poll::Ready(Err(error)),
            },
        }
    }
}

impl AsyncWrite for Socket {
    fn is_write_vectored(&self) -> bool {
        true
    }

    fn poll_write_vectored(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        bufs: &[IoSlice<'_>],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        match &mut this.stream {
            Stream::Registered(stream) => Pin::new(stream).poll_write_vectored(cx, bufs),
            Stream::Plain(stream) => {
                match stream
                    .as_mut()
                    .expect("socket missing")
                    .write_vectored(bufs)
                {
                    Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                        this.blocked = true;
                        Poll::Pending
                    }
                    result => Poll::Ready(result),
                }
            }
        }
    }

    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        let this = self.get_mut();
        match &mut this.stream {
            Stream::Registered(stream) => Pin::new(stream).poll_write(cx, buf),
            Stream::Plain(stream) => match stream.as_mut().expect("socket missing").write(buf) {
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                    this.blocked = true;
                    Poll::Pending
                }
                result => Poll::Ready(result),
            },
        }
    }

    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        // Neither transport buffers writes; Hyper owns the output buffer.
        Poll::Ready(Ok(()))
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match &mut self.get_mut().stream {
            Stream::Plain(stream) => Poll::Ready(
                stream
                    .as_ref()
                    .expect("socket missing")
                    .shutdown(Shutdown::Write),
            ),
            Stream::Registered(stream) => Pin::new(stream).poll_shutdown(cx),
        }
    }
}
