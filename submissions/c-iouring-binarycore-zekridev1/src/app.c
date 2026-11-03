#define _GNU_SOURCE
#include "app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Constant error-response buffers. Defined here (one translation unit owns
 * them; app.h declares them extern). */
uint8_t bc_r_404[256], bc_r_400_id[256], bc_r_404_post[256],
    bc_r_401_missing[256], bc_r_401_invalid[256], bc_r_401_payload[256],
    bc_r_400_json[256], bc_r_400_body[256], bc_r_400_long[256],
    bc_r_500[256], bc_r_400_bad[256], bc_r_503[256];
unsigned bc_l_404, bc_l_400_id, bc_l_404_post, bc_l_401_missing,
    bc_l_401_invalid, bc_l_401_payload, bc_l_400_json, bc_l_400_body,
    bc_l_400_long, bc_l_500, bc_l_400_bad, bc_l_503;

void bc_app_init_responses(void) {
    bc_l_404 = bc_mk_resp(bc_r_404, "404 Not Found",
                          "{\"error\":\"not found\"}", 0);
    bc_l_400_id = bc_mk_resp(bc_r_400_id, "400 Bad Request",
                             "{\"error\":\"invalid post id\"}", 0);
    bc_l_404_post = bc_mk_resp(bc_r_404_post, "404 Not Found",
                               "{\"error\":\"post not found\"}", 0);
    bc_l_401_missing = bc_mk_resp(bc_r_401_missing, "401 Unauthorized",
                                  "{\"error\":\"missing bearer token\"}", 0);
    bc_l_401_invalid = bc_mk_resp(bc_r_401_invalid, "401 Unauthorized",
                                  "{\"error\":\"invalid or expired token\"}", 0);
    bc_l_401_payload = bc_mk_resp(bc_r_401_payload, "401 Unauthorized",
                                  "{\"error\":\"invalid token payload\"}", 0);
    bc_l_400_json = bc_mk_resp(bc_r_400_json, "400 Bad Request",
                               "{\"error\":\"malformed JSON body\"}", 0);
    bc_l_400_body = bc_mk_resp(bc_r_400_body, "400 Bad Request",
                               "{\"error\":\"body is required\"}", 0);
    bc_l_400_long = bc_mk_resp(bc_r_400_long, "400 Bad Request",
                               "{\"error\":\"body must be at most 500 characters\"}", 0);
    bc_l_500 = bc_mk_resp(bc_r_500, "500 Internal Server Error",
                          "{\"error\":\"internal server error\"}", 0);
    bc_l_400_bad = bc_mk_resp(bc_r_400_bad, "400 Bad Request",
                              "{\"error\":\"bad request\"}", 1);
    bc_l_503 = bc_mk_resp(bc_r_503, "503 Service Unavailable",
                          "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"select failed\"}", 0);
}

void bc_app_init(struct bc_app *a, const char *db_path, const char *secret,
                 int cache_kib) {
    memset(a, 0, sizeof *a);
    bc_app_init_responses();
    bc_hmac_key_init(&a->key, (const uint8_t *)secret, strlen(secret));
    a->start = a->now = (int64_t)time(NULL);
    if (bc_db_open(&a->db, db_path, cache_kib) < 0) {
        fprintf(stderr, "failed to open database %s\n", db_path);
    }
}

static inline int64_t bc_app_uptime(const struct bc_app *a) {
    return a->now - a->start;
}

