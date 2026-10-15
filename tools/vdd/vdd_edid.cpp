/*++

Module Name:

    vdd_edid.cpp

Abstract:

    EDID generation for the AppSandbox virtual monitor (see vdd_edid.h).

    IddCx takes the monitor's modes from the driver (EvtIddCxParseMonitorDescription), not
    from the EDID. Windows uses the EDID for the monitor's identity (the key of its saved
    display layout), its name and its physical size.

    A base-block detailed timing holds at most 4095 active pixels and a 655.35 MHz pixel
    clock, so a mode beyond that goes in a DisplayID 1.3 extension block (Type I timing),
    as on real 5K monitors; the base block then carries 1920x1080@60.

--*/

#include "vdd_edid.h"

#include <string.h>
#include <stdio.h>

/* EDID 1.4 base block for the stock mode, as reported by every driver before modes became
   configurable. Manufacturer "ASB", 1920x1080@60, 530x300 mm. Byte 127 is the checksum,
   filled in by VddBuildEdid. */
static const BYTE g_StockEdid[128] = {
    /* Header (bytes 0-7) */
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,

    /* Manufacturer "ASB" = 0x06 0x62  (5-bit packed: A=1,S=19,B=2) */
    /* Product code 0x0001, serial 0x00000001 */
    0x06, 0x62, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,

    /* Week 1, year 2024 (offset from 1990 = 34 = 0x22) */
    0x01, 0x22,

    /* EDID version 1.4 */
    0x01, 0x04,

    /* Digital input, 8-bit color depth, DisplayPort */
    0xA5,

    /* 53cm x 30cm (approx 24" diagonal for 1920x1080) */
    0x35, 0x1E,

    /* Gamma 2.2 (value = (gamma*100)-100 = 120 = 0x78) */
    0x78,

    /* Supported features: RGB color, preferred timing in DTD1 */
    0x22,

    /* Chromaticity coordinates (standard sRGB-ish values) */
    0xFC, 0x81, 0xA4, 0x55, 0x4D, 0x9D, 0x25, 0x12, 0x50, 0x54,

    /* Established timings */
    0x00, 0x00, 0x00,

    /* Standard timings (unused, filled with 0x01 0x01) */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,

    /* Detailed Timing Descriptor #1: 1920x1080 @ 60Hz */
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40,
    0x58, 0x2C, 0x45, 0x00, 0x12, 0x2C, 0x21, 0x00,
    0x00, 0x1E,

    /* Descriptor #2: Monitor name "AppSandboxVDD" */
    0x00, 0x00, 0x00, 0xFC, 0x00,
    'A', 'p', 'p', 'S', 'a', 'n', 'd', 'b', 'o', 'x', 'V', 'D', 'D',

    /* Descriptor #3: Monitor range limits */
    0x00, 0x00, 0x00, 0xFD, 0x00,
    0x38, 0x4C, 0x1E, 0x51, 0x11, 0x00, 0x0A,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20,

    /* Descriptor #4: Dummy (zero) */
    0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,

    /* Extension block count */
    0x00,

    /* Checksum placeholder - computed at runtime */
    0x00
};

#define EDID_DTD1       54
#define EDID_DESC2      72
#define EDID_DESC3      90
#define EDID_DESC4      108

typedef struct _VDD_TIMING {
    UINT    hActive, hBlank, hFront, hSync;
    UINT    vActive, vBlank, vFront, vSync;
    UINT    refresh;
    UINT64  pixelClockHz;
} VDD_TIMING;

/* Reduced-blanking timing, CVT-RB v2 style: an 80-pixel horizontal blank and at least
   460 us of vertical blank. The front porch is held at 3 lines and the rest goes to the
   back porch, so the timing always fits a base-block descriptor's 6-bit porch field. */
static void VddReducedBlanking(UINT width, UINT height, UINT refresh, VDD_TIMING* t)
{
    double linePeriodUs = (1000000.0 / refresh - 460.0) / height;
    UINT vBlank = (UINT)(460.0 / linePeriodUs) + 1;
    if (vBlank < 3 + 8 + 6) vBlank = 3 + 8 + 6;     /* front porch + sync + minimum back porch */

    t->hActive = width;  t->hBlank = 80;     t->hFront = 8; t->hSync = 32;
    t->vActive = height; t->vBlank = vBlank; t->vFront = 3; t->vSync = 8;
    t->refresh = refresh;
    t->pixelClockHz = (UINT64)(width + t->hBlank) * (height + vBlank) * refresh;
}

static UINT VddClock10kHz(const VDD_TIMING* t)
{
    return (UINT)((t->pixelClockHz + 9999) / 10000);
}

static BOOL VddFitsBaseDtd(const VDD_TIMING* t)
{
    return t->hActive <= 4095 && t->vActive <= 4095 && t->hBlank <= 4095 && t->vBlank <= 4095 &&
           VddClock10kHz(t) <= 0xFFFF;
}

