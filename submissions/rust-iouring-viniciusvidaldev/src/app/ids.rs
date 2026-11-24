#[derive(Clone, Copy)]
struct Id(i64);

impl Id {
    pub fn parse(s: &str) -> Option<Self> {
        let n: i64 = s.parse().ok()?;
        if n <= 0 {
            return None;
        }
        Some(Self(n))
    }

    pub fn get(self) -> i64 {
        self.0
    }
}

macro_rules! id_type {
    ($name:ident) => {
        #[derive(Clone, Copy)]
        pub struct $name(Id);

        impl $name {
            pub fn parse(s: &str) -> Option<Self> {
                Id::parse(s).map(Self)
            }

            pub fn get(self) -> i64 {
                self.0.get()
            }
        }
    };
}

id_type!(PostId);
id_type!(UserId);
