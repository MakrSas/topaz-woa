/*
 * topaz-woa: EDID 1.4 for the Redmi Note 12 4G panel (m7 38 0c 0a, 1080x2400 DSI video
 * mode). Values from the DTB panel node: h fp/pw/bp 120/28/120, v fp/pw/bp 20/2/10,
 * 60 Hz timing (the panel also does 90/120 Hz), physical 69.5 x 154.6 mm.
 */
#include "BDD.hxx"

#pragma code_seg("PAGE")

VOID BuildTopazEdid(BYTE* e)
{
    PAGED_CODE();

    static const BYTE header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    const UINT hActive = 1080, hFp = 120, hSync = 28, hBp = 120;
    const UINT vActive = 2400, vFp = 20, vSync = 2, vBp = 10;
    const UINT hBlank = hFp + hSync + hBp, vBlank = vFp + vSync + vBp;
    const UINT clk10k = (hActive + hBlank) * (vActive + vBlank) * 60 / 10000;  /* pixel clock, 10 kHz units */
    const UINT wMm = 70, hMm = 155;
    BYTE* d;
    UINT i, sum = 0;

    RtlZeroMemory(e, EDID_V1_BLOCK_SIZE);
    RtlCopyMemory(e, header, sizeof(header));
    e[8] = 0x52; e[9] = 0x1A;            /* manufacturer "TPZ" */
    e[10] = 0x25; e[11] = 0x62;          /* product 0x6225 */
    e[16] = 1; e[17] = 2026 - 1990;      /* week, year */
    e[18] = 1; e[19] = 4;                /* EDID 1.4 */
    e[20] = 0xA0;                        /* digital, 8 bpc */
    e[21] = (BYTE)((wMm + 5) / 10);      /* cm */
    e[22] = (BYTE)((hMm + 5) / 10);
    e[23] = 0x78;                        /* gamma 2.2 */
    e[24] = 0x02;                        /* preferred timing is native */
    {
        static const BYTE chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
        RtlCopyMemory(&e[25], chroma, sizeof(chroma));
    }
    for (i = 38; i < 54; i++) {
        e[i] = 0x01;                     /* no standard timings */
    }

    d = &e[54];                          /* detailed timing descriptor */
    d[0] = (BYTE)(clk10k & 0xFF);
    d[1] = (BYTE)(clk10k >> 8);
    d[2] = (BYTE)(hActive & 0xFF);
    d[3] = (BYTE)(hBlank & 0xFF);
    d[4] = (BYTE)(((hActive >> 8) << 4) | (hBlank >> 8));
    d[5] = (BYTE)(vActive & 0xFF);
    d[6] = (BYTE)(vBlank & 0xFF);
    d[7] = (BYTE)(((vActive >> 8) << 4) | (vBlank >> 8));
    d[8] = (BYTE)(hFp & 0xFF);
    d[9] = (BYTE)(hSync & 0xFF);
    d[10] = (BYTE)(((vFp & 0xF) << 4) | (vSync & 0xF));
    d[11] = (BYTE)(((hFp >> 8) << 6) | ((hSync >> 8) << 4) | ((vFp >> 4) << 2) | (vSync >> 4));
    d[12] = (BYTE)(wMm & 0xFF);
    d[13] = (BYTE)(hMm & 0xFF);
    d[14] = (BYTE)(((wMm >> 8) << 4) | (hMm >> 8));
    d[17] = 0x1E;                        /* digital separate sync, +h +v */

    d = &e[72];                          /* monitor name */
    d[3] = 0xFC;
    {
        static const char name[13] = { 'T','o','p','a','z',' ','P','a','n','e','l','\n',' ' };
        RtlCopyMemory(&d[5], name, sizeof(name));
    }
    e[90 + 3] = 0x10;                    /* dummy descriptors */
    e[108 + 3] = 0x10;

    for (i = 0; i < 127; i++) {
        sum += e[i];
    }
    e[127] = (BYTE)(256 - (sum & 0xFF));
}
