#include "sv24_slice.h"

#define MODE_A1  0x11u
#define MODE_A2  0x12u
#define MODE_A4  0x13u
#define MODE_A8  0x14u
#define MODE_A16 0x15u
#define MODE_RAW 0x1fu
#define MODE_A1_565  0x21u
#define MODE_A2_565  0x22u
#define MODE_A4_565  0x23u
#define MODE_A8_565  0x24u
#define MODE_A16_565 0x25u
#define MODE_PATCH   0x30u

/* v25 smooth-field modes.  The display remains 8x8-cell based; only the
   compressed prediction unit may span multiple horizontal cells. */
#define MODE_PLANE      0x40u
#define MODE_PLANE16X8  0x41u
#define MODE_PLANE32X8  0x42u
#define MODE_HGRAD      0x43u
#define MODE_LERP4      0x45u
#define MODE_LERP8      0x46u
#define MODE_LERP16     0x47u
#define MODE_CB1        0x51u
#define MODE_CB2        0x52u
#define MODE_CB4        0x53u
#define MODE_CB8        0x54u
#define MODE_CB16       0x55u
#define MODE_CINE4      0x56u
#define MODE_CINE4B     0x57u
#define MODE_CINE2R      0x58u
#define MODE_CINE4R3     0x59u
#define MODE_LVQ2_44     0x5au
#define MODE_LVQ2_82     0x5bu
#define MODE_LVQ2_28     0x5cu
#define MODE_LVQ2_H4     0x5du
#define MODE_LVQ2_V4     0x5eu
#define MODE_LVQ4_H4     0x5fu
#define MODE_MCG        0x60u
#define MODE_MC8        0x61u
#define MODE_MCG_DELTA  0x62u
#define MODE_MC8_DELTA  0x63u
#define MODE_MCCOMP     0x64u
#define MODE_MCG_PATCH  0x65u
#define MODE_MC8_PATCH  0x66u
#define MODE_MCCOMP_G   0x67u
#define MODE_MCRES2_G   0x68u
#define MODE_MCRES2     0x69u
#define MODE_MCRES4_G   0x6au
#define MODE_MCRES4     0x6bu
#define MODE_MCRVQ_G     0x6cu
#define MODE_MCRVQ       0x6du
#define MODE_LVQ4_V4     0x6eu
#define MODE_LVQ4_44     0x6fu
/* v27.0 local true-colour quadrant VQ. Each 8x8 tile is split into four
   independent 4x4 quadrants. Q4A2 stores two RGB888 colours + a 16-bit
   selector mask per quadrant; Q4A4 stores four RGB888 colours + four bytes
   of 2-bit selectors per quadrant. Smaller spatial support makes palette
   quantisation failures much less visibly 8x8-shaped. */
#define MODE_Q4A2       0x71u
#define MODE_Q4A4       0x72u
#define MODE_DSP_SKIP   0x73u

#include "sv256_codebook.inc"

/* PATCH hot path helper.  Each 4-bit nibble maps to:
   bits  0..7  : up to four 2-bit pixel positions in ascending order
   bits  8..10 : population count
   This tiny 32-byte table lets PATCH walk only set pixels instead of
   testing all 64 mask bits, while keeping the table comfortably hot in
   each SH-2 cache. */
static const uint16_t patch_nibble_lut[16] = {
    0x0000u, 0x0103u, 0x0102u, 0x020eu, 0x0101u, 0x020du, 0x0209u, 0x0339u,
    0x0100u, 0x020cu, 0x0208u, 0x0338u, 0x0204u, 0x0334u, 0x0324u, 0x04e4u
};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t vdp2_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

/* RGB565 is separable across its two source bytes after bit replication.
   These two 1 KiB tables reduce each conversion to two indexed loads + ORs. */
static const uint32_t rgb565_hi[256] = {
    0x00000000u, 0x00002000u, 0x00004100u, 0x00006100u, 0x00008200u, 0x0000a200u, 0x0000c300u, 0x0000e300u,
    0x00000008u, 0x00002008u, 0x00004108u, 0x00006108u, 0x00008208u, 0x0000a208u, 0x0000c308u, 0x0000e308u,
    0x00000010u, 0x00002010u, 0x00004110u, 0x00006110u, 0x00008210u, 0x0000a210u, 0x0000c310u, 0x0000e310u,
    0x00000018u, 0x00002018u, 0x00004118u, 0x00006118u, 0x00008218u, 0x0000a218u, 0x0000c318u, 0x0000e318u,
    0x00000021u, 0x00002021u, 0x00004121u, 0x00006121u, 0x00008221u, 0x0000a221u, 0x0000c321u, 0x0000e321u,
    0x00000029u, 0x00002029u, 0x00004129u, 0x00006129u, 0x00008229u, 0x0000a229u, 0x0000c329u, 0x0000e329u,
    0x00000031u, 0x00002031u, 0x00004131u, 0x00006131u, 0x00008231u, 0x0000a231u, 0x0000c331u, 0x0000e331u,
    0x00000039u, 0x00002039u, 0x00004139u, 0x00006139u, 0x00008239u, 0x0000a239u, 0x0000c339u, 0x0000e339u,
    0x00000042u, 0x00002042u, 0x00004142u, 0x00006142u, 0x00008242u, 0x0000a242u, 0x0000c342u, 0x0000e342u,
    0x0000004au, 0x0000204au, 0x0000414au, 0x0000614au, 0x0000824au, 0x0000a24au, 0x0000c34au, 0x0000e34au,
    0x00000052u, 0x00002052u, 0x00004152u, 0x00006152u, 0x00008252u, 0x0000a252u, 0x0000c352u, 0x0000e352u,
    0x0000005au, 0x0000205au, 0x0000415au, 0x0000615au, 0x0000825au, 0x0000a25au, 0x0000c35au, 0x0000e35au,
    0x00000063u, 0x00002063u, 0x00004163u, 0x00006163u, 0x00008263u, 0x0000a263u, 0x0000c363u, 0x0000e363u,
    0x0000006bu, 0x0000206bu, 0x0000416bu, 0x0000616bu, 0x0000826bu, 0x0000a26bu, 0x0000c36bu, 0x0000e36bu,
    0x00000073u, 0x00002073u, 0x00004173u, 0x00006173u, 0x00008273u, 0x0000a273u, 0x0000c373u, 0x0000e373u,
    0x0000007bu, 0x0000207bu, 0x0000417bu, 0x0000617bu, 0x0000827bu, 0x0000a27bu, 0x0000c37bu, 0x0000e37bu,
    0x00000084u, 0x00002084u, 0x00004184u, 0x00006184u, 0x00008284u, 0x0000a284u, 0x0000c384u, 0x0000e384u,
    0x0000008cu, 0x0000208cu, 0x0000418cu, 0x0000618cu, 0x0000828cu, 0x0000a28cu, 0x0000c38cu, 0x0000e38cu,
    0x00000094u, 0x00002094u, 0x00004194u, 0x00006194u, 0x00008294u, 0x0000a294u, 0x0000c394u, 0x0000e394u,
    0x0000009cu, 0x0000209cu, 0x0000419cu, 0x0000619cu, 0x0000829cu, 0x0000a29cu, 0x0000c39cu, 0x0000e39cu,
    0x000000a5u, 0x000020a5u, 0x000041a5u, 0x000061a5u, 0x000082a5u, 0x0000a2a5u, 0x0000c3a5u, 0x0000e3a5u,
    0x000000adu, 0x000020adu, 0x000041adu, 0x000061adu, 0x000082adu, 0x0000a2adu, 0x0000c3adu, 0x0000e3adu,
    0x000000b5u, 0x000020b5u, 0x000041b5u, 0x000061b5u, 0x000082b5u, 0x0000a2b5u, 0x0000c3b5u, 0x0000e3b5u,
    0x000000bdu, 0x000020bdu, 0x000041bdu, 0x000061bdu, 0x000082bdu, 0x0000a2bdu, 0x0000c3bdu, 0x0000e3bdu,
    0x000000c6u, 0x000020c6u, 0x000041c6u, 0x000061c6u, 0x000082c6u, 0x0000a2c6u, 0x0000c3c6u, 0x0000e3c6u,
    0x000000ceu, 0x000020ceu, 0x000041ceu, 0x000061ceu, 0x000082ceu, 0x0000a2ceu, 0x0000c3ceu, 0x0000e3ceu,
    0x000000d6u, 0x000020d6u, 0x000041d6u, 0x000061d6u, 0x000082d6u, 0x0000a2d6u, 0x0000c3d6u, 0x0000e3d6u,
    0x000000deu, 0x000020deu, 0x000041deu, 0x000061deu, 0x000082deu, 0x0000a2deu, 0x0000c3deu, 0x0000e3deu,
    0x000000e7u, 0x000020e7u, 0x000041e7u, 0x000061e7u, 0x000082e7u, 0x0000a2e7u, 0x0000c3e7u, 0x0000e3e7u,
    0x000000efu, 0x000020efu, 0x000041efu, 0x000061efu, 0x000082efu, 0x0000a2efu, 0x0000c3efu, 0x0000e3efu,
    0x000000f7u, 0x000020f7u, 0x000041f7u, 0x000061f7u, 0x000082f7u, 0x0000a2f7u, 0x0000c3f7u, 0x0000e3f7u,
    0x000000ffu, 0x000020ffu, 0x000041ffu, 0x000061ffu, 0x000082ffu, 0x0000a2ffu, 0x0000c3ffu, 0x0000e3ffu,
};
static const uint32_t rgb565_lo[256] = {
    0x00000000u, 0x00080000u, 0x00100000u, 0x00180000u, 0x00210000u, 0x00290000u, 0x00310000u, 0x00390000u,
    0x00420000u, 0x004a0000u, 0x00520000u, 0x005a0000u, 0x00630000u, 0x006b0000u, 0x00730000u, 0x007b0000u,
    0x00840000u, 0x008c0000u, 0x00940000u, 0x009c0000u, 0x00a50000u, 0x00ad0000u, 0x00b50000u, 0x00bd0000u,
    0x00c60000u, 0x00ce0000u, 0x00d60000u, 0x00de0000u, 0x00e70000u, 0x00ef0000u, 0x00f70000u, 0x00ff0000u,
    0x00000400u, 0x00080400u, 0x00100400u, 0x00180400u, 0x00210400u, 0x00290400u, 0x00310400u, 0x00390400u,
    0x00420400u, 0x004a0400u, 0x00520400u, 0x005a0400u, 0x00630400u, 0x006b0400u, 0x00730400u, 0x007b0400u,
    0x00840400u, 0x008c0400u, 0x00940400u, 0x009c0400u, 0x00a50400u, 0x00ad0400u, 0x00b50400u, 0x00bd0400u,
    0x00c60400u, 0x00ce0400u, 0x00d60400u, 0x00de0400u, 0x00e70400u, 0x00ef0400u, 0x00f70400u, 0x00ff0400u,
    0x00000800u, 0x00080800u, 0x00100800u, 0x00180800u, 0x00210800u, 0x00290800u, 0x00310800u, 0x00390800u,
    0x00420800u, 0x004a0800u, 0x00520800u, 0x005a0800u, 0x00630800u, 0x006b0800u, 0x00730800u, 0x007b0800u,
    0x00840800u, 0x008c0800u, 0x00940800u, 0x009c0800u, 0x00a50800u, 0x00ad0800u, 0x00b50800u, 0x00bd0800u,
    0x00c60800u, 0x00ce0800u, 0x00d60800u, 0x00de0800u, 0x00e70800u, 0x00ef0800u, 0x00f70800u, 0x00ff0800u,
    0x00000c00u, 0x00080c00u, 0x00100c00u, 0x00180c00u, 0x00210c00u, 0x00290c00u, 0x00310c00u, 0x00390c00u,
    0x00420c00u, 0x004a0c00u, 0x00520c00u, 0x005a0c00u, 0x00630c00u, 0x006b0c00u, 0x00730c00u, 0x007b0c00u,
    0x00840c00u, 0x008c0c00u, 0x00940c00u, 0x009c0c00u, 0x00a50c00u, 0x00ad0c00u, 0x00b50c00u, 0x00bd0c00u,
    0x00c60c00u, 0x00ce0c00u, 0x00d60c00u, 0x00de0c00u, 0x00e70c00u, 0x00ef0c00u, 0x00f70c00u, 0x00ff0c00u,
    0x00001000u, 0x00081000u, 0x00101000u, 0x00181000u, 0x00211000u, 0x00291000u, 0x00311000u, 0x00391000u,
    0x00421000u, 0x004a1000u, 0x00521000u, 0x005a1000u, 0x00631000u, 0x006b1000u, 0x00731000u, 0x007b1000u,
    0x00841000u, 0x008c1000u, 0x00941000u, 0x009c1000u, 0x00a51000u, 0x00ad1000u, 0x00b51000u, 0x00bd1000u,
    0x00c61000u, 0x00ce1000u, 0x00d61000u, 0x00de1000u, 0x00e71000u, 0x00ef1000u, 0x00f71000u, 0x00ff1000u,
    0x00001400u, 0x00081400u, 0x00101400u, 0x00181400u, 0x00211400u, 0x00291400u, 0x00311400u, 0x00391400u,
    0x00421400u, 0x004a1400u, 0x00521400u, 0x005a1400u, 0x00631400u, 0x006b1400u, 0x00731400u, 0x007b1400u,
    0x00841400u, 0x008c1400u, 0x00941400u, 0x009c1400u, 0x00a51400u, 0x00ad1400u, 0x00b51400u, 0x00bd1400u,
    0x00c61400u, 0x00ce1400u, 0x00d61400u, 0x00de1400u, 0x00e71400u, 0x00ef1400u, 0x00f71400u, 0x00ff1400u,
    0x00001800u, 0x00081800u, 0x00101800u, 0x00181800u, 0x00211800u, 0x00291800u, 0x00311800u, 0x00391800u,
    0x00421800u, 0x004a1800u, 0x00521800u, 0x005a1800u, 0x00631800u, 0x006b1800u, 0x00731800u, 0x007b1800u,
    0x00841800u, 0x008c1800u, 0x00941800u, 0x009c1800u, 0x00a51800u, 0x00ad1800u, 0x00b51800u, 0x00bd1800u,
    0x00c61800u, 0x00ce1800u, 0x00d61800u, 0x00de1800u, 0x00e71800u, 0x00ef1800u, 0x00f71800u, 0x00ff1800u,
    0x00001c00u, 0x00081c00u, 0x00101c00u, 0x00181c00u, 0x00211c00u, 0x00291c00u, 0x00311c00u, 0x00391c00u,
    0x00421c00u, 0x004a1c00u, 0x00521c00u, 0x005a1c00u, 0x00631c00u, 0x006b1c00u, 0x00731c00u, 0x007b1c00u,
    0x00841c00u, 0x008c1c00u, 0x00941c00u, 0x009c1c00u, 0x00a51c00u, 0x00ad1c00u, 0x00b51c00u, 0x00bd1c00u,
    0x00c61c00u, 0x00ce1c00u, 0x00d61c00u, 0x00de1c00u, 0x00e71c00u, 0x00ef1c00u, 0x00f71c00u, 0x00ff1c00u,
};

