package main

const hexDigits = "0123456789abcdef"

// appendJSONString appends s as a JSON string. Non-ASCII bytes pass through
// unchanged (valid UTF-8 in a JSON document); only the characters JSON requires
// to be escaped are escaped. The common case (no escapes needed) is a single copy.
func appendJSONString(dst, s []byte) []byte {
	needs := false
	for _, b := range s {
		if b == '"' || b == '\\' || b < 0x20 {
			needs = true
			break
		}
	}
	if !needs {
		dst = append(dst, '"')
		dst = append(dst, s...)
		return append(dst, '"')
	}
	dst = append(dst, '"')
	for _, b := range s {
		switch {
		case b == '"':
			dst = append(dst, '\\', '"')
		case b == '\\':
			dst = append(dst, '\\', '\\')
		case b == '\n':
			dst = append(dst, '\\', 'n')
		case b == '\r':
			dst = append(dst, '\\', 'r')
		case b == '\t':
			dst = append(dst, '\\', 't')
		case b < 0x20:
			dst = append(dst, '\\', 'u', '0', '0', hexDigits[b>>4], hexDigits[b&0xf])
		default:
			dst = append(dst, b)
		}
	}
	return append(dst, '"')
}
