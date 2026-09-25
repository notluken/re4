// game/yz2code: set-up side of Capcom's yz2 decompressor (the room archives "rNNN.das" are yz2
// streams: a text header "<packed hex> <unpacked hex>" then the bit stream at the next 32-byte
// boundary). Yz2DecodeSet parses the header, Yz2DecodeExec builds the adaptive frequency models
// (a 0x500-symbol main model and a 0x100-symbol one) and the 256-entry dictionary in a work
// buffer, then hands over to the assembly decoder (yz2asm.cpp yz2Decode_Decode).
#include "types.h"
#include <string.h>
#include <stdlib.h>
#ifdef TARGET_PC
#include <vector>
#endif

extern "C" {
void yz2Decode_Decode(void* ctx, void* dst, u32 size, void* ev);
u32 Yz2DecodeSet(char* str, void* buf);
void Yz2DecodeExec(void* dst);
}

// Input state shared with the assembly decoder (yz2asm).
struct Yz2InEv {
    u8* src;      // 0x00  compressed stream read pointer
    u8* heap;     // 0x04  work buffer start
    u8* free;     // 0x08  work buffer allocation pointer
    u32 size0;    // 0x0C  first hex field of the header string
    u32 size1;    // 0x10  second hex field of the header string
};

static Yz2InEv in_ev;

// Adaptive frequency table.
struct Yz2Freq {
    u16* freq;    // 0x00
    u32 max;      // 0x04
    int bits;     // 0x08
    u32 range;    // 0x0C
    u16* cum;     // 0x10
    int n;        // 0x14
    u32 acc;      // 0x18
};

struct Yz2Dec;

struct Yz2Model {
    Yz2Dec* dec;    // 0x00
    Yz2Freq fd;     // 0x04
    u8* table;      // 0x20
};

// Bit reader and the two models.
struct Yz2Dec {
    u32 mask;       // 0x00
    u32 byte;       // 0x04
    Yz2Model m1;    // 0x08
    Yz2Model m2;    // 0x2C
};

// Dictionary entry (0x1004 bytes, 0x100 of them).
struct Yz2DicEnt {
    u32 cnt;        // 0x00
    u8 a[0x800];    // 0x04
    u8 b[0x800];    // 0x804
};

struct Yz2Ctx {
    Yz2DicEnt* dic; // 0x00
    Yz2Dec d;       // 0x04
    u32 pad_54;     // 0x54
    int n;          // 0x58
};

// Parses the yz2 header at `str` (packed size, unpacked size in hex), sets the work buffer `buf`
// and the stream start (32-byte aligned after the header). Returns the unpacked size.
u32 Yz2DecodeSet(char* str, void* buf)
{
    char* p = str;
    u32 size;

    in_ev.size0 = strtoul(p, &p, 16);
    p++;
    size = strtoul(p, &p, 16);
    in_ev.size1 = size;
    in_ev.heap = (u8*) buf;
    in_ev.free = (u8*) buf;
    p = (char*) (((u32) p + 0x20) & ~0x1F);
    in_ev.src = (u8*) p;
    return size;
}

// Bump allocation in the work buffer.
static inline void* yz2Alloc(u32 size)
{
    void* p = in_ev.free;
    in_ev.free += size;
    return p;
}

#define FREQ_RESET(fdp)                                   \
    {                                                     \
        Yz2Freq* f = (fdp);                                \
        int i;                                            \
        for (i = 0; i < f->n; i++) {                     \
            f->freq[i] = 1;                              \
        }                                                 \
        f->bits = 0;                                     \
        f->max = f->n;                                  \
        if (f->max >= (one << f->bits)) {               \
            u32 lim = 1;                                  \
            do {                                          \
                f->bits++;                               \
                if (f->bits > 14) {                      \
                    break;                                \
                }                                         \
            } while (f->max >= (lim << f->bits));       \
        }                                                 \
        f->range = 1 << f->bits;                        \
        f->acc = 0;                                      \
    }

#define MODEL_SETUP(mp, dp, cnt)                          \
    {                                                     \
        Yz2Model* m = (mp);                               \
        Yz2Freq* fd = &m->fd;                             \
        int i;                                            \
        int j;                                            \
        int n;                                            \
        u16 s;                                            \
        m->dec = (dp);                                    \
        fd->n = (cnt);                                    \
        fd->freq = (u16*) yz2Alloc((cnt) * sizeof(u16));  \
        memset(fd->freq, 0, (cnt) * sizeof(u16));         \
        fd->cum = (u16*) yz2Alloc((cnt) * sizeof(u16) * 2); \
        j = 0;                                            \
        for (i = 0; i < 0x8000; i++) {                    \
            fd->freq[j]++;                                \
            j++;                                          \
            if (j >= fd->n) {                             \
                j = 0;                                    \
            }                                             \
        }                                                 \
        s = 0;                                            \
        n = fd->n;                                        \
        for (i = 0; i < n; i++) {                         \
            fd->cum[i * 2] = fd->freq[i];                 \
            fd->cum[i * 2 + 1] = s;                       \
            s += fd->freq[i];                             \
        }                                                 \
        FREQ_RESET(fd);                                   \
        m->table = (u8*) yz2Alloc(0x20000);               \
    }