static uint32_t vdp2_rgb565(const uint8_t *p)
{
    return 0x80000000u | rgb565_hi[p[0]] | rgb565_lo[p[1]];
}

static unsigned mode_k(uint8_t m)
{
    if (m == MODE_A1) return 1u;
    if (m == MODE_A2) return 2u;
    if (m == MODE_A4) return 4u;
    if (m == MODE_A8 || m == MODE_A8_565) return 8u;
    if (m == MODE_A16 || m == MODE_A16_565) return 16u;
    if (m == MODE_A1_565) return 1u;
    if (m == MODE_A2_565) return 2u;
    if (m == MODE_A4_565) return 4u;
    return 0u;
}

static unsigned selector_bytes(unsigned k)
{
    if (k == 2u) return 8u;
    if (k == 4u) return 16u;
    if (k == 8u) return 24u;
    if (k == 16u) return 32u;
    return 0u;
}

static unsigned selector(const uint8_t *s, unsigned k, unsigned i)
{
    if (k == 1u) return 0u;
    if (k == 2u) return (s[i >> 3] >> (7u - (i & 7u))) & 1u;
    if (k == 4u) return (s[i >> 2] >> (6u - ((i & 3u) << 1))) & 3u;
    if (k == 16u) return (s[i >> 1] >> ((i & 1u) ? 0u : 4u)) & 15u;
    {
        unsigned bit = i * 3u;
        unsigned v = 0u;
        unsigned n;
        for (n = 0u; n < 3u; n++) {
            unsigned pos = bit + n;
            v = (v << 1) | ((s[pos >> 3] >> (7u - (pos & 7u))) & 1u);
        }
        return v;
    }
}

static unsigned mode_span_cells(uint8_t mode)
{
    if (mode == MODE_PLANE16X8) return 2u;
    if (mode == MODE_PLANE32X8) return 4u;
    return 1u;
}

static uint8_t clamp8s(int v)
{
    if (v < 0) return 0u;
    if (v > 255) return 255u;
    return (uint8_t)v;
}

/* A linear plane reaches its extrema at rectangle corners.  Test the three
   channels once per record so the overwhelmingly common in-range case can
   skip 192 per-pixel saturation branches.  Values are tested in the plane's
   fixed-point domain; limit is ((255 << q) | ((1 << q) - 1)). */
static int plane_fixed_fits(int bq, int dx, int dy, int xmax, int limit)
{
    int x = dx * xmax;
    int y = dy * 7;
    int c0 = bq;
    int c1 = bq + x;
    int c2 = bq + y;
    int c3 = c1 + y;
    int mn = c0, mx = c0;
    if (c1 < mn) mn = c1; if (c1 > mx) mx = c1;
    if (c2 < mn) mn = c2; if (c2 > mx) mx = c2;
    if (c3 < mn) mn = c3; if (c3 > mx) mx = c3;
    return mn >= 0 && mx <= limit;
}

/* Keep the common palette families in separate noinline helpers.  The old
   fast decoder still had to reserve the A16 worst-case palette (64 bytes) and
   save a large register set for every A1/A2/A4 tile.  Family splitting lets
   GCC size each stack frame to the selector width actually being decoded. */
#define HOT_NOINLINE __attribute__((noinline))

static HOT_NOINLINE int decode_raw(const uint8_t **pp, const uint8_t *end,
                                   uint32_t *dst)
{
    const uint8_t *p = *pp;
    unsigned i;
    if ((size_t)(end - p) < 192u) return -1;
    for (i = 0u; i < 64u; i++)
        dst[i] = vdp2_rgb(p[i * 3u], p[i * 3u + 1u], p[i * 3u + 2u]);
    *pp = p + 192u;
    return 0;
}

static HOT_NOINLINE int decode_patch(const uint8_t **pp, const uint8_t *end,
                                     uint32_t *dst)
{
    const uint8_t *p = *pp;
    const uint8_t *mask;
    unsigned row;
    if ((size_t)(end - p) < 8u) return -1;
    mask = p; p += 8u;
    for (row = 0u; row < 8u; row++) {
        uint8_t q = mask[row];
        uint32_t *d;
        uint16_t th, tl;
        unsigned n;
        if (q == 0u) continue;
        d = dst + (row << 3);
        th = patch_nibble_lut[q >> 4];
        tl = patch_nibble_lut[q & 0x0fu];
        n = (unsigned)(th >> 8) + (unsigned)(tl >> 8);
        if ((size_t)(end - p) < (size_t)(n * 3u)) return -1;
        n = (unsigned)(th >> 8);
        while (n-- != 0u) {
            unsigned x = th & 3u;
            d[x] = vdp2_rgb(p[0], p[1], p[2]);
            p += 3u; th >>= 2;
        }
        n = (unsigned)(tl >> 8);
        while (n-- != 0u) {
            unsigned x = 4u + (tl & 3u);
            d[x] = vdp2_rgb(p[0], p[1], p[2]);
            p += 3u; tl >>= 2;
        }
    }
    *pp = p;
    return 0;
}

