/* Converts a 2048-byte/sector ISO9660 image into a MODE1/2352 raw CD sector stream (sync, header, mode,
 * data, EDC, zeroed reserved bytes, P/Q L-EC parity), so it can be concatenated with the CD-DA tracks into
 * one bin, referenced by a single-FILE cue (tools/saturn/make_bincue.py). LBA 0 = MSF 00:02:00 (track starts
 * after the 2-second lead-in), matching every real Saturn/PS1 disc.
 *
 * The EDC/ECC math (Reed-Solomon P/Q parity over GF(256), poly 0x11D; EDC a CRC-32 variant, poly 0xD8018001)
 * is the standard CD-ROM Yellow Book L-EC layer. Verified byte-for-byte against a real Saturn CD dump's
 * MODE1/2352 sectors (Yumimi Mix Remix (English v1.0).bin, sectors 0-49, 4198-4202, 5000-5004, 130000-130004).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ecc_f_lut[256];
static uint8_t ecc_b_lut[256];
static uint32_t edc_lut[256];

static void eccedc_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
        ecc_f_lut[i] = (uint8_t)j;
        ecc_b_lut[i ^ j] = (uint8_t)i;
        uint32_t edc = i;
        for (int k = 0; k < 8; k++)
            edc = (edc >> 1) ^ ((edc & 1) ? 0xD8018001u : 0);
        edc_lut[i] = edc;
    }
}

static uint32_t edc_partial_computeblock(uint32_t edc, const uint8_t *src, size_t size) {
    while (size--)
        edc = edc_lut[(edc ^ *src++) & 0xFF] ^ (edc >> 8);
    return edc;
}

static void ecc_computeblock(uint8_t *address, uint32_t major_count, uint32_t minor_count,
                              uint32_t major_mult, uint32_t minor_inc, uint8_t *ecc) {
    uint32_t size = major_count * minor_count;
    for (uint32_t major = 0; major < major_count; major++) {
        uint32_t index = (major >> 1) * major_mult + (major & 1);
        uint8_t ecc_a = 0, ecc_b = 0;
        for (uint32_t minor = 0; minor < minor_count; minor++) {
            uint8_t temp = address[index];
            index += minor_inc;
            if (index >= size)
                index -= size;
            ecc_a ^= temp;
            ecc_b ^= temp;
            ecc_a = ecc_f_lut[ecc_a];
        }
        ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
        ecc[major] = ecc_a;
        ecc[major + major_count] = ecc_a ^ ecc_b;
    }
}

static void mode1_finish(uint8_t *sector) {
    uint32_t edc = edc_partial_computeblock(0, sector, 2064);
    sector[2064] = edc & 0xFF;
    sector[2065] = (edc >> 8) & 0xFF;
    sector[2066] = (edc >> 16) & 0xFF;
    sector[2067] = (edc >> 24) & 0xFF;
    memset(sector + 2068, 0, 8);
    ecc_computeblock(sector + 12, 86, 24, 2, 86, sector + 2076);   /* P parity */
    ecc_computeblock(sector + 12, 52, 43, 86, 88, sector + 2248);  /* Q parity */
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s in.iso out.bin\n", argv[0]);
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out) {
        perror("fopen");
        return 1;
    }
    eccedc_init();
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    uint8_t sector[2352], data[2048];
    uint32_t lba = 0;
    size_t n;
    while ((n = fread(data, 1, 2048, in)) > 0) {
        if (n < 2048)
            memset(data + n, 0, 2048 - n);
        memcpy(sector, sync, 12);
        uint32_t f = lba + 150; /* 2-second lead-in */
        unsigned m = f / 4500, s = (f / 75) % 60, fr = f % 75;
        sector[12] = (uint8_t)(((m / 10) << 4) | (m % 10));
        sector[13] = (uint8_t)(((s / 10) << 4) | (s % 10));
        sector[14] = (uint8_t)(((fr / 10) << 4) | (fr % 10));
        sector[15] = 0x01; /* Mode 1 */
        memcpy(sector + 16, data, 2048);
        mode1_finish(sector);
        if (fwrite(sector, 1, 2352, out) != 2352) {
            perror("fwrite");
            return 1;
        }
        lba++;
    }
    fclose(in);
    fclose(out);
    return 0;
}
