/* Re-encoder of the stand-alone beacon loop (see beacon_enc.h). Every step follows host/dvbs2.py and host/make_beacon.py. */
#include "beacon_enc.h"
#include <stdlib.h>
#include <string.h>
#include "beacon_cfg.h"
#include "beacon_fec.h"

#define HDR        90u                                           /* PLHEADER symbols */
#define DATA_SYMS  (BFEC_NLDPC / 2)                              /* QPSK: 32400 */
#define SLOTS      (DATA_SYMS / 90)
#define PBLOCKS    ((SLOTS - 1) / 16)                            /* 36 pilot symbols after every 16 slots, none after the last */
#define BB_SYMS    (BFEC_KBCH / 2)                               /* symbols that carry the BBFRAME */
#define NK         (BFEC_NLDPC - BFEC_KLDPC)                     /* LDPC parity bits */
#define PER_FRAME  32u                                           /* TS packets per BBFRAME (make_beacon.py) */
#define SDT_PKT    3u                                            /* the SDT is packet 3 of the loop (make_beacon.py psi_slots) */
#define PID_SDT    0x11u
#define PKT        188u
#define P0         (10u + PKT * SDT_PKT + 1u)                    /* BBFRAME bytes P0 .. P0 + 187: the SDT packet without its sync byte, */
#define P1         (P0 + PKT)                                    /* then the CRC-8 of it in the sync position of the next packet */

uint32_t benc_frame_syms(bool pilots) { return HDR + DATA_SYMS + (pilots ? 36u * PBLOCKS : 0u); }
uint32_t benc_frames(void) { return BEACON_SYMBOLS / benc_frame_syms(BEACON_PILOTS); }
size_t benc_loop_bytes(bool pilots) { return benc_frames() * benc_frame_syms(pilots) / 4u + 16u; }
_Static_assert(BEACON_SYMBOLS % (HDR + DATA_SYMS + (BEACON_PILOTS ? 36u * PBLOCKS : 0u)) == 0, "beacon.bin: whole PLFRAMEs");
_Static_assert(BFEC_KLDPC == BFEC_KBCH + BFEC_BCH && BFEC_BCH == 192u, "BCH t = 12 (six 32-bit words)");

/* ------------------------------------------------------------ packed 2-bit symbols, first symbol in the low bits; level 1 = +1 */

static inline unsigned sym_get(const uint8_t *b, uint32_t k) { return b[k >> 2] >> (2 * (k & 3)) & 3u; }
static inline void sym_put(uint8_t *b, uint32_t k, unsigned v) {
    const unsigned sh = 2 * (k & 3);
    b[k >> 2] = (uint8_t)((b[k >> 2] & ~(3u << sh)) | v << sh);
}

/* PL scrambling multiplies a symbol by j^R: on the levels (bit 0 = I, bit 1 = Q) a permutation, and its inverse */
static const uint8_t ROT[4][4] = {{0, 1, 2, 3}, {1, 3, 0, 2}, {3, 2, 1, 0}, {2, 0, 3, 1}};
static const uint8_t UNROT[4][4] = {{0, 1, 2, 3}, {2, 0, 3, 1}, {3, 2, 1, 0}, {1, 3, 0, 2}};

/* ------------------------------------------------------------ PL scrambling sequence (Gold code 0): R(n) = z(n) + 2 z(n + 131072) */

typedef struct { uint32_t x, y; } gold_t;                        /* bit k = x(i + k), y(i + k) */
typedef struct { gold_t a, b; uint32_t n; } rot_t;               /* a at n, b at n + 131072 */

static inline void gold_step(gold_t *g) {
    const uint32_t nx = (g->x ^ g->x >> 7) & 1u, ny = (g->y ^ g->y >> 5 ^ g->y >> 7 ^ g->y >> 10) & 1u;
    g->x = g->x >> 1 | nx << 17;
    g->y = g->y >> 1 | ny << 17;
}

static void rot_init(rot_t *r) {
    r->a.x = 1;
    r->a.y = 0x3FFFFu;
    r->b = r->a;
    for (uint32_t i = 0; i < 131072u; ++i) gold_step(&r->b);
    r->n = 0;
}

static inline unsigned rot_at(rot_t *r, uint32_t n) {           /* n never goes back */
    for (; r->n < n; ++r->n) { gold_step(&r->a); gold_step(&r->b); }
    return ((r->a.x ^ r->a.y) & 1u) | ((r->b.x ^ r->b.y) & 1u) << 1;
}

/* ------------------------------------------------------------ CRCs and the SDT packet (make_beacon.py section / psi_packet) */

static uint8_t crc8(const uint8_t *d, size_t n) {               /* x^8 + x^7 + x^6 + x^4 + x^2 + 1 */
    uint8_t c = 0;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; ++i) c = (uint8_t)(c & 0x80 ? c << 1 ^ 0xD5 : c << 1);
    }
    return c;
}

