package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding"
	"hash"
)

// HS256 verification. Every token's signature, alg and exp are checked on every request (rule 5).
// Only the HMAC key's padded blocks are hashed once at startup: their SHA-256 states are saved,
// and each verification restores them instead of hashing the key again.

const (
	authOK = iota
	authMissing
	authInvalid
	authPayload
)

type verifier struct {
	h            hash.Hash
	inner, outer []byte // saved SHA-256 states after the ipad / opad block
	mac          [32]byte
	sum          [32]byte
	dec          []byte // decoded header, payload and signature
	name         []byte // the unescaped username
	keys         []string
	ms           [3]member
}

func newVerifier(secret []byte) *verifier {
	key := make([]byte, 64)
	if len(secret) > 64 {
		s := sha256.Sum256(secret)
		copy(key, s[:])
	} else {
		copy(key, secret)
	}
	state := func(pad byte) []byte {
		h := sha256.New()
		b := make([]byte, 64)
		for i := range b {
			b[i] = key[i] ^ pad
		}
		h.Write(b)
		s, err := h.(encoding.BinaryMarshaler).MarshalBinary()
		if err != nil {
			panic(err)
		}
		return s
	}
	return &verifier{h: sha256.New(), inner: state(0x36), outer: state(0x5c), dec: make([]byte, 0, 4096),
		name: make([]byte, 0, 256), keys: []string{"sub", "username", "exp"}}
}

var algKey = []string{"alg"}

var b64 = func() (t [256]byte) {
	for i := range t {
		t[i] = 0xff
	}
	const a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
	for i := 0; i < 64; i++ {
		t[a[i]] = byte(i)
	}
	return
}()

// b64url decodes unpadded base64url (canonical spelling only) onto dst.
func b64url(s, dst []byte) ([]byte, bool) {
	if len(s)%4 == 1 {
		return dst, false
	}
	i := 0
	for ; i+4 <= len(s); i += 4 {
		a, b, c, d := b64[s[i]], b64[s[i+1]], b64[s[i+2]], b64[s[i+3]]
		if (a|b|c|d)&0xc0 != 0 {
			return dst, false
		}
		v := uint32(a)<<18 | uint32(b)<<12 | uint32(c)<<6 | uint32(d)
		dst = append(dst, byte(v>>16), byte(v>>8), byte(v))
	}
	if r := len(s) - i; r > 0 {
		var v uint32
		for k := 0; k < r; k++ {
			x := b64[s[i+k]]
			if x&0xc0 != 0 {
				return dst, false
			}
			v |= uint32(x) << (18 - 6*k)
		}
		if r == 2 {
			if v&0xffff != 0 {
				return dst, false
			}
			dst = append(dst, byte(v>>16))
		} else {
			if v&0xff != 0 {
				return dst, false
			}
			dst = append(dst, byte(v>>16), byte(v>>8))
		}
	}
	return dst, true
}

func (v *verifier) restore(state []byte) {
	if err := v.h.(encoding.BinaryUnmarshaler).UnmarshalBinary(state); err != nil {
		panic(err)
	}
}

