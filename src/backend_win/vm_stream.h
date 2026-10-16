#ifndef VM_STREAM_H
#define VM_STREAM_H

#include <windows.h>
#include "hcs_vm.h"

/* Moonlight viewer for VMs created with the native display option (see vm_stream.c). */

/* Called with the lock held, from any thread, when a VM's status line or state changes (launched
   FALSE), or when Moonlight is launched for it (launched TRUE). The GUI uses it to refresh the VM
   list (the Native column) and to close the raw viewer; it must not block or call back in here. */
typedef void (*VmStreamChangedFn)(UINT64 vm_id, BOOL launched);

void vm_stream_init(VmStreamChangedFn on_changed);
void vm_stream_shutdown(void);              /* ends the monitor thread and any running Moonlight */

/* Launches Moonlight full screen for vm, or brings an open one to the front. When the VM is not
   ready yet and queue is TRUE, Moonlight opens by itself once it is (pairing retries every
   OPEN_RETRY_MS meanwhile, for up to OPEN_RETRY_FOR_MS of failures); the queued open is dropped
   when the VM stops or vm_stream_cancel_open is called. */
typedef enum { VM_STREAM_NOT_OPENED = 0, VM_STREAM_OPENED, VM_STREAM_QUEUED } VmStreamOpen;
VmStreamOpen vm_stream_open(VmInstance *vm, BOOL queue);
void vm_stream_cancel_open(UINT64 vm_id);
BOOL vm_stream_is_open(UINT64 vm_id);
BOOL vm_stream_is_ready(UINT64 vm_id);
void vm_stream_close(UINT64 vm_id);

/* Where a native display VM's stream stands, as the GUI's Native column shows it. */
typedef enum {
    VM_STREAM_OFF = 0,      /* gray: the VM is stopped or still starting */
    VM_STREAM_PAIRING,      /* yellow: the VM is up; Moonlight is installing, waiting or pairing */
    VM_STREAM_READY,        /* green: paired, ready to stream or streaming */
    VM_STREAM_HEALING,      /* orange: recovering by itself (pairing again, retrying) */
    VM_STREAM_FAILED        /* red: failed; needs attention (see the log) */
} VmStreamState;

const char *vm_stream_state_name(VmStreamState state);   /* "off", "pairing", "ready", "healing", "failed" */

/* One line on where the VM's stream stands (installing, waiting for Sunshine, pairing, ready,
   streaming, or what failed), empty for a VM without the native display option; returns its state. */
VmStreamState vm_stream_status(UINT64 vm_id, wchar_t *out, size_t cch);

#endif
