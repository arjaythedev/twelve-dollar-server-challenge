//! The data model on xitdb, reads, and the group commit.
//!
//! The root is a list: users, posts, likes, timeline, source stamp.
//! - `users[id-1]`: bytes, `created_at` (24) then `username`.
//! - `posts[id-1]`: bytes, `user_id` (8, big endian), `created_at` (24), then `body`.
//! - `likes[id-1]`: the likes of post `id`: a counted map from user to `created_at` in ms, so
//!   `like_count` is the size xitdb keeps in the map's header.
//! - `timeline`: post ids in `(created_at, id)` order, which is `posts_created_at_id_idx`
//!   upside down: the feed reads its end.
//!
//! Every request reads its rows here while it is served (rule 5): `View` holds only the
//! positions of the four lists in one immutable, durable commit. Nothing else is kept.
use crate::import;
use crate::json::{self, push_int};
use crate::store::{FileCore, Shared, Syncer};
use std::fs::File;
use std::os::fd::{AsRawFd, IntoRawFd, RawFd};
use std::os::unix::fs::OpenOptionsExt;
use std::rc::Rc;
use xitdb::arraylist::ArrayList;
use xitdb::counted_hashmap::CountedHashMap;
use xitdb::cursor::{ReadOnly, Writeable};
use xitdb::database::{Database, InitOpts};
use xitdb::error::Error;
use xitdb::hasher::{bytes_to_id, Hasher};
use xitdb::slot::Slot;
use xitdb::tag::Tag;

/// Keys here are integers, so the "hash" is a bijective mix of one: 8 bytes in each entry
/// instead of SHA-1's 20, and two keys can't collide.
pub struct Mix;

#[inline]
pub fn mix(id: i64) -> [u8; 8] {
    // The splitmix64 finalizer.
    let mut z = id as u64;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
    (z ^ (z >> 31)).to_be_bytes()
}

impl Hasher for Mix {
    fn digest_len(&self) -> usize {
        8
    }
    fn digest(&self, data: &[u8]) -> Vec<u8> {
        // Every key here goes through `mix`; this only keeps the trait whole.
        let v = data.iter().fold(0xcbf29ce484222325u64, |h, &b| (h ^ b as u64).wrapping_mul(0x100000001b3));
        mix(v as i64).to_vec()
    }
    fn id(&self) -> u32 {
        bytes_to_id(*b"mix8")
    }
}

pub type Xdb = Database<FileCore, Mix>;
type List = ArrayList<'static, FileCore, Mix, ReadOnly>;

pub const USERS: i64 = 0;
pub const POSTS: i64 = 1;
pub const LIKES: i64 = 2;
pub const TIMELINE: i64 = 3;
pub const STAMP: i64 = 4;
pub const TS_LEN: usize = 24;
pub const POST_HEAD: usize = 8 + TS_LEN;
const FEED_LEN: usize = 20;

pub const OPTS: InitOpts = InitOpts { hash_id: None, file_id: None, fsync: false };

#[derive(Debug)]
pub struct DbError;

impl From<Error> for DbError {
    fn from(_: Error) -> DbError {
        DbError
    }
}

pub enum PostLookup {
    Found,
    Missing,
}

pub enum LikeState {
    Missing,
    Already,
    New,
}

/// A validated write waiting for the end of the pass. `to` is who gets the answer.
pub struct Pending {
    pub to: (u8, u32, u16),
    pub kind: Kind,
}

pub enum Kind {
    Post { user_id: i64, body: (u32, u32), username: (u32, u32) },
    Like { user_id: i64, post_id: i64 },
}

pub enum Outcome {
    Post { id: i64, created: [u8; TS_LEN] },
    Like { new: bool },
}

struct View {
    users: Slot,
    posts: Slot,
    likes: Slot,
    timeline: Slot,
    /// `likes` again, for `like_state`: looking up one user goes through the crate.
    like_maps: List,
}

#[derive(Clone, Copy)]
struct Row {
    id: i64,
    body: &'static [u8],
    created: &'static [u8],
    author: &'static [u8],
    likes: i64,
}

pub struct Db {
    x: &'static Xdb,
    file: Rc<Shared>,
    syncer: Syncer,
    /// The newest commit known to be durable. Reads see this one.
    view: View,
    /// The commit the running sync round makes durable.
    next: Option<View>,
    /// Sync rounds asked for.
    seq: u64,
    rows: Vec<Row>,
}

