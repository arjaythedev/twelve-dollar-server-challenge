package main

// parseID accepts a positive decimal integer that fits in int64; anything else is "invalid post id".
func parseID(b []byte) (int64, bool) {
	if len(b) == 0 || len(b) > 19 {
		return 0, false
	}
	var n int64
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		d := int64(c - '0')
		if n > (1<<63-1-d)/10 {
			return 0, false
		}
		n = n*10 + d
	}
	if n == 0 {
		return 0, false
	}
	return n, true
}