/* 18-byte detailed timing descriptor. Digital separate sync, hsync +, vsync - (CVT-RB). */
static void VddPutDtd(BYTE* d, const VDD_TIMING* t, UINT widthMm, UINT heightMm)
{
    UINT clk = VddClock10kHz(t);
    d[0]  = (BYTE)clk;
    d[1]  = (BYTE)(clk >> 8);
    d[2]  = (BYTE)t->hActive;
    d[3]  = (BYTE)t->hBlank;
    d[4]  = (BYTE)(((t->hActive >> 8) << 4) | (t->hBlank >> 8));
    d[5]  = (BYTE)t->vActive;
    d[6]  = (BYTE)t->vBlank;
    d[7]  = (BYTE)(((t->vActive >> 8) << 4) | (t->vBlank >> 8));
    d[8]  = (BYTE)t->hFront;
    d[9]  = (BYTE)t->hSync;
    d[10] = (BYTE)(((t->vFront & 0xF) << 4) | (t->vSync & 0xF));
    d[11] = (BYTE)(((t->hFront >> 8) << 6) | ((t->hSync >> 8) << 4) |
                   ((t->vFront >> 4) << 2) | (t->vSync >> 4));
    d[12] = (BYTE)widthMm;
    d[13] = (BYTE)heightMm;
    d[14] = (BYTE)(((widthMm >> 8) << 4) | (heightMm >> 8));
    d[15] = 0;
    d[16] = 0;
    d[17] = 0x1A;
}

/* Display descriptor carrying up to 13 characters of text (name 0xFC, serial 0xFF). */
static void VddPutText(BYTE* d, BYTE tag, const char* text)
{
    size_t n = strlen(text);
    if (n > 13) n = 13;
    memset(d, 0, 5);
    d[3] = tag;
    memcpy(d + 5, text, n);
    if (n < 13) {
        d[5 + n] = 0x0A;
        memset(d + 6 + n, 0x20, 13 - n - 1);
    }
}

/* Range limits covering every timing in the EDID. Horizontal rates above 255 kHz use the
   EDID 1.4 +255 offset flags. */
static void VddPutRangeLimits(BYTE* d, const VDD_TIMING* timings, UINT count)
{
    UINT vMin = 255, vMax = 0, hMin = 0xFFFF, hMax = 0, clkMax = 0;
    for (UINT i = 0; i < count; i++) {
        const VDD_TIMING* t = &timings[i];
        UINT64 hz = t->pixelClockHz / (t->hActive + t->hBlank);
        UINT hLo = (UINT)(hz / 1000), hHi = (UINT)((hz + 999) / 1000);
        UINT clk = (UINT)((t->pixelClockHz + 9999999) / 10000000);
        if (t->refresh < vMin) vMin = t->refresh;
        if (t->refresh > vMax) vMax = t->refresh;
        if (hLo < hMin) hMin = hLo;
        if (hHi > hMax) hMax = hHi;
        if (clk > clkMax) clkMax = clk;
    }
    if (hMax > 510) hMax = 510;
    if (hMin > hMax) hMin = hMax;
    if (clkMax > 255) clkMax = 255;

    BYTE flags = 0;
    if (hMax > 255) { flags |= 0x08; hMax -= 255; }
    if (hMin > 255) { flags |= 0x04; hMin -= 255; }

    memset(d, 0, 5);
    d[3]  = 0xFD;
    d[4]  = flags;
    d[5]  = (BYTE)vMin;
    d[6]  = (BYTE)vMax;
    d[7]  = (BYTE)hMin;
    d[8]  = (BYTE)hMax;
    d[9]  = (BYTE)clkMax;
    d[10] = 0x01;               /* range limits only, no timing formula */
    d[11] = 0x0A;
    memset(d + 12, 0x20, 6);
}

/* DisplayID 1.3 aspect-ratio code for a Type I timing; 8 = calculate from the active size. */
static BYTE VddAspectCode(UINT w, UINT h)
{
    static const UINT ratios[][2] = {
        { 1, 1 }, { 5, 4 }, { 4, 3 }, { 15, 9 }, { 16, 9 }, { 16, 10 }, { 64, 27 }, { 256, 135 },
    };
    for (BYTE i = 0; i < ARRAYSIZE(ratios); i++)
        if ((UINT64)w * ratios[i][1] == (UINT64)h * ratios[i][0])
            return i;
    return 8;
}

static void VddPut16(BYTE* d, UINT v) { d[0] = (BYTE)v; d[1] = (BYTE)(v >> 8); }

/* EDID extension block holding a DisplayID 1.3 section for a standalone display: product
   identification (the base block's vendor, product and name), display parameters, the
   interface, and the mode as the one preferred Type I timing. */
