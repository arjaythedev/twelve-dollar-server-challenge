use serde::Serialize;

#[derive(Debug, Serialize)]
pub struct Post<'a> {
    pub id: i64,
    pub body: &'a str,
    pub created_at: &'a str,
    pub author: &'a str,
    pub like_count: i64,
}

impl Post<'_> {
    pub fn write_json(&self, output: &mut Vec<u8>) -> serde_json::Result<()> {
        output.extend_from_slice(b"{\"id\":");
        serde_json::to_writer(&mut *output, &self.id)?;
        output.extend_from_slice(b",\"body\":");
        serde_json::to_writer(&mut *output, self.body)?;
        output.extend_from_slice(b",\"created_at\":");
        serde_json::to_writer(&mut *output, self.created_at)?;
        output.extend_from_slice(b",\"author\":");
        serde_json::to_writer(&mut *output, self.author)?;
        output.extend_from_slice(b",\"like_count\":");
        serde_json::to_writer(&mut *output, &self.like_count)?;
        output.push(b'}');
        Ok(())
    }
}

#[cfg(test)]
include!("tests/model.rs");
