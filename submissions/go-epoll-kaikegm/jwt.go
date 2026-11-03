package main

import (
	"errors"

	"github.com/KaikeGM/zerojwt/v2"
)

type verifier struct {
	v       *zerojwt.Verifier
	scratch *zerojwt.Scratch
	claims  zerojwt.Claims
}

func newVerifier(secret string) *verifier {
	v, err := zerojwt.New([]byte(secret), zerojwt.Options{
		AllowShortSecret: true,
	})
	if err != nil {
		panic(err)
	}
	return &verifier{
		v:       v,
		scratch: v.NewScratch(),
	}
}

// verifyToken verifies a raw bearer token slice using zerojwt/v2 (0 B/op, 0 allocs/op).
// Returns (uid, username, ok, payloadOK).
func (vr *verifier) verifyToken(tok []byte, now int64) (uid int64, username []byte, ok, payloadOK bool) {
	err := vr.v.VerifyWithScratch(tok, now, &vr.claims, vr.scratch)
	if err != nil {
		if errors.Is(err, zerojwt.ErrInvalidPayload) {
			return 0, nil, true, false
		}
		return 0, nil, false, false
	}
	if !vr.claims.HasSub || vr.claims.SubjectID <= 0 || !vr.claims.HasUsername || len(vr.claims.Username()) == 0 {
		return 0, nil, true, false
	}
	return vr.claims.SubjectID, vr.claims.Username(), true, true
}