fn open_rw(path: &str) -> Result<RawFd, String> {
    let f = std::fs::OpenOptions::new().read(true).write(true).custom_flags(libc::O_CLOEXEC).open(path).map_err(|e| format!("{path}: {e}"))?;
    // xitdb asks its callers for an exclusive lock, and two writers would corrupt the file.
    if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(format!("{path}: already in use"));
    }
    Ok(f.into_raw_fd())
}

fn view(x: &'static Xdb) -> Result<View, DbError> {
    let root = ArrayList::new(x.root_cursor())?;
    let like_maps = ArrayList::new(root.get(LIKES)?.ok_or(DbError)?)?;
    Ok(View { users: root.get_slot(USERS)?, posts: root.get_slot(POSTS)?, likes: root.get_slot(LIKES)?, timeline: root.get_slot(TIMELINE)?, like_maps })
}

/// Whether the database at `path` was imported from the source with this stamp.
fn matches_source(path: &str, stamp: &[u8]) -> Result<bool, String> {
    let fd = open_rw(path)?;
    let (core, _) = FileCore::new(fd).map_err(|e| e.to_string())?;
    let x = Database::open(core, Mix, OPTS).map_err(|e| format!("{path}: {e}"))?;
    let same = (|| -> Result<bool, Error> {
        let root = ArrayList::new(x.root_cursor())?;
        let Some(c) = root.get(STAMP)? else { return Ok(false) };
        Ok(c.read_bytes_alloc()? == stamp)
    })()
    .map_err(|e| format!("{path}: {e}"));
    drop(x);
    unsafe { libc::close(fd) };
    same
}

impl Db {
    /// Opens `<source>.xitdb`, importing the seed database at `source` first if there's none
    /// made from it. A restart finds the file and carries on with it.
    pub fn open(source: &str) -> Result<Db, String> {
        let path = format!("{source}.xitdb");
        let stamp = import::stamp(source)?;
        let current = std::fs::metadata(&path).is_ok() && matches_source(&path, &stamp)?;
        if !current {
            import::run(source, &path, &stamp)?;
        }
        let fd = open_rw(&path)?;
        let (core, file) = FileCore::new(fd).map_err(|e| e.to_string())?;
        let x: &'static Xdb = Box::leak(Box::new(Database::open(core, Mix, OPTS).map_err(|e| format!("{path}: {e}"))?));
        let v = view(x).map_err(|_| format!("{path}: not a feed database"))?;
        Ok(Db { x, file, syncer: Syncer::start(fd), view: v, next: None, seq: 0, rows: Vec::with_capacity(FEED_LEN) })
    }

    pub fn health(&self) -> Result<(), String> {
        // Reads the root header and checks the commit's checksum.
        self.x.committed_size().map(|_| ()).map_err(|e| e.to_string())
    }

    /// The row with this id in a list. An empty slot if there is none.
    #[inline]
    fn get(&self, list: Slot, id: i64) -> Result<Slot, DbError> {
        self.file.list_get(list, (id as u64).wrapping_sub(1)).ok_or(DbError)
    }

    /// `(user_id, created_at, body)` of a post, if there is one.
    #[inline]
    fn post_row(&self, id: i64) -> Result<Option<(i64, &'static [u8], &'static [u8])>, DbError> {
        let s = self.get(self.view.posts, id)?;
        if s.tag == Tag::None {
            return Ok(None); // beyond the end, or a gap in the source's ids
        }
        let r = self.file.bytes(s).ok_or(DbError)?;
        if r.len() < POST_HEAD {
            return Err(DbError);
        }
        let mut u = [0u8; 8];
        u.copy_from_slice(&r[..8]);
        Ok(Some((i64::from_be_bytes(u), &r[8..POST_HEAD], &r[POST_HEAD..])))
    }

