// Package auth verifies the HS256 JWTs the challenge hands out.
package auth

//go:generate go tool ggen -simd off .

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
	"strings"
	"sync"
	"time"
	"unsafe"

	"feed/domain"
)

//ggen:generate ignoreunknown allowdups
type header struct {
	Alg string `json:"alg" pipe:"required"`
}

// claims holds the payload's fields untyped: a wrong type is not a malformed
// token but a bad payload, which only Verify can tell apart.
//
//ggen:generate ignoreunknown allowdups
type claims struct {
	Sub      any `json:"sub"`
	Username any `json:"username"`
	Exp      any `json:"exp"`
}

// Verifier checks tokens signed with one secret. Every call verifies the
// signature anew; nothing is remembered between requests.
type Verifier struct {
	macs sync.Pool // of *signer
	now  func() time.Time
}

// signer is a keyed HMAC and the array it sums into. The sum goes through
// the hash.Hash interface, so a local array would escape to the heap on
// every call; pooled with the mac, it is allocated once.
type signer struct {
	mac hash.Hash
	sum [sha256.Size]byte
}

func NewVerifier(secret []byte) *Verifier {
	key := append([]byte(nil), secret...)
	return &Verifier{
		macs: sync.Pool{New: func() any { return &signer{mac: hmac.New(sha256.New, key)} }},
		now:  time.Now,
	}
}

var b64 = base64.RawURLEncoding

// Verify reads a bearer token (without the "Bearer " prefix). A token that is
// malformed, badly signed, not HS256 or expired is ErrInvalidToken; a valid
// one whose sub is not a positive integer string, or whose username is not a
// string, is ErrInvalidPayload.
func (v *Verifier) Verify(token string) (domain.Principal, error) {
	h, rest, ok := strings.Cut(token, ".")
	if !ok {
		return domain.Principal{}, domain.ErrInvalidToken
	}
	p, sig, ok := strings.Cut(rest, ".")
	if !ok || strings.IndexByte(sig, '.') >= 0 {
		return domain.Principal{}, domain.ErrInvalidToken
	}

	var buf [512]byte
	hb, err := b64.AppendDecode(buf[:0], []byte(h))
	if err != nil {
		return domain.Principal{}, domain.ErrInvalidToken
	}
	hdr, _, err := header{}.DecodeFrom(hb)
	if err != nil || hdr.Alg != "HS256" {
		return domain.Principal{}, domain.ErrInvalidToken
	}

	var sigBuf [sha256.Size]byte
	if b64.DecodedLen(len(sig)) != sha256.Size {
		return domain.Principal{}, domain.ErrInvalidToken
	}
	if _, err := b64.Decode(sigBuf[:], []byte(sig)); err != nil {
		return domain.Principal{}, domain.ErrInvalidToken
	}
	sg := v.macs.Get().(*signer)
	sg.mac.Reset()
	// []byte(signed) would be copied to the heap: the slice goes through an
	// interface call, so the compiler can't prove Write keeps no hold of it.
	// hash.Hash.Write never retains or modifies its input.
	signed := token[:len(h)+1+len(p)]
	sg.mac.Write(unsafe.Slice(unsafe.StringData(signed), len(signed)))
	valid := hmac.Equal(sg.mac.Sum(sg.sum[:0]), sigBuf[:])
	v.macs.Put(sg)
	if !valid {
		return domain.Principal{}, domain.ErrInvalidToken
	}

	pb, err := b64.AppendDecode(buf[:0], []byte(p))
	if err != nil {
		return domain.Principal{}, domain.ErrInvalidToken
	}
	c, _, err := claims{}.DecodeFrom(pb)
	if err != nil {
		return domain.Principal{}, domain.ErrInvalidToken
	}
	exp, ok := c.Exp.(float64)
	if !ok || float64(v.now().Unix()) >= exp {
		return domain.Principal{}, domain.ErrInvalidToken
	}

	sub, ok := c.Sub.(string)
	if !ok {
		return domain.Principal{}, domain.ErrInvalidPayload
	}
	id, err := domain.ParsePostID(sub) // the same rule: a positive decimal integer
	if err != nil {
		return domain.Principal{}, domain.ErrInvalidPayload
	}
	name, ok := c.Username.(string)
	if !ok {
		return domain.Principal{}, domain.ErrInvalidPayload
	}
	return domain.Principal{UserID: id, Username: name}, nil
}
