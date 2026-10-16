package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
	"time"
)

var (
	jwtSecret      []byte
	jwtMAC         hash.Hash
	dotByte        = []byte(".")
	sigScratch     [32]byte
	payloadScratch [1024]byte
	hdrScratch     [128]byte
)

func setJWTSecret(secret []byte) {
	jwtSecret = secret
	jwtMAC = hmac.New(sha256.New, jwtSecret)
}

func authenticate(authHeader []byte) (uid int64, username []byte, status int, msg string) {
	if !bytes.HasPrefix(authHeader, []byte("Bearer ")) {
		return 0, nil, 401, "missing bearer token"
	}
	token := authHeader[len("Bearer "):]
	uid, username, ok, payloadOK := verifyJWT(token, time.Now().Unix())
	if !ok {
		return 0, nil, 401, "invalid or expired token"
	}
	if !payloadOK {
		return 0, nil, 401, "invalid token payload"
	}
	return uid, username, 0, ""
}

// verifyJWT validates the token without heap allocations on the hot path.
func verifyJWT(tok []byte, now int64) (uid int64, username []byte, ok, payloadOK bool) {
	// 1. Split header.payload.signature using bytes.Cut (0 allocations)
	hBytes, rest, hasDot1 := bytes.Cut(tok, dotByte)
	if !hasDot1 {
		return 0, nil, false, false
	}
	pBytes, sBytes, hasDot2 := bytes.Cut(rest, dotByte)
	if !hasDot2 || bytes.IndexByte(sBytes, '.') >= 0 {
		return 0, nil, false, false
	}

	enc := base64.RawURLEncoding

	// Decode signature (must be 32 bytes for HS256)
	var sigBuf [48]byte
	nSig, err := enc.Decode(sigBuf[:], sBytes)
	if err != nil || nSig != 32 {
		return 0, nil, false, false
	}

	// 2. Decode header into scratch buffer
	if enc.DecodedLen(len(hBytes)) > len(hdrScratch) {
		return 0, nil, false, false
	}
	nHdr, err := enc.Decode(hdrScratch[:], hBytes)
	if err != nil || nHdr == 0 {
		return 0, nil, false, false
	}
	hdr := hdrScratch[:nHdr]

	// Header check: must have "alg":"HS256"
	if !isHS256Header(hdr) {
		return 0, nil, false, false
	}

	// 3. Verify HMAC signature using the single-threaded preallocated hasher
	jwtMAC.Reset()
	_, _ = jwtMAC.Write(hBytes)
	_, _ = jwtMAC.Write(dotByte)
	_, _ = jwtMAC.Write(pBytes)
	calculated := jwtMAC.Sum(sigScratch[:0])

	if !hmac.Equal(sigBuf[:32], calculated) {
		return 0, nil, false, false
	}

	// 4. Decode payload into scratch buffer (up to 1024 bytes)
	var plBytes []byte
	decLen := enc.DecodedLen(len(pBytes))
	if decLen <= len(payloadScratch) {
		nPl, err := enc.Decode(payloadScratch[:], pBytes)
		if err != nil || nPl == 0 {
			return 0, nil, false, false
		}
		plBytes = payloadScratch[:nPl]
	} else {
		// Fallback for unusually large payload
		var err error
		plBytes, err = enc.DecodeString(string(pBytes))
		if err != nil || len(plBytes) == 0 {
			return 0, nil, false, false
		}
	}

	// 5. Parse claims (sub, username, exp, nbf) with full JSON awareness
	uid, username, expValid, nbfValid, plValid := parseJWTClaims(plBytes, now)
	if !expValid || !nbfValid {
		return 0, nil, false, false
	}
	if !plValid {
		return 0, nil, true, false
	}

	return uid, username, true, true
}

// isHS256Header parses the JSON header properly and validates that alg is "HS256".
func isHS256Header(h []byte) bool {
	h = bytes.TrimSpace(h)
	if len(h) < 2 || h[0] != '{' || h[len(h)-1] != '}' {
		return false
	}
	h = h[1 : len(h)-1]

	for len(h) > 0 {
		key, val, isStr, rest, ok := nextJSONProp(h)
		if !ok {
			return false
		}
		h = rest
		if bytes.Equal(key, []byte("alg")) {
			return isStr && bytes.Equal(val, []byte("HS256"))
		}
	}
	return false
}