#ifdef TARGET_PC
// TARGET_PC-only re-implementation of the vendor's whole-function PPC asm decode loop
// (src/game/yz2asm.cpp's yz2Decode_Decode/FrequencyDecode_Decode/FrequencyDecode_Decode768):
// that file is excluded from this port's build (Phase 5, real PPC asm with no C equivalent --
// see CMakeLists.txt/cmake/boot_exclude.txt), so the room archives ("stN/rNNN.das") were left
// undecoded (a boot stub), leaving every GetDataExt() lookup on the raw, still-zeroed room
// buffer fail. This is a from-scratch C++ port, transliterated from tools/motion/yz2.py's
// decode() (itself a register-level reading of yz2asm.cpp's control flow -- see that file's
// docstring for the derivation), independently verified there against real `st1/r120.das`
// bytes read off the actual disc image (13 correctly-tagged sub-files, including "SAT").
// Model (adaptive range coder over a 0x500-symbol main alphabet and a 0x100-symbol side
// alphabet) and dictionary (256 contexts, a 512-entry ring of prior runs each) match
// MODEL_SETUP/FREQ_RESET above and Yz2DicEnt exactly; only the decode loop itself (real asm on
// GameCube) is reimplemented.
namespace {

constexpr u32 kYz2Main = 0x500;
constexpr u32 kYz2Side = 0x100;
constexpr u32 kYz2CumTotal = 0x8000;
constexpr u32 kYz2Ring = 0x200;

// Yz2Freq + the coder's cumulative table (MODEL_SETUP's round-robin initial distribution,
// FREQ_RESET's freq/bits/range/total).
struct Yz2ModelPC {
    u32 n;
    std::vector<u16> freq;
    std::vector<u32> cum_f;
    std::vector<u32> cum_s;
    u32 total;
    int bits;
    u32 range;

    explicit Yz2ModelPC(u32 count) : n(count), freq(count), cum_f(count), cum_s(count)
    {
        u32 base = kYz2CumTotal / n;
        u32 rem = kYz2CumTotal % n;
        u32 s = 0;
        for (u32 i = 0; i < n; i++) {
            u32 f = base + (i < rem ? 1 : 0);
            cum_f[i] = f;
            cum_s[i] = s;
            s += f;
        }
        for (u32 i = 0; i < n; i++) {
            freq[i] = 1;
        }
        total = n;
        int b = 0;
        for (;;) {
            b++;
            if (b > 14 || n < (1u << b)) {
                break;
            }
        }
        bits = b;
        range = 1u << bits;
    }
};

// Bit/range-coder state (Yz2Dec.mask/byte) plus one symbol decode (FrequencyDecode_Decode /
// FrequencyDecode_Decode768 are the same routine on two different models).
struct Yz2DecoderPC {
    const u8* src;
    u32 mask;  // R
    u32 byte;  // C

    u32 decodeSym(Yz2ModelPC& m)
    {
        if (mask <= 0x800000) {
            if (mask > 0x8000) {
                byte = (byte << 8) | *src++;
                mask <<= 8;
            } else if (mask > 0x80) {
                byte = (byte << 16) | ((u32) src[0] << 8) | src[1];
                src += 2;
                mask <<= 16;
            } else {
                byte = (byte << 24) | ((u32) src[0] << 16) | ((u32) src[1] << 8) | src[2];
                src += 3;
                mask <<= 24;
            }
        }
        u32 step = mask >> 14;
        u32 value = byte / step;
        // bisect_right(cum_s, value) - 1: largest index with cum_s[index] <= value.
        u32 lo = 0;
        u32 hi = m.n;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;
            if (m.cum_s[mid] <= value) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        u32 s = lo - 1;
        byte -= step * m.cum_s[s];
        mask = (step * m.cum_f[s]) >> 1;
        m.freq[s]++;
        m.total++;
        if (m.bits <= 14) {
            if (m.total == m.range) {
                int sh = 15 - m.bits;
                u32 run = 0;
                for (u32 i = 0; i < m.n; i++) {
                    u32 v = (u32) m.freq[i] << sh;
                    m.cum_s[i] = run;
                    run += v;
                    m.cum_f[i] = v;
                }
                m.bits++;
                m.range = 1u << m.bits;
            }
        } else if (m.total > 0x7FFF) {
            u32 run = 0;
            u32 newTotal = 0;
            for (u32 i = 0; i < m.n; i++) {
                u32 v = m.freq[i];
                m.cum_f[i] = v;
                m.cum_s[i] = run;
                run += v;
                if (v > 1) {
                    v >>= 1;
                    m.freq[i] = (u16) v;
                }
                newTotal += v;
            }
            m.total = newTotal;
        }
        return s;
    }
};

} // namespace

