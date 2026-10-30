#define _GNU_SOURCE
#include "raw_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

u32 root_users      = 2;
u32 root_posts      = 4;
u32 root_likes      = 5;
u32 root_likes_pk   = 6;
u32 root_posts_time = 8;
u32 root_posts_user = 9;
u32 root_likes_pid  = 10;

static int db_fd  = -1;
static int wal_fd = -1;

static u8    *db_mmap       = NULL;
static size_t db_mmap_sz    = 0;
static u32    db_page_count = 0;

#define MAX_DB_PAGES 262144
static u8 *page_ptrs[MAX_DB_PAGES];

static u16 batch_dirty_mark[MAX_DB_PAGES];
static u16 cur_batch_id = 1;
static u32 dirty_pnos[4096];
static int ndirty = 0;

static u8  ckpt_dirty_mark[MAX_DB_PAGES];
static u32 ckpt_dirty_list[32768];
static u32 n_ckpt_dirty = 0;

#define MAX_WAL_FRAMES 16384
typedef struct __attribute__((packed)) {
  wal_frame_hdr_t hdr;
  u8 page[SQLITE_PAGE_SIZE];
} wal_frame_slot_t;

static wal_frame_slot_t wal_frames_buf[MAX_WAL_FRAMES];

static u32 wal_seq           = 1;
static u32 wal_salt1         = 0x38472910;
static u32 wal_salt2         = 0x82910472;
static u32 wal_last_cksum[2] = {0, 0};
static u32 wal_mx_frame      = 0;

static pthread_mutex_t ckpt_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ckpt_cv   = PTHREAD_COND_INITIALIZER;
static pthread_t       ckpt_tid;
static int             ckpt_init = 0;
static int             ckpt_needed = 0;
static int             ckpt_stop   = 0;

static i64 highest_post_id     = 500000;
static u32 rightmost_post_int  = 21966;
static u32 rightmost_post_leaf = 21981;

static i64 highest_like_rowid  = 2016005;
static u32 rightmost_like_int  = 47202;
static u32 rightmost_like_leaf = 47396;

static inline const u8 *get_page(u32 page_num) {
  return page_ptrs[page_num];
}

static void write_wal_header(void) {
  wal_hdr_t wh;
  wh.magic     = __builtin_bswap32(WAL_MAGIC_LE);
  wh.version   = __builtin_bswap32(WAL_MAX_VERSION);
  wh.page_size = __builtin_bswap32(SQLITE_PAGE_SIZE);
  wh.seq       = __builtin_bswap32(wal_seq);
  wh.salt1     = wal_salt1;
  wh.salt2     = wal_salt2;
  u32 ck[2] = {0, 0};
  wal_checksum_bytes(1, (const u8 *)&wh, 24, NULL, ck);
  wh.cksum1 = ck[0];
  wh.cksum2 = ck[1];
  wal_last_cksum[0] = ck[0];
  wal_last_cksum[1] = ck[1];
  if (pwrite(wal_fd, &wh, sizeof wh, 0) < 0) {}
}

static void flush_ckpt_to_mmap(void) {
  for (u32 i = 0; i < n_ckpt_dirty; i++) {
    u32 pno = ckpt_dirty_list[i];
    u8 *dst = db_mmap + (size_t)(pno - 1) * SQLITE_PAGE_SIZE;
    if (page_ptrs[pno] != dst) {
      memcpy(dst, page_ptrs[pno], SQLITE_PAGE_SIZE);
      page_ptrs[pno] = dst;
    }
    ckpt_dirty_mark[pno] = 0;
  }
  if (n_ckpt_dirty > 0) {
    wr_be32(db_mmap + 28, db_page_count);
  }
  n_ckpt_dirty = 0;
  wal_mx_frame = 0;
  wal_seq++;
  wal_salt1 += 0x9e3779b9U;
  write_wal_header();
}

static inline u8 *get_page_write(u32 page_num) {
  if (batch_dirty_mark[page_num] == cur_batch_id) {
    return page_ptrs[page_num];
  }
  u32 slot = wal_mx_frame + (u32)ndirty;
  if (UNLIKELY(slot >= MAX_WAL_FRAMES)) abort();
  u8 *dst = wal_frames_buf[slot].page;
  const u8 *src = page_ptrs[page_num];
  if (LIKELY(src != NULL)) {
    memcpy(dst, src, SQLITE_PAGE_SIZE);
  } else {
    memset(dst, 0, SQLITE_PAGE_SIZE);
  }
  page_ptrs[page_num] = dst;
  batch_dirty_mark[page_num] = cur_batch_id;
  dirty_pnos[ndirty++] = page_num;
  if (!ckpt_dirty_mark[page_num]) {
    ckpt_dirty_mark[page_num] = 1;
    ckpt_dirty_list[n_ckpt_dirty++] = page_num;
  }
  return dst;
}

static u32 alloc_db_page(void) {
  u32 new_pno = ++db_page_count;
  if (UNLIKELY(new_pno >= MAX_DB_PAGES)) abort();
  size_t need_sz = (size_t)new_pno * SQLITE_PAGE_SIZE;
  if (UNLIKELY(need_sz > db_mmap_sz)) {
    size_t new_sz = db_mmap_sz + 32 * 1024 * 1024;
    if (ftruncate(db_fd, (off_t)new_sz) == 0) {
      u8 *old_map = db_mmap;
      size_t old_sz = db_mmap_sz;
      void *new_map = mremap(db_mmap, db_mmap_sz, new_sz, MREMAP_MAYMOVE);
      if (new_map != MAP_FAILED) {
        db_mmap = (u8 *)new_map;
        db_mmap_sz = new_sz;
        if (db_mmap != old_map) {
          for (u32 p = 1; p < new_pno; p++) {
            if (page_ptrs[p] >= old_map && page_ptrs[p] < old_map + old_sz) {
              page_ptrs[p] = db_mmap + (page_ptrs[p] - old_map);
            }
          }
        }
      }
    }
  }
  page_ptrs[new_pno] = NULL;
  return new_pno;
}