// parseJWTClaims parses sub (positive int string), username (string), exp and nbf.
// It iterates top-level key/value pairs robustly, completely immune to key order or
// substrings inside strings.
func parseJWTClaims(b []byte, now int64) (uid int64, username []byte, expValid, nbfValid, payloadValid bool) {
	expValid = true
	nbfValid = true

	b = bytes.TrimSpace(b)
	if len(b) < 2 || b[0] != '{' || b[len(b)-1] != '}' {
		return 0, nil, false, false, false
	}
	b = b[1 : len(b)-1]

	var hasSub, hasUsername bool

	for len(b) > 0 {
		key, val, isStr, rest, ok := nextJSONProp(b)
		if !ok {
			return 0, nil, true, true, false
		}
		b = rest

		switch string(key) {
		case "exp":
			if isStr {
				return 0, nil, false, false, false
			}
			expSec, ok := parseNumeric(val)
			if !ok {
				return 0, nil, false, false, false
			}
			if float64(now) >= expSec {
				expValid = false
				return 0, nil, false, true, false
			}
		case "nbf":
			if isStr {
				return 0, nil, false, false, false
			}
			nbfSec, ok := parseNumeric(val)
			if !ok {
				return 0, nil, false, false, false
			}
			if float64(now) < nbfSec {
				nbfValid = false
				return 0, nil, true, false, false
			}
		case "sub":
			if !isStr || len(val) == 0 || len(val) > 18 {
				return 0, nil, true, true, false
			}
			var parsedUID int64
			for i := 0; i < len(val); i++ {
				c := val[i]
				if c < '0' || c > '9' {
					return 0, nil, true, true, false
				}
				parsedUID = parsedUID*10 + int64(c-'0')
			}
			if parsedUID <= 0 {
				return 0, nil, true, true, false
			}
			uid = parsedUID
			hasSub = true
		case "username":
			if !isStr {
				return 0, nil, true, true, false
			}
			username = val
			hasUsername = true
		}
	}

	if hasSub && hasUsername {
		payloadValid = true
	}
	return uid, username, expValid, nbfValid, payloadValid
}

// nextJSONProp extracts the next "key": value pair from a JSON object body.
func nextJSONProp(b []byte) (key, val []byte, isStr bool, rest []byte, ok bool) {
	// Skip whitespace and commas
	i := 0
	n := len(b)
	for i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n' || b[i] == ',') {
		i++
	}
	if i >= n {
		return nil, nil, false, nil, false
	}

	// Key must be a quoted string
	if b[i] != '"' {
		return nil, nil, false, nil, false
	}
	i++
	kStart := i
	for i < n {
		if b[i] == '\\' {
			i += 2
			continue
		}
		if b[i] == '"' {
			break
		}
		i++
	}
	if i >= n {
		return nil, nil, false, nil, false
	}
	key = b[kStart:i]
	i++ // skip closing quote

	// Expect ':'
	for i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n') {
		i++
	}
	if i >= n || b[i] != ':' {
		return nil, nil, false, nil, false
	}
	i++ // skip ':'

	// Skip whitespace before value
	for i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n') {
		i++
	}
	if i >= n {
		return nil, nil, false, nil, false
	}

	// Value
	if b[i] == '"' {
		isStr = true
		i++
		vStart := i
		hasEscape := false
		for i < n {
			if b[i] == '\\' {
				hasEscape = true
				i += 2
				continue
			}
			if b[i] == '"' {
				break
			}
			i++
		}
		if i >= n {
			return nil, nil, false, nil, false
		}
		raw := b[vStart:i]
		if hasEscape {
			val = unescapeJSON(raw)
		} else {
			val = raw
		}
		i++ // skip closing quote
	} else if b[i] == '{' || b[i] == '[' {
		open := b[i]
		closeChar := byte('}')
		if open == '[' {
			closeChar = ']'
		}
		depth := 1
		vStart := i
		i++
		inS := false
		for i < n && depth > 0 {
			if inS {
				if b[i] == '\\' {
					i += 2
					continue
				}
				if b[i] == '"' {
					inS = false
				}
				i++
				continue
			}
			if b[i] == '"' {
				inS = true
			} else if b[i] == open {
				depth++
			} else if b[i] == closeChar {
				depth--
			}
			i++
		}
		val = b[vStart:i]
	} else {
		vStart := i
		for i < n && b[i] != ',' && b[i] != '}' && b[i] != ' ' && b[i] != '\t' && b[i] != '\r' && b[i] != '\n' {
			i++
		}
		val = b[vStart:i]
	}

	return key, val, isStr, b[i:], true
}

func parseNumeric(b []byte) (float64, bool) {
	if len(b) == 0 {
		return 0, false
	}
	i := 0
	for i < len(b) && (b[i] >= '0' && b[i] <= '9') {
		i++
	}
	if i == 0 {
		return 0, false
	}
	var val int64
	for j := 0; j < i; j++ {
		val = val*10 + int64(b[j]-'0')
	}
	if i < len(b) && b[i] == '.' {
		i++
		var frac int64
		var div float64 = 1
		for i < len(b) && (b[i] >= '0' && b[i] <= '9') {
			frac = frac*10 + int64(b[i]-'0')
			div *= 10
			i++
		}
		return float64(val) + float64(frac)/div, true
	}
	return float64(val), true
}

func unescapeJSON(s []byte) []byte {
	w := 0
	for r := 0; r < len(s); r++ {
		if s[r] == '\\' && r+1 < len(s) {
			r++
			switch s[r] {
			case '"', '\\', '/':
				s[w] = s[r]
			case 'n':
				s[w] = '\n'
			case 'r':
				s[w] = '\r'
			case 't':
				s[w] = '\t'
			case 'u':
				if r+4 < len(s) && s[r+1] == '0' && s[r+2] == '0' && s[r+3] == '2' && s[r+4] == '2' {
					s[w] = '"'
					r += 4
				} else {
					s[w] = s[r]
				}
			default:
				s[w] = s[r]
			}
			w++
		} else {
			s[w] = s[r]
			w++
		}
	}
	return s[:w]
}
