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
 * --no-transparency and --scale PCT set first-sign-in defaults for native
 * display VMs, once per user (see their functions).
 *
 * Must be launched in the interactive session (WinSta0\Default) so that
 * EnumDisplayDevices / EnumDisplaySettings see all displays.
 */

#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#include <stdlib.h>
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

/* --no-transparency: native display VMs start with transparency effects off (translucent
   surfaces flicker under Windows Graphics Capture on the virtual display). Done once per user,
   recorded in HKCU\Software\AppSandbox, so turning them back on sticks. Prints "set" or "kept". */
static int transparency_default(void)
{
    static const wchar_t personalize[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD done = 0, zero = 0, one = 1, size = sizeof(done);
    DWORD_PTR result;

    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AppSandbox", L"TransparencyDefaulted", RRF_RT_REG_DWORD,
                     NULL, &done, &size) == ERROR_SUCCESS && done) {
        printf("kept");
        return 0;
    }
    if (RegSetKeyValueW(HKEY_CURRENT_USER, personalize, L"EnableTransparency", REG_DWORD, &zero, sizeof(zero)) != ERROR_SUCCESS ||
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\AppSandbox", L"TransparencyDefaulted", REG_DWORD, &one, sizeof(one)) != ERROR_SUCCESS) {
        printf("error registry");
        return 1;
    }
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet",
                        SMTO_ABORTIFHUNG, 5000, &result);
    printf("set");
    return 0;
}

/* Display scaling through DisplayConfigGet/SetDeviceInfo's undocumented DPI packets (what the
   Settings app uses). Values are steps from the monitor's recommended scale; the lowest step is
   always 100%. */
#define DPI_GET_SCALE ((DISPLAYCONFIG_DEVICE_INFO_TYPE)-3)
#define DPI_SET_SCALE ((DISPLAYCONFIG_DEVICE_INFO_TYPE)-4)
typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; INT32 min_rel, cur_rel, max_rel; } DpiGet;
typedef struct { DISPLAYCONFIG_DEVICE_INFO_HEADER header; INT32 rel; } DpiSet;
static const DWORD dpi_steps[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };
#define DPI_STEP_COUNT ((int)(sizeof(dpi_steps) / sizeof(dpi_steps[0])))

/* The App Sandbox display's source, and its scaling as step indexes. */
static BOOL vdd_scale(LUID *adapter, UINT32 *source, int *recommended, int *current, int *max)
{
    DISPLAY_DEVICEW dd;
    wchar_t gdi[32] = L"";
    UINT32 npaths = 0, nmodes = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    BOOL found = FALSE;

    for (i = 0; ; i++) {
        memset(&dd, 0, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) break;
        if (wcsstr(dd.DeviceString, VDD_ADAPTER_NAME) && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            wcscpy_s(gdi, 32, dd.DeviceName);
            break;
        }
    }
    if (!gdi[0] || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npaths, &nmodes) != ERROR_SUCCESS)
        return FALSE;
    paths = calloc(npaths, sizeof(*paths));
    modes = calloc(nmodes, sizeof(*modes));
    if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npaths, paths, &nmodes, modes, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < npaths && !found; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name;
            DpiGet get;
            memset(&name, 0, sizeof(name));
            name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            name.header.size = sizeof(name);
            name.header.adapterId = paths[i].sourceInfo.adapterId;
            name.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || wcscmp(name.viewGdiDeviceName, gdi) != 0)
                continue;
            memset(&get, 0, sizeof(get));
            get.header.type = DPI_GET_SCALE;
            get.header.size = sizeof(get);
            get.header.adapterId = name.header.adapterId;
            get.header.id = name.header.id;
            if (DisplayConfigGetDeviceInfo(&get.header) != ERROR_SUCCESS) break;
            *adapter = get.header.adapterId;
            *source = get.header.id;
            *recommended = -get.min_rel;
            *current = *recommended + get.cur_rel;
            *max = *recommended + get.max_rel;
            /* Steps are kept across modes, so they can fall outside this mode's range; Windows clamps. */
            if (*current < 0) *current = 0;
            if (*current > *max) *current = *max;
            found = *recommended >= 0 && *max < DPI_STEP_COUNT;
        }
    }
    free(paths);
    free(modes);
    return found;
}

/* --scale [PCT]: native display VMs start at the host monitor's scaling. Done once per user,
   recorded in HKCU\Software\AppSandbox, so a later change sticks. Prints "set <pct>", "kept" or
   an error; without PCT, prints "current <pct> recommended <pct>". */
static int scale_default(const wchar_t *arg)
{
    DWORD done = 0, one = 1, size = sizeof(done), want = arg ? (DWORD)_wtoi(arg) : 0;
    int rec, cur, max, target = 0, i;
    LUID adapter;
    UINT32 source;
    DpiSet set;

    if (!vdd_scale(&adapter, &source, &rec, &cur, &max)) {
        printf("error no scaling for the App Sandbox display");
        return 1;
    }
    if (!want) {
        printf("current %lu recommended %lu", dpi_steps[cur], dpi_steps[rec]);
        return 0;
    }
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\AppSandbox", L"ScaleDefaulted", RRF_RT_REG_DWORD,
                     NULL, &done, &size) == ERROR_SUCCESS && done) {
        printf("kept");
        return 0;
    }
    for (i = 0; i <= max; i++)   /* the largest step not above the host's */
        if (dpi_steps[i] <= want) target = i;
    if (target != cur) {
        memset(&set, 0, sizeof(set));
        set.header.type = DPI_SET_SCALE;
        set.header.size = sizeof(set);
        set.header.adapterId = adapter;
        set.header.id = source;
        set.rel = target - rec;
        if (DisplayConfigSetDeviceInfo(&set.header) != ERROR_SUCCESS) {
            printf("error set");
            return 1;
        }
    }
    if (RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\AppSandbox", L"ScaleDefaulted", REG_DWORD, &one, sizeof(one)) != ERROR_SUCCESS) {
        printf("error registry");
        return 1;
    }
    printf("set %lu", dpi_steps[target]);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc > 1 && wcscmp(argv[1], L"--no-transparency") == 0)
        return transparency_default();
    if (argc > 1 && wcscmp(argv[1], L"--scale") == 0)
        return scale_default(argc > 2 ? argv[2] : NULL);
    if (argc > 1 && wcscmp(argv[1], L"--layout") == 0) {
        DWORD w = 0, h = 0;
        if (argc > 2 && swscanf_s(argv[2], L"%lux%lu", &w, &h) != 2) w = h = 0;
        return layout_displays(w, h);
    }
    return list_displays();
}