// verify checks an Authorization header value. On success it returns the user id and the
// username (valid until the next call).
func (v *verifier) verify(hdr []byte, has bool, now int64) (int, int64, []byte) {
	if !has || len(hdr) < 7 || string(hdr[:7]) != "Bearer " {
		return authMissing, 0, nil
	}
	tok := hdr[7:]
	d1, d2 := -1, -1
	for i, c := range tok {
		if c == '.' {
			if d1 < 0 {
				d1 = i
			} else if d2 < 0 {
				d2 = i
			} else {
				return authInvalid, 0, nil
			}
		}
	}
	if d2 < 0 {
		return authInvalid, 0, nil
	}
	// Signature first: nothing in an unsigned token is looked at.
	sig, ok := b64url(tok[d2+1:], v.dec[:0])
	if !ok || len(sig) != 32 {
		return authInvalid, 0, nil
	}
	copy(v.sum[:], sig)
	v.restore(v.inner)
	v.h.Write(tok[:d2])
	in := v.h.Sum(v.mac[:0])
	v.restore(v.outer)
	v.h.Write(in)
	mac := v.h.Sum(v.mac[:0])
	if subtle.ConstantTimeCompare(mac, v.sum[:]) != 1 {
		return authInvalid, 0, nil
	}
	// Header: alg must be HS256.
	head, ok := b64url(tok[:d1], v.dec[:0])
	if !ok {
		return authInvalid, 0, nil
	}
	var hm [1]member
	if ok, isObj := object(head, algKey, hm[:], v.name[:0]); !ok || !isObj {
		return authInvalid, 0, nil
	}
	if !hm[0].found || hm[0].kind != jStr || string(hm[0].val) != "HS256" {
		return authInvalid, 0, nil
	}
	pay, ok := b64url(tok[d1+1:d2], v.dec[:0])
	if !ok {
		return authInvalid, 0, nil
	}
	ms := v.ms[:]
	if ok, isObj := object(pay, v.keys, ms, v.name[:0]); !ok || !isObj {
		return authInvalid, 0, nil
	}
	// exp is required and must be in the future.
	exp := ms[2]
	if !exp.found || exp.kind != jNum || !expAfter(exp.val, now) {
		return authInvalid, 0, nil
	}
	sub, name := ms[0], ms[1]
	if !sub.found || sub.kind != jStr || sub.esc {
		return authPayload, 0, nil
	}
	uid, ok := positive(sub.val)
	if !ok {
		return authPayload, 0, nil
	}
	if !name.found || name.kind != jStr {
		return authPayload, 0, nil
	}
	user := name.val
	if name.esc {
		v.name = unescape(name.val, v.name[:0])
		user = v.name
	} else {
		// The payload buffer is reused by the next call; copy the name out.
		v.name = append(v.name[:0], user...)
		user = v.name
	}
	return authOK, uid, user
}

// expAfter reports whether the JSON number exp is later than now (seconds).
func expAfter(num []byte, now int64) bool {
	if len(num) == 0 || num[0] == '-' {
		return false
	}
	var whole int64
	i := 0
	for ; i < len(num) && num[i] >= '0' && num[i] <= '9'; i++ {
		if whole > 1e15 {
			return true
		}
		whole = whole*10 + int64(num[i]-'0')
	}
	if i < len(num) && (num[i] == 'e' || num[i] == 'E') {
		// Exponent form: rare; evaluate it the slow way.
		return parseFloat(num) > float64(now)
	}
	frac := false
	if i < len(num) && num[i] == '.' {
		for _, c := range num[i+1:] {
			if c != '0' {
				frac = true
			}
		}
	}
	return whole > now || (whole == now && frac)
}

func parseFloat(num []byte) float64 {
	var mant float64
	i := 0
	neg := false
	if i < len(num) && num[i] == '-' {
		neg = true
		i++
	}
	for ; i < len(num) && num[i] >= '0' && num[i] <= '9'; i++ {
		mant = mant*10 + float64(num[i]-'0')
	}
	scale := 0
	if i < len(num) && num[i] == '.' {
		for i++; i < len(num) && num[i] >= '0' && num[i] <= '9'; i++ {
			mant = mant*10 + float64(num[i]-'0')
			scale--
		}
	}
	if i < len(num) && (num[i] == 'e' || num[i] == 'E') {
		i++
		es := 1
		if i < len(num) && (num[i] == '+' || num[i] == '-') {
			if num[i] == '-' {
				es = -1
			}
			i++
		}
		e := 0
		for ; i < len(num) && num[i] >= '0' && num[i] <= '9'; i++ {
			if e < 10000 {
				e = e*10 + int(num[i]-'0')
			}
		}
		scale += es * e
	}
	for ; scale > 0; scale-- {
		mant *= 10
	}
	for ; scale < 0; scale++ {
		mant /= 10
	}
	if neg {
		mant = -mant
	}
	return mant
}

// positive parses a positive decimal integer without sign or leading zeros (below 2^53).
func positive(s []byte) (int64, bool) {
	if len(s) == 0 || len(s) > 16 || s[0] == '0' {
		return 0, false
	}
	var v int64
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	return v, v <= 1<<53
}
