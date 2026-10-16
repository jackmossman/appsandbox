/*
 * appsandbox-displays.exe — Tiny helper that enumerates active monitors
 * and prints count + resolutions to stdout, then exits.
 *
 * Output format (single line):
 *   <count>,<W>x<H>,<W>x<H>,...
 *
 * Example:
 *   2,1920x1080,1024x768
 *
 * With --layout [WxH] it instead makes the App Sandbox display the only
 * active display, primary, at WxH (the raw viewer's size, which the driver
 * must be offering) or else its largest mode, and prints one line:
 *   ok <W>x<H>@<Hz>          already laid out, nothing changed
 *   changed <W>x<H>@<Hz>     laid out now
 *   error <reason>
 * Used by the native display option: a new mode gives the monitor a new
 * identity, and Windows lays a new identity out with its default extended
 * topology, which re-attaches the basic display as primary.
 *
 * Must be launched in the interactive session (WinSta0\Default) so that
 * EnumDisplayDevices / EnumDisplaySettings see all displays.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define VDD_ADAPTER_NAME L"App Sandbox Virtual Display"

static int list_displays(void)
{
    DISPLAY_DEVICEW dd;
    DEVMODEW dm;
    char buf[512];
    int count = 0;
    int off = 0;
    DWORD i;

    buf[0] = '\0';

    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);

    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
            dd.cb = sizeof(dd);
            continue;
        }

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);

        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
            if (count > 0 && off < (int)sizeof(buf) - 1) {
                buf[off++] = ',';
                buf[off] = '\0';
            }
            if (off < (int)sizeof(buf) - 1) {
                int n = snprintf(buf + off, sizeof(buf) - (size_t)off, "%lux%lu",
                                 dm.dmPelsWidth, dm.dmPelsHeight);
                if (n < 0) break;
                off += n;
                if (off >= (int)sizeof(buf)) {
                    off = (int)sizeof(buf) - 1;
                    buf[off] = '\0';
                    break;
                }
            }
        }
        count++;

        dd.cb = sizeof(dd);
    }

    printf("%d,%s", count, buf);
    return 0;
}

static int layout_displays(DWORD want_w, DWORD want_h)
{
    DISPLAY_DEVICEW dd;
    DEVMODEW best, dm, cur;
    wchar_t target[32] = L"";
    wchar_t others[16][32];
    int other_count = 0;
    BOOL target_primary = FALSE;
    DWORD i;
    LONG rc;

    SetProcessDPIAware();

    for (i = 0; ; i++) {
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (wcsstr(dd.DeviceString, VDD_ADAPTER_NAME)) {
            if (!target[0]) {
                wcscpy_s(target, 32, dd.DeviceName);
                target_primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            }
        } else if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && other_count < 16) {
            wcscpy_s(others[other_count++], 32, dd.DeviceName);
        }
    }
    if (!target[0]) {
        printf("error no App Sandbox display");
        return 1;
    }

    /* Largest mode, then highest refresh. The driver offers one mode; Windows may add none. */
    memset(&best, 0, sizeof(best));
    best.dmSize = sizeof(best);
    for (i = 0; ; i++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(target, i, &dm)) break;
        if ((ULONGLONG)dm.dmPelsWidth * dm.dmPelsHeight > (ULONGLONG)best.dmPelsWidth * best.dmPelsHeight ||
            ((ULONGLONG)dm.dmPelsWidth * dm.dmPelsHeight == (ULONGLONG)best.dmPelsWidth * best.dmPelsHeight &&
             dm.dmDisplayFrequency > best.dmDisplayFrequency))
            best = dm;
    }
    if (!best.dmPelsWidth) {
        printf("error no modes");
        return 1;
    }
    if (want_w && want_h) {   /* the raw viewer's size, at the native refresh rate */
        best.dmPelsWidth = want_w;
        best.dmPelsHeight = want_h;
    }

    memset(&cur, 0, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (other_count == 0 && target_primary &&
        EnumDisplaySettingsW(target, ENUM_CURRENT_SETTINGS, &cur) &&
        cur.dmPelsWidth == best.dmPelsWidth && cur.dmPelsHeight == best.dmPelsHeight &&
        cur.dmDisplayFrequency == best.dmDisplayFrequency) {
        printf("ok %lux%lu@%lu", best.dmPelsWidth, best.dmPelsHeight, best.dmDisplayFrequency);
        return 0;
    }

    best.dmPosition.x = 0;
    best.dmPosition.y = 0;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
    rc = ChangeDisplaySettingsExW(target, &best, NULL, CDS_UPDATEREGISTRY | CDS_SET_PRIMARY | CDS_NORESET, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        printf("error set %ld", rc);
        return 1;
    }
    for (int o = 0; o < other_count; o++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsW(others[o], ENUM_CURRENT_SETTINGS, &dm);
        dm.dmPelsWidth = 0;
        dm.dmPelsHeight = 0;
        dm.dmPosition.x = 0;
        dm.dmPosition.y = 0;
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        ChangeDisplaySettingsExW(others[o], &dm, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    }
    rc = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        printf("error apply %ld", rc);
        return 1;
    }
    printf("changed %lux%lu@%lu", best.dmPelsWidth, best.dmPelsHeight, best.dmDisplayFrequency);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc > 1 && wcscmp(argv[1], L"--layout") == 0) {
        DWORD w = 0, h = 0;
        if (argc > 2 && swscanf_s(argv[2], L"%lux%lu", &w, &h) != 2) w = h = 0;
        return layout_displays(w, h);
    }
    return list_displays();
}