    /// A user's name. None where the SQL join would find no row.
    #[inline]
    fn author(&self, user_id: i64) -> Result<Option<&'static [u8]>, DbError> {
        let s = self.get(self.view.users, user_id)?;
        if s.tag == Tag::None {
            return Ok(None);
        }
        Ok(Some(self.file.bytes(s).ok_or(DbError)?.get(TS_LEN..).ok_or(DbError)?))
    }

    #[inline]
    fn like_count(&self, id: i64) -> Result<i64, DbError> {
        let s = self.get(self.view.likes, id)?;
        Ok(self.file.map_len(s).ok_or(DbError)? as i64)
    }

    /// `{"post":{...}}` for GET /posts/:id.
    pub fn post(&mut self, id: i64, out: &mut Vec<u8>) -> Result<PostLookup, DbError> {
        let Some((user_id, created, body)) = self.post_row(id)? else { return Ok(PostLookup::Missing) };
        let Some(author) = self.author(user_id)? else { return Ok(PostLookup::Missing) };
        let likes = self.like_count(id)?;
        out.reserve(128);
        out.extend_from_slice(b"{\"post\":{\"id\":");
        push_int(out, id);
        out.extend_from_slice(b",\"body\":\"");
        json::escape_into(out, body);
        out.extend_from_slice(b"\",\"created_at\":\"");
        json::escape_into(out, created);
        out.extend_from_slice(b"\",\"author\":\"");
        json::escape_into(out, author);
        out.extend_from_slice(b"\",\"like_count\":");
        push_int(out, likes);
        out.extend_from_slice(b"}}");
        Ok(PostLookup::Found)
    }

    /// `{"posts":[...]}`: the 20 newest by (created_at DESC, id DESC).
    pub fn feed(&mut self, out: &mut Vec<u8>) -> Result<(), DbError> {
        let mut rows = std::mem::take(&mut self.rows);
        rows.clear();
        let mut i = self.file.list_len(self.view.timeline).ok_or(DbError)?;
        while i > 0 && rows.len() < FEED_LEN {
            i -= 1;
            let id = self.file.list_get(self.view.timeline, i).ok_or(DbError)?.value;
            let Some((user_id, created, body)) = self.post_row(id)? else { return Err(DbError) };
            let Some(author) = self.author(user_id)? else { continue }; // the join drops it
            rows.push(Row { id, body, created, author, likes: self.like_count(id)? });
        }
        const ROW: usize = b",{\"id\":,\"body\":\"\",\"created_at\":\"\",\"author\":\"\",\"like_count\":}".len() + 2 * json::INT_BOUND;
        let text: usize = rows.iter().map(|r| r.body.len() + r.created.len() + r.author.len()).sum();
        let bound = b"{\"posts\":[]}".len() + rows.len() * ROW + json::esc_bound(text);
        json::fill(out, bound, |c| {
            c.lit(b"{\"posts\":[");
            for (k, row) in rows.iter().enumerate() {
                if k > 0 {
                    c.byte(b',');
                }
                c.lit(b"{\"id\":");
                c.int(row.id);
                c.lit(b",\"body\":\"");
                c.esc(row.body);
                c.lit(b"\",\"created_at\":\"");
                c.esc(row.created);
                c.lit(b"\",\"author\":\"");
                c.esc(row.author);
                c.lit(b"\",\"like_count\":");
                c.int(row.likes);
                c.byte(b'}');
            }
            c.lit(b"]}");
        });
        self.rows = rows;
        Ok(())
    }

    /// Whether a like has anything to do: checked against the durable commit, so `New` may
    /// still turn out to be a repeat in `apply`.
    pub fn like_state(&self, user_id: i64, post_id: i64) -> Result<LikeState, DbError> {
        if self.post_row(post_id)?.is_none() {
            return Ok(LikeState::Missing);
        }
        if self.get(self.view.likes, post_id)?.tag == Tag::None {
            return Ok(LikeState::New);
        }
        let c = self.view.like_maps.get(post_id - 1)?.ok_or(DbError)?;
        let liked = CountedHashMap::new(c)?.get_by_hash(&mix(user_id))?.is_some();
        Ok(if liked { LikeState::Already } else { LikeState::New })
    }

    /// One transaction for all of `writes`. `done` gets one outcome per write, in order. An
    /// error means none of them happened.
    pub fn apply(&mut self, writes: &[Pending], arena: &[u8], done: &mut Vec<Outcome>) -> Result<(), DbError> {
        let now = now_ms();
        let any_post = writes.iter().any(|w| matches!(w.kind, Kind::Post { .. }));
        let r = self.x.run_context(|w| {
            let root = ArrayList::new_write(w)?;
            let likes = ArrayList::new_write(&root.put_cursor(LIKES)?)?;
            let mut created = [0u8; TS_LEN];
            let mut lists = None;
            if any_post {
                let posts = ArrayList::new_write(&root.put_cursor(POSTS)?)?;
                let timeline = ArrayList::new_write(&root.put_cursor(TIMELINE)?)?;
                // The timeline must stay sorted, so a clock that stepped back can't date a post
                // before the newest one.
                created = format_ts(now);
                if let Some(s) = timeline.get(-1)? {
                    let last = posts.get(s.read_uint()? as i64 - 1)?.ok_or(Error::Corrupted)?.read_bytes_alloc()?;
                    let last = last.get(8..POST_HEAD).ok_or(Error::Corrupted)?;
                    if &created[..] < last {
                        created.copy_from_slice(last);
                    }
                }
                lists = Some((posts, timeline));
            }
            for p in writes {
                match p.kind {
                    Kind::Like { user_id, post_id } => {
                        let c = likes.put_cursor(post_id - 1)?;
                        let map = CountedHashMap::new_write(&c)?;
                        let hash = mix(user_id);
                        let new = map.get_by_hash(&hash)?.is_none();
                        if new {
                            map.put_by_hash(&hash, Writeable::Uint(now))?;
                        }
                        done.push(Outcome::Like { new });
                    }
                    Kind::Post { user_id, body, .. } => {
                        let (posts, timeline) = lists.as_ref().ok_or(Error::Corrupted)?;
                        let id = posts.count()? as i64 + 1;
                        let body = &arena[body.0 as usize..body.1 as usize];
                        let mut rec = Vec::with_capacity(POST_HEAD + body.len());
                        rec.extend_from_slice(&user_id.to_be_bytes());
                        rec.extend_from_slice(&created);
                        rec.extend_from_slice(body);
                        posts.append(Writeable::Bytes(rec))?;
                        likes.append(Writeable::None)?;
                        timeline.append(Writeable::Uint(id as u64))?;
                        done.push(Outcome::Post { id, created });
                    }
                }
            }
            Ok(())
        });
        if r.is_err() {
            done.clear();
        }
        Ok(r?)
    }

    /// Starts a sync round for everything committed so far.
    pub fn start_sync(&mut self) -> Result<(), DbError> {
        self.next = Some(view(self.x)?);
        self.seq += 1;
        self.syncer.request(self.seq, &self.file.head());
        Ok(())
    }

    /// True once, when the running sync round has finished: its commit is durable, and reads
    /// move on to it.
    #[inline]
    pub fn sync_finished(&mut self) -> bool {
        if self.next.is_some() && self.syncer.done() == self.seq {
            self.view = self.next.take().expect("a sync round");
            return true;
        }
        false
    }

    #[inline]
    pub fn syncing(&self) -> bool {
        self.next.is_some()
    }
}