static void VddPutDisplayIdExtension(BYTE* e, const VDD_TIMING* t, UINT widthMm, UINT heightMm)
{
    static const char name[] = "AppSandboxVDD";
    UINT clk = VddClock10kHz(t) - 1;
    BYTE* b = e + 5;            /* current data block */

    memset(e, 0, 128);
    e[0] = 0x70;                /* EDID extension tag: DisplayID */
    e[1] = 0x13;                /* DisplayID 1.3 */
    e[3] = 0x03;                /* product type: standalone display */
    e[4] = 0x00;                /* extension count */

    b[0]  = 0x00;               /* product identification */
    b[2]  = (BYTE)(12 + sizeof(name) - 1);
    b[3]  = 'A'; b[4] = 'S'; b[5] = 'B';
    b[6]  = 0x01;               /* product code 1, serial 1, week 1 of 2024: as the base block */
    b[8]  = 0x01;
    b[12] = 1;
    b[13] = 24;
    b[14] = (BYTE)(sizeof(name) - 1);
    memcpy(b + 15, name, sizeof(name) - 1);
    b += 3 + b[2];

    b[0]  = 0x01;               /* display parameters */
    b[2]  = 12;
    VddPut16(b + 3, widthMm * 10);                                  /* 0.1 mm */
    VddPut16(b + 5, heightMm * 10);
    VddPut16(b + 7, t->hActive);
    VddPut16(b + 9, t->vActive);
    b[12] = 0x78;               /* gamma 2.2 */
    {
        /* aspect ratio, long axis over short, capped at the byte's 3.55 */
        UINT lng = t->hActive > t->vActive ? t->hActive : t->vActive;
        UINT shrt = t->hActive > t->vActive ? t->vActive : t->hActive;
        UINT ratio = (lng * 100 + shrt / 2) / shrt;
        b[13] = (BYTE)((ratio > 355 ? 355 : ratio) - 100);
    }
    b[14] = 0x77;               /* 8 bpc native and overall */
    b += 3 + b[2];

    b[0]  = 0x0F;               /* display interface */
    b[2]  = 10;
    b[3]  = 0xA1;               /* DisplayPort, 1 link, as the base block */
    b[4]  = 0x14;               /* DisplayPort 1.4 */
    b[5]  = 0x02;               /* RGB 8 bpc */
    b += 3 + b[2];

    b[0] = 0x03;                /* Type I detailed timing */
    b[2] = 20;
    b[3] = (BYTE)clk;
    b[4] = (BYTE)(clk >> 8);
    b[5] = (BYTE)(clk >> 16);
    b[6] = (BYTE)(0x80 | VddAspectCode(t->hActive, t->vActive));   /* preferred */
    VddPut16(b + 7,  t->hActive - 1);
    VddPut16(b + 9,  t->hBlank - 1);
    VddPut16(b + 11, (t->hFront - 1) | 0x8000);                     /* hsync + */
    VddPut16(b + 13, t->hSync - 1);
    VddPut16(b + 15, t->vActive - 1);
    VddPut16(b + 17, t->vBlank - 1);
    VddPut16(b + 19, t->vFront - 1);                                /* vsync - */
    VddPut16(b + 21, t->vSync - 1);
    b += 3 + b[2];

    e[2] = (BYTE)(b - (e + 5));                 /* section payload: the data blocks */
    BYTE sum = 0;
    for (BYTE* i = e + 1; i < b; i++) sum += *i;
    *b = (BYTE)(0x100 - sum);
}

static void VddSetChecksum(BYTE* block)
{
    BYTE sum = 0;
    for (int i = 0; i < 127; i++) sum += block[i];
    block[127] = (BYTE)(0x100 - sum);
}

UINT VddBuildEdid(UINT width, UINT height, UINT refresh, UINT widthMm, UINT heightMm,
                  BYTE out[VDD_EDID_MAX_SIZE])
{
    memcpy(out, g_StockEdid, 128);

    if (width == 1920 && height == 1080 && refresh == 60 && widthMm == 0 && heightMm == 0) {
        VddSetChecksum(out);
        return 128;
    }

    if (widthMm == 0 || heightMm == 0) {
        widthMm  = (width  * 254 + 480) / 960;
        heightMm = (height * 254 + 480) / 960;
    }

    VDD_TIMING timings[2];
    UINT count = 0;
    VddReducedBlanking(width, height, refresh, &timings[count++]);
    BOOL native = VddFitsBaseDtd(&timings[0]);
    if (!native)
        VddReducedBlanking(1920, 1080, 60, &timings[count++]);

    out[21] = (BYTE)((widthMm  + 5) / 10);      /* cm */
    out[22] = (BYTE)((heightMm + 5) / 10);
    out[24] = native ? 0x22 : 0x20;             /* bit 1: DTD1 is the native mode */
    VddPutDtd(out + EDID_DTD1, &timings[count - 1], widthMm, heightMm);
    VddPutRangeLimits(out + EDID_DESC3, timings, count);

    char serial[16];
    sprintf_s(serial, sizeof(serial), "%ux%u@%u", width, height, refresh);
    VddPutText(out + EDID_DESC4, 0xFF, serial);

    if (native) {
        out[126] = 0;
        VddSetChecksum(out);
        return 128;
    }

    out[126] = 1;
    VddSetChecksum(out);
    VddPutDisplayIdExtension(out + 128, &timings[0], widthMm, heightMm);
    VddSetChecksum(out + 128);
    return 256;
}
