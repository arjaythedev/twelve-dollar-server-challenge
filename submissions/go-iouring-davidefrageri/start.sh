#!/usr/bin/env bash
# Runs the server in the foreground; configured by SQLITE_PATH, JWT_SECRET, HOST and PORT.
# One OS thread does all the work (see main.go); the runtime is told not to add more.
# asyncpreemptoff: no preemption signals aimed at the one busy thread (nothing else needs the CPU).
export GOMAXPROCS=1 GODEBUG=asyncpreemptoff=1
exec "$(dirname "$0")/bin/server"