pub fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map_or(0, |d| d.as_millis() as u64)
}

/// `YYYY-MM-DDTHH:MM:SS.mmmZ`.
pub fn format_ts(ms: u64) -> [u8; TS_LEN] {
    let secs = ms / 1000;
    // Days to civil date (Howard Hinnant's algorithm).
    let z = (secs / 86400) as i64 + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    let mut t = *b"0000-00-00T00:00:00.000Z";
    let mut put = |at: usize, n: usize, mut v: u64| {
        for i in (at..at + n).rev() {
            t[i] = b'0' + (v % 10) as u8;
            v /= 10;
        }
    };
    put(0, 4, y as u64);
    put(5, 2, m as u64);
    put(8, 2, d as u64);
    put(11, 2, secs / 3600 % 24);
    put(14, 2, secs / 60 % 60);
    put(17, 2, secs % 60);
    put(20, 3, ms % 1000);
    t
}

/// Durable create-then-rename support for the importer.
pub fn sync_dir(path: &str) -> Result<(), String> {
    let dir = std::path::Path::new(path).parent().filter(|p| !p.as_os_str().is_empty()).map_or_else(|| ".".into(), |p| p.to_path_buf());
    File::open(&dir).and_then(|f| f.sync_all()).map_err(|e| format!("{}: {e}", dir.display()))
}