void Yz2DecodeExec(void* dst)
{
    u32 unpacked = in_ev.size1;
    u8* out = (u8*) dst;
    Yz2ModelPC main_(kYz2Main);
    Yz2ModelPC side(kYz2Side);
    Yz2DecoderPC dec;

    dec.src = in_ev.src;
    dec.mask = 0x80;
    dec.byte = *dec.src++;

    // dic[ctx][slot]: byte offset from `out` (-1 = never written) / run length.
    std::vector<s32> dicPtr(256 * (size_t) kYz2Ring, -1);
    std::vector<u32> dicLen(256 * (size_t) kYz2Ring, 0);
    std::vector<u32> dicCnt(256, 0);

    u32 o = 0;
    u32 scan = 0;
    while (o < unpacked) {
        u32 s = dec.decodeSym(main_);
        u32 length;
        if (s > 0x3FF) {
            out[o] = (u8) s;
            o++;
            length = 1;
        } else {
            u32 ctx = out[scan];
            u32 ebase = ctx * kYz2Ring;
            u32 slot = (s + dicCnt[ctx]) & 0x1FF;
            s32 srcOfs = dicPtr[ebase + slot];
            if (s > 0x1FF) {
                length = dicLen[ebase + slot];
            } else {
                u32 t = dec.decodeSym(side);
                if (t > 2) {
                    length = t;
                } else if (t == 2) {
                    u32 b1 = dec.decodeSym(side);
                    u32 b0 = dec.decodeSym(side);
                    length = (b1 << 8) | b0;
                } else if (t == 1) {
                    u32 b2 = dec.decodeSym(side);
                    u32 b1 = dec.decodeSym(side);
                    u32 b0 = dec.decodeSym(side);
                    length = (b2 << 16) | (b1 << 8) | b0;
                } else {
                    u32 b3 = dec.decodeSym(side);
                    u32 b2 = dec.decodeSym(side);
                    u32 b1 = dec.decodeSym(side);
                    u32 b0 = dec.decodeSym(side);
                    length = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
                }
                length--;
            }
            u32 srcPos = (u32) srcOfs;
            if (srcOfs + (s64) length <= (s64) o) {
                memmove(out + o, out + srcPos, length);
            } else {
                for (u32 k = 0; k < length; k++) {
                    out[o + k] = out[srcPos + k];
                }
            }
            o += length;
        }
        u32 last = o - 1;
        if (scan < last) {
            u32 ctx = out[scan];
            u32 cnt = dicCnt[ctx];
            dicPtr[ctx * kYz2Ring + cnt] = (s32) (scan + 1);
            dicLen[ctx * kYz2Ring + cnt] = length;
            dicCnt[ctx] = (cnt + 1) & 0x1FF;
            scan = last;
        }
    }
}
#else
// Decompresses the stream prepared by Yz2DecodeSet into `dst`: models and dictionary built in the
// work buffer, first byte primed, then the decoder loop.
void Yz2DecodeExec(void* dst)
{
    Yz2Ctx ctx;
    Yz2Ctx* c = &ctx;
    Yz2Dec* d = &c->d;
    int i;
    u32 one = 1;

    d->mask = 0x80;
    d->byte = *in_ev.src++;
    ctx.n = 0x500;

    MODEL_SETUP(&d->m1, d, ctx.n);
    MODEL_SETUP(&d->m2, d, 0x100);

    c->dic = (Yz2DicEnt*) yz2Alloc(0x100 * sizeof(Yz2DicEnt));
    for (i = 0; i < 0x100; i++) {
        Yz2DicEnt* p = (Yz2DicEnt*) (i * sizeof(Yz2DicEnt) + (u32) c->dic);
        p->cnt = 0;
        memset(p->a, 0, sizeof(p->a));
        memset(p->b, 0, sizeof(p->b));
    }

    {
        Yz2Dec* dd = &c->d;
        FREQ_RESET(&dd->m1.fd);
        FREQ_RESET(&dd->m2.fd);
    }
    yz2Decode_Decode(&ctx, dst, in_ev.size1, &in_ev);
}
#endif