static inline i64 parse_index_first_int(const u8 *payload) {
  u8 hlen = payload[0];
  u8 t = payload[1];
  if (LIKELY(hlen < 0x80 && t < 0x80)) {
    return col_decode_int(t, payload + hlen);
  }
  u64 hdr_len, tv;
  const u8 *p = rd_varint_fast(payload, &hdr_len);
  rd_varint_fast(p, &tv);
  return col_decode_int(tv, payload + hdr_len);
}

static inline void parse_index_two_ints(const u8 *payload, i64 *out1, i64 *out2) {
  u8 hlen = payload[0];
  u8 t1 = payload[1];
  u8 t2 = payload[2];
  if (LIKELY(hlen < 0x80 && t1 < 0x80 && t2 < 0x80)) {
    const u8 *p1 = payload + hlen;
    *out1 = col_decode_int(t1, p1);
    *out2 = col_decode_int(t2, p1 + serial_len(t1));
    return;
  }
  u64 hdr_len, tv1, tv2;
  const u8 *p = rd_varint_fast(payload, &hdr_len);
  p = rd_varint_fast(p, &tv1);
  rd_varint_fast(p, &tv2);
  const u8 *d = payload + hdr_len;
  *out1 = col_decode_int(tv1, d);
  *out2 = col_decode_int(tv2, d + serial_len(tv1));
}

int raw_seek_table(raw_table_cursor_t *cur, u32 root_page, i64 target_rowid) {
  cur->depth = 0;
  u32 page_num = root_page;
  for (;;) {
    const u8 *page = get_page(page_num);
    u8 ptype = page[0];
    int ncells = rd_be16(page + 3);
    if (ptype == BTREE_INT_TABLE) {
      int lo = 0, hi = ncells - 1, best = ncells;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u16 cell_off = rd_be16(page + 12 + mid * 2);
        u64 rowid;
        rd_varint_fast(page + cell_off + 4, &rowid);
        if (target_rowid <= (i64)rowid) {
          best = mid;
          hi = mid - 1;
        } else {
          lo = mid + 1;
        }
      }
      cur->stack[cur->depth].page_num = page_num;
      cur->stack[cur->depth].cell_idx = best;
      cur->depth++;
      if (best < ncells) {
        u16 cell_off = rd_be16(page + 12 + best * 2);
        page_num = rd_be32(page + cell_off);
      } else {
        page_num = rd_be32(page + 8);
      }
    } else if (LIKELY(ptype == BTREE_LEAF_TABLE)) {
      cur->page = page;
      cur->page_num = page_num;
      cur->ncells = ncells;
      int lo = 0, hi = ncells - 1;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u16 cell_off = rd_be16(page + 8 + mid * 2);
        u64 payload_sz, rowid;
        const u8 *p = rd_varint_fast(page + cell_off, &payload_sz);
        p = rd_varint_fast(p, &rowid);
        if ((i64)rowid == target_rowid) {
          cur->cell_idx = mid;
          cur->rowid = (i64)rowid;
          cur->payload = p;
          cur->payload_len = (u32)payload_sz;
          return 0;
        }
        if (target_rowid < (i64)rowid) hi = mid - 1;
        else lo = mid + 1;
      }
      return 1;
    } else {
      return -1;
    }
  }
}

int raw_last_table(raw_table_cursor_t *cur, u32 root_page) {
  cur->depth = 0;
  u32 page_num = root_page;
  for (;;) {
    const u8 *page = get_page(page_num);
    u8 ptype = page[0];
    int ncells = rd_be16(page + 3);
    if (ptype == BTREE_INT_TABLE) {
      cur->stack[cur->depth].page_num = page_num;
      cur->stack[cur->depth].cell_idx = ncells;
      cur->depth++;
      page_num = rd_be32(page + 8);
    } else if (LIKELY(ptype == BTREE_LEAF_TABLE)) {
      cur->page = page;
      cur->page_num = page_num;
      cur->ncells = ncells;
      if (UNLIKELY(ncells == 0)) return 1;
      cur->cell_idx = ncells - 1;
      u16 cell_off = rd_be16(page + 8 + (ncells - 1) * 2);
      u64 payload_sz, rowid;
      const u8 *p = rd_varint_fast(page + cell_off, &payload_sz);
      p = rd_varint_fast(p, &rowid);
      cur->rowid = (i64)rowid;
      cur->payload = p;
      cur->payload_len = (u32)payload_sz;
      return 0;
    } else {
      return -1;
    }
  }
}

int raw_prev_table(raw_table_cursor_t *cur) {
  if (LIKELY(cur->cell_idx > 0)) {
    cur->cell_idx--;
    u16 cell_off = rd_be16(cur->page + 8 + cur->cell_idx * 2);
    u64 payload_sz, rowid;
    const u8 *p = rd_varint_fast(cur->page + cell_off, &payload_sz);
    p = rd_varint_fast(p, &rowid);
    cur->rowid = (i64)rowid;
    cur->payload = p;
    cur->payload_len = (u32)payload_sz;
    return 0;
  }
  while (cur->depth > 0) {
    cur->depth--;
    raw_cursor_frame_t *f = &cur->stack[cur->depth];
    const u8 *parent_page = get_page(f->page_num);
    if (f->cell_idx > 0) {
      f->cell_idx--;
      u16 cell_off = rd_be16(parent_page + 12 + f->cell_idx * 2);
      u32 pnum = rd_be32(parent_page + cell_off);
      cur->depth++;
      for (;;) {
        const u8 *cp = get_page(pnum);
        u8 cptype = cp[0];
        int cncells = rd_be16(cp + 3);
        if (cptype == BTREE_INT_TABLE) {
          cur->stack[cur->depth].page_num = pnum;
          cur->stack[cur->depth].cell_idx = cncells;
          cur->depth++;
          pnum = rd_be32(cp + 8);
        } else if (LIKELY(cptype == BTREE_LEAF_TABLE)) {
          cur->page = cp;
          cur->page_num = pnum;
          cur->ncells = cncells;
          cur->cell_idx = cncells - 1;
          u16 coff = rd_be16(cp + 8 + (cncells - 1) * 2);
          u64 psz, rid;
          const u8 *p = rd_varint_fast(cp + coff, &psz);
          p = rd_varint_fast(p, &rid);
          cur->rowid = (i64)rid;
          cur->payload = p;
          cur->payload_len = (u32)psz;
          return 0;
        } else return -1;
      }
    }
  }
  return 1;
}

