//go:build linux && train

// Build time only. build.sh compiles db.c twice: first with counters in it, then, after this
// program has run, with what the counters recorded (which branch is the usual one, which
// functions call each other). This file is the run in between. A binary built with the
// "train" tag does nothing else: it makes a scratch database (train.c), sends requests through
// the same parse and execute functions the server uses, writes the profile and exits. No
// socket is opened; the code being profiled is the C side, and it is the same from here.
//
// The requests are the challenge's five endpoints in the proportions of its public load
// script (bench/load.js): per round one feed and one post, a like in 15 % and a new post in
// 2 % of rounds. Posts and likes go to a post from a feed read a little earlier.
package main

/*
#include <stdlib.h>
int train_make_db(const char *path);
void train_write_profile(void);
*/
import "C"

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"os"
	"strconv"
	"time"
	"unsafe"
)

const trainUsers, trainPosts, trainRounds = 50000, 500000, 20000

func init() {
	if len(os.Args) != 2 {
		trainFail("usage: server.train <path of the scratch database to create>")
	}
	path := C.CString(os.Args[1])
	if C.train_make_db(path) != 0 {
		trainFail("could not make the scratch database")
	}
	C.free(unsafe.Pointer(path))
	if err := dbOpen(os.Args[1]); err != nil {
		trainFail(err.Error())
	}
	secret := []byte("training")
	s := &server{secret: secret, verify: newVerifier(secret), json: make([]byte, 0, sendBufSize)}
	out := make([]byte, 0, sendBufSize)

	seed := uint64(1)
	random := func(n int) int {
		seed = seed*6364136223846793005 + 1442695040888963407
		return int((seed >> 33) % uint64(n))
	}
	const agent = " HTTP/1.1\r\nHost: 10.0.0.1\r\nUser-Agent: Grafana k6/1.2.3\r\n"
	type user struct {
		name, auth string
	}
	users := make([]user, 256)
	for i := range users {
		id := 1 + random(trainUsers)
		name := "quiet_otter_" + strconv.Itoa(id)
		users[i] = user{name, "Authorization: Bearer " + trainToken(secret, id, name) + "\r\n"}
	}
	answered := 0
	do := func(head string, body []byte, status string) {
		r := s.parse([]byte(head), body)
		if o := s.execute(out[:0], &r); !bytes.HasPrefix(o, []byte("HTTP/1.1 "+status)) {
			trainFail(fmt.Sprintf("%q answered %.60q", head[:40], o))
		}
		// The loop commits once per pass; 64 answers is a pass under a middling load.
		if answered++; answered%64 == 0 && s.inTxn {
			if !dbCommit() {
				trainFail("commit: " + dbErr().Error())
			}
			s.inTxn = false
		}
	}
	newest := trainPosts
	create := func(round int) {
		u := users[random(len(users))]
		body := []byte(`{"body":"` + u.name + ` says hi at ` + time.Now().UTC().Format("2006-01-02T15:04:05.000Z") +
			` (VU ` + strconv.Itoa(1+random(200000)) + `, iter ` + strconv.Itoa(round%40) + `)"}`)
		do("POST /posts"+agent+u.auth+"Content-Type: application/json\r\nContent-Length: "+strconv.Itoa(len(body))+"\r\n\r\n", body, "201")
		newest++
	}
	// Under load the feed's twenty posts are a fraction of a second old, by twenty different
	// users, with no likes yet. Start there.
	for i := 0; i < 20; i++ {
		create(i)
	}
	for round := 0; round < trainRounds; round++ {
		do("GET /feed"+agent+"\r\n", nil, "200")
		// The post a client reads and likes is one from a feed it fetched some seconds ago.
		post := strconv.Itoa(newest - 100 - random(2000))
		do("GET /posts/"+post+agent+"\r\n", nil, "200")
		if random(100) < 15 {
			do("POST /posts/"+post+"/like"+agent+users[random(len(users))].auth+"Content-Length: 0\r\n\r\n", nil, "2")
		}
		if random(100) < 2 {
			create(round)
		}
	}
	if s.inTxn && !dbCommit() {
		trainFail("commit: " + dbErr().Error())
	}
	C.train_write_profile()
	os.Exit(0)
}

func trainToken(secret []byte, id int, name string) string {
	enc := base64.RawURLEncoding.EncodeToString
	now := time.Now().Unix()
	signed := enc([]byte(`{"alg":"HS256","typ":"JWT"}`)) + "." +
		enc([]byte(fmt.Sprintf(`{"sub":"%d","username":"%s","iat":%d,"exp":%d}`, id, name, now, now+3600)))
	m := hmac.New(sha256.New, secret)
	m.Write([]byte(signed))
	return signed + "." + enc(m.Sum(nil))
}

func trainFail(msg string) {
	fmt.Fprintln(os.Stderr, "training:", msg)
	os.Exit(1)
}