void bc_app_handle(struct bc_app *a, const struct bc_req *r, uint8_t *free,
                   int writes_ok, struct bc_resp *out) {
    out->close = !r->keep_alive;
    out->db = 0;
    out->held = 0;
    a->n_req++;
    uint8_t *body = free + bc_hdr_room;
    struct bc_ob ob;
    bc_ob_init(&ob, body, bc_app_resp_max - bc_hdr_room);

    if (r->method == bc_m_get) {
        if (bc_path_is(r, "/feed", 5)) {
            a->n_feed++;
            out->db = 1;
            if (bc_db_feed(&a->db, &ob) < 0 || ob.err) goto err500;
            bc_resp_frame(out, free, "200 OK", 6, (unsigned)ob.n, out->close);
            return;
        }
        if (r->path_len > 7 && memcmp(r->path, "/posts/", 7) == 0) {
            a->n_post++;
            uint64_t id;
            if (!bc_parse_id(r->path + 7, r->path_len - 7, &id)) {
                bc_resp_static(out, free, bc_r_400_id, bc_l_400_id);
                return;
            }
            out->db = 1;
            int rc = bc_db_post(&a->db, &ob, (int64_t)id);
            if (rc < 0 || ob.err) goto err500;
            if (rc == 0) { bc_resp_static(out, free, bc_r_404_post, bc_l_404_post); return; }
            bc_resp_frame(out, free, "200 OK", 6, (unsigned)ob.n, out->close);
            return;
        }
        if (bc_path_is(r, "/health", 7)) {
            if (bc_db_health(&a->db) < 0) {
                bc_resp_static(out, free, bc_r_503, bc_l_503);
                return;
            }
            bc_ob_lit(&ob, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            bc_ob_u64(&ob, (uint64_t)bc_app_uptime(a));
            bc_ob_char(&ob, '}');
            bc_resp_frame(out, free, "200 OK", 6, (unsigned)ob.n, out->close);
            return;
        }
        bc_resp_static(out, free, bc_r_404, bc_l_404);
        return;
    }

    if (r->method == bc_m_post) {
        int is_create = bc_path_is(r, "/posts", 6);
        int is_like = r->path_len > 12 &&
            memcmp(r->path, "/posts/", 7) == 0 &&
            memcmp(r->path + r->path_len - 5, "/like", 5) == 0;
        if (!is_create && !is_like) {
            bc_resp_static(out, free, bc_r_404, bc_l_404);
            return;
        }
        if (!writes_ok) { out->held = 1; a->n_req--; return; }
        struct bc_jwt_claims claims;
        if (!bc_app_auth(a, r, free, out, &claims)) return;

        if (is_like) {
            a->n_like++;
            uint64_t id;
            if (!bc_parse_id(r->path + 7, r->path_len - 12, &id)) {
                bc_resp_static(out, free, bc_r_400_id, bc_l_400_id);
                return;
            }
            out->db = 1;
            int rc = bc_db_like(&a->db, (int64_t)claims.sub, (int64_t)id);
            if (rc < 0) goto err500;
            if (rc == 0) { bc_resp_static(out, free, bc_r_404_post, bc_l_404_post); return; }
            bc_ob_lit(&ob, "{\"liked\":true,\"already_liked\":");
            if (rc == 2) bc_ob_lit(&ob, "true,\"post_id\":");
            else bc_ob_lit(&ob, "false,\"post_id\":");
            bc_ob_u64(&ob, id);
            bc_ob_char(&ob, '}');
            if (rc == 1)
                bc_resp_frame(out, free, "201 Created", 11, (unsigned)ob.n, out->close);
            else
                bc_resp_frame(out, free, "200 OK", 6, (unsigned)ob.n, out->close);
            return;
        }

        /* POST /posts (create). */
        a->n_create++;
        struct bc_jwant w[1] = {{"body", 4, {0}}};
        int type = bc_json_doc((const char *)r->body, r->body_len, w, 1);
        if (type < 0) { bc_resp_static(out, free, bc_r_400_json, bc_l_400_json); return; }
        if (type != bc_j_object || w[0].v.type != bc_j_string) {
            bc_resp_static(out, free, bc_r_400_body, bc_l_400_body);
            return;
        }
        uint8_t text[4096];
        long tn = bc_json_unescape(w[0].v.p, w[0].v.n, text, sizeof text);
        if (tn < 0) { bc_resp_static(out, free, bc_r_400_long, bc_l_400_long); return; }
        uint8_t *t = text, *te = text + (size_t)tn;
        while (t < te && bc_is_ws(*t)) t++;
        while (te > t && bc_is_ws(te[-1])) te--;
        if (te == t) { bc_resp_static(out, free, bc_r_400_body, bc_l_400_body); return; }
        unsigned chars = 0;
        for (uint8_t *q = t; q < te; q++)
            if ((*q & 0xC0) != 0x80) chars++;
        if (chars > 500) { bc_resp_static(out, free, bc_r_400_long, bc_l_400_long); return; }
        out->db = 1;
        if (bc_db_create(&a->db, &ob, (int64_t)claims.sub, t, (unsigned)(te - t),
                         claims.username, claims.username_len) < 0 || ob.err)
            goto err500;
        bc_resp_frame(out, free, "201 Created", 11, (unsigned)ob.n, out->close);
        return;
    }

    bc_resp_static(out, free, bc_r_404, bc_l_404);
    return;

err500:
    a->n_err++;
    bc_resp_static(out, free, bc_r_500, bc_l_500);
}
