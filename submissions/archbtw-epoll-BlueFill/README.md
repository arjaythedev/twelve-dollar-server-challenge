# I Use Arch BTW Server
> I actually use Debian. (Only for saying)

A server implementation for the Twelve-Dollar Server Challenge.
This is a Server written in `archbtw` (I Don't know how the official spelling is).

## Build/Run
The Compiler transpiles the `server.archbtw`-Code to C-Code.
`gcc` compiles it to native Machine Code with the `handler` as Wrapper.

The Server creates an own Webserver. (**no NGINX required**)

## Optimizations
- Non-blocking sockets with edge-triggered `epoll`.
- HTTP keep-alive for persistent connections.
- Reuse of prepared SQLite statements without caching query results.
- Native compilation using GCC `-O3`, `-march=native` and `-flto`.

## Versions
Programmed with this versions
- Java: 25.0.4.1
- GCC: 11.4.0
- SQLite: 3.37.2
- OpenSSL: 3.0.2