static uint32_t crc32_mpeg(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= (uint32_t)*d++ << 24;
        for (int i = 0; i < 8; ++i) c = c & 0x80000000u ? c << 1 ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

static size_t name_len(const char *s) {                         /* strnlen(s, BENC_NAME_MAX): strnlen is POSIX, not C11 */
    size_t n = 0;
    while (n < BENC_NAME_MAX && s[n]) ++n;
    return n;
}

static void sdt_packet(uint8_t *p, const char *service, const char *provider) {
    const size_t sn = name_len(service), pn = name_len(provider);
    uint8_t s[PKT];
    size_t k = 0;
    const size_t dl = 5 + pn + sn, n = 5 + 3 + 5 + dl + 4;    /* service descriptor; section length after the length field */
    const uint8_t head[] = {0x42, (uint8_t)(0xB0 | n >> 8), (uint8_t)n, 0, 1, 0xC1, 0, 0, 0, 1, 0xFF,
                            0, 1, 0xFC, (uint8_t)(0x80 | dl >> 8), (uint8_t)dl, 0x48, (uint8_t)(3 + pn + sn), 0x01, (uint8_t)pn};
    memcpy(s, head, sizeof(head));
    k = sizeof(head);
    memcpy(s + k, provider, pn);
    k += pn;
    s[k++] = (uint8_t)sn;
    memcpy(s + k, service, sn);
    k += sn;
    const uint32_t crc = crc32_mpeg(s, k);
    for (int i = 3; i >= 0; --i) s[k++] = (uint8_t)(crc >> (8 * i));
    static const uint8_t ts[] = {0x47, 0x40 | PID_SDT >> 8, PID_SDT & 0xFF, 0x30, 1, 0x80, 0};   /* discontinuity, pointer field */
    memset(p, 0xFF, PKT);
    memcpy(p, ts, sizeof(ts));
    memcpy(p + sizeof(ts), s, k);
}

/* ------------------------------------------------------------ BBFRAME bits as they are in the built-in loop */

static inline uint32_t data_pos(uint32_t d, bool pilots) { return d + (pilots ? 36u * (d / 1440u) : 0u); }  /* index after the header */

/* One byte of the (descrambled) BBFRAME of frame 0, read out of the loop: to check that packet 3 is the SDT. */
static uint8_t bb_byte(const uint8_t *src, uint32_t j) {
    rot_t r;
    rot_init(&r);
    uint32_t prbs = 0xA9u, v = 0;                                /* 1 + x^14 + x^15, initial 100101010000000 */
    for (uint32_t m = 0; m < 8 * j + 8; ++m) {
        const uint32_t o = (prbs >> 13 ^ prbs >> 14) & 1u;
        prbs = (prbs << 1 | o) & 0x7FFFu;
        if (m < 8 * j) continue;
        const uint32_t n = data_pos(m / 2, BEACON_PILOTS);
        const unsigned lv = UNROT[rot_at(&r, n)][sym_get(src, HDR + n)] ^ 3u;   /* bit 0 = b0, bit 1 = b1 */
        v = v << 1 | (((lv >> (m & 1)) & 1u) ^ o);
    }
    return (uint8_t)v;
}

/* ------------------------------------------------------------ PLHEADER: SOF + PLSCODE, pi/2 BPSK */

static void plheader(uint8_t *hdr, bool pilots) {
    static const uint32_t RM[6] = {0x55555555u, 0x33333333u, 0x0F0F0F0Fu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu};
    const unsigned pls = BFEC_MODCOD << 2 | (pilots ? 1u : 0u);
    uint32_t cw = 0;
    for (int i = 0; i < 6; ++i)
        if (pls >> (6 - i) & 1u) cw ^= RM[i];
    const uint64_t scr = 0x719D83C953422DFAull;
    for (int i = 0; i < 26; ++i) hdr[i] = 0x18D2E82u >> (25 - i) & 1u;
    for (int i = 0; i < 32; ++i) {
        const unsigned w = cw >> (31 - i) & 1u;
        hdr[26 + 2 * i] = (uint8_t)(w ^ (unsigned)(scr >> (63 - 2 * i) & 1u));
        hdr[27 + 2 * i] = (uint8_t)(w ^ (pls & 1u) ^ (unsigned)(scr >> (62 - 2 * i) & 1u));
    }
    for (int i = 0; i < 90; ++i) hdr[i] = i % 2 ? (hdr[i] ? 1 : 2) : (hdr[i] ? 0 : 3);   /* bit -> levels: (1-2b)(1+j), (1-2b)(-1+j) */
}

/* ------------------------------------------------------------ one frame */

typedef struct {
    uint8_t *dst;
    uint32_t base;                                               /* first symbol of the frame in dst */
    bool pilots;
    rot_t r;
} out_t;

static void put_data(out_t *o, uint32_t d, unsigned bits) {      /* bits: b0 | b1 << 1 of data symbol d */
    if (o->pilots && d && d % 1440u == 0) {                      /* the pilot block in front of this slot */
        const uint32_t n0 = data_pos(d, true) - 36u;
        for (uint32_t n = n0; n < n0 + 36u; ++n) sym_put(o->dst, o->base + HDR + n, ROT[rot_at(&o->r, n)][3]);
    }
    const uint32_t n = data_pos(d, o->pilots);
    sym_put(o->dst, o->base + HDR + n, ROT[rot_at(&o->r, n)][bits ^ 3u]);
}

static inline void ldpc_add(uint8_t *par, uint32_t m) {          /* information bit m (= 1) into the parity accumulators */
    const uint32_t g = m / 360u, i = m % 360u;
    for (uint32_t k = bfec_row[g]; k < bfec_row[g + 1]; ++k) {
        uint32_t a = bfec_addr[k] + BFEC_Q * i;
        if (a >= NK) a -= NK;
        par[a >> 3] ^= (uint8_t)(0x80u >> (a & 7));
    }
}

static void frame(const uint8_t *src, uint32_t sbase, out_t *o, const rot_t *r0, uint8_t *par, const uint8_t *patch, const uint8_t *hdr) {
    rot_t rs = *r0;
    o->r = *r0;
    uint32_t bch[6] = {0}, prbs = 0xA9u;
    memset(par, 0, NK / 8);
    for (uint32_t d = 0; d < BFEC_KLDPC / 2; ++d) {
        unsigned bits = 0;
        for (unsigned h = 0; h < 2; ++h) {
            const uint32_t m = 2 * d + h;
            unsigned b;
            if (m < BFEC_KBCH) {
                const uint32_t o_ = (prbs >> 13 ^ prbs >> 14) & 1u;
                prbs = (prbs << 1 | o_) & 0x7FFFu;
                if (patch && m >= 8 * P0 && m < 8 * P1) {
                    b = (patch[m / 8 - P0] >> (7 - m % 8) & 1u) ^ o_;
                } else {
                    const uint32_t n = data_pos(d, BEACON_PILOTS);
                    b = (UNROT[rot_at(&rs, n)][sym_get(src, sbase + HDR + n)] ^ 3u) >> h & 1u;
                }
                const uint32_t fb = b ^ bch[0] >> 31;            /* BCH: remainder of data * x^192 */
                for (int w = 0; w < 5; ++w) bch[w] = bch[w] << 1 | bch[w + 1] >> 31;
                bch[5] <<= 1;
                if (fb)
                    for (int w = 0; w < 6; ++w) bch[w] ^= bfec_bch_g[w];
            } else {
                const uint32_t k = m - BFEC_KBCH;
                b = bch[k / 32] >> (31 - k % 32) & 1u;
            }
            if (b) ldpc_add(par, m);
            bits |= b << h;
        }
        put_data(o, d, bits);
    }
    unsigned acc = 0;                                            /* the accumulator: p[j] ^= p[j - 1] */
    for (uint32_t j = 0; j < NK; ++j) {
        acc ^= par[j >> 3] >> (7 - (j & 7)) & 1u;
        if (acc) par[j >> 3] |= (uint8_t)(0x80u >> (j & 7));
        else par[j >> 3] &= (uint8_t)~(0x80u >> (j & 7));
    }
    for (uint32_t d = BFEC_KLDPC / 2; d < DATA_SYMS; ++d) {
        const uint32_t j = 2 * d - BFEC_KLDPC;
        put_data(o, d, (par[j >> 3] >> (7 - (j & 7)) & 1u) | (par[(j + 1) >> 3] >> (7 - ((j + 1) & 7)) & 1u) << 1);
    }
    for (uint32_t i = 0; i < HDR; ++i) sym_put(o->dst, o->base + i, hdr[i]);
}

uint32_t benc_build(uint8_t *dst, const uint8_t *src, bool pilots, const char *service, const char *provider) {
    if ((bb_byte(src, P0) & 0x1F) != (PID_SDT >> 8) || bb_byte(src, P0 + 1) != (PID_SDT & 0xFF)) return 0;
    uint8_t *par = malloc(NK / 8);
    if (!par) return 0;
    uint8_t patch[PKT + 1], pkt[PKT], hdr[HDR];
    sdt_packet(pkt, service, provider);
    memcpy(patch, pkt + 1, PKT - 1);
    patch[PKT - 1] = crc8(pkt + 1, PKT - 1);                     /* the CRC-8 of a packet replaces the sync byte of the next one */
    plheader(hdr, pilots);
    rot_t r0;
    rot_init(&r0);
    const uint32_t fs = benc_frame_syms(BEACON_PILOTS), fd = benc_frame_syms(pilots), nf = benc_frames();
    for (uint32_t f = 0; f < nf; ++f) {
        out_t o = {.dst = dst, .base = f * fd, .pilots = pilots};
        frame(src, f * fs, &o, &r0, par, f == 0 ? patch : NULL, hdr);
    }
    free(par);
    const uint32_t nsym = nf * fd;
    memcpy(dst + nsym / 4, dst, 16);                             /* the loop reads up to 3 symbols past its end before it wraps */
    return nsym;
}
