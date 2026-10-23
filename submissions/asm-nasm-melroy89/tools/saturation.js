'use strict'
// Substitute-generator CPU diagnostic, never an official capacity score.
const fs = require('node:fs')
const autocannon = require('/usr/lib/node_modules/autocannon')
const tokens = JSON.parse(fs.readFileSync(process.argv[2]))
const url = process.argv[3]
const duration = Number(process.argv[4] || 10)
const offset = Number(process.argv[5] || 0)
let serial = offset * Math.floor(tokens.length / 4)
let semanticErrors = 0
let validated = 0
const requests = []
function setup(req, ctx) {
  if (!ctx.user) ctx.user = tokens[(serial++) % tokens.length]
  req.headers = { authorization: 'Bearer ' + ctx.user.token, 'content-type': 'application/json' }
  return req
}
function response(kind) {
  return (status, body, ctx) => {
    validated++
    try {
      const data = JSON.parse(body)
      let ok = false
      if (kind === 'feed') {
        ok = status === 200 && Array.isArray(data.posts) && data.posts.length === 20
        if (ok) ctx.id = data.posts[Math.floor(Math.random() * data.posts.length)].id
      } else if (kind === 'post') ok = status === 200 && data.post.id === ctx.id && typeof data.post.body === 'string'
      else if (kind === 'like') ok = (status === 200 || status === 201) && data.liked === true && data.post_id === ctx.id
      else ok = status === 201 && Number.isInteger(data.post.id) && data.post.body === 'NASM saturation diagnostic'
      if (!ok) semanticErrors++
    } catch (_) { semanticErrors++ }
  }
}
for (let i = 0; i < 100; i++) {
  requests.push({ method: 'GET', path: '/feed', setupRequest: setup, onResponse: response('feed') })
  requests.push({ method: 'GET', path: '/posts/1', setupRequest: (req, ctx) => { setup(req, ctx); req.path = '/posts/' + ctx.id; return req }, onResponse: response('post') })
  if (i < 15) requests.push({ method: 'POST', path: '/posts/1/like', setupRequest: (req, ctx) => { setup(req, ctx); req.path = '/posts/' + ctx.id + '/like'; return req }, onResponse: response('like') })
  if (i < 2) requests.push({ method: 'POST', path: '/posts', body: '{"body":"NASM saturation diagnostic"}', setupRequest: setup, onResponse: response('create') })
}
autocannon({ url, connections: 16, pipelining: 1, duration, requests }, (err, result) => {
  if (err) { console.error(err); process.exitCode = 1; return }
  console.log(JSON.stringify({ requests: result.requests, latency: result.latency, errors: result.errors, timeouts: result.timeouts, non2xx: result.non2xx, duration: result.duration, semanticErrors, validated }))
  if (semanticErrors || result.errors || result.timeouts || result.non2xx) process.exitCode = 1
})
