#!/usr/bin/env bash
# Runs the server (an octave-cli process) in the foreground. Config: SQLITE_PATH, JWT_SECRET, HOST, PORT.
cd "$(dirname "$0")"
exec octave-cli --norc --no-history --no-gui --quiet server.m
