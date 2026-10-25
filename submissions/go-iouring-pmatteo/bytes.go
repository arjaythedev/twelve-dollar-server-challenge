package main

func eq(b []byte, s string) bool { return string(b) == s }

func hasPrefix(b []byte, s string) bool {
	return len(b) >= len(s) && string(b[:len(s)]) == s
}

func hasSuffix(b []byte, s string) bool {
	return len(b) >= len(s) && string(b[len(b)-len(s):]) == s
}

func isASCIISpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'
}

func trimSpace(b []byte) []byte {
	i, j := 0, len(b)
	for i < j && isASCIISpace(b[i]) {
		i++
	}
	for j > i && isASCIISpace(b[j-1]) {
		j--
	}
	return b[i:j]
}

// runeCount counts UTF-8 sequences by their lead bytes, which is what SQLite's length() counts.
func runeCount(b []byte) int {
	n := 0
	for _, c := range b {
		if c&0xC0 != 0x80 {
			n++
		}
	}
	return n
}

func appendInt(out []byte, v int64) []byte {
	if v == 0 {
		return append(out, '0')
	}
	if v < 0 {
		out = append(out, '-')
		v = -v
	}
	var buf [20]byte
	i := len(buf)
	for v > 0 {
		i--
		buf[i] = byte('0' + v%10)
		v /= 10
	}
	return append(out, buf[i:]...)
}

const hexDigits = "0123456789abcdef"

// appendJSONString writes a quoted JSON string. Escaping <>&/ and non-ASCII is optional per the
// spec, so only the characters JSON actually requires are escaped and UTF-8 passes through.
func appendJSONString(out, s []byte) []byte {
	out = append(out, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		out = append(out, s[start:i]...)
		switch c {
		case '"':
			out = append(out, '\\', '"')
		case '\\':
			out = append(out, '\\', '\\')
		case '\n':
			out = append(out, '\\', 'n')
		case '\r':
			out = append(out, '\\', 'r')
		case '\t':
			out = append(out, '\\', 't')
		case '\b':
			out = append(out, '\\', 'b')
		case '\f':
			out = append(out, '\\', 'f')
		default:
			out = append(out, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xF])
		}
		start = i + 1
	}
	out = append(out, s[start:]...)
	return append(out, '"')
}
