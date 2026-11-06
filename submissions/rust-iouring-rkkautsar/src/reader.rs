use std::sync::Arc;

#[derive(Clone)]
pub struct PostResponse {
    pub normal: Arc<Vec<u8>>,
}