int raw_index_seek_ge(raw_index_cursor_t *cur, u32 root_page, i64 target_post_id) {
  cur->depth = 0;
  u32 page_num = root_page;
  for (;;) {
    const u8 *page = get_page(page_num);
    u8 ptype = page[0];
    int ncells = rd_be16(page + 3);
    if (ptype == BTREE_INT_INDEX) {
      int lo = 0, hi = ncells - 1, best = ncells;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u16 cell_off = rd_be16(page + 12 + mid * 2);
        u64 payload_sz;
        const u8 *p = rd_varint_fast(page + cell_off + 4, &payload_sz);
        i64 pid = parse_index_first_int(p);
        if (target_post_id <= pid) {
          best = mid;
          hi = mid - 1;
        } else {
          lo = mid + 1;
        }
      }
      cur->stack[cur->depth].page_num = page_num;
      cur->stack[cur->depth].cell_idx = best;
      cur->depth++;
      if (best < ncells) {
        u16 cell_off = rd_be16(page + 12 + best * 2);
        page_num = rd_be32(page + cell_off);
      } else {
        page_num = rd_be32(page + 8);
      }
    } else if (LIKELY(ptype == BTREE_LEAF_INDEX)) {
      int lo = 0, hi = ncells - 1, best = ncells;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u16 cell_off = rd_be16(page + 8 + mid * 2);
        u64 payload_sz;
        const u8 *p = rd_varint_fast(page + cell_off, &payload_sz);
        i64 pid = parse_index_first_int(p);
        if (target_post_id <= pid) {
          best = mid;
          hi = mid - 1;
        } else {
          lo = mid + 1;
        }
      }
      if (LIKELY(best < ncells)) {
        cur->page = page;
        cur->page_num = page_num;
        cur->ncells = ncells;
        cur->cell_idx = best;
        cur->on_interior = 0;
        u16 cell_off = rd_be16(page + 8 + best * 2);
        u64 payload_sz;
        cur->payload = rd_varint_fast(page + cell_off, &payload_sz);
        cur->payload_len = (u32)payload_sz;
        return 0;
      }
      while (cur->depth > 0) {
        cur->depth--;
        raw_cursor_frame_t *f = &cur->stack[cur->depth];
        const u8 *pp = get_page(f->page_num);
        int pncells = rd_be16(pp + 3);
        if (f->cell_idx < pncells) {
          cur->page = pp;
          cur->page_num = f->page_num;
          cur->ncells = pncells;
          cur->cell_idx = f->cell_idx;
          cur->on_interior = 1;
          u16 coff = rd_be16(pp + 12 + f->cell_idx * 2);
          u64 psz;
          cur->payload = rd_varint_fast(pp + coff + 4, &psz);
          cur->payload_len = (u32)psz;
          return 0;
        }
      }
      return 1;
    } else return -1;
  }
}

int raw_index_next(raw_index_cursor_t *cur) {
  if (LIKELY(!cur->on_interior)) {
    cur->cell_idx++;
    if (LIKELY(cur->cell_idx < cur->ncells)) {
      u16 coff = rd_be16(cur->page + 8 + cur->cell_idx * 2);
      u64 psz;
      cur->payload = rd_varint_fast(cur->page + coff, &psz);
      cur->payload_len = (u32)psz;
      return 0;
    }
    while (cur->depth > 0) {
      cur->depth--;
      raw_cursor_frame_t *f = &cur->stack[cur->depth];
      const u8 *pp = get_page(f->page_num);
      int pncells = rd_be16(pp + 3);
      if (f->cell_idx < pncells) {
        cur->page = pp;
        cur->page_num = f->page_num;
        cur->ncells = pncells;
        cur->cell_idx = f->cell_idx;
        cur->on_interior = 1;
        u16 coff = rd_be16(pp + 12 + f->cell_idx * 2);
        u64 psz;
        cur->payload = rd_varint_fast(pp + coff + 4, &psz);
        cur->payload_len = (u32)psz;
        return 0;
      }
    }
    return 1;
  } else {
    int next_idx = cur->cell_idx + 1;
    u32 child_pno;
    if (next_idx < cur->ncells) {
      u16 coff = rd_be16(cur->page + 12 + next_idx * 2);
      child_pno = rd_be32(cur->page + coff);
    } else {
      child_pno = rd_be32(cur->page + 8);
    }
    cur->stack[cur->depth].page_num = cur->page_num;
    cur->stack[cur->depth].cell_idx = next_idx;
    cur->depth++;
    u32 pnum = child_pno;
    for (;;) {
      const u8 *cp = get_page(pnum);
      u8 cptype = cp[0];
      int cncells = rd_be16(cp + 3);
      if (cptype == BTREE_INT_INDEX) {
        cur->stack[cur->depth].page_num = pnum;
        cur->stack[cur->depth].cell_idx = 0;
        cur->depth++;
        u16 coff = rd_be16(cp + 12);
        pnum = rd_be32(cp + coff);
      } else if (LIKELY(cptype == BTREE_LEAF_INDEX)) {
        cur->page = cp;
        cur->page_num = pnum;
        cur->ncells = cncells;
        cur->cell_idx = 0;
        cur->on_interior = 0;
        if (UNLIKELY(cncells == 0)) return 1;
        u16 coff = rd_be16(cp + 8);
        u64 psz;
        cur->payload = rd_varint_fast(cp + coff, &psz);
        cur->payload_len = (u32)psz;
        return 0;
      } else return -1;
    }
  }
}

