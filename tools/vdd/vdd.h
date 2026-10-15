/*++

Module Name:

    vdd.h

Abstract:

    AppSandbox Virtual Display Driver - single monitor with direct HvSocket
    frame transport to the host (no agent middleman).

    One mode, read from the registry when the device starts (stock
    1920x1080@60 when unset), with framebuffer readback sent directly over
    Hyper-V sockets.

Environment:

    Windows User-Mode Driver Framework 2

--*/

#pragma once

#include <winsock2.h>   /* must precede windows.h */
#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <IddCx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>

#include "Trace.h"
#include "vdd_edid.h"
#include "../transport/asb_transport.h"   /* AsbConn — host connection (AF_HYPERV on PC, ivshmem on Mac) */

/* ============================================================================
 *  Display mode
 *
 *  Read once per device start from HKLM\SOFTWARE\AppSandbox\VDD, DWORD values:
 *    Width, Height, RefreshRate   the single mode offered (all three, or none)
 *    WidthMm, HeightMm            optional physical size for the EDID (DPI scaling)
 *  Unset or out of range -> stock 1920x1080@60. The value has to exist before the
 *  device is first installed, so it lives in a fixed key rather than the device's
 *  own registry key. Restart the device to apply a change.
 * ============================================================================ */
#define VDD_REG_KEY          L"SOFTWARE\\AppSandbox\\VDD"
#define VDD_DEFAULT_WIDTH    1920
#define VDD_DEFAULT_HEIGHT   1080
#define VDD_DEFAULT_REFRESH  60
#define VDD_MIN_WIDTH        640
#define VDD_MIN_HEIGHT       480
#define VDD_MAX_DIMENSION    8192
#define VDD_MAX_PIXELS       (5120 * 2160)  /* host viewer's frame buffer (vm_display_idd.c MAX_FRAME_DATA_SIZE) */
#define VDD_MIN_REFRESH      30
#define VDD_MAX_REFRESH      240
#define VDD_BPP              4          /* BGRA 8-bit */
#define VDD_MAX_DIRTY_RECTS  64

typedef struct _VDD_MODE {
    UINT    width;
    UINT    height;
    UINT    refresh;
    UINT    stride;             /* width * VDD_BPP, tight rows on the wire */
    SIZE_T  pixelBytes;         /* stride * height */
    UINT    widthMm;            /* EDID physical size; 0 = derived from the mode */
    UINT    heightMm;
} VDD_MODE;

/* ============================================================================
 *  Wire protocol: ASFR frame header (sent over HvSocket to host)
 *  Must match FrameHeader in vm_display_idd.c
 * ============================================================================ */
#define VDD_FRAME_MAGIC     0x52465341  /* "ASFR" little-endian */

#pragma pack(push, 1)
typedef struct _VDD_WIRE_FRAME_HEADER {
    UINT32  magic;              /* VDD_FRAME_MAGIC */
    UINT32  width;
    UINT32  height;
    UINT32  stride;
    UINT64  frame_seq;
    UINT32  dirty_rect_count;   /* 0 = full frame */
} VDD_WIRE_FRAME_HEADER;

/* Wire protocol: ASCR cursor update (sent over HvSocket to host)
 * Must match CursorHeader in vm_display_idd.c */
#define VDD_CURSOR_MAGIC    0x52435341  /* "ASCR" little-endian */

typedef struct _VDD_WIRE_CURSOR_HEADER {
    UINT32  magic;              /* VDD_CURSOR_MAGIC */
    INT32   x;                  /* cursor position X (can be negative) */
    INT32   y;                  /* cursor position Y (can be negative) */
    UINT32  visible;            /* 0 = hidden, 1 = visible */
    UINT32  shape_updated;      /* 0 = position only, 1 = shape data follows */
    UINT32  shape_id;           /* monotonic shape identifier */
    UINT32  width;              /* cursor width in pixels */
    UINT32  height;             /* cursor height in pixels */
    UINT32  pitch;              /* row pitch in bytes */
    UINT32  xhot;               /* hotspot X */
    UINT32  yhot;               /* hotspot Y */
    UINT32  cursor_type;        /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32  shape_data_size;    /* bytes of shape data following header (0 if !shape_updated) */
} VDD_WIRE_CURSOR_HEADER;
#pragma pack(pop)

/* ============================================================================
 *  Swap chain processor state (heap-allocated per AssignSwapChain)
 * ============================================================================ */
