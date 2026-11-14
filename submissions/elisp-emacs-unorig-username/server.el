;;; server.el --- the feed API, served by GNU Emacs -*- lexical-binding: t -*-

(setq gc-cons-threshold (* 16 1024 1024) read-process-output-max 65536)

(defconst feed-start (time-convert nil 'integer))
(defconst feed-db (sqlite-open (getenv "SQLITE_PATH")))
(sqlite-pragma feed-db "journal_mode=WAL")
(sqlite-pragma feed-db "synchronous=NORMAL")

(defconst feed-post-sql
  "SELECT json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, 'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id)) o FROM posts p JOIN users u ON u.id = p.user_id ")
(defconst feed-list-sql (concat "SELECT '{\"posts\":[' || coalesce(group_concat(o), '') || ']}' FROM (" feed-post-sql "ORDER BY p.created_at DESC, p.id DESC LIMIT 20)"))
(defconst feed-one-sql (concat feed-post-sql "WHERE p.id = ?"))

;; HMAC-SHA256 key pads (RFC 2104), built once from JWT_SECRET.
(defconst feed-key
  (let ((k (encode-coding-string (getenv "JWT_SECRET") 'utf-8)))
    (when (> (length k) 64) (setq k (secure-hash 'sha256 k nil nil t)))
    (concat k (make-string (- 64 (length k)) 0))))
(defconst feed-ipad (apply #'unibyte-string (mapcar (lambda (b) (logxor b #x36)) feed-key)))
(defconst feed-opad (apply #'unibyte-string (mapcar (lambda (b) (logxor b #x5c)) feed-key)))

(defun feed-hmac (msg)
  (secure-hash 'sha256 (concat feed-opad (secure-hash 'sha256 (concat feed-ipad msg) nil nil t)) nil nil t))

(defun feed-reply (proc status body)
  (process-send-string
   proc (concat "HTTP/1.1 " status "\r\nContent-Type: application/json\r\nContent-Length: "
                (number-to-string (string-bytes body)) "\r\n\r\n" body)))

(defun feed-error (proc status msg)
  (feed-reply proc status (concat "{\"error\":\"" msg "\"}")))

(defun feed-pos-int (s)
  "S as a positive integer, or nil."
  (and (string-match-p "\\`[0-9]+\\'" s) (string-match-p "[1-9]" s) (min (string-to-number s) most-positive-fixnum)))

(defun feed-header (re buf pos end)
  "Value of the header matched by RE in the request at BUF[POS..END], or nil."
  (let ((case-fold-search t))
    (and (string-match re buf pos)
         (< (match-beginning 0) end)
         (match-string 1 buf))))

(defun feed-auth (buf pos end)
  "The request's user as (ID . USERNAME), or the 401 message."
  (let ((v (feed-header "\r\nauthorization: *\\([^\r]*\\)" buf pos end)))
    (if (not (and v (string-prefix-p "Bearer " v)))
        "missing bearer token"
      (let* ((parts (split-string (substring v 7) "\\."))
             (h (car parts)) (p (cadr parts))
             (c (ignore-errors
                  (let* ((c (and (= (length parts) 3)
                                 (equal (gethash "alg" (json-parse-string (base64-decode-string h t))) "HS256")
                                 (string= (nth 2 parts) (base64url-encode-string (feed-hmac (concat h "." p)) t))
                                 (json-parse-string (decode-coding-string (base64-decode-string p t) 'utf-8))))
                         (exp (gethash "exp" c)) (nbf (gethash "nbf" c)))
                    (and (numberp exp) (> exp (float-time)) (or (not nbf) (and (numberp nbf) (<= nbf (float-time)))) c)))))
        (if (not c)
            "invalid or expired token"
          (let* ((sub (gethash "sub" c)) (name (gethash "username" c))
                 (id (and (stringp sub) (feed-pos-int sub))))
            (if (and id (stringp name)) (cons id name) "invalid token payload")))))))

(defun feed-get-post (proc s)
  (let* ((id (feed-pos-int s)) (r (and id (sqlite-select feed-db feed-one-sql (list id)))))
    (cond ((not id) (feed-error proc "400 Bad Request" "invalid post id"))
          (r (feed-reply proc "200 OK" (concat "{\"post\":" (caar r) "}")))
          (t (feed-error proc "404 Not Found" "post not found")))))

(defun feed-create (proc user json)
  (let* ((j (ignore-errors (list (json-parse-string (decode-coding-string json 'utf-8)))))
         (b (and (hash-table-p (car j)) (gethash "body" (car j))))
         (b (and (stringp b) (string-trim b))))
    (cond ((not j) (feed-error proc "400 Bad Request" "malformed JSON body"))
          ((member b '(nil "")) (feed-error proc "400 Bad Request" "body is required"))
          ((> (length b) 500) (feed-error proc "400 Bad Request" "body must be at most 500 characters"))
          (t (let ((r (car (sqlite-select feed-db "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
                                          (list (car user) b)))))
               (feed-reply proc "201 Created"
                           (json-serialize (list :post (list :id (car r) :body b :created_at (cadr r)
                                                             :author (cdr user) :like_count 0)))))))))

(defun feed-like (proc uid s)
  (let ((id (feed-pos-int s)))
    (cond ((not id) (feed-error proc "400 Bad Request" "invalid post id"))
          ((= 1 (sqlite-execute feed-db "INSERT INTO likes (user_id, post_id) SELECT ?, id FROM posts WHERE id = ? ON CONFLICT (user_id, post_id) DO NOTHING"
                                (list uid id)))
           (feed-reply proc "201 Created" (format "{\"liked\":true,\"already_liked\":false,\"post_id\":%d}" id)))
          ((sqlite-select feed-db "SELECT 1 FROM posts WHERE id = ?" (list id))
           (feed-reply proc "200 OK" (format "{\"liked\":true,\"already_liked\":true,\"post_id\":%d}" id)))
          (t (feed-error proc "404 Not Found" "post not found")))))

(defun feed-health (proc)
  (condition-case e
      (progn (sqlite-select feed-db "SELECT 1")
             (feed-reply proc "200 OK" (format "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":%d}"
                                               (- (time-convert nil 'integer) feed-start))))
    (error (feed-reply proc "503 Service Unavailable"
                       (json-serialize (list :status "degraded" :db "unreachable" :error (error-message-string e)))))))

(defun feed-handle (proc buf pos end total)
  "Answer the request at BUF[POS..TOTAL], whose headers end at END."
  (condition-case nil
      (let* ((line (substring buf pos (string-search " HTTP/" buf pos)))
             (line (substring line 0 (string-search "?" line))))
        (cond ((string= line "GET /feed")
               (feed-reply proc "200 OK" (caar (sqlite-select feed-db feed-list-sql))))
              ((string-match-p "\\`GET /posts/." line) (feed-get-post proc (substring line 11)))
              ((or (string= line "POST /posts") (string-match "\\`POST /posts/\\(.*\\)/like\\'" line))
               (let ((user (feed-auth buf pos end)))
                 (cond ((stringp user) (feed-error proc "401 Unauthorized" user))
                       ((= (length line) 11) (feed-create proc user (substring buf (+ end 4) total)))
                       (t (feed-like proc (car user) (substring line 12 -5))))))
              ((string= line "GET /health") (feed-health proc))
              (t (feed-error proc "404 Not Found" "not found"))))
    (error (feed-error proc "500 Internal Server Error" "internal server error"))))

(defun feed-filter (proc chunk)
  "Answer each complete request in the input so far; keep a partial one."
  ;; An error that escapes a filter kills a batch Emacs, so a failed write drops the connection.
  (condition-case nil
      (let* ((old (process-get proc 'buf)) (buf (if old (concat old chunk) chunk)) (pos 0) end total)
        (while (and (setq end (string-search "\r\n\r\n" buf pos))
                    (<= (setq total (+ end 4 (string-to-number (or (feed-header "\r\ncontent-length: *\\([0-9]+\\)" buf pos end) "0"))))
                        (length buf)))
          (feed-handle proc buf pos end total)
          (setq pos total))
        (when (> (- (length buf) pos) 1048576) (error "Request too large"))
        (process-put proc 'buf (and (< pos (length buf)) (substring buf pos))))
    (error (delete-process proc))))

(defvar feed-conns 0)
(defvar feed-server)

(defun feed-sentinel (_proc msg)
  "Count connections; stop accepting near the 1024 descriptors of `select'."
  (setq feed-conns (+ feed-conns (if (string-prefix-p "open" msg) 1 -1)))
  (if (< feed-conns 1000) (continue-process feed-server) (stop-process feed-server)))

(setq feed-server
      (make-network-process :name "feed" :server 1024 :family 'ipv4 :host (getenv "HOST")
                            :service (string-to-number (getenv "PORT")) :coding '(binary . utf-8) :nodelay t
                            :noquery t :filter #'feed-filter :sentinel #'feed-sentinel))
(while t (accept-process-output nil 3600))
