/* jwt/jwt_mb.inc (sha256 fc1162b151bb), developed separately for this entry. */
/* jwt_mb.inc — multi-buffer HMAC-SHA256 "template", #included by jwt.c once per ISA.
 *
 * The includer defines:
 *   MB(name)             name mangler, e.g. name##_avx2
 *   MB_LANES             number of independent messages per vector
 *   V                    vector type holding MB_LANES uint32 lanes
 *   VSET1(x) VADD(a,b) VXOR(a,b) VSHR(x,n) VROR(x,n)
 *   VXOR3(a,b,c)         a ^ b ^ c
 *   VCH(e,f,g)           (e & f) | (~e & g)        (bit select)
 *   VMAJ(a,b,c)          majority
 *   VSEL(m,x,y)          m ? x : y per bit (m is all-ones/all-zeros per lane)
 *   VMASK(u32 arr)       load an MB_LANES-element uint32 mask array into a V
 *   MB_LOAD16(ptr, w)    w[0..15] = big-endian words 0..15 of the 64-byte blocks ptr[0..LANES-1]
 *   MB_STORE8(s, out)    out[l][j] = lane l of s[j], for l < MB_LANES
 *   MB_MIN_LANES         a group with fewer live messages than this goes to the scalar engine
 *
 * Everything below is ISA-agnostic: the scalar engine is this same file with V = uint32_t,
 * which is how the AVX-512 instantiation (which the author cannot run) is "tested by construction":
 * test.c also instantiates it with scalar emulations of vpternlogd using the same immediates.
 */

static inline void MB(compress)(V s[8], V w[16]) {
    V a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
#define MB_RND(a, b, c, d, e, f, g, h, t, wt)                                                \
    do {                                                                                     \
        V t1 = VADD(VADD(VADD(h, VXOR3(VROR(e, 6), VROR(e, 11), VROR(e, 25))),               \
                         VADD(VCH(e, f, g), VSET1(K256[t]))), (wt));                         \
        V t2 = VADD(VXOR3(VROR(a, 2), VROR(a, 13), VROR(a, 22)), VMAJ(a, b, c));             \
        d = VADD(d, t1);                                                                     \
        h = VADD(t1, t2);                                                                    \
    } while (0)
#define MB_SCHED(t) /* w[t&15] becomes W[t] for t >= 16 */                                   \
    (w[(t) & 15] = VADD(VADD(w[(t) & 15],                                                    \
                             VXOR3(VROR(w[((t) - 15) & 15], 7), VROR(w[((t) - 15) & 15], 18), \
                                   VSHR(w[((t) - 15) & 15], 3))),                            \
                        VADD(w[((t) - 7) & 15],                                              \
                             VXOR3(VROR(w[((t) - 2) & 15], 17), VROR(w[((t) - 2) & 15], 19),  \
                                   VSHR(w[((t) - 2) & 15], 10)))))
#define MB_8R(t, W)                                                                          \
    MB_RND(a, b, c, d, e, f, g, h, (t) + 0, W((t) + 0));                                     \
    MB_RND(h, a, b, c, d, e, f, g, (t) + 1, W((t) + 1));                                     \
    MB_RND(g, h, a, b, c, d, e, f, (t) + 2, W((t) + 2));                                     \
    MB_RND(f, g, h, a, b, c, d, e, (t) + 3, W((t) + 3));                                     \
    MB_RND(e, f, g, h, a, b, c, d, (t) + 4, W((t) + 4));                                     \
    MB_RND(d, e, f, g, h, a, b, c, (t) + 5, W((t) + 5));                                     \
    MB_RND(c, d, e, f, g, h, a, b, (t) + 6, W((t) + 6));                                     \
    MB_RND(b, c, d, e, f, g, h, a, (t) + 7, W((t) + 7))
#define MB_WD(t) w[t]
    MB_8R(0, MB_WD);
    MB_8R(8, MB_WD);
    MB_8R(16, MB_SCHED);
    MB_8R(24, MB_SCHED);
    MB_8R(32, MB_SCHED);
    MB_8R(40, MB_SCHED);
    MB_8R(48, MB_SCHED);
    MB_8R(56, MB_SCHED);
#undef MB_WD
#undef MB_8R
#undef MB_SCHED
#undef MB_RND
    s[0] = VADD(s[0], a), s[1] = VADD(s[1], b), s[2] = VADD(s[2], c), s[3] = VADD(s[3], d);
    s[4] = VADD(s[4], e), s[5] = VADD(s[5], f), s[6] = VADD(s[6], g), s[7] = VADD(s[7], h);
}

