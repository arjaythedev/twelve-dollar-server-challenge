package main

// #include "shim.h"
import "C"

func gcovDump() { C.shim_gcov_dump() }
