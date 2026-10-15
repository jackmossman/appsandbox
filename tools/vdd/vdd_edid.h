/*++

Module Name:

    vdd_edid.h

Abstract:

    EDID for the AppSandbox virtual monitor. Kept free of WDK headers so the
    generator can be built and checked on its own (tools/vdd/test/edid_dump.cpp).

--*/

#pragma once

#include <windows.h>

#define VDD_EDID_MAX_SIZE   256     /* base block + one extension */

/* Writes the EDID for a width x height @ refresh monitor into out and returns its size
   (128 or 256). widthMm/heightMm set the physical size Windows uses for its scaling
   recommendation; 0 derives it from the mode at 96 dpi.

   The stock 1920x1080@60 mode without a physical size returns the EDID the driver has always
   reported, so existing VMs keep their monitor identity (saved layout, scaling). Any other
   mode gets a generated EDID whose serial string names the mode: a different monitor to
   Windows, which then applies the new mode instead of a layout saved for the old one. */
UINT VddBuildEdid(UINT width, UINT height, UINT refresh, UINT widthMm, UINT heightMm,
                  BYTE out[VDD_EDID_MAX_SIZE]);