int raw_get_username(i64 uid, const u8 **name, u32 *name_len) {
  raw_table_cursor_t cur;
  if (UNLIKELY(raw_seek_table(&cur, root_users, uid) != 0)) return -1;
  const u8 *p = cur.payload;
  u8 hlen = p[0];
  u8 t1 = p[2];
  if (LIKELY(hlen < 0x80 && p[1] == 0 && (t1 & 0x81) == 1 && t1 >= 13)) {
    *name = p + hlen;
    *name_len = (u32)((t1 - 13) >> 1);
    return 0;
  }
  col_ref_t cols[3];
  if (parse_record_cols(cur.payload, cur.payload_len, 3, cols) != 0) return -1;
  if (!col_is_text(cols[1].t)) return -1;
  *name = cols[1].p;
  *name_len = (u32)serial_len(cols[1].t);
  return 0;
}

int raw_post_exists(i64 post_id) {
  raw_table_cursor_t cur;
  return raw_seek_table(&cur, root_posts, post_id) == 0 ? 0 : 1;
}

int raw_range_counts(i64 lo, i64 hi, i64 *counts) {
  memset(counts, 0, sizeof(i64) * (size_t)(hi - lo + 1));
  raw_index_cursor_t icur;
  if (raw_index_seek_ge(&icur, root_likes_pid, lo) == 0) {
    for (;;) {
      i64 pid = parse_index_first_int(icur.payload);
      if (pid > hi) break;
      if (LIKELY(pid >= lo)) counts[pid - lo]++;
      if (raw_index_next(&icur) != 0) break;
    }
  }
  return 0;
}

static void table_append_rowid(u32 root_pno, u32 *right_int_pno, u32 *right_leaf_pno,
                               const u8 *cell, u32 cell_sz, i64 new_rowid) {
  u8 *page = get_page_write(*right_leaf_pno);
  int ncells = rd_be16(page + 3);
  u16 content_start = rd_be16(page + 5);
  if (UNLIKELY(content_start == 0)) content_start = SQLITE_PAGE_SIZE;

  if (LIKELY(content_start >= 8 + (ncells + 1) * 2 + cell_sz)) {
    content_start -= (u16)cell_sz;
    memcpy(page + content_start, cell, cell_sz);
    wr_be16(page + 5, content_start);
    wr_be16(page + 8 + ncells * 2, content_start);
    wr_be16(page + 3, (u16)(ncells + 1));
    return;
  }

  u32 new_leaf_pno = alloc_db_page();
  u8 *new_leaf = get_page_write(new_leaf_pno);
  new_leaf[0] = BTREE_LEAF_TABLE;
  u16 nl_content = (u16)(SQLITE_PAGE_SIZE - cell_sz);
  memcpy(new_leaf + nl_content, cell, cell_sz);
  wr_be16(new_leaf + 3, 1);
  wr_be16(new_leaf + 5, nl_content);
  wr_be16(new_leaf + 8, nl_content);

  u8 icell[16];
  wr_be32(icell, *right_leaf_pno);
  u32 icell_sz = 4 + (u32)wr_varint(icell + 4, (u64)(new_rowid - 1));

  u8 *parent = get_page_write(*right_int_pno);
  int pncells = rd_be16(parent + 3);
  u16 pcontent = rd_be16(parent + 5);
  if (UNLIKELY(pcontent == 0)) pcontent = SQLITE_PAGE_SIZE;

  if (LIKELY(pcontent >= 12 + (pncells + 1) * 2 + icell_sz)) {
    pcontent -= (u16)icell_sz;
    memcpy(parent + pcontent, icell, icell_sz);
    wr_be16(parent + 5, pcontent);
    wr_be16(parent + 12 + pncells * 2, pcontent);
    wr_be16(parent + 3, (u16)(pncells + 1));
    wr_be32(parent + 8, new_leaf_pno);
  } else {
    u16 last_off = rd_be16(parent + 12 + (pncells - 1) * 2);
    u32 prev_leaf = rd_be32(parent + last_off);
    u64 prev_key;
    const u8 *end_p = rd_varint_fast(parent + last_off + 4, &prev_key);
    u32 last_sz = (u32)(end_p - (parent + last_off));

    wr_be16(parent + 3, (u16)(pncells - 1));
    wr_be32(parent + 8, prev_leaf);
    if (last_off == pcontent) {
      memset(parent + last_off, 0, last_sz);
      wr_be16(parent + 5, (u16)(pcontent + last_sz));
    } else {
      parent[7] = (u8)(parent[7] + last_sz);
    }

    u32 new_int_pno = alloc_db_page();
    u8 *new_int = get_page_write(new_int_pno);
    new_int[0] = BTREE_INT_TABLE;
    u16 ni_content = (u16)(SQLITE_PAGE_SIZE - icell_sz);
    memcpy(new_int + ni_content, icell, icell_sz);
    wr_be16(new_int + 3, 1);
    wr_be16(new_int + 5, ni_content);
    wr_be32(new_int + 8, new_leaf_pno);
    wr_be16(new_int + 12, ni_content);

    u8 rcell[16];
    wr_be32(rcell, *right_int_pno);
    u32 rcell_sz = 4 + (u32)wr_varint(rcell + 4, prev_key);
    u8 *root = get_page_write(root_pno);
    int rncells = rd_be16(root + 3);
    u16 rcontent = rd_be16(root + 5);
    if (UNLIKELY(rcontent == 0)) rcontent = SQLITE_PAGE_SIZE;
    rcontent -= (u16)rcell_sz;
    memcpy(root + rcontent, rcell, rcell_sz);
    wr_be16(root + 5, rcontent);
    wr_be16(root + 12 + rncells * 2, rcontent);
    wr_be16(root + 3, (u16)(rncells + 1));
    wr_be32(root + 8, new_int_pno);

    *right_int_pno = new_int_pno;
  }
  *right_leaf_pno = new_leaf_pno;
}