typedef struct _VDD_SWAP_PROC {
    IDDCX_SWAPCHAIN     hSwapChain;
    HANDLE              hAvailableEvent;    /* from pInArgs->hNextSurfaceAvailable */
    HANDLE              hTerminateEvent;
    HANDLE              hThread;

    /* Non-owning pointers to cached D3D11 device (owned by VDD_DEVICE_CONTEXT).
       The device is cached across swap chain transitions to avoid destroying it
       during IddCx teardown to avoid re-creation overhead. */
    ID3D11Device*       pDevice;
    ID3D11DeviceContext* pDeviceContext;

    /* Staging texture for CPU readback (owned, per-swap-chain) */
    ID3D11Texture2D*    pStagingTex;

    /* Host connection via asb_transport — AF_HYPERV HvSocket on a Windows host, ivshmem stream
       slot on a macOS host. One emit path, two backends. */
    HANDLE              hNetworkThread;     /* listener/accept thread */
    AsbConn * volatile  hClientConn;        /* active client, owned by swap chain thread */
    AsbConn * volatile  hPendingConn;       /* new client from network thread, picked up by swap chain */
    volatile BOOL       bStopNetwork;
    UINT64              frameSeq;

    /* Hardware cursor */
    IDDCX_MONITOR       hMonitor;           /* needed for QueryHardwareCursor */
    HANDLE              hCursorEvent;       /* signaled when cursor data changes */
    PBYTE               pCursorShapeBuffer; /* buffer for cursor bitmap */
    UINT                cursorBufSize;      /* size of pCursorShapeBuffer (2x for MASKED_COLOR) */
    DWORD               lastShapeId;        /* last received shape ID */

    /* Decoupled sending. The swap chain thread copies each frame into pendBuf and hands the
       surface back to IddCx at once; the send thread sends the newest pending frame.
       connLock serializes every write to (and close of) hClientConn: frames, cursor updates. */
    SRWLOCK             connLock;
    SRWLOCK             pendLock;           /* guards pend* and the pendBuf/sendBuf swap */
    HANDLE              hSendEvent;         /* auto-reset: a frame is pending */
    HANDLE              hSendThread;
    volatile BOOL       bStopSend;
    volatile LONG       cursorPending;      /* set by the swap chain thread, sent by the send thread */
    PBYTE               pendBuf;            /* g_mode.pixelBytes, tight rows */
    PBYTE               sendBuf;            /* g_mode.pixelBytes, owned by the send thread */
    PBYTE               packBuf;            /* g_mode.pixelBytes, dirty rects packed for one write */
    RECT                pendRects[VDD_MAX_DIRTY_RECTS];
    UINT                pendRectCount;
    BOOL                pendFull;
    BOOL                pendValid;
} VDD_SWAP_PROC;

/* ============================================================================
 *  Device context (heap-allocated, stored via IndirectDeviceContextWrapper)
 * ============================================================================ */
typedef struct _VDD_DEVICE_CONTEXT {
    WDFDEVICE           wdfDevice;
    IDDCX_ADAPTER       hAdapter;
    IDDCX_MONITOR       hMonitor;
    VDD_SWAP_PROC*      pSwapProc;          /* Active swap chain processor */

    /* Cached D3D11 device — persists across swap chain transitions.
       Kept alive here so that WdfObjectDelete's IddCx teardown never triggers
       destruction of the D3D device (which crashes WUDFHost.exe). */
    ID3D11Device*       pCachedDevice;
    ID3D11DeviceContext* pCachedCtx;
    LUID                cachedDeviceLuid;

    /* Recovery: if no AssignSwapChain arrives within 5s of Unassign,
       depart and re-arrive the monitor to force DWM re-engagement.
       Uses a WDF timer so the callback runs on a proper WDF thread
       (IddCx APIs crash when called from raw CreateThread threads). */
    WDFTIMER            hRecoveryTimer;
} VDD_DEVICE_CONTEXT;

/* ============================================================================
 *  WDF Context Wrappers
 * ============================================================================ */

struct IndirectDeviceContextWrapper
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

struct MonitorContextWrapper
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

struct RecoveryTimerContext
{
    VDD_DEVICE_CONTEXT* pContext;
};

WDF_DECLARE_CONTEXT_TYPE(RecoveryTimerContext);

/* ============================================================================
 *  IddCx Callback declarations
 * ============================================================================ */

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD           VddDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY            VddDeviceD0Entry;
EVT_WDF_DRIVER_UNLOAD              VddDriverUnload;

EVT_IDD_CX_ADAPTER_INIT_FINISHED                   VddAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES                    VddAdapterCommitModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2                   VddAdapterCommitModes2;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION               VddParseMonitorDescription;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2              VddParseMonitorDescription2;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES   VddMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES              VddMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2             VddMonitorQueryTargetModes2;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN                VddMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN              VddMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO               VddAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA        VddMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP                  VddMonitorSetGammaRamp;
