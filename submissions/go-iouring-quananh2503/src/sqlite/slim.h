#ifndef SLIM_H
#define SLIM_H

#include <stdint.h>

// Khởi tạo B-Tree & SQL Engine với đường dẫn database SQLite
int init_feed_engine(const char *db_path);

// Kiểm tra sức khỏe DB: trả về 1 nếu ok, 0 nếu hỏng
int db_health(void);

// Lấy 20 bài viết mới nhất dưới dạng JSON: {"posts":[...]}
// Ghi thẳng vào buffer `out`, trả về 0 nếu thành công, mã lỗi < 0 nếu thất bại.
int get_feed(char *out, int cap, int *outlen);

// Lấy 1 bài viết theo id dưới dạng JSON: {"post":{...}}
// Trả về: 200 (tìm thấy), 404 (không tìm thấy), < 0 (lỗi hệ thống)
int get_post(int64_t id, char *out, int cap, int *outlen);

// Kiểm tra bài viết id có tồn tại không: 1 có, 0 không, < 0 lỗi
int post_exists(int64_t id);

// Kết quả của từng bài viết trong get_posts_batch
typedef struct {
    int32_t status; // 200, 404, hoặc < 0
    int32_t offset; // vị trí bắt đầu trong out_buf
    int32_t len;    // độ dài chuỗi JSON trong out_buf
} PostBatchItem;

// Lấy nhiều bài viết theo batch trong 1 transaction duy nhất:
int get_posts_batch(const int64_t *ids, int n, char *out_buf, int out_cap, PostBatchItem *items);

// Kiểm tra nhiều bài viết có tồn tại hay không theo batch trong 1 transaction:
int post_exists_batch(const int64_t *ids, int n, int32_t *results);

// Kết quả của từng bài viết tạo mới trong create_posts_batch
typedef struct {
    int64_t id;         // ID vừa được tạo
    int32_t status;     // 201 nếu OK, 500 nếu lỗi
    int32_t cat_offset; // vị trí created_at trong out_cat_buf
    int32_t cat_len;    // độ dài created_at
} CreatePostBatchItem;

// Tạo nhiều bài viết theo batch trong 1 CGO call & 1 Write Transaction (Group Commit):
int create_posts_batch(
    const int64_t *user_ids,
    const char *bodies_buf,
    const int32_t *body_offsets,
    const int32_t *body_lens,
    int n,
    char *out_cat_buf,
    int out_cat_cap,
    CreatePostBatchItem *items
);

// Chèn nhiều like theo batch trong 1 CGO call & 1 Write Transaction (Group Commit):
// results: 201 (vừa like), 200 (đã like trước đó), 404 (post không tồn tại), 500 (lỗi)
int insert_likes_batch(
    const int64_t *user_ids,
    const int64_t *post_ids,
    int n,
    int32_t *results
);

// Đóng và giải phóng tài nguyên B-Tree Engine
void close_feed_engine(void);

#endif
