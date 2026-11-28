use std::io::Write;

pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
}

impl Response {
    pub fn new() -> Self {
        Self {
            status: 200,
            body: Vec::with_capacity(16 * 1024),
        }
    }

    pub fn reset(&mut self) {
        self.status = 200;
        self.body.clear();
    }

    pub fn write_to(&self, out: &mut Vec<u8>) {
        let reason = match self.status {
            200 => "OK",
            201 => "Created",
            400 => "Bad Request",
            401 => "Unauthorized",
            404 => "Not Found",
            500 => "Internal Server Error",
            503 => "Service Unavailable",
            _ => "",
        };

        let _ = write!(
            out,
            "HTTP/1.1 {} {reason}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n",
            self.status,
            self.body.len()
        );
        out.extend_from_slice(&self.body);
    }
}