static void index_insert_interior(u32 root_pno, int d0_idx, u32 d1_pno, int d1_idx,
                                  const u8 *prom_cell, u16 prom_cell_sz) {
  u8 *d1_page = get_page_write(d1_pno);
  int d1_ncells = rd_be16(d1_page + 3);
  u16 d1_content = rd_be16(d1_page + 5);
  if (UNLIKELY(d1_content == 0)) d1_content = SQLITE_PAGE_SIZE;

  if (LIKELY(d1_content >= 12 + (d1_ncells + 1) * 2 + prom_cell_sz)) {
    d1_content -= prom_cell_sz;
    memcpy(d1_page + d1_content, prom_cell, prom_cell_sz);
    wr_be16(d1_page + 5, d1_content);
    if (d1_idx < d1_ncells) {
      memmove(d1_page + 12 + (d1_idx + 1) * 2,
              d1_page + 12 + d1_idx * 2,
              (size_t)(d1_ncells - d1_idx) * 2);
    }
    wr_be16(d1_page + 12 + d1_idx * 2, d1_content);
    wr_be16(d1_page + 3, (u16)(d1_ncells + 1));
    return;
  }

  u8 old_int[SQLITE_PAGE_SIZE];
  memcpy(old_int, d1_page, SQLITE_PAGE_SIZE);
  u32 orig_right_child = rd_be32(old_int + 8);

  int itotal = d1_ncells + 1;
  const u8 *ic_ptr[512];
  u16 ic_len[512];
  for (int i = 0, j = 0; i < itotal; i++) {
    if (i == d1_idx) {
      ic_ptr[i] = prom_cell;
      ic_len[i] = prom_cell_sz;
    } else {
      u16 off = rd_be16(old_int + 12 + j * 2);
      u64 psz;
      const u8 *p = rd_varint_fast(old_int + off + 4, &psz);
      ic_ptr[i] = old_int + off;
      ic_len[i] = (u16)((p - (old_int + off)) + psz);
      j++;
    }
  }

  int imid = itotal >> 1;
  u32 left_int_pno = alloc_db_page();
  u8 *left_int = get_page_write(left_int_pno);
  left_int[0] = BTREE_INT_INDEX;
  u16 li_content = SQLITE_PAGE_SIZE;
  for (int i = 0; i < imid; i++) {
    li_content -= ic_len[i];
    memcpy(left_int + li_content, ic_ptr[i], ic_len[i]);
    wr_be16(left_int + 12 + i * 2, li_content);
  }
  wr_be16(left_int + 3, (u16)imid);
  wr_be16(left_int + 5, li_content);
  wr_be32(left_int + 8, rd_be32(ic_ptr[imid]));

  d1_page = get_page_write(d1_pno);
  memset(d1_page, 0, SQLITE_PAGE_SIZE);
  d1_page[0] = BTREE_INT_INDEX;
  int r_ncells = itotal - (imid + 1);
  u16 ri_content = SQLITE_PAGE_SIZE;
  for (int i = 0; i < r_ncells; i++) {
    int src = imid + 1 + i;
    ri_content -= ic_len[src];
    memcpy(d1_page + ri_content, ic_ptr[src], ic_len[src]);
    wr_be16(d1_page + 12 + i * 2, ri_content);
  }
  wr_be16(d1_page + 3, (u16)r_ncells);
  wr_be16(d1_page + 5, ri_content);
  wr_be32(d1_page + 8, orig_right_child);

  u8 root_prom[128];
  u16 root_prom_sz = ic_len[imid];
  memcpy(root_prom, ic_ptr[imid], root_prom_sz);
  wr_be32(root_prom, left_int_pno);

  u8 *root_page = get_page_write(root_pno);
  int r_nc = rd_be16(root_page + 3);
  u16 r_cont = rd_be16(root_page + 5);
  if (UNLIKELY(r_cont == 0)) r_cont = SQLITE_PAGE_SIZE;
  if (UNLIKELY(r_cont < 12 + (r_nc + 1) * 2 + root_prom_sz)) {
    return;
  }
  r_cont -= root_prom_sz;
  memcpy(root_page + r_cont, root_prom, root_prom_sz);
  wr_be16(root_page + 5, r_cont);
  if (d0_idx < r_nc) {
    memmove(root_page + 12 + (d0_idx + 1) * 2,
            root_page + 12 + d0_idx * 2,
            (size_t)(r_nc - d0_idx) * 2);
  }
  wr_be16(root_page + 12 + d0_idx * 2, r_cont);
  wr_be16(root_page + 3, (u16)(r_nc + 1));
}

