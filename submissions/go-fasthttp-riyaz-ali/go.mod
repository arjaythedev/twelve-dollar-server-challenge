module github.com/riyaz-ali/twelve-dollar-server-challenge

go 1.27.3

require (
	crawshaw.io/sqlite v0.3.2
	github.com/fasthttp/router v1.5.4
	github.com/valyala/fasthttp v1.77.0
	github.com/valyala/fastjson v1.6.10
)

require (
	github.com/klauspost/compress v1.20.3 // indirect
	github.com/molecule-man/go-brrr v1.3.1 // indirect
	github.com/savsgio/gotils v0.0.0-20250924091648-bce9a52d7761 // indirect
	github.com/valyala/bytebufferpool v1.0.0 // indirect
)

replace crawshaw.io/sqlite v0.3.2 => github.com/riyaz-ali/sqlite3 v0.0.0-20260516143954-99e6c1ebd044