static HOT_NOINLINE int decode_a1_rgb(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp; uint32_t c; unsigned i;
    if ((size_t)(end-p)<3u) return -1;
    c=vdp2_rgb(p[0],p[1],p[2]); p+=3u;
    for(i=0u;i<64u;i+=8u){dst[i]=c;dst[i+1]=c;dst[i+2]=c;dst[i+3]=c;dst[i+4]=c;dst[i+5]=c;dst[i+6]=c;dst[i+7]=c;}
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_a2_rgb(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[2]; unsigned i;
    if ((size_t)(end-p)<14u) return -1;
    pal[0]=vdp2_rgb(p[0],p[1],p[2]); pal[1]=vdp2_rgb(p[3],p[4],p[5]); sel=p+6u; p=sel+8u;
    for(i=0u;i<8u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*8u;
      d[0]=pal[(q>>7)&1u];d[1]=pal[(q>>6)&1u];d[2]=pal[(q>>5)&1u];d[3]=pal[(q>>4)&1u];
      d[4]=pal[(q>>3)&1u];d[5]=pal[(q>>2)&1u];d[6]=pal[(q>>1)&1u];d[7]=pal[q&1u];}
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_a1(const uint8_t **pp, const uint8_t *end,
                                  uint32_t *dst, unsigned rgb565)
{
    const uint8_t *p = *pp;
    uint32_t c;
    unsigned i;
    unsigned need = rgb565 ? 2u : 3u;
    if ((size_t)(end - p) < need) return -1;
    c = rgb565 ? vdp2_rgb565(p) : vdp2_rgb(p[0], p[1], p[2]);
    p += need;
    for (i = 0u; i < 64u; i += 8u) {
        dst[i+0u]=c; dst[i+1u]=c; dst[i+2u]=c; dst[i+3u]=c;
        dst[i+4u]=c; dst[i+5u]=c; dst[i+6u]=c; dst[i+7u]=c;
    }
    *pp = p;
    return 0;
}

static HOT_NOINLINE int decode_a2(const uint8_t **pp, const uint8_t *end,
                                  uint32_t *dst, unsigned rgb565)
{
    const uint8_t *p=*pp, *sel;
    uint32_t pal[2];
    unsigned i, cb=rgb565?2u:3u;
    if ((size_t)(end-p) < (size_t)(2u*cb+8u)) return -1;
    if (rgb565) { pal[0]=vdp2_rgb565(p); pal[1]=vdp2_rgb565(p+2); }
    else { pal[0]=vdp2_rgb(p[0],p[1],p[2]); pal[1]=vdp2_rgb(p[3],p[4],p[5]); }
    sel=p+2u*cb; p=sel+8u;
    for(i=0u;i<8u;i++) { uint8_t q=sel[i]; uint32_t*d=dst+i*8u;
        d[0]=pal[(q>>7)&1u]; d[1]=pal[(q>>6)&1u]; d[2]=pal[(q>>5)&1u]; d[3]=pal[(q>>4)&1u];
        d[4]=pal[(q>>3)&1u]; d[5]=pal[(q>>2)&1u]; d[6]=pal[(q>>1)&1u]; d[7]=pal[q&1u]; }
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_a4(const uint8_t **pp, const uint8_t *end,
                                  uint32_t *dst, unsigned rgb565)
{
    const uint8_t *p=*pp, *sel;
    uint32_t pal[4];
    unsigned i, cb=rgb565?2u:3u;
    if ((size_t)(end-p) < (size_t)(4u*cb+16u)) return -1;
    if (rgb565) for(i=0u;i<4u;i++) pal[i]=vdp2_rgb565(p+i*2u);
    else for(i=0u;i<4u;i++) pal[i]=vdp2_rgb(p[i*3u],p[i*3u+1u],p[i*3u+2u]);
    sel=p+4u*cb; p=sel+16u;
    for(i=0u;i<16u;i++) { uint8_t q=sel[i]; uint32_t*d=dst+i*4u;
        d[0]=pal[(q>>6)&3u]; d[1]=pal[(q>>4)&3u]; d[2]=pal[(q>>2)&3u]; d[3]=pal[q&3u]; }
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_a8(const uint8_t **pp, const uint8_t *end,
                                  uint32_t *dst, unsigned rgb565)
{
    const uint8_t *p=*pp, *sel;
    uint32_t pal[8];
    unsigned i, cb=rgb565?2u:3u;
    if ((size_t)(end-p) < (size_t)(8u*cb+24u)) return -1;
    if (rgb565) for(i=0u;i<8u;i++) pal[i]=vdp2_rgb565(p+i*2u);
    else for(i=0u;i<8u;i++) pal[i]=vdp2_rgb(p[i*3u],p[i*3u+1u],p[i*3u+2u]);
    sel=p+8u*cb; p=sel+24u;
    for(i=0u;i<8u;i++) { uint32_t q=((uint32_t)sel[i*3u]<<16)|((uint32_t)sel[i*3u+1u]<<8)|sel[i*3u+2u]; uint32_t*d=dst+i*8u;
        d[0]=pal[(q>>21)&7u];d[1]=pal[(q>>18)&7u];d[2]=pal[(q>>15)&7u];d[3]=pal[(q>>12)&7u];
        d[4]=pal[(q>>9)&7u];d[5]=pal[(q>>6)&7u];d[6]=pal[(q>>3)&7u];d[7]=pal[q&7u]; }
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_a16(const uint8_t **pp, const uint8_t *end,
                                   uint32_t *dst, unsigned rgb565)
{
    const uint8_t *p=*pp, *sel;
    uint32_t pal[16];
    unsigned i, cb=rgb565?2u:3u;
    if ((size_t)(end-p) < (size_t)(16u*cb+32u)) return -1;
    if (rgb565) for(i=0u;i<16u;i++) pal[i]=vdp2_rgb565(p+i*2u);
    else for(i=0u;i<16u;i++) pal[i]=vdp2_rgb(p[i*3u],p[i*3u+1u],p[i*3u+2u]);
    sel=p+16u*cb; p=sel+32u;
    for(i=0u;i<32u;i++) { uint8_t q=sel[i]; uint32_t*d=dst+i*2u; d[0]=pal[(q>>4)&15u]; d[1]=pal[q&15u]; }
    *pp=p; return 0;
}


/* Dedicated RGB565 helpers for the dominant palette modes. */
static HOT_NOINLINE int decode_a1_565(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp; uint32_t c; unsigned i;
    if ((size_t)(end-p)<2u) return -1;
    c=vdp2_rgb565(p); p+=2u;
    for(i=0u;i<64u;i+=8u){dst[i]=c;dst[i+1]=c;dst[i+2]=c;dst[i+3]=c;dst[i+4]=c;dst[i+5]=c;dst[i+6]=c;dst[i+7]=c;}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_a2_565(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[2]; unsigned i;
    if ((size_t)(end-p)<12u) return -1;
    pal[0]=vdp2_rgb565(p); pal[1]=vdp2_rgb565(p+2); sel=p+4; p=sel+8;
    for(i=0u;i<8u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*8u;
      d[0]=pal[(q>>7)&1u];d[1]=pal[(q>>6)&1u];d[2]=pal[(q>>5)&1u];d[3]=pal[(q>>4)&1u];
      d[4]=pal[(q>>3)&1u];d[5]=pal[(q>>2)&1u];d[6]=pal[(q>>1)&1u];d[7]=pal[q&1u];}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_a4_565(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[4]; unsigned i;
    if ((size_t)(end-p)<24u) return -1;
    for(i=0u;i<4u;i++) pal[i]=vdp2_rgb565(p+i*2u); sel=p+8; p=sel+16;
    for(i=0u;i<16u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*4u;
      d[0]=pal[(q>>6)&3u];d[1]=pal[(q>>4)&3u];d[2]=pal[(q>>2)&3u];d[3]=pal[q&3u];}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_a8_565(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[8]; unsigned i;
    if ((size_t)(end-p)<40u) return -1;
    for(i=0u;i<8u;i++) pal[i]=vdp2_rgb565(p+i*2u); sel=p+16; p=sel+24;
    for(i=0u;i<8u;i++) {
        uint8_t b0=sel[i*3u], b1=sel[i*3u+1u], b2=sel[i*3u+2u]; uint32_t*d=dst+i*8u;
        d[0]=pal[b0>>5]; d[1]=pal[(b0>>2)&7u];
        d[2]=pal[((b0&3u)<<1)|(b1>>7)]; d[3]=pal[(b1>>4)&7u];
        d[4]=pal[(b1>>1)&7u]; d[5]=pal[((b1&1u)<<2)|(b2>>6)];
        d[6]=pal[(b2>>3)&7u]; d[7]=pal[b2&7u];
    }
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_a16_565(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[16]; unsigned i;
    if ((size_t)(end-p)<64u) return -1;
    for(i=0u;i<16u;i++) pal[i]=vdp2_rgb565(p+i*2u); sel=p+32; p=sel+32;
    for(i=0u;i<32u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*2u;d[0]=pal[(q>>4)&15u];d[1]=pal[q&15u];}
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_q4a2(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp;
    unsigned by,bx,y;
    if ((size_t)(end-p) < 32u) return -1;
    for (by=0u; by<2u; by++) {
        for (bx=0u; bx<2u; bx++) {
            uint32_t c0=vdp2_rgb(p[0],p[1],p[2]);
            uint32_t c1=vdp2_rgb(p[3],p[4],p[5]);
            uint16_t m=(uint16_t)(((uint16_t)p[6]<<8)|p[7]);
            p += 8u;
            for (y=0u; y<4u; y++) {
                uint32_t *d=dst+(by*4u+y)*8u+bx*4u;
                unsigned sh=12u-y*4u;
                uint8_t q=(uint8_t)((m>>sh)&15u);
                d[0]=(q&8u)?c1:c0; d[1]=(q&4u)?c1:c0;
                d[2]=(q&2u)?c1:c0; d[3]=(q&1u)?c1:c0;
            }
        }
    }
    *pp=p; return 0;
}

static HOT_NOINLINE int decode_q4a4(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp;
    unsigned by,bx,y;
    if ((size_t)(end-p) < 64u) return -1;
    for (by=0u; by<2u; by++) {
        for (bx=0u; bx<2u; bx++) {
            uint32_t pal[4];
            pal[0]=vdp2_rgb(p[0],p[1],p[2]);
            pal[1]=vdp2_rgb(p[3],p[4],p[5]);
            pal[2]=vdp2_rgb(p[6],p[7],p[8]);
            pal[3]=vdp2_rgb(p[9],p[10],p[11]);
            p += 12u;
            for (y=0u; y<4u; y++) {
                uint8_t q=*p++;
                uint32_t *d=dst+(by*4u+y)*8u+bx*4u;
                d[0]=pal[(q>>6)&3u]; d[1]=pal[(q>>4)&3u];
                d[2]=pal[(q>>2)&3u]; d[3]=pal[q&3u];
            }
        }
    }
    *pp=p; return 0;
}



/* v25.6 fixed-dictionary palette modes.  The 256-entry RGB888 dictionary is
   preconverted to VDP2 word layout, so each palette colour is one byte read
   plus one indexed longword load. */
static HOT_NOINLINE int decode_cb1(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp; uint32_t c; unsigned i;
    if ((size_t)(end-p)<1u) return -1;
    c=sv256_codebook[*p++];
    for(i=0u;i<64u;i+=8u){dst[i]=c;dst[i+1]=c;dst[i+2]=c;dst[i+3]=c;dst[i+4]=c;dst[i+5]=c;dst[i+6]=c;dst[i+7]=c;}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_cb2(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[2]; unsigned i;
    if ((size_t)(end-p)<10u) return -1;
    pal[0]=sv256_codebook[p[0]]; pal[1]=sv256_codebook[p[1]]; sel=p+2; p=sel+8;
    for(i=0u;i<8u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*8u;
      d[0]=pal[(q>>7)&1u];d[1]=pal[(q>>6)&1u];d[2]=pal[(q>>5)&1u];d[3]=pal[(q>>4)&1u];
      d[4]=pal[(q>>3)&1u];d[5]=pal[(q>>2)&1u];d[6]=pal[(q>>1)&1u];d[7]=pal[q&1u];}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_cb4(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[4]; unsigned i;
    if ((size_t)(end-p)<20u) return -1;
    for(i=0u;i<4u;i++) pal[i]=sv256_codebook[p[i]]; sel=p+4; p=sel+16;
    for(i=0u;i<16u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*4u;
      d[0]=pal[(q>>6)&3u];d[1]=pal[(q>>4)&3u];d[2]=pal[(q>>2)&3u];d[3]=pal[q&3u];}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_cb8(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[8]; unsigned i;
    if ((size_t)(end-p)<32u) return -1;
    for(i=0u;i<8u;i++) pal[i]=sv256_codebook[p[i]]; sel=p+8; p=sel+24;
    for(i=0u;i<8u;i++){uint8_t b0=sel[i*3u],b1=sel[i*3u+1u],b2=sel[i*3u+2u];uint32_t*d=dst+i*8u;
      d[0]=pal[b0>>5];d[1]=pal[(b0>>2)&7u];d[2]=pal[((b0&3u)<<1)|(b1>>7)];d[3]=pal[(b1>>4)&7u];
      d[4]=pal[(b1>>1)&7u];d[5]=pal[((b1&1u)<<2)|(b2>>6)];d[6]=pal[(b2>>3)&7u];d[7]=pal[b2&7u];}
    *pp=p; return 0;
}
static HOT_NOINLINE int decode_cb16(const uint8_t **pp, const uint8_t *end, uint32_t *dst)
{
    const uint8_t *p=*pp,*sel; uint32_t pal[16]; unsigned i;
    if ((size_t)(end-p)<48u) return -1;
    for(i=0u;i<16u;i++) pal[i]=sv256_codebook[p[i]]; sel=p+16; p=sel+32;
    for(i=0u;i<32u;i++){uint8_t q=sel[i];uint32_t*d=dst+i*2u;d[0]=pal[(q>>4)&15u];d[1]=pal[q&15u];}
    *pp=p; return 0;
}


/* v25.8 Cinepak-like soft-texture modes.  A 4x4 codebook raster is expanded
   to 8x8.  CINE4 uses 2x2 constant samples; CINE4B bilinearly blends the
   4x4 samples so bandwidth-pressure failures become soft rather than 8x8
   macroblocks. */
static __attribute__((always_inline)) inline uint32_t avg_rgb_ceil(uint32_t a,uint32_t b)
{
    return 0x80000000u | (((a & 0x00fefefeu)>>1) + ((b & 0x00fefefeu)>>1) + ((a|b)&0x00010101u));
}
static HOT_NOINLINE int decode_cine4(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned gy,gx;
    if((size_t)(end-p)<16u)return -1;
    for(gy=0u;gy<4u;gy++) for(gx=0u;gx<4u;gx++){
        uint32_t c=sv256_codebook[*p++]; unsigned y=gy<<1,x=gx<<1;
        dst[y*8u+x]=c;dst[y*8u+x+1u]=c;dst[(y+1u)*8u+x]=c;dst[(y+1u)*8u+x+1u]=c;
    }
    *pp=p;return 0;
}
static HOT_NOINLINE int decode_cine4b(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; uint32_t g[4][4],r[4][8]; unsigned y,x;
    if((size_t)(end-p)<16u)return -1;
    for(y=0u;y<4u;y++)for(x=0u;x<4u;x++)g[y][x]=sv256_codebook[*p++];
    for(y=0u;y<4u;y++){
        r[y][0]=g[y][0];r[y][2]=g[y][1];r[y][4]=g[y][2];r[y][6]=g[y][3];
        r[y][1]=avg_rgb_ceil(g[y][0],g[y][1]);r[y][3]=avg_rgb_ceil(g[y][1],g[y][2]);
        r[y][5]=avg_rgb_ceil(g[y][2],g[y][3]);r[y][7]=g[y][3];
    }
    for(y=0u;y<8u;y++){unsigned sy=y>>1;uint32_t *d=dst+y*8u;
        if(!(y&1u)||sy==3u){for(x=0u;x<8u;x++)d[x]=r[sy][x];}
        else {for(x=0u;x<8u;x++)d[x]=avg_rgb_ceil(r[sy][x],r[sy+1u][x]);}
    }
    *pp=p;return 0;
}



/* v25.9 high-detail residual-codebook modes. */
static const int8_t cine2r_residuals[32][3] = {
    {0,0,0},{1,-6,-18},{10,13,12},{-14,-14,-10},{21,22,20},{-21,-23,-24},{22,32,37},{-34,-33,-34},
    {44,35,18},{-57,-47,-17},{44,47,45},{-43,-48,-51},{-77,-21,24},{12,-36,-90},{-66,-62,-60},{81,67,30},
    {64,65,63},{-91,-73,-23},{22,76,97},{112,67,-7},{-75,-82,-84},{85,85,80},{-124,-102,7},{-117,-106,-43},
    {116,105,49},{125,114,-6},{-102,-98,-99},{98,105,104},{-122,-117,-71},{121,119,82},{-124,-123,-122},{124,125,123}
};

static __attribute__((always_inline)) inline uint32_t add_rgb_residual(uint32_t c, int dr, int dg, int db)
{
    int r=(int)(c&255u)+dr;
    int g=(int)((c>>8)&255u)+dg;
    int b=(int)((c>>16)&255u)+db;
    return vdp2_rgb(clamp8s(r),clamp8s(g),clamp8s(b));
}

/* Payload: 16 pairs [base-index, residual-index:5 | p1/p2/p3 mask:3]. */
static HOT_NOINLINE int decode_cine2r(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned gy,gx;
    if((size_t)(end-p)<32u)return -1;
    for(gy=0u;gy<4u;gy++) for(gx=0u;gx<4u;gx++){
        uint32_t base=sv256_codebook[p[0]];
        uint8_t rm=p[1]; unsigned ri=(unsigned)(rm>>3); uint8_t m=(uint8_t)(rm&7u);
        const int8_t *rv=cine2r_residuals[ri];
        uint32_t alt=add_rgb_residual(base,rv[0],rv[1],rv[2]);
        unsigned y=gy<<1,x=gx<<1;
        dst[y*8u+x]=base;
        dst[y*8u+x+1u]=(m&4u)?alt:base;
        dst[(y+1u)*8u+x]=(m&2u)?alt:base;
        dst[(y+1u)*8u+x+1u]=(m&1u)?alt:base;
        p+=2u;
    }
    *pp=p;return 0;
}

/* Payload: signed RGB residual (3), sixteen 2x2 codebook bases (16), then
   eight per-row mask bytes (8).  Each mask bit selects base+residual. */
static HOT_NOINLINE int decode_cine4r3(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; int dr,dg,db; uint32_t base[16],alt[16]; const uint8_t *mask; unsigned g,y,x;
    if((size_t)(end-p)<27u)return -1;
    dr=(int)(int8_t)p[0];dg=(int)(int8_t)p[1];db=(int)(int8_t)p[2];p+=3u;
    for(g=0u;g<16u;g++){
        base[g]=sv256_codebook[*p++];
        alt[g]=add_rgb_residual(base[g],dr,dg,db);
    }
    mask=p;p+=8u;
    for(y=0u;y<8u;y++){
        uint8_t m=mask[y]; uint32_t *d=dst+y*8u; unsigned gy=y>>1;
        for(x=0u;x<8u;x++){
            g=gy*4u+(x>>1);
            d[x]=(m&(0x80u>>x))?alt[g]:base[g];
        }
    }
    *pp=p;return 0;
}


/* v26.0 local two-colour VQ.  Each record contains four independent
   16-pixel regions.  A region stores two global-codebook indices plus a
   16-bit selector mask.  Three mode values select 4x4 quadrants, 8x2 strips,
   or 2x8 strips without spending an extra geometry byte.  This preserves
   one-pixel anime edges at the same 17-byte record size as CINE4. */
static HOT_NOINLINE int decode_lvq2_44(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned by,bx,y,x,q;
    if((size_t)(end-p)<16u)return -1;
    for(by=0u;by<2u;by++)for(bx=0u;bx<2u;bx++){
        uint32_t c0=sv256_codebook[p[0]],c1=sv256_codebook[p[1]];
        uint16_t m=(uint16_t)(((uint16_t)p[2]<<8)|p[3]);p+=4u;q=0u;
        for(y=0u;y<4u;y++)for(x=0u;x<4u;x++,q++)
            dst[(by*4u+y)*8u+bx*4u+x]=(m&(0x8000u>>q))?c1:c0;
    }
    *pp=p;return 0;
}
static HOT_NOINLINE int decode_lvq2_82(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned b,y,x,q;
    if((size_t)(end-p)<16u)return -1;
    for(b=0u;b<4u;b++){
        uint32_t c0=sv256_codebook[p[0]],c1=sv256_codebook[p[1]];
        uint16_t m=(uint16_t)(((uint16_t)p[2]<<8)|p[3]);p+=4u;q=0u;
        for(y=0u;y<2u;y++)for(x=0u;x<8u;x++,q++)
            dst[(b*2u+y)*8u+x]=(m&(0x8000u>>q))?c1:c0;
    }
    *pp=p;return 0;
}
static HOT_NOINLINE int decode_lvq2_28(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned b,y,x,q;
    if((size_t)(end-p)<16u)return -1;
    for(b=0u;b<4u;b++){
        uint32_t c0=sv256_codebook[p[0]],c1=sv256_codebook[p[1]];
        uint16_t m=(uint16_t)(((uint16_t)p[2]<<8)|p[3]);p+=4u;q=0u;
        for(y=0u;y<8u;y++)for(x=0u;x<2u;x++,q++)
            dst[y*8u+b*2u+x]=(m&(0x8000u>>q))?c1:c0;
    }
    *pp=p;return 0;
}


/* v26.1 experimental local VQ extensions.  Byte-oriented selectors avoid
   expensive 64-bit shifts on SH-2. */
static HOT_NOINLINE int decode_lvq2_h4(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned b,y,x; uint32_t c0,c1,m;
    if((size_t)(end-p)<12u)return -1;
    for(b=0u;b<2u;b++){
        c0=sv256_codebook[p[0]];c1=sv256_codebook[p[1]];
        m=((uint32_t)p[2]<<24)|((uint32_t)p[3]<<16)|((uint32_t)p[4]<<8)|p[5];p+=6u;
        for(y=0u;y<4u;y++)for(x=0u;x<8u;x++)dst[(b*4u+y)*8u+x]=(m&(0x80000000u>>(y*8u+x)))?c1:c0;
    }
    *pp=p;return 0;
}
static HOT_NOINLINE int decode_lvq2_v4(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned b,y,x; uint32_t c0,c1,m;
    if((size_t)(end-p)<12u)return -1;
    for(b=0u;b<2u;b++){
        c0=sv256_codebook[p[0]];c1=sv256_codebook[p[1]];
        m=((uint32_t)p[2]<<24)|((uint32_t)p[3]<<16)|((uint32_t)p[4]<<8)|p[5];p+=6u;
        for(y=0u;y<8u;y++)for(x=0u;x<4u;x++)dst[y*8u+b*4u+x]=(m&(0x80000000u>>(y*4u+x)))?c1:c0;
    }
    *pp=p;return 0;
}
static __attribute__((always_inline)) inline void lvq4_store4(uint8_t q,const uint32_t *pal,uint32_t *d)
{
    d[0]=pal[(q>>6)&3u];d[1]=pal[(q>>4)&3u];d[2]=pal[(q>>2)&3u];d[3]=pal[q&3u];
}
static HOT_NOINLINE int decode_lvq4_h4(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned b,y; uint32_t pal[4];
    if((size_t)(end-p)<24u)return -1;
    for(b=0u;b<2u;b++){
        pal[0]=sv256_codebook[p[0]];pal[1]=sv256_codebook[p[1]];pal[2]=sv256_codebook[p[2]];pal[3]=sv256_codebook[p[3]];p+=4u;
        for(y=0u;y<4u;y++){uint32_t*d=dst+(b*4u+y)*8u;lvq4_store4(*p++,pal,d);lvq4_store4(*p++,pal,d+4u);}
    }
    *pp=p;return 0;
}
static HOT_NOINLINE int decode_lvq4_v4(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned b,y; uint32_t pal[4];
    if((size_t)(end-p)<24u)return -1;
    for(b=0u;b<2u;b++){
        pal[0]=sv256_codebook[p[0]];pal[1]=sv256_codebook[p[1]];pal[2]=sv256_codebook[p[2]];pal[3]=sv256_codebook[p[3]];p+=4u;
        for(y=0u;y<8u;y++)lvq4_store4(*p++,pal,dst+y*8u+b*4u);
    }
    *pp=p;return 0;
}
static HOT_NOINLINE int decode_lvq4_44(const uint8_t **pp,const uint8_t *end,uint32_t *dst)
{
    const uint8_t *p=*pp; unsigned by,bx,y; uint32_t pal[4];
    if((size_t)(end-p)<32u)return -1;
    for(by=0u;by<2u;by++)for(bx=0u;bx<2u;bx++){
        pal[0]=sv256_codebook[p[0]];pal[1]=sv256_codebook[p[1]];pal[2]=sv256_codebook[p[2]];pal[3]=sv256_codebook[p[3]];p+=4u;
        for(y=0u;y<4u;y++)lvq4_store4(*p++,pal,dst+(by*4u+y)*8u+bx*4u);
    }
    *pp=p;return 0;
}

static __attribute__((noinline)) int decode_lerp(const uint8_t **pp, const uint8_t *end,
                                                    uint8_t mode, uint32_t *dst)
{
    const uint8_t *p = *pp;
    unsigned i;

        uint32_t pal[16];
        const uint8_t *sel;
        unsigned j;

        /* Keep the interpolation denominator compile-time constant.  The old
           generic loop generated a software unsigned divide for every palette
           component on SH-2. */
#define BUILD_LERP(K,DEN) do { \
        for (j = 0u; j < (K); j++) { \
            unsigned r = (((DEN) - j) * p[0] + j * p[3] + (DEN) / 2u) / (DEN); \
            unsigned g = (((DEN) - j) * p[1] + j * p[4] + (DEN) / 2u) / (DEN); \
            unsigned b = (((DEN) - j) * p[2] + j * p[5] + (DEN) / 2u) / (DEN); \
            pal[j] = vdp2_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b); \
        } \
    } while (0)
        if (mode == MODE_LERP4) {
            if ((size_t)(end - p) < 22u) return -1;
            BUILD_LERP(4u,3u); sel=p+6u; p=sel+16u;
            for (i=0u;i<16u;i++) { uint8_t q=sel[i]; uint32_t*d=dst+i*4u;
                d[0]=pal[(q>>6)&3u]; d[1]=pal[(q>>4)&3u]; d[2]=pal[(q>>2)&3u]; d[3]=pal[q&3u]; }
        } else if (mode == MODE_LERP8) {
            if ((size_t)(end - p) < 30u) return -1;
            BUILD_LERP(8u,7u); sel=p+6u; p=sel+24u;
            for (i=0u;i<8u;i++) { uint32_t q=((uint32_t)sel[i*3u]<<16)|((uint32_t)sel[i*3u+1u]<<8)|sel[i*3u+2u]; uint32_t*d=dst+i*8u;
                d[0]=pal[(q>>21)&7u]; d[1]=pal[(q>>18)&7u]; d[2]=pal[(q>>15)&7u]; d[3]=pal[(q>>12)&7u];
                d[4]=pal[(q>>9)&7u]; d[5]=pal[(q>>6)&7u]; d[6]=pal[(q>>3)&7u]; d[7]=pal[q&7u]; }
        } else {
            if ((size_t)(end - p) < 38u) return -1;
            BUILD_LERP(16u,15u); sel=p+6u; p=sel+32u;
            for (i=0u;i<32u;i++) { uint8_t q=sel[i]; uint32_t*d=dst+i*2u;
                d[0]=pal[(q>>4)&15u]; d[1]=pal[q&15u]; }
        }
#undef BUILD_LERP
        *pp = p;
        return 0;

    return -2;
}

static int decode_tile_slow(const uint8_t **pp, const uint8_t *end,
                            uint8_t mode, uint32_t *dst, const uint32_t *left);

static __attribute__((always_inline)) inline int decode_tile(const uint8_t **pp, const uint8_t *end,
                       uint8_t mode, uint32_t *dst, const uint32_t *left)
{
    switch (mode) {
    case MODE_RAW: return decode_raw(pp,end,dst);
    case MODE_PATCH: return decode_patch(pp,end,dst);
    case MODE_A1: return decode_a1_rgb(pp,end,dst);
    case MODE_A1_565: return decode_a1_565(pp,end,dst);
    case MODE_A2: return decode_a2_rgb(pp,end,dst);
    case MODE_A2_565: return decode_a2_565(pp,end,dst);
    case MODE_A4: return decode_a4(pp,end,dst,0u);
    case MODE_A4_565: return decode_a4_565(pp,end,dst);
    case MODE_A8: return decode_a8(pp,end,dst,0u);
    case MODE_A8_565: return decode_a8_565(pp,end,dst);
    case MODE_A16: return decode_a16(pp,end,dst,0u);
    case MODE_A16_565: return decode_a16_565(pp,end,dst);
    case MODE_CB1: return decode_cb1(pp,end,dst);
    case MODE_CB2: return decode_cb2(pp,end,dst);
    case MODE_CB4: return decode_cb4(pp,end,dst);
    case MODE_CB8: return decode_cb8(pp,end,dst);
    case MODE_CB16: return decode_cb16(pp,end,dst);
    case MODE_CINE4: return decode_cine4(pp,end,dst);
    case MODE_CINE4B: return decode_cine4b(pp,end,dst);
    case MODE_CINE2R: return decode_cine2r(pp,end,dst);
    case MODE_CINE4R3: return decode_cine4r3(pp,end,dst);
    case MODE_LVQ2_44: return decode_lvq2_44(pp,end,dst);
    case MODE_LVQ2_82: return decode_lvq2_82(pp,end,dst);
    case MODE_LVQ2_28: return decode_lvq2_28(pp,end,dst);
    case MODE_LVQ2_H4: return decode_lvq2_h4(pp,end,dst);
    case MODE_LVQ2_V4: return decode_lvq2_v4(pp,end,dst);
    case MODE_LVQ4_H4: return decode_lvq4_h4(pp,end,dst);
    case MODE_LVQ4_V4: return decode_lvq4_v4(pp,end,dst);
    case MODE_LVQ4_44: return decode_lvq4_44(pp,end,dst);
    case MODE_Q4A2: return decode_q4a2(pp,end,dst);
    case MODE_Q4A4: return decode_q4a4(pp,end,dst);
    case MODE_DSP_SKIP: return 0; /* SCU-DSP owns this tile; payload is in packet sidecar. */
    case MODE_LERP4: case MODE_LERP8: case MODE_LERP16:
        return decode_lerp(pp,end,mode,dst);
    default: return decode_tile_slow(pp,end,mode,dst,left);
    }
}

static int decode_tile_slow(const uint8_t **pp, const uint8_t *end,
                       uint8_t mode, uint32_t *dst, const uint32_t *left)
{
    const uint8_t *p = *pp;
    unsigned i;

    if (mode == MODE_PLANE) {
        unsigned y, x, q;
        int br, bg, bb, dxr, dxg, dxb, dyr, dyg, dyb;
        int rr, rg, rb;
        if ((size_t)(end - p) < 10u) return -1;
        br = p[0]; bg = p[1]; bb = p[2];
        dxr = (int)(int8_t)p[3]; dxg = (int)(int8_t)p[4]; dxb = (int)(int8_t)p[5];
        dyr = (int)(int8_t)p[6]; dyg = (int)(int8_t)p[7]; dyb = (int)(int8_t)p[8];
        q = p[9]; p += 10u;

        /* SH-2 has no cheap variable arithmetic shift.  A variable >>q made
           GCC emit ___ashrsi3 calls in the per-pixel hot loop.  Dispatch once
           per tile and compile the shift as an immediate sequence instead. */
#define PLANE8_Q(Q,LIMIT) do { \
        int fast; \
        rr = br << (Q); rg = bg << (Q); rb = bb << (Q); \
        fast = plane_fixed_fits(rr, dxr, dyr, 7, (LIMIT)) && \
               plane_fixed_fits(rg, dxg, dyg, 7, (LIMIT)) && \
               plane_fixed_fits(rb, dxb, dyb, 7, (LIMIT)); \
        for (y = 0u; y < 8u; y++) { \
            int ar = rr, ag = rg, ab = rb; uint32_t *d = dst + (y << 3); \
            if (fast) { \
                for (x = 0u; x < 8u; x++) { \
                    d[x] = 0x80000000u | ((uint32_t)(ab >> (Q)) << 16) | \
                           ((uint32_t)(ag >> (Q)) << 8) | (uint32_t)(ar >> (Q)); \
                    ar += dxr; ag += dxg; ab += dxb; \
                } \
            } else { \
                for (x = 0u; x < 8u; x++) { \
                    d[x] = vdp2_rgb(clamp8s(ar >> (Q)), clamp8s(ag >> (Q)), clamp8s(ab >> (Q))); \
                    ar += dxr; ag += dxg; ab += dxb; \
                } \
            } \
            rr += dyr; rg += dyg; rb += dyb; \
        } \
    } while (0)
        switch (q) {
        case 2u: PLANE8_Q(2, 1023); break;
        case 3u: PLANE8_Q(3, 2047); break;
        case 4u: PLANE8_Q(4, 4095); break;
        case 5u: PLANE8_Q(5, 8191); break;
        case 6u: PLANE8_Q(6, 16383); break;
        default: return -1;
        }
#undef PLANE8_Q
        *pp = p;
        return 0;
    }

    if (mode == MODE_PLANE16X8 || mode == MODE_PLANE32X8) {
        unsigned cells = mode_span_cells(mode);
        unsigned width = cells << 3;
        unsigned y, x, q;
        int br, bg, bb, dxr, dxg, dxb, dyr, dyg, dyb;
        if ((size_t)(end - p) < 10u) return -1;
        br = p[0]; bg = p[1]; bb = p[2];
        dxr = (int)(int8_t)p[3]; dxg = (int)(int8_t)p[4]; dxb = (int)(int8_t)p[5];
        dyr = (int)(int8_t)p[6]; dyg = (int)(int8_t)p[7]; dyb = (int)(int8_t)p[8];
        q = p[9];
        p += 10u;
#define PLANESPAN_Q(Q,LIMIT) do { \
        int rr = br << (Q), rg = bg << (Q), rb = bb << (Q); \
        int fast = plane_fixed_fits(rr, dxr, dyr, (int)width - 1, (LIMIT)) && \
                   plane_fixed_fits(rg, dxg, dyg, (int)width - 1, (LIMIT)) && \
                   plane_fixed_fits(rb, dxb, dyb, (int)width - 1, (LIMIT)); \
        for (y = 0u; y < 8u; y++) { \
            int ar = rr, ag = rg, ab = rb; \
            if (fast) { \
                for (x = 0u; x < width; x++) { \
                    unsigned di = (x >> 3) * 64u + y * 8u + (x & 7u); \
                    dst[di] = 0x80000000u | ((uint32_t)(ab >> (Q)) << 16) | \
                              ((uint32_t)(ag >> (Q)) << 8) | (uint32_t)(ar >> (Q)); \
                    ar += dxr; ag += dxg; ab += dxb; \
                } \
            } else { \
                for (x = 0u; x < width; x++) { \
                    unsigned di = (x >> 3) * 64u + y * 8u + (x & 7u); \
                    dst[di] = vdp2_rgb(clamp8s(ar >> (Q)), clamp8s(ag >> (Q)), clamp8s(ab >> (Q))); \
                    ar += dxr; ag += dxg; ab += dxb; \
                } \
            } \
            rr += dyr; rg += dyg; rb += dyb; \
        } \
    } while (0)
        switch (q) {
        case 2u: PLANESPAN_Q(2, 1023); break;
        case 3u: PLANESPAN_Q(3, 2047); break;
        case 4u: PLANESPAN_Q(4, 4095); break;
        case 5u: PLANESPAN_Q(5, 8191); break;
        case 6u: PLANESPAN_Q(6, 16383); break;
        default: return -1;
        }
#undef PLANESPAN_Q
        *pp = p;
        return 0;
    }

    if (mode == MODE_HGRAD) {
        unsigned y, x;
        int dr, dg, db;
        if (!left || (size_t)(end - p) < 3u) return -1;
        dr = (int)(int8_t)p[0]; dg = (int)(int8_t)p[1]; db = (int)(int8_t)p[2];
        p += 3u;
        {
            int fast = 1;
            for (y = 0u; y < 8u; y++) {
                uint32_t lv = left[y * 8u + 7u];
                int r0 = ((int)(lv & 255u) << 4) + dr;
                int g0 = ((int)((lv >> 8) & 255u) << 4) + dg;
                int b0 = ((int)((lv >> 16) & 255u) << 4) + db;
                int r7 = r0 + dr * 7, g7 = g0 + dg * 7, b7 = b0 + db * 7;
                if (r0 < 0 || r0 > 4095 || r7 < 0 || r7 > 4095 ||
                    g0 < 0 || g0 > 4095 || g7 < 0 || g7 > 4095 ||
                    b0 < 0 || b0 > 4095 || b7 < 0 || b7 > 4095) { fast = 0; break; }
            }
            for (y = 0u; y < 8u; y++) {
                uint32_t lv = left[y * 8u + 7u];
                int ar = ((int)(lv & 255u) << 4) + dr;
                int ag = ((int)((lv >> 8) & 255u) << 4) + dg;
                int ab = ((int)((lv >> 16) & 255u) << 4) + db;
                uint32_t *d = dst + y * 8u;
                if (fast) {
                    for (x = 0u; x < 8u; x++) {
                        d[x] = 0x80000000u | ((uint32_t)(ab >> 4) << 16) |
                               ((uint32_t)(ag >> 4) << 8) | (uint32_t)(ar >> 4);
                        ar += dr; ag += dg; ab += db;
                    }
                } else {
                    for (x = 0u; x < 8u; x++) {
                        d[x] = vdp2_rgb(clamp8s(ar >> 4), clamp8s(ag >> 4), clamp8s(ab >> 4));
                        ar += dr; ag += dg; ab += db;
                    }
                }
            }
        }
        *pp = p;
        return 0;
    }

    /* Common palette/RAW/PATCH modes are dispatched to the small hot helpers. */
    return -2;
}

/* For map-safe frames, walk only set bits.  Low byte = first changed-cell
 * position (MSB-first), high byte = map byte with that bit removed. */
static const uint16_t mapwalk8_lut[256] = {
    0x0000u, 0x0007u, 0x0006u, 0x0106u, 0x0005u, 0x0105u, 0x0205u, 0x0305u,
    0x0004u, 0x0104u, 0x0204u, 0x0304u, 0x0404u, 0x0504u, 0x0604u, 0x0704u,
    0x0003u, 0x0103u, 0x0203u, 0x0303u, 0x0403u, 0x0503u, 0x0603u, 0x0703u,
    0x0803u, 0x0903u, 0x0a03u, 0x0b03u, 0x0c03u, 0x0d03u, 0x0e03u, 0x0f03u,
    0x0002u, 0x0102u, 0x0202u, 0x0302u, 0x0402u, 0x0502u, 0x0602u, 0x0702u,
    0x0802u, 0x0902u, 0x0a02u, 0x0b02u, 0x0c02u, 0x0d02u, 0x0e02u, 0x0f02u,
    0x1002u, 0x1102u, 0x1202u, 0x1302u, 0x1402u, 0x1502u, 0x1602u, 0x1702u,
    0x1802u, 0x1902u, 0x1a02u, 0x1b02u, 0x1c02u, 0x1d02u, 0x1e02u, 0x1f02u,
    0x0001u, 0x0101u, 0x0201u, 0x0301u, 0x0401u, 0x0501u, 0x0601u, 0x0701u,
    0x0801u, 0x0901u, 0x0a01u, 0x0b01u, 0x0c01u, 0x0d01u, 0x0e01u, 0x0f01u,
    0x1001u, 0x1101u, 0x1201u, 0x1301u, 0x1401u, 0x1501u, 0x1601u, 0x1701u,
    0x1801u, 0x1901u, 0x1a01u, 0x1b01u, 0x1c01u, 0x1d01u, 0x1e01u, 0x1f01u,
    0x2001u, 0x2101u, 0x2201u, 0x2301u, 0x2401u, 0x2501u, 0x2601u, 0x2701u,
    0x2801u, 0x2901u, 0x2a01u, 0x2b01u, 0x2c01u, 0x2d01u, 0x2e01u, 0x2f01u,
    0x3001u, 0x3101u, 0x3201u, 0x3301u, 0x3401u, 0x3501u, 0x3601u, 0x3701u,
    0x3801u, 0x3901u, 0x3a01u, 0x3b01u, 0x3c01u, 0x3d01u, 0x3e01u, 0x3f01u,
    0x0000u, 0x0100u, 0x0200u, 0x0300u, 0x0400u, 0x0500u, 0x0600u, 0x0700u,
    0x0800u, 0x0900u, 0x0a00u, 0x0b00u, 0x0c00u, 0x0d00u, 0x0e00u, 0x0f00u,
    0x1000u, 0x1100u, 0x1200u, 0x1300u, 0x1400u, 0x1500u, 0x1600u, 0x1700u,
    0x1800u, 0x1900u, 0x1a00u, 0x1b00u, 0x1c00u, 0x1d00u, 0x1e00u, 0x1f00u,
    0x2000u, 0x2100u, 0x2200u, 0x2300u, 0x2400u, 0x2500u, 0x2600u, 0x2700u,
    0x2800u, 0x2900u, 0x2a00u, 0x2b00u, 0x2c00u, 0x2d00u, 0x2e00u, 0x2f00u,
    0x3000u, 0x3100u, 0x3200u, 0x3300u, 0x3400u, 0x3500u, 0x3600u, 0x3700u,
    0x3800u, 0x3900u, 0x3a00u, 0x3b00u, 0x3c00u, 0x3d00u, 0x3e00u, 0x3f00u,
    0x4000u, 0x4100u, 0x4200u, 0x4300u, 0x4400u, 0x4500u, 0x4600u, 0x4700u,
    0x4800u, 0x4900u, 0x4a00u, 0x4b00u, 0x4c00u, 0x4d00u, 0x4e00u, 0x4f00u,
    0x5000u, 0x5100u, 0x5200u, 0x5300u, 0x5400u, 0x5500u, 0x5600u, 0x5700u,
    0x5800u, 0x5900u, 0x5a00u, 0x5b00u, 0x5c00u, 0x5d00u, 0x5e00u, 0x5f00u,
    0x6000u, 0x6100u, 0x6200u, 0x6300u, 0x6400u, 0x6500u, 0x6600u, 0x6700u,
    0x6800u, 0x6900u, 0x6a00u, 0x6b00u, 0x6c00u, 0x6d00u, 0x6e00u, 0x6f00u,
    0x7000u, 0x7100u, 0x7200u, 0x7300u, 0x7400u, 0x7500u, 0x7600u, 0x7700u,
    0x7800u, 0x7900u, 0x7a00u, 0x7b00u, 0x7c00u, 0x7d00u, 0x7e00u, 0x7f00u,
};

static int slice_head(const uint8_t *slice, size_t size,
                      uint16_t y0, uint8_t rows,
                      const uint8_t **map, const uint8_t **p,
                      const uint8_t **end, unsigned *tc, unsigned *cc)
{
    unsigned mb;
    if (!slice || size < 4u || rows == 0u || y0 + rows > SV24_TILES_Y)
        return -1;
    *tc = be16(slice);
    *cc = be16(slice + 2);
    if (*tc != (unsigned)rows * SV24_TILES_X || *cc > *tc)
        return -2;
    mb = (*tc + 7u) >> 3;
    if (size < 4u + mb) return -3;
    *map = slice + 4u;
    *p = *map + mb;
    *end = slice + size;
    return 0;
}

int sv24_decode_abs_slice(const uint8_t *slice, size_t size,
                          uint16_t y0, uint8_t rows,
                          sv24_slice_output_t *out)
{
    const uint8_t *map, *p, *end;
    unsigned tc, cc, ti = 0u, records = 0u, written = 0u, ry, tx;
    int r;

    if (!out) return -1;
    r = slice_head(slice, size, y0, rows, &map, &p, &end, &tc, &cc);
    if (r) return r;

    for (ry = 0u; ry < rows; ry++) {
        for (tx = 0u; tx < SV24_TILES_X; tx++, ti++) {
            unsigned span, j;
            uint8_t mode;
            if (!((map[ti >> 3] >> (7u - (ti & 7u))) & 1u)) continue;
            if (p >= end) return -4;
            mode = *p++;
            if (mode == MODE_HGRAD) return -5; /* compact output has no skipped-cell neighbor state */
            span = mode_span_cells(mode);
            if (tx + span > SV24_TILES_X || written + span > SV24_SLICE_MAX_TILES) return -4;
            r = decode_tile(&p, end, mode, &out->pixels[written * 64u], 0);
            if (r) return -5;
            for (j = 0u; j < span; j++)
                out->tile_id[written + j] = (uint16_t)((y0 + ry) * SV24_TILES_X + tx + j);
            written += span;
            records++;
        }
    }
    if (records != cc || p != end) return -6;
    out->count = (uint16_t)written;
    return 0;
}

static int motion_src_ok(unsigned id, int dx, int dy, int *sx, int *sy)
{
    int tx = (int)(id % SV24_TILES_X);
    int ty = (int)(id / SV24_TILES_X);
    int x = tx * 8 + dx;
    int y = ty * 8 + dy;
    if (x < 0 || y < 0 || x + 8 > 352 || y + 8 > 240) return 0;
    *sx = x; *sy = y; return 1;
}

static uint32_t avg_packed_rgb(uint32_t a, uint32_t b)
{
    return (a | b) - (((a ^ b) & 0xfefefefeu) >> 1);
}

/* The motion reference is intentionally kept in the same tile-major layout as
   staging/VDP2 character data: tile id * 64, then 8 rows of 8 pixels.  This
   lets the slave SH-2 update runs with long contiguous DMAC copies.  Arbitrary
   pixel motion only needs to stitch across a horizontal tile boundary. */
static void motion_ref_row_tilemajor(const uint32_t *ref, int sx, int sy, uint32_t *d)
{
    unsigned tx=(unsigned)sx>>3, px=(unsigned)sx&7u;
    unsigned ty=(unsigned)sy>>3, py=(unsigned)sy&7u;
    const uint32_t *a=ref+((ty*SV24_TILES_X+tx)*64u)+(py*8u)+px;
    unsigned n=8u-px, i;
    for(i=0u;i<n;i++) d[i]=a[i];
    if(n<8u){
        const uint32_t *b=ref+((ty*SV24_TILES_X+tx+1u)*64u)+(py*8u);
        for(i=n;i<8u;i++) d[i]=b[i-n];
    }
}

static int motion_copy_block(uint32_t *dst, const uint32_t *ref,
                             unsigned id, int dx, int dy)
{
    int sx,sy; unsigned y;
    if (!ref || !motion_src_ok(id,dx,dy,&sx,&sy)) return -1;

    /* Extremely common case: exact previous cell.  Keep this fully unrolled so
       GCC emits a compact load/store stream and no coordinate work per row. */
    if(dx==0 && dy==0){
        const uint32_t *s=ref+id*64u;
        for(y=0u;y<64u;y+=8u){
            dst[y+0u]=s[y+0u];dst[y+1u]=s[y+1u];dst[y+2u]=s[y+2u];dst[y+3u]=s[y+3u];
            dst[y+4u]=s[y+4u];dst[y+5u]=s[y+5u];dst[y+6u]=s[y+6u];dst[y+7u]=s[y+7u];
        }
        return 0;
    }

    /* Any motion landing on an 8x8 boundary is also one contiguous source
       character.  This covers the useful +/-8-pixel vectors cheaply. */
    if(((unsigned)sx&7u)==0u && ((unsigned)sy&7u)==0u){
        unsigned sid=((unsigned)sy>>3)*SV24_TILES_X+((unsigned)sx>>3);
        const uint32_t *s=ref+sid*64u;
        for(y=0u;y<64u;y+=8u){
            dst[y+0u]=s[y+0u];dst[y+1u]=s[y+1u];dst[y+2u]=s[y+2u];dst[y+3u]=s[y+3u];
            dst[y+4u]=s[y+4u];dst[y+5u]=s[y+5u];dst[y+6u]=s[y+6u];dst[y+7u]=s[y+7u];
        }
        return 0;
    }

    for(y=0u;y<8u;y++) motion_ref_row_tilemajor(ref,sx,sy+(int)y,dst+y*8u);
    return 0;
}

static int motion_compound_block(uint32_t *dst, const uint32_t *ref,
                                 unsigned id, int dx0, int dy0, int dx1, int dy1)
{
    int sx0,sy0,sx1,sy1; unsigned y,x; uint32_t arow[8],brow[8];
    if (!ref || !motion_src_ok(id,dx0,dy0,&sx0,&sy0) || !motion_src_ok(id,dx1,dy1,&sx1,&sy1)) return -1;
    for(y=0u;y<8u;y++) {
        uint32_t *d=dst+y*8u;
        motion_ref_row_tilemajor(ref,sx0,sy0+(int)y,arow);
        motion_ref_row_tilemajor(ref,sx1,sy1+(int)y,brow);
        for(x=0u;x<8u;x++) d[x]=avg_packed_rgb(arow[x],brow[x]);
    }
    return 0;
}

static int motion_add_delta(uint32_t *dst, int dr, int dg, int db)
{
    unsigned i; int r,g,b;
    for(i=0u;i<64u;i++) {
        uint32_t q=dst[i];
        r=(int)(q&255u)+dr; g=(int)((q>>8)&255u)+dg; b=(int)((q>>16)&255u)+db;
        dst[i]=vdp2_rgb(clamp8s(r),clamp8s(g),clamp8s(b));
    }
    return 0;
}

static int motion_patch(uint32_t *dst, const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p=*pp,*mask; unsigned row;
    if((size_t)(end-p)<8u)return -1;
    mask=p;p+=8u;
    for(row=0u;row<8u;row++){
        uint8_t q=mask[row]; unsigned x;
        for(x=0u;x<8u;x++) if(q&(uint8_t)(0x80u>>x)){
            if((size_t)(end-p)<3u)return -1;
            dst[row*8u+x]=vdp2_rgb(p[0],p[1],p[2]);p+=3u;
        }
    }
    *pp=p; return 0;
}


static const int8_t mcrvq16[16][3] = {
    {   0,   0,   0}, {  -2,  -3,  -3}, {   8,   8,   6}, { -17, -15, -11},
    {  21,  27,  26}, {  -9, -30, -46}, { -55, -51, -42}, {  85,  27, -25},
    {  62,  59,  51}, { -91, -21,  48}, {  11,  88, 116}, { -51, -88,-106},
    {-123,-107,  -5}, { 123, 108, -10}, { 119, 119, 111}, {-124,-123,-119}
};

static int motion_rvq16(uint32_t *dst, const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p=*pp; unsigned qy,qx;
    if ((size_t)(end-p) < 8u) return -1;
    for (qy=0u; qy<4u; qy++) {
        for (qx=0u; qx<4u; qx++) {
            unsigned qi=qy*4u+qx;
            unsigned ix=(qi&1u)?(unsigned)(p[qi>>1]&15u):(unsigned)(p[qi>>1]>>4);
            int dr=(int)mcrvq16[ix][0], dg=(int)mcrvq16[ix][1], db=(int)mcrvq16[ix][2];
            unsigned yy,xx;
            for (yy=0u; yy<2u; yy++) for (xx=0u; xx<2u; xx++) {
                unsigned pi=(qy*2u+yy)*8u + qx*2u+xx; uint32_t q=dst[pi];
                int r=(int)(q&255u)+dr, g=(int)((q>>8)&255u)+dg, b=(int)((q>>16)&255u)+db;
                dst[pi]=vdp2_rgb(clamp8s(r),clamp8s(g),clamp8s(b));
            }
        }
    }
    *pp=p+8u; return 0;
}

static inline uint32_t motion_respal_pixel(uint32_t q, const int8_t *c)
{
    int r=(int)(q&255u)+(int)c[0];
    int g=(int)((q>>8)&255u)+(int)c[1];
    int b=(int)((q>>16)&255u)+(int)c[2];
    return vdp2_rgb(clamp8s(r),clamp8s(g),clamp8s(b));
}
static int motion_respal2(uint32_t *dst, const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p=*pp,*sel; int8_t c[2][3]; unsigned y,x;
    if((size_t)(end-p)<14u)return -1;
    c[0][0]=(int8_t)p[0];c[0][1]=(int8_t)p[1];c[0][2]=(int8_t)p[2];
    c[1][0]=(int8_t)p[3];c[1][1]=(int8_t)p[4];c[1][2]=(int8_t)p[5];
    sel=p+6u;p=sel+8u;
    for(y=0u;y<8u;y++){unsigned q=sel[y];uint32_t*d=dst+y*8u;
        for(x=0u;x<8u;x++){unsigned ix=(q&0x80u)?1u:0u;q=(q<<1)&255u;d[x]=motion_respal_pixel(d[x],c[ix]);}}
    *pp=p;return 0;
}
static int motion_respal4(uint32_t *dst, const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p=*pp,*sel; int8_t c[4][3]; unsigned i,x;
    if((size_t)(end-p)<28u)return -1;
    for(i=0u;i<4u;i++){c[i][0]=(int8_t)p[i*3u];c[i][1]=(int8_t)p[i*3u+1u];c[i][2]=(int8_t)p[i*3u+2u];}
    sel=p+12u;p=sel+16u;
    for(i=0u;i<16u;i++){unsigned q=sel[i];uint32_t*d=dst+i*4u;
        for(x=0u;x<4u;x++){unsigned ix=(q>>6)&3u;q=(q<<2)&255u;d[x]=motion_respal_pixel(d[x],c[ix]);}}
    *pp=p;return 0;
}

static int decode_motion(const uint8_t **pp,const uint8_t *end,uint8_t mode,
                         uint32_t *dst,const uint32_t *ref,unsigned id,
                         int gdx,int gdy)
{
    const uint8_t *p=*pp; int dx=gdx,dy=gdy,r;
    if(mode==MODE_MC8 || mode==MODE_MC8_DELTA || mode==MODE_MC8_PATCH || mode==MODE_MCCOMP || mode==MODE_MCRES2 || mode==MODE_MCRES4 || mode==MODE_MCRVQ){
        if((size_t)(end-p)<2u)return -1; dx=(int8_t)p[0];dy=(int8_t)p[1];p+=2u;
    }
    if(mode==MODE_MCCOMP){
        int dx1,dy1;if((size_t)(end-p)<2u)return -1;dx1=(int8_t)p[0];dy1=(int8_t)p[1];p+=2u;
        r=motion_compound_block(dst,ref,id,dx,dy,dx1,dy1);if(r)return r;*pp=p;return 0;
    }
    if(mode==MODE_MCCOMP_G){
        int dx1,dy1;if((size_t)(end-p)<2u)return -1;dx1=(int8_t)p[0];dy1=(int8_t)p[1];p+=2u;
        r=motion_compound_block(dst,ref,id,gdx,gdy,dx1,dy1);if(r)return r;*pp=p;return 0;
    }
    r=motion_copy_block(dst,ref,id,dx,dy);if(r)return r;
    if(mode==MODE_MCG_DELTA || mode==MODE_MC8_DELTA){
        if((size_t)(end-p)<3u)return -1;
        motion_add_delta(dst,(int8_t)p[0],(int8_t)p[1],(int8_t)p[2]);p+=3u;
    } else if(mode==MODE_MCG_PATCH || mode==MODE_MC8_PATCH){
        r=motion_patch(dst,&p,end);if(r)return r;
    } else if(mode==MODE_MCRES2_G || mode==MODE_MCRES2){
        r=motion_respal2(dst,&p,end);if(r)return r;
    } else if(mode==MODE_MCRES4_G || mode==MODE_MCRES4){
        r=motion_respal4(dst,&p,end);if(r)return r;
    } else if(mode==MODE_MCRVQ_G || mode==MODE_MCRVQ){
        r=motion_rvq16(dst,&p,end);if(r)return r;
    }
    *pp=p;return 0;
}

static int is_motion_mode(uint8_t mode)
{
    return mode>=MODE_MCG && mode<=MODE_MCRVQ;
}

unsigned sv24_output_tiles_x=SV24_TILES_X;
unsigned sv24_output_tile_rows=SV24_TILES_Y;
static int __attribute__((noinline)) output_record(const uint8_t **p,const uint8_t *end,uint8_t mode,
                         uint32_t *frame,const uint32_t *ref,unsigned id,int gdx,int gdy)
{
    unsigned pitch=sv24_output_tiles_x;
    if(!pitch||pitch>SV24_TILES_X||is_motion_mode(mode)||mode_span_cells(mode)!=1u)return -8;
    unsigned x=id%SV24_TILES_X,y=id/SV24_TILES_X;
    /* Padding records still consume bytes. No adjacent/motion references
     * are allowed outside the compact surface. Each CPU has its own scratch. */
    uint32_t discard[64];
    if(x>=pitch)return decode_tile(p,end,mode,discard,NULL);
    uint32_t *dst=&frame[(y*pitch+x)*64u];
    return decode_tile(p,end,mode,dst,x?dst-64u:NULL);
}

static int decode_compact_core(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,
                               uint32_t *frame,sv24_slice_dma_plan_t *plan)
{
    const uint8_t *map,*p,*end;unsigned tc,cc,records=0;int r;
    if(!frame||y0+rows>sv24_output_tile_rows)return -1;
    r=slice_head(slice,size,y0,rows,&map,&p,&end,&tc,&cc);if(r)return r;
    if(plan){plan->y0=y0;plan->rows=rows;plan->reserved=0;
        for(unsigned y=0;y<3;y++)plan->row_lo[y]=plan->row_hi[y]=0;}
    for(unsigned t=0;t<tc;t++)if(map[t>>3]&(0x80u>>(t&7))) {
        if(p>=end)return -4;
        unsigned id=(unsigned)y0*44u+t;uint8_t mode=*p++;
        r=output_record(&p,end,mode,frame,NULL,id,0,0);if(r)return -6;
        if(plan){unsigned x=t%44u,y=t/44u;if(x<32u)plan->row_lo[y]|=1u<<x;else plan->row_hi[y]|=1u<<(x-32u);}
        records++;
    }
    return records==cc&&p==end?0:-7;
}

static int decode_frame_core(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,
                             uint32_t *frame,const uint32_t *ref,int gdx,int gdy)
{
    const uint8_t *map,*p,*end;unsigned tc,cc,records=0u,bi,mb,base_id;int r;
    if(!frame||y0+rows>sv24_output_tile_rows)return -1;r=slice_head(slice,size,y0,rows,&map,&p,&end,&tc,&cc);if(r)return r;
    mb=(tc+7u)>>3;base_id=(unsigned)y0*SV24_TILES_X;
    for(bi=0u;bi<mb;bi++){
        uint8_t q=map[bi];if(bi+1u==mb&&(tc&7u))q&=(uint8_t)(0xffu<<(8u-(tc&7u)));
        while(q){uint16_t w=mapwalk8_lut[q];unsigned pos=(unsigned)(w&7u);unsigned ti=(bi<<3)+pos;uint8_t mode;unsigned id;
            if(ti>=tc||p>=end)return -4;q=(uint8_t)(w>>8);mode=*p++;id=base_id+ti;
            if(id%SV24_TILES_X+mode_span_cells(mode)>SV24_TILES_X)return -8;
            r=is_motion_mode(mode)?decode_motion(&p,end,mode,&frame[id*64u],ref,id,gdx,gdy):decode_tile(&p,end,mode,&frame[id*64u],(id%SV24_TILES_X)?&frame[(id-1u)*64u]:0);
            if(r)return -6;records++;}
    }
    if(records!=cc||p!=end)return -7;return 0;
}

int sv24_decode_abs_slice_frame_motion(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,
                                       uint32_t *frame,const uint32_t *ref,int gdx,int gdy)
{return sv24_output_tiles_x==44u?decode_frame_core(slice,size,y0,rows,frame,ref,gdx,gdy):-8;}

int sv24_decode_abs_slice_frame(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,uint32_t *frame)
{return sv24_output_tiles_x==44u?decode_frame_core(slice,size,y0,rows,frame,0,0,0):decode_compact_core(slice,size,y0,rows,frame,NULL);}

static int decode_plan_core(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,
                            uint32_t *frame,sv24_slice_dma_plan_t *plan,unsigned max_gap_pixels,
                            const uint32_t *ref,int gdx,int gdy)
{
    const uint8_t *map,*p,*end,*map_next;unsigned tc,cc,ti=0u,records=0u,ry,tx;(void)max_gap_pixels;
    uint8_t map_byte,map_bit;int r;if(!frame||!plan||y0+rows>sv24_output_tile_rows)return -1;
    plan->y0=y0;plan->rows=rows;plan->reserved=0u;plan->row_lo[0]=plan->row_lo[1]=plan->row_lo[2]=0u;plan->row_hi[0]=plan->row_hi[1]=plan->row_hi[2]=0u;
    r=slice_head(slice,size,y0,rows,&map,&p,&end,&tc,&cc);if(r)return r;map_byte=*map;map_next=map+1u;map_bit=0x80u;
    for(ry=0u;ry<rows;ry++)for(tx=0u;tx<SV24_TILES_X;tx++,ti++){
        unsigned id,span;uint8_t mode,changed;
        if(map_bit==0x80u&&map_byte==0u&&tx+8u<=SV24_TILES_X&&ti+8u<=tc){tx+=7u;ti+=7u;if(ti+1u<tc)map_byte=*map_next++;continue;}
        changed=(uint8_t)(map_byte&map_bit);if(map_bit==1u){map_bit=0x80u;if(ti+1u<tc)map_byte=*map_next++;}else map_bit>>=1;
        if(!changed)continue;if(p>=end)return -4;id=(y0+ry)*SV24_TILES_X+tx;mode=*p++;span=mode_span_cells(mode);
        if(tx+span>SV24_TILES_X||(mode==MODE_HGRAD&&tx==0u))return -5;
        {unsigned pj;for(pj=0u;pj<span;pj++){unsigned cx=tx+pj;if(cx<32u)plan->row_lo[ry]|=1u<<cx;else plan->row_hi[ry]|=1u<<(cx-32u);}}
        r=is_motion_mode(mode)?decode_motion(&p,end,mode,&frame[id*64u],ref,id,gdx,gdy):decode_tile(&p,end,mode,&frame[id*64u],tx?&frame[(id-1u)*64u]:0);
        if(r)return -6;records++;
    }
    if(records!=cc||p!=end)return -7;return 0;
}

int sv24_decode_abs_slice_frame_plan_motion(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,
                                            uint32_t *frame,sv24_slice_dma_plan_t *plan,unsigned max_gap_pixels,
                                            const uint32_t *ref,int gdx,int gdy)
{return sv24_output_tiles_x==44u?decode_plan_core(slice,size,y0,rows,frame,plan,max_gap_pixels,ref,gdx,gdy):-8;}

int sv24_decode_abs_slice_frame_plan(const uint8_t *slice,size_t size,uint16_t y0,uint8_t rows,
                                     uint32_t *frame,sv24_slice_dma_plan_t *plan,unsigned max_gap_pixels)
{return sv24_output_tiles_x==44u?decode_plan_core(slice,size,y0,rows,frame,plan,max_gap_pixels,0,0,0):decode_compact_core(slice,size,y0,rows,frame,plan);}