static void index_insert_leaf(u32 root_pno, int d0_idx, u32 d1_pno, int d1_idx,
                              u32 leaf_pno, int ins_idx, const u8 *new_cell, u16 new_cell_sz) {
  u8 *page = get_page_write(leaf_pno);
  int ncells = rd_be16(page + 3);
  u16 content_start = rd_be16(page + 5);
  if (UNLIKELY(content_start == 0)) content_start = SQLITE_PAGE_SIZE;

  if (LIKELY(content_start >= 8 + (ncells + 1) * 2 + new_cell_sz)) {
    content_start -= new_cell_sz;
    memcpy(page + content_start, new_cell, new_cell_sz);
    wr_be16(page + 5, content_start);
    if (ins_idx < ncells) {
      memmove(page + 8 + (ins_idx + 1) * 2,
              page + 8 + ins_idx * 2,
              (size_t)(ncells - ins_idx) * 2);
    }
    wr_be16(page + 8 + ins_idx * 2, content_start);
    wr_be16(page + 3, (u16)(ncells + 1));
    return;
  }

  u8 old_leaf[SQLITE_PAGE_SIZE];
  memcpy(old_leaf, page, SQLITE_PAGE_SIZE);

  int total = ncells + 1;
  const u8 *c_ptr[512];
  u16 c_len[512];
  for (int i = 0, j = 0; i < total; i++) {
    if (i == ins_idx) {
      c_ptr[i] = new_cell;
      c_len[i] = new_cell_sz;
    } else {
      u16 off = rd_be16(old_leaf + 8 + j * 2);
      u64 psz;
      const u8 *p = rd_varint_fast(old_leaf + off, &psz);
      c_ptr[i] = old_leaf + off;
      c_len[i] = (u16)((p - (old_leaf + off)) + psz);
      j++;
    }
  }

  int mid = total >> 1;
  u32 left_leaf_pno = alloc_db_page();
  u8 *left_leaf = get_page_write(left_leaf_pno);
  left_leaf[0] = BTREE_LEAF_INDEX;
  u16 l_content = SQLITE_PAGE_SIZE;
  for (int i = 0; i < mid; i++) {
    l_content -= c_len[i];
    memcpy(left_leaf + l_content, c_ptr[i], c_len[i]);
    wr_be16(left_leaf + 8 + i * 2, l_content);
  }
  wr_be16(left_leaf + 3, (u16)mid);
  wr_be16(left_leaf + 5, l_content);

  page = get_page_write(leaf_pno);
  memset(page, 0, SQLITE_PAGE_SIZE);
  page[0] = BTREE_LEAF_INDEX;
  int r_ncells = total - (mid + 1);
  u16 r_content = SQLITE_PAGE_SIZE;
  for (int i = 0; i < r_ncells; i++) {
    int src = mid + 1 + i;
    r_content -= c_len[src];
    memcpy(page + r_content, c_ptr[src], c_len[src]);
    wr_be16(page + 8 + i * 2, r_content);
  }
  wr_be16(page + 3, (u16)r_ncells);
  wr_be16(page + 5, r_content);

  u8 prom_cell[128];
  wr_be32(prom_cell, left_leaf_pno);
  memcpy(prom_cell + 4, c_ptr[mid], c_len[mid]);
  u16 prom_cell_sz = (u16)(4 + c_len[mid]);

  index_insert_interior(root_pno, d0_idx, d1_pno, d1_idx, prom_cell, prom_cell_sz);
}

static void index_insert_int_rowid(u32 root_pno, i64 key_int, u8 t_key, const u8 *key_b, u32 key_l,
                                   u8 t_rid, const u8 *rid_b, u32 rid_l) {
  u8 idx_cell[32];
  u32 pay_sz = 3 + key_l + rid_l;
  u8 *pcp = idx_cell;
  pcp += wr_varint(pcp, pay_sz);
  *pcp++ = 3;
  *pcp++ = t_key;
  *pcp++ = t_rid;
  if (key_l) { memcpy(pcp, key_b, key_l); pcp += key_l; }
  if (rid_l) { memcpy(pcp, rid_b, rid_l); pcp += rid_l; }
  u16 idx_cell_sz = (u16)(pcp - idx_cell);

  u32 ppno = root_pno;
  int pd0_idx = 0, pd1_idx = 0;
  u32 pd1_pno = root_pno;
  int pdepth = 0;
  for (;;) {
    const u8 *ppage = get_page(ppno);
    u8 pptype = ppage[0];
    int pncells = rd_be16(ppage + 3);
    if (pptype == BTREE_INT_INDEX) {
      int plo = 0, phi = pncells - 1, pbest = pncells;
      while (plo <= phi) {
        int pmid = (plo + phi) >> 1;
        u16 pcoff = rd_be16(ppage + 12 + pmid * 2);
        u64 ppsz;
        const u8 *pp = rd_varint_fast(ppage + pcoff + 4, &ppsz);
        i64 ckey = parse_index_first_int(pp);
        if (key_int < ckey) {
          pbest = pmid;
          phi = pmid - 1;
        } else {
          plo = pmid + 1;
        }
      }
      if (pdepth == 0) pd0_idx = pbest;
      else { pd1_pno = ppno; pd1_idx = pbest; }
      pdepth++;
      if (pbest < pncells) {
        u16 pcoff = rd_be16(ppage + 12 + pbest * 2);
        ppno = rd_be32(ppage + pcoff);
      } else {
        ppno = rd_be32(ppage + 8);
      }
    } else {
      int plo = 0, phi = pncells - 1, pbest = pncells;
      while (plo <= phi) {
        int pmid = (plo + phi) >> 1;
        u16 pcoff = rd_be16(ppage + 8 + pmid * 2);
        u64 ppsz;
        const u8 *pp = rd_varint_fast(ppage + pcoff, &ppsz);
        i64 ckey = parse_index_first_int(pp);
        if (key_int < ckey) {
          pbest = pmid;
          phi = pmid - 1;
        } else {
          plo = pmid + 1;
        }
      }
      index_insert_leaf(root_pno, pd0_idx, pd1_pno, pd1_idx, ppno, pbest, idx_cell, idx_cell_sz);
      return;
    }
  }
}

int raw_insert_post(i64 uid, const u8 *body, u32 body_len, const u8 *created_at, u32 created_at_len,
                    const u8 *author, u32 author_len, raw_post_view_t *out_post) {
  i64 new_id = ++highest_post_id;

  u8 uid_bytes[8];
  u32 uid_len;
  u8 t_uid = encode_int_serial(uid, uid_bytes, &uid_len);

  u8 rec[2048];
  u8 *p = rec + 1;
  *p++ = 0;
  *p++ = t_uid;
  p += wr_varint(p, (u64)body_len * 2 + 13);
  p += wr_varint(p, (u64)created_at_len * 2 + 13);
  rec[0] = (u8)(p - rec);

  if (uid_len) { memcpy(p, uid_bytes, uid_len); p += uid_len; }
  memcpy(p, body, body_len); p += body_len;
  memcpy(p, created_at, created_at_len); p += created_at_len;
  u32 payload_sz = (u32)(p - rec);

  u8 cell[2064];
  u8 *cp = cell;
  cp += wr_varint(cp, payload_sz);
  cp += wr_varint(cp, (u64)new_id);
  memcpy(cp, rec, payload_sz);
  u32 cell_sz = (u32)(cp - cell) + payload_sz;

  table_append_rowid(root_posts, &rightmost_post_int, &rightmost_post_leaf, cell, cell_sz, new_id);

  out_post->id = new_id;
  out_post->user_id = uid;
  out_post->body = body;
  out_post->body_len = body_len;
  out_post->created_at = created_at;
  out_post->created_at_len = created_at_len;
  out_post->author = author;
  out_post->author_len = author_len;
  out_post->like_count = 0;
  return 0;
}