/* One vector's worth of HMACs: nl <= MB_LANES plans, absent lanes run on a dummy zero block. */
static void MB(lanes)(const jwt_key_t *k, int nl, const hplan_t *const *pl, uint32_t (*mac)[8]) {
    V st[8];
    int nb[MB_LANES], maxnb = 0, ragged = 0;
    for (int i = 0; i < 8; i++) st[i] = VSET1(k->ih[i]);
    for (int l = 0; l < MB_LANES; l++) {
        nb[l] = l < nl ? pl[l]->nb : 0;
        if (nb[l] > maxnb) maxnb = nb[l];
    }
    for (int l = 0; l < nl; l++) ragged |= nb[l] != maxnb;
    for (int b = 0; b < maxnb; b++) {
        const uint8_t *ptr[MB_LANES];
        uint32_t live[MB_LANES];
        V w[16], s[8];
        for (int l = 0; l < MB_LANES; l++) {
            live[l] = b < nb[l] ? ~0u : 0u;
            ptr[l] = live[l] ? hplan_block(pl[l], b) : zero_block;
        }
        MB_LOAD16(ptr, w);
        for (int i = 0; i < 8; i++) s[i] = st[i];
        MB(compress)(s, w);
        if (ragged) { /* lanes that already ran out of blocks keep their state */
            V m = VMASK(live);
            for (int i = 0; i < 8; i++) st[i] = VSEL(m, s[i], st[i]);
        } else {
            for (int i = 0; i < 8; i++) st[i] = s[i];
        }
    }
    /* outer hash: one block = inner digest || 0x80 || zeros || bitlen(64 + 32) */
    V w[16];
    for (int i = 0; i < 8; i++) w[i] = st[i];
    w[8] = VSET1(0x80000000u);
    for (int i = 9; i < 15; i++) w[i] = VSET1(0);
    w[15] = VSET1((64 + 32) * 8);
    for (int i = 0; i < 8; i++) st[i] = VSET1(k->oh[i]);
    MB(compress)(st, w);
    if (nl == MB_LANES) {
        MB_STORE8(st, mac);
    } else {
        uint32_t tmp[MB_LANES][8];
        MB_STORE8(st, tmp);
        memcpy(mac, tmp, sizeof(uint32_t[8]) * nl);
    }
}

#ifndef MB_NO_DRIVER
/* n messages, any lengths: plan, sort by block count so vectors are rarely ragged, run groups. */
void MB(jwt_hmac)(const jwt_key_t *k, int n, const uint8_t *const *msg, const int *len,
                  uint32_t (*mac)[8]) {
    enum { CH = 64 };
    hplan_t plan[CH];
    const hplan_t *pp[CH];
    uint32_t tmp[MB_LANES][8];
    for (int base = 0; base < n; base += CH) {
        int m = n - base < CH ? n - base : CH;
        for (int i = 0; i < m; i++) hplan_init(&plan[i], msg[base + i], len[base + i]);
        /* insertion sort of plan pointers by nb (m <= 64, nearly always already uniform) */
        for (int i = 0; i < m; i++) {
            const hplan_t *p = &plan[i];
            int j = i;
            while (j > 0 && pp[j - 1]->nb > p->nb) pp[j] = pp[j - 1], j--;
            pp[j] = p;
        }
        for (int g = 0; g < m; g += MB_LANES) {
            int nl = m - g < MB_LANES ? m - g : MB_LANES;
            if (nl < MB_MIN_LANES) {
                for (int l = 0; l < nl; l++) {
                    const hplan_t *p = pp[g + l];
                    hmac1_scalar(k, p, mac[base + (int)(p - plan)]);
                }
                continue;
            }
            MB(lanes)(k, nl, &pp[g], tmp);
            for (int l = 0; l < nl; l++) memcpy(mac[base + (int)(pp[g + l] - plan)], tmp[l], 32);
        }
    }
}
#endif
