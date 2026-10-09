#!/usr/bin/env python3
"""Give the pinned fio HTTP finish task its own connection reference."""
import hashlib
from pathlib import Path
import sys

source, target = map(Path, sys.argv[1:])
raw = source.read_bytes()
expected = "fa05bdd4c193cf77cc1791f4e5a0c356c56e7abba38af284786231f0d0082154"
if hashlib.sha256(raw).hexdigest() != expected:
    raise SystemExit("Unexpected fio-stl source; refusing to patch")
text = raw.decode()
start = text.index("FIO_SFUNC void fio___http_controller_http1_on_finish_task(")
end = text.index("/* *****************************************************************************\nHTTP/1.1 Finish", start)
region = text[start:end]
old = "  fio_io_free(c->io);\n  return;"
assert region.count(old) == 2
region = region.replace(old, "  fio_io_free(c->io);\n  fio___http_connection_free(c); /* release the finish task's reference */\n  return;")
old = "fio_io_defer(fio___http_controller_http1_on_finish_task, (void *)(c), NULL);"
assert region.count(old) == 1
region = region.replace(old, "fio_io_defer(fio___http_controller_http1_on_finish_task,\n               (void *)fio___http_connection_dup(c), NULL);")
old = "fio_io_defer(fio___http_controller_http1_on_finish_task,\n               (void *)(c),"
assert region.count(old) == 1
region = region.replace(old, "fio_io_defer(fio___http_controller_http1_on_finish_task,\n               (void *)fio___http_connection_dup(c),")
target.write_text(text[:start] + region + text[end:])
print("Patched fio HTTP finish-task ownership:", hashlib.sha256(target.read_bytes()).hexdigest())
