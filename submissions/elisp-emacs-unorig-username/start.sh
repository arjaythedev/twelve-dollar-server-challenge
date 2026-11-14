#!/usr/bin/env bash
# Runs the server in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
# A batch Emacs keeps the default SIGPIPE action, so ignore it here: a client that hangs up
# mid-response must be a Lisp error on that connection, not the end of the server.
trap '' PIPE
exec emacs --batch -Q -l "$(dirname "$0")/server.elc"
