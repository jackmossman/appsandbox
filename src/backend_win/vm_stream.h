#ifndef VM_STREAM_H
#define VM_STREAM_H

#include <windows.h>
#include "hcs_vm.h"

/* Moonlight viewer for VMs created with the native display option (see vm_stream.c). */

/* Called from the monitor thread when a VM becomes ready to stream (Moonlight installed and paired
   with its Sunshine). The GUI uses it to switch an open raw viewer over to Moonlight. */
typedef void (*VmStreamReadyFn)(UINT64 vm_id);

void vm_stream_init(VmStreamReadyFn on_ready);
void vm_stream_shutdown(void);              /* ends the monitor thread and any running Moonlight */

/* Launches Moonlight full screen for vm, or brings an open one to the front. FALSE when the VM
   is not a native display VM or not ready yet: the caller opens the raw viewer instead. */
BOOL vm_stream_open(VmInstance *vm);
BOOL vm_stream_is_open(UINT64 vm_id);
BOOL vm_stream_is_ready(UINT64 vm_id);
void vm_stream_close(UINT64 vm_id);

/* One line on where the VM's stream stands (installing, waiting for Sunshine, pairing, ready,
   streaming, or what failed); empty for a VM without the native display option. */
void vm_stream_status(UINT64 vm_id, wchar_t *out, size_t cch);

#endif
