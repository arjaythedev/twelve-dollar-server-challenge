package main

import "core:encoding/json"
import "core:strings"
import "core:time"

import http "http:."

MAX_BODY_BYTES :: 16 * 1024
MAX_BODY_CHARS :: 500

started: time.Tick

send :: proc(res: ^http.Response, status: http.Status, body: []byte) {
	http.headers_set_unsafe(&res.headers, "content-type", "application/json")
	res.status = status
	http.body_set(res, body)
	http.respond(res)
}

send_str :: #force_inline proc(res: ^http.Response, status: http.Status, body: string) {
	send(res, status, transmute([]byte)body)
}

ERR_NOT_FOUND      :: `{"error":"not found"}`
ERR_INVALID_ID     :: `{"error":"invalid post id"}`
ERR_POST_NOT_FOUND :: `{"error":"post not found"}`
ERR_MISSING_TOKEN  :: `{"error":"missing bearer token"}`
ERR_INVALID_TOKEN  :: `{"error":"invalid or expired token"}`
ERR_TOKEN_PAYLOAD  :: `{"error":"invalid token payload"}`
ERR_MALFORMED_JSON :: `{"error":"malformed JSON body"}`
ERR_BODY_REQUIRED  :: `{"error":"body is required"}`
ERR_BODY_TOO_LONG  :: `{"error":"body must be at most 500 characters"}`
ERR_INTERNAL       :: `{"error":"internal server error"}`

handle :: proc(req: ^http.Request, res: ^http.Response) {
	path := req.url.path
	#partial switch req.line.?.method {
	case .Get:
		switch {
		case path == "/feed":
			handle_feed(res)
			return
		case strings.has_prefix(path, "/posts/"):
			handle_get_post(res, path[len("/posts/"):])
			return
		case path == "/health":
			handle_health(res)
			return
		}
	case .Post:
		switch {
		case path == "/posts":
			handle_create(req, res)
			return
		case strings.has_prefix(path, "/posts/") && strings.has_suffix(path, "/like") && len(path) >= len("/posts//like"):
			handle_like(req, res, path[len("/posts/"):len(path) - len("/like")])
			return
		}
	}
	send_str(res, .Not_Found, ERR_NOT_FOUND)
}

handle_feed :: proc(res: ^http.Response) {
	out_reset()
	if !write_feed() {
		send_str(res, .Internal_Server_Error, ERR_INTERNAL)
		return
	}
	send(res, .OK, out_bytes())
}

handle_get_post :: proc(res: ^http.Response, id_str: string) {
	id, ok := parse_positive_id(id_str)
	if !ok {
		send_str(res, .Bad_Request, ERR_INVALID_ID)
		return
	}
	out_reset()
	switch write_post_by_id(id) {
	case .Found:   send(res, .OK, out_bytes())
	case .Missing: send_str(res, .Not_Found, ERR_POST_NOT_FOUND)
	case .Failed:  send_str(res, .Internal_Server_Error, ERR_INTERNAL)
	}
}

handle_health :: proc(res: ^http.Response) {
	out_reset()
	if db_healthy() {
		write_str(`{"status":"ok","db":"ok","uptime_s":`)
		write_int(i64(time.duration_seconds(time.tick_since(started))))
		write_str(`}`)
		send(res, .OK, out_bytes())
	} else {
		write_str(`{"status":"degraded","db":"unreachable","error":`)
		write_json_string(transmute([]byte)string(errmsg(db)))
		write_str(`}`)
		send(res, .Service_Unavailable, out_bytes())
	}
}

// Writes a 401 for the auth failure; false when the request may go on.
reject_auth :: proc(res: ^http.Response, err: Auth_Error) -> bool {
	switch err {
	case .None:    return false
	case .Missing: send_str(res, .Unauthorized, ERR_MISSING_TOKEN)
	case .Invalid: send_str(res, .Unauthorized, ERR_INVALID_TOKEN)
	case .Payload: send_str(res, .Unauthorized, ERR_TOKEN_PAYLOAD)
	}
	return true
}

handle_like :: proc(req: ^http.Request, res: ^http.Response, id_str: string) {
	auth, auth_err := verify_bearer(req)
	if reject_auth(res, auth_err) { return }
	id, ok := parse_positive_id(id_str)
	if !ok {
		send_str(res, .Bad_Request, ERR_INVALID_ID)
		return
	}
	switch post_exists(id) {
	case .Found:
	case .Missing:
		send_str(res, .Not_Found, ERR_POST_NOT_FOUND)
		return
	case .Failed:
		send_str(res, .Internal_Server_Error, ERR_INTERNAL)
		return
	}
	inserted, like_ok := insert_like(auth.user_id, id)
	if !like_ok {
		send_str(res, .Internal_Server_Error, ERR_INTERNAL)
		return
	}
	out_reset()
	write_str(`{"liked":true,"already_liked":`)
	write_str("false" if inserted else "true")
	write_str(`,"post_id":`)
	write_int(id)
	write_str(`}`)
	send(res, .Created if inserted else .OK, out_bytes())
}

Create_Request :: struct {
	res:  ^http.Response,
	auth: Auth,
}

handle_create :: proc(req: ^http.Request, res: ^http.Response) {
	auth, auth_err := verify_bearer(req)
	if reject_auth(res, auth_err) { return }
	cr := new(Create_Request, context.temp_allocator)
	cr^ = {res = res, auth = auth}
	http.body(req, MAX_BODY_BYTES, cr, on_create_body)
}

on_create_body :: proc(user_data: rawptr, body: http.Body, err: http.Body_Error) {
	cr := (^Create_Request)(user_data)
	res := cr.res
	if err != nil {
		send_str(res, .Bad_Request, ERR_MALFORMED_JSON)
		return
	}
	value, json_err := json.parse_string(body, .JSON, false, context.temp_allocator)
	if json_err != .None {
		send_str(res, .Bad_Request, ERR_MALFORMED_JSON)
		return
	}
	obj, is_obj := value.(json.Object)
	if !is_obj {
		send_str(res, .Bad_Request, ERR_BODY_REQUIRED)
		return
	}
	text, is_str := obj["body"].(json.String)
	if !is_str {
		send_str(res, .Bad_Request, ERR_BODY_REQUIRED)
		return
	}
	text = js_trim(text)
	if len(text) == 0 {
		send_str(res, .Bad_Request, ERR_BODY_REQUIRED)
		return
	}
	if count_code_points(text) > MAX_BODY_CHARS {
		send_str(res, .Bad_Request, ERR_BODY_TOO_LONG)
		return
	}
	out_reset()
	if !create_post(cr.auth.user_id, text, cr.auth.username) {
		send_str(res, .Internal_Server_Error, ERR_INTERNAL)
		return
	}
	send(res, .Created, out_bytes())
}
