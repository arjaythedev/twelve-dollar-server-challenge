//go:build !sqlite_llvm_pgo

package main

func startSQLiteProfile() error { return nil }
func dumpSQLiteProfile() error  { return nil }