int raw_insert_like(i64 uid, i64 pid, const u8 *created_at, u32 created_at_len, bool *already_liked) {
  u32 pno = root_likes_pk;
  int d0_idx = 0, d1_idx = 0;
  u32 d1_pno = root_likes_pk;
  int depth = 0;

  for (;;) {
    const u8 *page = get_page(pno);
    u8 ptype = page[0];
    int ncells = rd_be16(page + 3);
    if (ptype == BTREE_INT_INDEX) {
      int lo = 0, hi = ncells - 1, best = ncells;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u16 coff = rd_be16(page + 12 + mid * 2);
        u64 psz;
        const u8 *p = rd_varint_fast(page + coff + 4, &psz);
        i64 cu, cp;
        parse_index_two_ints(p, &cu, &cp);
        if (UNLIKELY(uid == cu && pid == cp)) {
          *already_liked = true;
          return 0;
        }
        if (uid < cu || (uid == cu && pid < cp)) {
          best = mid;
          hi = mid - 1;
        } else {
          lo = mid + 1;
        }
      }
      if (depth == 0) {
        d0_idx = best;
      } else {
        d1_pno = pno;
        d1_idx = best;
      }
      depth++;
      if (best < ncells) {
        u16 coff = rd_be16(page + 12 + best * 2);
        pno = rd_be32(page + coff);
      } else {
        pno = rd_be32(page + 8);
      }
    } else if (LIKELY(ptype == BTREE_LEAF_INDEX)) {
      int lo = 0, hi = ncells - 1;
      while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        u16 coff = rd_be16(page + 8 + mid * 2);
        u64 psz;
        const u8 *p = rd_varint_fast(page + coff, &psz);
        i64 cu, cp;
        parse_index_two_ints(p, &cu, &cp);
        if (UNLIKELY(uid == cu && pid == cp)) {
          *already_liked = true;
          return 0;
        }
        if (uid < cu || (uid == cu && pid < cp)) {
          hi = mid - 1;
        } else {
          lo = mid + 1;
        }
      }
      *already_liked = false;
      i64 new_rowid = ++highest_like_rowid;

      u8 uid_b[8], pid_b[8], rid_b[8];
      u32 uid_l, pid_l, rid_l;
      u8 t_uid = encode_int_serial(uid, uid_b, &uid_l);
      u8 t_pid = encode_int_serial(pid, pid_b, &pid_l);
      u8 t_rid = encode_int_serial(new_rowid, rid_b, &rid_l);

      u8 pk_cell[32];
      u32 pk_pay_sz = 4 + uid_l + pid_l + rid_l;
      u8 *pkp = pk_cell;
      pkp += wr_varint(pkp, pk_pay_sz);
      *pkp++ = 4;
      *pkp++ = t_uid;
      *pkp++ = t_pid;
      *pkp++ = t_rid;
      if (uid_l) { memcpy(pkp, uid_b, uid_l); pkp += uid_l; }
      if (pid_l) { memcpy(pkp, pid_b, pid_l); pkp += pid_l; }
      if (rid_l) { memcpy(pkp, rid_b, rid_l); pkp += rid_l; }
      index_insert_leaf(root_likes_pk, d0_idx, d1_pno, d1_idx, pno, lo, pk_cell, (u16)(pkp - pk_cell));

      index_insert_int_rowid(root_likes_pid, pid, t_pid, pid_b, pid_l, t_rid, rid_b, rid_l);

      u8 lrec[64];
      u8 *lp = lrec + 1;
      *lp++ = t_uid;
      *lp++ = t_pid;
      lp += wr_varint(lp, (u64)created_at_len * 2 + 13);
      lrec[0] = (u8)(lp - lrec);
      if (uid_l) { memcpy(lp, uid_b, uid_l); lp += uid_l; }
      if (pid_l) { memcpy(lp, pid_b, pid_l); lp += pid_l; }
      memcpy(lp, created_at, created_at_len); lp += created_at_len;
      u32 lpay_sz = (u32)(lp - lrec);

      u8 lcell[80];
      u8 *lcp = lcell;
      lcp += wr_varint(lcp, lpay_sz);
      lcp += wr_varint(lcp, (u64)new_rowid);
      memcpy(lcp, lrec, lpay_sz);
      u32 lcell_sz = (u32)(lcp - lcell) + lpay_sz;

      table_append_rowid(root_likes, &rightmost_like_int, &rightmost_like_leaf, lcell, lcell_sz, new_rowid);
      return 0;
    } else return -1;
  }
}

static void *checkpoint_thread_main(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&ckpt_mu);
    while (!ckpt_needed && !ckpt_stop) pthread_cond_wait(&ckpt_cv, &ckpt_mu);
    if (ckpt_stop) {
      pthread_mutex_unlock(&ckpt_mu);
      break;
    }
    pthread_mutex_unlock(&ckpt_mu);

    fdatasync(db_fd);

    pthread_mutex_lock(&ckpt_mu);
    __atomic_store_n(&ckpt_needed, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return NULL;
}

void raw_sqlite_batch_begin(void) {
  ndirty = 0;
  if (UNLIKELY(++cur_batch_id == 0)) {
    memset(batch_dirty_mark, 0, sizeof batch_dirty_mark);
    cur_batch_id = 1;
  }
  if (UNLIKELY(wal_mx_frame >= MAX_WAL_FRAMES - 512)) {
    flush_ckpt_to_mmap();
  }
}

