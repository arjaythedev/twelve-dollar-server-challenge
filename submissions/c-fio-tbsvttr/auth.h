static sqlite3_stmt *auth_query;
static void init_auth(void) {
  auth_query = prepare(
    "SELECT json_extract(?2,'$.exp'),json_extract(?2,'$.nbf'),"
    "json_extract(?1,'$.alg')='HS256' AND json_type(?2,'$.exp') IN ('integer','real')"
    " AND (json_type(?2,'$.nbf') IS NULL OR json_type(?2,'$.nbf') IN ('integer','real')),"
    "json_type(?2,'$.sub')='text' AND json_type(?2,'$.username')='text',"
    "json_extract(?2,'$.sub'),json_extract(?2,'$.username')");
}
static void auth_reset(void) {
  sqlite3_reset(auth_query);
  sqlite3_clear_bindings(auth_query);
}
static fio_str_info_s jwt_decode(fio_str_info_s encoded) {
  char *decoded = fio_bstr_write_base64dec(NULL, encoded.buf, encoded.len);
  if (!decoded) return (fio_str_info_s){0};
  // fio-stl pads the last partial group; JWT base64url has no '=' padding.
  size_t length = encoded.len * 3 / 4;
  decoded[length] = 0;
  return FIO_STR_INFO2(decoded, length);
}

// Username borrows SQLite storage; call auth_reset() after constructing the response.
static const char *authenticate(fio_str_info_s auth, const char *secret,
                                sqlite3_int64 *user_id, fio_str_info_s *username) {
  auth_reset();
  *username = (fio_str_info_s){0};
  *user_id = 0;
  if (auth.len < 7 || memcmp(auth.buf, "Bearer ", 7))
    return "missing bearer token";
  auth.buf += 7;
  auth.len -= 7;
  fio_str_info_s parts[3] = {{.buf = auth.buf}};
  unsigned part = 0;
  for (size_t i = 0; i < auth.len; ++i) {
    unsigned char c = auth.buf[i];
    if (c == '.') {
      if (part == 2) return "invalid or expired token";
      parts[++part].buf = auth.buf + i + 1;
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_') {
      ++parts[part].len;
    } else return "invalid or expired token";
  }
  if (part != 2 || parts[2].len != 43) return "invalid or expired token";
  for (unsigned i = 0; i < 2; ++i)
    if (!parts[i].len || parts[i].len % 4 == 1) return "invalid or expired token";
  char signature[48];
  fio_str_info_s actual = {.buf = signature, .capa = sizeof(signature)};
  if (fio_string_write_base64dec(&actual, NULL, parts[2].buf, parts[2].len))
    return "invalid or expired token";
  fio_u256 expected = fio_sha256_hmac(secret, strlen(secret), auth.buf,
                                     (size_t)(parts[2].buf - auth.buf - 1));
  if (!fio_ct_is_eq(signature, expected.u8, 32)) return "invalid or expired token";
  for (unsigned i = 0; i < 2; ++i) {
    fio_str_info_s decoded = jwt_decode(parts[i]);
    int valid = decoded.buf && strict_json(decoded);
    if (valid) valid = sqlite3_bind_text(auth_query, (int)i + 1, decoded.buf,
                                       (int)decoded.len, SQLITE_TRANSIENT) == SQLITE_OK;
    fio_bstr_free(decoded.buf);
    if (!valid) return "invalid or expired token";
  }
  if (sqlite3_step(auth_query) != SQLITE_ROW || !sqlite3_column_int(auth_query, 2))
    return "invalid or expired token";
  struct timespec clock = fio_time_real();
  double now = (double)clock.tv_sec + (double)clock.tv_nsec / 1000000000.0;
  double expiry = sqlite3_column_double(auth_query, 0);
  double not_before = sqlite3_column_double(auth_query, 1);
  if (!isfinite(expiry) || expiry <= now || !isfinite(not_before) || not_before > now)
    return "invalid or expired token";
  if (!sqlite3_column_int(auth_query, 3)) return "invalid token payload";
  *user_id = positive_id(FIO_STR_INFO2((char *)sqlite3_column_text(auth_query, 4),
                                      sqlite3_column_bytes(auth_query, 4)));
  if (!*user_id) return "invalid token payload";
  *username = FIO_STR_INFO2((char *)sqlite3_column_text(auth_query, 5),
                           sqlite3_column_bytes(auth_query, 5));
  if (!utf8_valid(*username)) return "invalid token payload";
  return NULL;
}
