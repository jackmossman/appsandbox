/*++

Module Name:

    vdd_modes.h

Abstract:

    The display modes the AppSandbox VDD offers, shared by the driver (its monitor mode list)
    and the host's raw viewer (resize to fit). Windows only switches to modes in the monitor's
    list, which is fixed when the monitor arrives, so the list is generated from the native mode:
    native first, then the native aspect, 16:9, 16:10 and 21:9 at every 120 px of height from 480
    (widths rounded down to multiples of 8), all within native. Plain C, no dependencies.

--*/

#ifndef VDD_MODES_H
#define VDD_MODES_H

#define VDD_MODES_MAX       96
#define VDD_MODES_MIN_W     640
#define VDD_MODES_MIN_H     480
#define VDD_MODES_STEP_H    120

/* Fills w[]/h[] (VDD_MODES_MAX entries) with the modes for a native_w x native_h display, native
   first; returns the count. */
static __inline unsigned vdd_modes(unsigned native_w, unsigned native_h, unsigned *w, unsigned *h)
{
    const unsigned aspects[4][2] = { { native_w, native_h }, { 16, 9 }, { 16, 10 }, { 21, 9 } };
    unsigned n = 0, a, height, i;

    w[n] = native_w; h[n] = native_h; n++;
    for (a = 0; a < 4; a++) {
        for (height = VDD_MODES_MIN_H; height <= native_h; height += VDD_MODES_STEP_H) {
            unsigned width = (unsigned)((unsigned long long)height * aspects[a][0] / aspects[a][1]) & ~7u;
            int dup = 0;
            if (width < VDD_MODES_MIN_W || width > native_w) continue;
            for (i = 0; i < n; i++)
                if (w[i] == width && h[i] == height) { dup = 1; break; }
            if (dup || n >= VDD_MODES_MAX) continue;
            w[n] = width; h[n] = height; n++;
        }
    }
    return n;
}

/* The largest offered mode (by area) that fits in avail_w x avail_h; the smallest mode when
   none fits. */
static __inline void vdd_fit_mode(unsigned native_w, unsigned native_h, unsigned avail_w, unsigned avail_h,
                         unsigned *out_w, unsigned *out_h)
{
    unsigned w[VDD_MODES_MAX], h[VDD_MODES_MAX], n = vdd_modes(native_w, native_h, w, h), i;
    unsigned long long best = 0, smallest = ~0ull;
    *out_w = w[0]; *out_h = h[0];
    for (i = 0; i < n; i++) {
        unsigned long long area = (unsigned long long)w[i] * h[i];
        if (w[i] <= avail_w && h[i] <= avail_h && area > best) {
            best = area; *out_w = w[i]; *out_h = h[i];
        }
    }
    if (best) return;
    for (i = 0; i < n; i++) {
        unsigned long long area = (unsigned long long)w[i] * h[i];
        if (area < smallest) { smallest = area; *out_w = w[i]; *out_h = h[i]; }
    }
}

/* Is width x height one of the offered modes? */
static __inline int vdd_is_mode(unsigned native_w, unsigned native_h, unsigned width, unsigned height)
{
    unsigned w[VDD_MODES_MAX], h[VDD_MODES_MAX], n = vdd_modes(native_w, native_h, w, h), i;
    for (i = 0; i < n; i++)
        if (w[i] == width && h[i] == height) return 1;
    return 0;
}

#endif