int raw_sqlite_batch_end(void) {
  if (ndirty == 0) return 0;

  u32 start_slot = wal_mx_frame;
  for (int i = 0; i < ndirty; i++) {
    wal_frame_slot_t *s = &wal_frames_buf[start_slot + (u32)i];
    s->hdr.page_no    = __builtin_bswap32(dirty_pnos[i]);
    s->hdr.n_truncate = (i == ndirty - 1) ? __builtin_bswap32(db_page_count) : 0;
    s->hdr.salt1      = wal_salt1;
    s->hdr.salt2      = wal_salt2;

    wal_checksum_bytes(1, (const u8 *)&s->hdr, 8, wal_last_cksum, wal_last_cksum);
    wal_checksum_bytes(1, s->page, SQLITE_PAGE_SIZE, wal_last_cksum, wal_last_cksum);
    s->hdr.cksum1 = wal_last_cksum[0];
    s->hdr.cksum2 = wal_last_cksum[1];
  }

  off_t off = WAL_HDR_SIZE + (off_t)start_slot * sizeof(wal_frame_slot_t);
  size_t total_bytes = (size_t)ndirty * sizeof(wal_frame_slot_t);
  if (pwrite(wal_fd, &wal_frames_buf[start_slot], total_bytes, off) < 0) {}

  wal_mx_frame += (u32)ndirty;
  ndirty = 0;

  if (wal_mx_frame >= 1000 && !__atomic_load_n(&ckpt_needed, __ATOMIC_ACQUIRE)) {
    flush_ckpt_to_mmap();
    pthread_mutex_lock(&ckpt_mu);
    __atomic_store_n(&ckpt_needed, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&ckpt_cv);
    pthread_mutex_unlock(&ckpt_mu);
  }
  return 0;
}

static void discover_schema_roots(void) {
  const u8 *p1 = get_page(1);
  if (!p1 || p1[100] != BTREE_LEAF_TABLE) return;
  int ncells = rd_be16(p1 + 103);
  for (int i = 0; i < ncells; i++) {
    u16 coff = rd_be16(p1 + 108 + i * 2);
    u64 psz, rid;
    const u8 *p = rd_varint_fast(p1 + coff, &psz);
    p = rd_varint_fast(p, &rid);
    col_ref_t cols[5];
    if (parse_record_cols(p, (u32)psz, 5, cols) != 0) continue;
    if (!col_is_text(cols[1].t) || !col_is_int(cols[3].t)) continue;
    const char *name = (const char *)cols[1].p;
    u32 nlen = serial_len(cols[1].t);
    u32 rp = (u32)col_decode_int(cols[3].t, cols[3].p);
    if (nlen == 5 && !memcmp(name, "users", 5)) root_users = rp;
    else if (nlen == 5 && !memcmp(name, "posts", 5)) root_posts = rp;
    else if (nlen == 5 && !memcmp(name, "likes", 5)) root_likes = rp;
    else if (nlen == 24 && !memcmp(name, "sqlite_autoindex_likes_1", 24)) root_likes_pk = rp;
    else if (nlen == 23 && !memcmp(name, "posts_created_at_id_idx", 23)) root_posts_time = rp;
    else if (nlen == 17 && !memcmp(name, "posts_user_id_idx", 17)) root_posts_user = rp;
    else if (nlen == 17 && !memcmp(name, "likes_post_id_idx", 17)) root_likes_pid = rp;
  }
}

int raw_sqlite_open(const char *path) {
  db_fd = open(path, O_RDWR);
  if (db_fd < 0) { perror("open db"); return -1; }
  struct stat st;
  fstat(db_fd, &st);
  db_mmap_sz = (size_t)st.st_size;
  db_page_count = (u32)(db_mmap_sz / SQLITE_PAGE_SIZE);

  db_mmap = mmap(NULL, db_mmap_sz, PROT_READ | PROT_WRITE, MAP_SHARED, db_fd, 0);
  if (db_mmap == MAP_FAILED) { perror("mmap db"); return -1; }
  madvise(db_mmap, db_mmap_sz, MADV_WILLNEED | MADV_HUGEPAGE);

  for (u32 p = 1; p <= db_page_count && p < MAX_DB_PAGES; p++) {
    page_ptrs[p] = db_mmap + (size_t)(p - 1) * SQLITE_PAGE_SIZE;
  }

  if (db_mmap[18] != 2 || db_mmap[19] != 2) {
    db_mmap[18] = 2;
    db_mmap[19] = 2;
  }

  char wal_path[1024];
  snprintf(wal_path, sizeof wal_path, "%s-wal", path);
  wal_fd = open(wal_path, O_RDWR | O_CREAT, 0644);
  if (wal_fd < 0) { perror("open wal"); return -1; }
  write_wal_header();

  discover_schema_roots();

  raw_table_cursor_t cur;
  if (raw_last_table(&cur, root_posts) == 0) {
    highest_post_id = cur.rowid;
    rightmost_post_leaf = cur.page_num;
    rightmost_post_int = (cur.depth > 0) ? cur.stack[cur.depth - 1].page_num : root_posts;
  }
  if (raw_last_table(&cur, root_likes) == 0) {
    highest_like_rowid = cur.rowid;
    rightmost_like_leaf = cur.page_num;
    rightmost_like_int = (cur.depth > 0) ? cur.stack[cur.depth - 1].page_num : root_likes;
  }

  if (pthread_create(&ckpt_tid, NULL, checkpoint_thread_main, NULL) == 0) {
    ckpt_init = 1;
  }
  return 0;
}

void raw_sqlite_close(void) {
  if (ckpt_init) {
    pthread_mutex_lock(&ckpt_mu);
    ckpt_stop = 1;
    pthread_cond_signal(&ckpt_cv);
    pthread_mutex_unlock(&ckpt_mu);
    pthread_join(ckpt_tid, NULL);
    ckpt_init = 0;
  }
  flush_ckpt_to_mmap();
  if (db_mmap && db_mmap != MAP_FAILED) {
    msync(db_mmap, db_mmap_sz, MS_SYNC);
    munmap(db_mmap, db_mmap_sz);
    db_mmap = NULL;
  }
  if (wal_fd >= 0) {
    if (ftruncate(wal_fd, WAL_HDR_SIZE) < 0) {}
    close(wal_fd);
    wal_fd = -1;
  }
  if (db_fd >= 0) {
    fdatasync(db_fd);
    close(db_fd);
    db_fd = -1;
  }
}
