#ifndef STREAMING_H
#define STREAMING_H

#include <windows.h>

/* Native display streaming: Sunshine in the guest, streamed to Moonlight on the host.
   Turned on by HKLM\SOFTWARE\AppSandbox\Streaming (written at provisioning):
     Enabled        DWORD  1 = on
     AllowedSubnet  SZ     the only addresses Sunshine's firewall rule lets in (the NAT subnet)
     User, Password SZ     Sunshine web UI credentials for host-side pairing; the password is
                           applied at the next sign-in and then deleted from the registry */

typedef void (*StreamLogFn)(const char *fmt, ...);

/* Reads the configuration and, when enabled, starts the thread that installs Sunshine and keeps
   it running in the signed-in user's session. */
void streaming_start(StreamLogFn log);

/* Stops that thread and Sunshine (service stop and OS shutdown). */
void streaming_stop(void);

/* The host assigned the guest ip/prefix (set_ip). The NAT subnet can differ from the one written
   at provisioning (the host falls back to another /24 when one clashes), so the firewall rule
   follows it. */
void streaming_network_changed(const char *ip, int prefix);

#endif
