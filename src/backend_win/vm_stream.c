/*
 * vm_stream.c -- Moonlight viewer for native display VMs.
 *
 * A VM created with the native display option runs Sunshine in the guest (tools/agent/streaming.c).
 * This is the host half. Moonlight is GPL-3 and App Sandbox is MIT, so Moonlight is downloaded on
 * first use rather than shipped: one pinned release, checked against its SHA-256 before anything is
 * extracted, into %ProgramData%\AppSandbox\Moonlight. Moonlight keeps its own settings and pairings
 * per user in HKCU.
 *
 * One monitor thread does the slow work: installing Moonlight, and pairing it with each running
 * VM's Sunshine (Moonlight's "pair" command plus the PIN submitted through Sunshine's web API, with
 * the credentials generated at create). It also closes Moonlight when its VM stops.
 * vm_stream_open only launches, so it is safe on the UI thread and the API request loop; until a VM
 * is ready it returns FALSE and the caller opens the raw viewer.
 */

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm_stream.h"
#include "asb_core.h"
#include "ui.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#define MOONLIGHT_VERSION   "6.2.0"
#define MOONLIGHT_HOST      L"github.com"
#define MOONLIGHT_PATH      L"/moonlight-stream/moonlight-qt/releases/download/v6.2.0/MoonlightPortable-x64-6.2.0.zip"
#define MOONLIGHT_SHA256    "6f91d268b41ed5ca8066d9d7578dc403b8db190991ff48f00b1700465f8f7ebf"
#define MOONLIGHT_RULE      L"App Sandbox Moonlight"
#define SUNSHINE_API_PORT   47990       /* web API, HTTPS */
#define SUNSHINE_HTTP_PORT  47989       /* serverinfo, HTTP */

#define CHECK_RETRY_MS      10000       /* between checks while Sunshine is not up yet */
#define PAIR_RETRY_MS       60000       /* after a failed pairing, doubling to PAIR_RETRY_MAX_MS: each */
#define PAIR_RETRY_MAX_MS   600000      /* attempt can show a Moonlight dialog on failure */
#define EARLY_EXIT_MS       10000       /* a stream that ends this soon after launch failed */

typedef struct {
    UINT64      vm_id;
    BOOL        ready;          /* Moonlight is paired with this VM's Sunshine */
    HANDLE      moonlight;      /* running stream, or NULL */
    ULONGLONG   launched;       /* GetTickCount64() at launch */
    ULONGLONG   next_check;     /* earliest next pairing check */
    ULONGLONG   pair_wait;      /* current wait after a failed pairing */
} StreamSlot;

/* What the monitor thread needs from a VmInstance, copied so slow work never holds the pointer. */
typedef struct {
    UINT64      vm_id;
    wchar_t     name[256];
    char        ip[32];
    wchar_t     user[32];
    wchar_t     password[64];
} StreamTarget;

static StreamSlot       g_slots[ASB_MAX_VMS];
static CRITICAL_SECTION g_lock;                 /* g_slots */
static HANDLE           g_thread;
static volatile BOOL    g_running;
static VmStreamReadyFn  g_on_ready;
static wchar_t          g_base[MAX_PATH];       /* %ProgramData%\AppSandbox */
static wchar_t          g_dir[MAX_PATH];        /* <base>\Moonlight */
static wchar_t          g_exe[MAX_PATH];        /* <dir>\Moonlight.exe */
static wchar_t          g_ini[MAX_PATH];        /* Moonlight's portable settings */
static volatile BOOL    g_installed;
static wchar_t          g_rule_subnet[32];      /* subnet of the current firewall rule */

/* ---- helpers ---- */

static StreamSlot *slot_find(UINT64 vm_id, BOOL add)   /* caller holds g_lock */
{
    int i;
    if (!vm_id) return NULL;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (g_slots[i].vm_id == vm_id) return &g_slots[i];
    if (!add) return NULL;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (!g_slots[i].vm_id) {
            ZeroMemory(&g_slots[i], sizeof(g_slots[i]));
            g_slots[i].vm_id = vm_id;
            return &g_slots[i];
        }
    return NULL;
}

/* Run a command hidden and wait for it. cmd must be writable. Returns its exit code. */
static DWORD run_hidden(wchar_t *cmd, DWORD wait_ms)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    DWORD result;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return GetLastError();
    if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(pi.hProcess, &result)) result = GetLastError();
    } else {
        TerminateProcess(pi.hProcess, 1);
        result = ERROR_TIMEOUT;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

static void remove_tree(const wchar_t *path)
{
    wchar_t cmd[MAX_PATH + 64];
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    swprintf_s(cmd, ARRAYSIZE(cmd), L"cmd.exe /c rmdir /s /q \"%s\"", path);
    run_hidden(cmd, 60000);
}

static void target_from(const VmInstance *v, StreamTarget *t)
{
    ZeroMemory(t, sizeof(*t));
    t->vm_id = v->unique_id;
    wcscpy_s(t->name, ARRAYSIZE(t->name), v->name);
    strcpy_s(t->ip, sizeof(t->ip), v->nat_ip);
    wcscpy_s(t->user, ARRAYSIZE(t->user), v->stream_user);
    wcscpy_s(t->password, ARRAYSIZE(t->password), v->stream_password);
}

/* ---- Moonlight install ---- */

static BOOL moonlight_installed(void)
{
    wchar_t marker[MAX_PATH];
    char ver[32] = "";
    FILE *f = NULL;
    swprintf_s(marker, MAX_PATH, L"%s\\appsandbox-version.txt", g_dir);
    if (GetFileAttributesW(g_exe) == INVALID_FILE_ATTRIBUTES || _wfopen_s(&f, marker, L"r") != 0 || !f)
        return FALSE;
    fgets(ver, sizeof(ver), f);
    fclose(f);
    ver[strcspn(ver, "\r\n")] = '\0';
    return strcmp(ver, MOONLIGHT_VERSION) == 0;
}

/* Download the pinned release to path, hashing as it arrives. FALSE unless the hash matches. */
static BOOL download_verified(const wchar_t *path)
{
    HINTERNET s = NULL, c = NULL, r = NULL;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE f = INVALID_HANDLE_VALUE;
    static BYTE buf[65536];
    BYTE digest[32];
    char hex[65];
    DWORD status = 0, size = sizeof(status), got;
    ULONGLONG total = 0;
    BOOL ok = FALSE;
    int i;

    s = WinHttpOpen(L"AppSandbox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto done;
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    c = WinHttpConnect(s, MOONLIGHT_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) goto done;
    r = WinHttpOpenRequest(c, L"GET", MOONLIGHT_PATH, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r || !WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL)) {
        ui_log(L"Moonlight: download request failed (%lu).", GetLastError());
        goto done;
    }
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        ui_log(L"Moonlight: download returned HTTP %lu.", status);
        goto done;
    }
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) != 0)
        goto done;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) goto done;
    for (;;) {
        DWORD written;
        if (!g_running) goto done;
        if (!WinHttpReadData(r, buf, sizeof(buf), &got)) {
            ui_log(L"Moonlight: download interrupted after %llu bytes (%lu).", total, GetLastError());
            goto done;
        }
        if (got == 0) break;
        if (!WriteFile(f, buf, got, &written, NULL) || written != got) goto done;
        BCryptHashData(hash, buf, got, 0);
        total += got;
    }
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    for (i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    ok = strcmp(hex, MOONLIGHT_SHA256) == 0;
    ui_log(L"Moonlight: downloaded %llu bytes, SHA-256 %S (%s).", total, hex, ok ? L"matches" : L"DOES NOT MATCH");

done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static BOOL install_moonlight(void)
{
    wchar_t zip[MAX_PATH], staging[MAX_PATH], sys[MAX_PATH], cmd[3 * MAX_PATH], check[MAX_PATH];
    FILE *f = NULL;
    DWORD rc;

    swprintf_s(zip, MAX_PATH, L"%s\\moonlight-%S.zip", g_base, MOONLIGHT_VERSION);
    swprintf_s(staging, MAX_PATH, L"%s\\Moonlight.new", g_base);
    CreateDirectoryW(g_base, NULL);

    ui_log(L"Moonlight: installing %S for native display streaming...", MOONLIGHT_VERSION);
    if (!download_verified(zip)) {
        DeleteFileW(zip);
        return FALSE;
    }
    remove_tree(staging);
    CreateDirectoryW(staging, NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"", sys, zip, staging);
    rc = run_hidden(cmd, 300000);
    DeleteFileW(zip);
    swprintf_s(check, MAX_PATH, L"%s\\Moonlight.exe", staging);
    if (rc != 0 || GetFileAttributesW(check) == INVALID_FILE_ATTRIBUTES) {
        ui_log(L"Moonlight: extracting failed (tar exit %lu).", rc);
        remove_tree(staging);
        return FALSE;
    }
    remove_tree(g_dir);   /* a previous version; Moonlight's settings and pairings are in HKCU */
    if (!MoveFileExW(staging, g_dir, 0)) {
        ui_log(L"Moonlight: cannot move the install in place (%lu).", GetLastError());
        remove_tree(staging);
        return FALSE;
    }
    swprintf_s(check, MAX_PATH, L"%s\\appsandbox-version.txt", g_dir);
    if (_wfopen_s(&f, check, L"w") == 0 && f) {
        fprintf(f, "%s\n", MOONLIGHT_VERSION);
        fclose(f);
    }
    ui_log(L"Moonlight: %S installed in %s.", MOONLIGHT_VERSION, g_dir);
    return TRUE;
}

/* Inbound rule for Moonlight's stream ports, limited to the VMs' NAT subnet. */
static void apply_firewall(const char *vm_ip)
{
    wchar_t subnet[32], cmd[2 * MAX_PATH + 256];
    unsigned a, b, c, d;
    DWORD rc;

    if (sscanf_s(vm_ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255) return;
    swprintf_s(subnet, ARRAYSIZE(subnet), L"%u.%u.%u.0/24", a, b, c);
    if (wcscmp(subnet, g_rule_subnet) == 0) return;
    swprintf_s(cmd, ARRAYSIZE(cmd), L"netsh advfirewall firewall delete rule name=\"%s\"", MOONLIGHT_RULE);
    run_hidden(cmd, 30000);
    swprintf_s(cmd, ARRAYSIZE(cmd),
               L"netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow program=\"%s\" "
               L"remoteip=%s profile=any enable=yes", MOONLIGHT_RULE, g_exe, subnet);
    rc = run_hidden(cmd, 30000);
    if (rc == 0) wcscpy_s(g_rule_subnet, ARRAYSIZE(g_rule_subnet), subnet);
    ui_log(L"Moonlight: firewall rule for %s %s (netsh exit %lu).", subnet, rc == 0 ? L"set" : L"FAILED", rc);
}

/* ---- Sunshine ---- */

/* One HTTP request to the VM's Sunshine. Returns the HTTP status (0 = no answer).
   secure: the web API (HTTPS, port 47990), authenticated with the VM's credentials. Sunshine's
   certificate is self-signed and the VM is only reachable over the host's NAT subnet, so the
   certificate is not validated. Otherwise the unauthenticated serverinfo port (HTTP, 47989). */
static DWORD sunshine_request(const StreamTarget *t, BOOL secure, const wchar_t *method, const wchar_t *path,
                              const char *body, DWORD timeout_ms, char *out, DWORD out_size)
{
    HINTERNET s = NULL, c = NULL, r = NULL;
    wchar_t host[32];
    DWORD status = 0, size = sizeof(status), flags, got, pos = 0;

    if (out && out_size) out[0] = '\0';
    MultiByteToWideChar(CP_UTF8, 0, t->ip, -1, host, ARRAYSIZE(host));
    s = WinHttpOpen(L"AppSandbox", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto done;
    WinHttpSetTimeouts(s, 3000, 3000, timeout_ms, timeout_ms);
    c = WinHttpConnect(s, host, secure ? SUNSHINE_API_PORT : SUNSHINE_HTTP_PORT, 0);
    if (!c) goto done;
    r = WinHttpOpenRequest(c, method, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           secure ? WINHTTP_FLAG_SECURE : 0);
    if (!r) goto done;
    if (secure) {
        flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(r, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
        WinHttpSetCredentials(r, WINHTTP_AUTH_TARGET_SERVER, WINHTTP_AUTH_SCHEME_BASIC, t->user, t->password, NULL);
    }
    if (!WinHttpSendRequest(r, body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
                            (DWORD)-1L, (LPVOID)body, body ? (DWORD)strlen(body) : 0,
                            body ? (DWORD)strlen(body) : 0, 0) ||
        !WinHttpReceiveResponse(r, NULL))
        goto done;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    while (out && pos + 1 < out_size && WinHttpReadData(r, out + pos, out_size - 1 - pos, &got) && got)
        pos += got;
    if (out && out_size) out[pos] = '\0';
done:
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return status;
}

/* Text between open and close in s (an XML tag's content, or a JSON string after "key":"). */
static BOOL find_between(const char *s, const char *open, const char *close, char *out, size_t out_size)
{
    const char *p = strstr(s, open), *e;
    if (!p) return FALSE;
    p += strlen(open);
    e = strstr(p, close);
    if (!e || (size_t)(e - p) >= out_size) return FALSE;
    memcpy(out, p, e - p);
    out[e - p] = '\0';
    return TRUE;
}

/* Sunshine's server ID, which Moonlight keys its host record on. */
static BOOL sunshine_uniqueid(const StreamTarget *t, char *out, size_t out_size)
{
    char resp[4096];
    return sunshine_request(t, FALSE, L"GET", L"/serverinfo", NULL, 5000, resp, sizeof(resp)) == 200 &&
           find_between(resp, "<uniqueid>", "</uniqueid>", out, out_size);
}

/* The name this host pairs under in Sunshine's client list. */
static void client_name(char *out, size_t out_size)
{
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = ARRAYSIZE(computer);
    if (!GetComputerNameW(computer, &len)) wcscpy_s(computer, ARRAYSIZE(computer), L"host");
    sprintf_s(out, out_size, "App Sandbox %S", computer);
}

/* Is this host in Sunshine's paired clients? With unpair set, remove it instead. -1 = no answer. */
static int sunshine_client(const StreamTarget *t, BOOL unpair)
{
    char resp[8192], name[96], pat[128], uuid[64], body[128];
    const char *p;
    int found = 0;
    if (sunshine_request(t, TRUE, L"GET", L"/api/clients/list", NULL, 5000, resp, sizeof(resp)) != 200)
        return -1;
    client_name(name, sizeof(name));
    sprintf_s(pat, sizeof(pat), "\"name\":\"%s\"", name);
    for (p = strstr(resp, pat); p; p = strstr(p + 1, pat)) {
        const char *obj = p;
        found = 1;
        if (!unpair) break;
        while (obj > resp && *obj != '{') obj--;   /* the uuid is in the same object */
        if (find_between(obj, "\"uuid\":\"", "\"", uuid, sizeof(uuid))) {
            sprintf_s(body, sizeof(body), "{\"uuid\":\"%s\"}", uuid);
            sunshine_request(t, TRUE, L"POST", L"/api/clients/unpair", body, 10000, NULL, 0);
        }
    }
    return found;
}

/* ---- Moonlight's host records ----
 *
 * Portable Moonlight keeps its settings in a QSettings ini beside it. Host records are the
 * [hosts] keys "N\field=value" (N from 1, apps nested as "N\apps\M\..."), with "size=" counting
 * hosts. Moonlight shows a modal "already paired" dialog when asked to pair a host it holds a
 * certificate for, and confuses records that share an address (a recreated VM), so stale records
 * for a VM are removed before pairing it. */

typedef struct { char *line; int index; } IniLine;

static char *read_text(const wchar_t *path, size_t *len)
{
    FILE *f = NULL;
    char *buf;
    long n;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf) { *len = fread(buf, 1, (size_t)n, f); buf[*len] = '\0'; }
    fclose(f);
    return buf;
}

/* Host index of a [hosts] key line ("12\uuid=..." -> 12), or 0. */
static int host_index(const char *line)
{
    int n = 0;
    while (*line >= '0' && *line <= '9') n = n * 10 + (*line++ - '0');
    return *line == '\\' ? n : 0;
}

/* Does Moonlight hold a certificate for server uuid? */
static BOOL moonlight_paired(const char *uuid)
{
    size_t len = 0;
    char *text = read_text(g_ini, &len), *line, *next, uuid_line[96];
    int idx = 0, cert_idx[64], certs = 0, i;
    BOOL paired = FALSE;
    if (!text) return FALSE;
    sprintf_s(uuid_line, sizeof(uuid_line), "uuid=%s", uuid);
    for (line = strtok_s(text, "\r\n", &next); line; line = strtok_s(NULL, "\r\n", &next)) {
        int n = host_index(line);
        const char *field = n ? strchr(line, '\\') + 1 : NULL;
        if (!n) continue;
        if (strncmp(field, "srvcert=@", 9) == 0 && certs < 64) cert_idx[certs++] = n;
        if (strcmp(field, uuid_line) == 0) idx = n;
    }
    for (i = 0; idx && i < certs; i++) if (cert_idx[i] == idx) paired = TRUE;
    free(text);
    return paired;
}

/* Remove host records for this address or server uuid, renumbering the rest. */
static void moonlight_forget(const char *ip, const char *uuid)
{
    size_t len = 0;
    char *text = read_text(g_ini, &len), *line, *next;
    char addr_local[64], addr_manual[64], uuid_line[96];
    BOOL drop[256] = { 0 }, in_hosts = FALSE;
    int map[256] = { 0 }, kept = 0, i, n, removed = 0;
    FILE *f = NULL;
    char **lines;
    int count = 0;

    if (!text) return;
    sprintf_s(addr_local, sizeof(addr_local), "localaddress=%s", ip);
    sprintf_s(addr_manual, sizeof(addr_manual), "manualaddress=%s", ip);
    sprintf_s(uuid_line, sizeof(uuid_line), "uuid=%s", uuid ? uuid : "-");

    lines = (char **)calloc(len / 2 + 2, sizeof(char *));
    if (!lines) { free(text); return; }
    for (line = strtok_s(text, "\r\n", &next); line; line = strtok_s(NULL, "\r\n", &next))
        lines[count++] = line;

    /* Which hosts go. */
    for (i = 0; i < count; i++) {
        if (lines[i][0] == '[') { in_hosts = strcmp(lines[i], "[hosts]") == 0; continue; }
        if (!in_hosts || !(n = host_index(lines[i])) || n >= 256) continue;
        {
            const char *field = strchr(lines[i], '\\') + 1;
            if (strcmp(field, addr_local) == 0 || strcmp(field, addr_manual) == 0 || strcmp(field, uuid_line) == 0)
                drop[n] = TRUE;
        }
    }
    for (n = 1; n < 256; n++) if (drop[n]) removed++;
    if (!removed) { free(lines); free(text); return; }

    /* New numbering for the hosts that stay. */
    in_hosts = FALSE;
    for (i = 0; i < count; i++) {
        if (lines[i][0] == '[') { in_hosts = strcmp(lines[i], "[hosts]") == 0; continue; }
        if (in_hosts && (n = host_index(lines[i])) && n < 256 && !drop[n] && !map[n]) map[n] = ++kept;
    }

    if (_wfopen_s(&f, g_ini, L"wb") == 0 && f) {
        in_hosts = FALSE;
        for (i = 0; i < count; i++) {
            if (lines[i][0] == '[') in_hosts = strcmp(lines[i], "[hosts]") == 0;
            else if (in_hosts && (n = host_index(lines[i])) && n < 256) {
                if (drop[n]) continue;
                fprintf(f, "%d%s\r\n", map[n], strchr(lines[i], '\\'));
                continue;
            } else if (in_hosts && strncmp(lines[i], "size=", 5) == 0) {
                fprintf(f, "size=%d\r\n", kept);
                continue;
            }
            fprintf(f, "%s\r\n", lines[i]);
        }
        fclose(f);
        ui_log(L"Moonlight: removed %d stale host record(s) for %S.", removed, ip);
    }
    free(lines);
    free(text);
}

/* ---- pairing ---- */

static HANDLE start_moonlight(const wchar_t *args)
{
    wchar_t cmd[MAX_PATH + 512];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\" %s", g_exe, args);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        ui_log(L"Moonlight: cannot start (%lu): %s", GetLastError(), args);
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* Pair Moonlight with the VM's Sunshine: Moonlight's "pair" waits for Sunshine to accept its PIN,
   which we submit through Sunshine's API (what its web UI's PIN page does). The result is judged
   by the caller from both sides' records, not from this exchange. Moonlight's "pair" ends on a
   modal result dialog it never dismisses, so it is ended as soon as both sides hold the pairing. */
static void pair(const StreamTarget *t, const char *uuid)
{
    wchar_t args[128];
    char resp[2048], pairing_id[128], body[512], name[96];
    DWORD code = 0, get_status = 0, post_status = 0;
    unsigned pin_value = 0;
    HANDLE proc;
    int i;

    BCryptGenRandom(NULL, (PUCHAR)&pin_value, sizeof(pin_value), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    pin_value %= 10000;
    client_name(name, sizeof(name));

    swprintf_s(args, ARRAYSIZE(args), L"pair %S --pin %04u", t->ip, pin_value);
    proc = start_moonlight(args);
    if (!proc) return;
    pairing_id[0] = '\0';
    for (i = 0; i < 30 && g_running && !post_status; i++) {
        Sleep(1000);
        if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) break;
        get_status = sunshine_request(t, TRUE, L"GET", L"/api/pin", NULL, 5000, resp, sizeof(resp));
        if (get_status != 200 || !find_between(resp, "\"id\":\"", "\"", pairing_id, sizeof(pairing_id)))
            continue;
        sprintf_s(body, sizeof(body), "{\"pin\":\"%04u\",\"name\":\"%s\",\"pairing_id\":\"%s\"}",
                  pin_value, name, pairing_id);
        post_status = sunshine_request(t, TRUE, L"POST", L"/api/pin", body, 30000, resp, sizeof(resp));
        ui_log(L"Moonlight: PIN for \"%s\" submitted (HTTP %lu): %.120S", t->name, post_status, resp);
    }
    for (i = 0; i < 30 && WaitForSingleObject(proc, 1000) == WAIT_TIMEOUT; i++)
        if (post_status && moonlight_paired(uuid) && sunshine_client(t, FALSE) > 0)
            break;   /* paired; what remains is Moonlight's result dialog */
    if (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 3000);
    }
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    if (!pairing_id[0])
        ui_log(L"Moonlight: no pairing request reached \"%s\"'s Sunshine (last HTTP %lu, exit %ld).",
               t->name, get_status, (long)code);
}

typedef enum { PREP_READY, PREP_NOT_UP, PREP_FAILED } PrepResult;

/* Get a running VM ready to stream: both Moonlight (a certificate for the server ID Sunshine
   reports) and Sunshine (this host in its client list) hold the pairing. */
static PrepResult prepare(const StreamTarget *t)
{
    char uuid[64];
    int client;
    if (!t->ip[0] || !t->password[0]) return PREP_FAILED;
    apply_firewall(t->ip);
    /* Sunshine starts at sign-in and is installed on first boot: wait until both ports answer. */
    if (!sunshine_uniqueid(t, uuid, sizeof(uuid)) || (client = sunshine_client(t, FALSE)) < 0)
        return PREP_NOT_UP;
    if (client && moonlight_paired(uuid)) return PREP_READY;

    ui_log(L"Moonlight: pairing with \"%s\" (Sunshine %S)...", t->name, uuid);
    moonlight_forget(t->ip, uuid);
    if (client) sunshine_client(t, TRUE);
    pair(t, uuid);
    client = sunshine_client(t, FALSE);
    if (client > 0 && moonlight_paired(uuid)) {
        ui_log(L"Moonlight: paired with \"%s\".", t->name);
        return PREP_READY;
    }
    ui_log(L"Moonlight: pairing with \"%s\" FAILED (Sunshine lists this host: %s, Moonlight holds its "
           L"certificate: %s).", t->name, client > 0 ? L"yes" : L"no", moonlight_paired(uuid) ? L"yes" : L"no");
    return PREP_FAILED;
}

/* ---- monitor thread ---- */

static void close_slot(StreamSlot *s)   /* caller holds g_lock */
{
    if (s->moonlight) {
        TerminateProcess(s->moonlight, 0);
        WaitForSingleObject(s->moonlight, 3000);
        CloseHandle(s->moonlight);
        s->moonlight = NULL;
    }
}

static DWORD WINAPI monitor_thread(LPVOID param)
{
    ULONGLONG next_install = 0, install_wait = 30000;
    (void)param;

    while (g_running) {
        ULONGLONG now = GetTickCount64();
        StreamTarget pending;
        BOOL any_native = FALSE, have_pending = FALSE;
        int i, count = asb_vm_count();

        EnterCriticalSection(&g_lock);
        for (i = 0; i < count; i++) {
            VmInstance *v = asb_vm_instance(asb_vm_get(i));
            StreamSlot *s;
            if (!v || !v->native_display) continue;
            any_native = TRUE;
            s = slot_find(v->unique_id, TRUE);
            if (!s) continue;
            if (s->moonlight && WaitForSingleObject(s->moonlight, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(s->moonlight, &code);
                if (now - s->launched < EARLY_EXIT_MS) {
                    ui_log(L"Moonlight for \"%s\" exited at once (code %ld); checking pairing again.",
                           v->name, (long)code);
                    s->ready = FALSE;
                }
                CloseHandle(s->moonlight);
                s->moonlight = NULL;
            }
            if (!v->running || v->shutdown_requested) {
                if (s->moonlight) ui_log(L"Moonlight: \"%s\" is stopping, closing the stream.", v->name);
                close_slot(s);
                s->ready = FALSE;
                s->next_check = 0;
                s->pair_wait = 0;
                continue;
            }
            if (!have_pending && !s->ready && v->agent_online && g_installed && now >= s->next_check) {
                target_from(v, &pending);
                have_pending = TRUE;
                s->next_check = now + CHECK_RETRY_MS;
            }
        }
        LeaveCriticalSection(&g_lock);

        if (any_native && !g_installed && now >= next_install) {
            g_installed = moonlight_installed() || install_moonlight();
            if (!g_installed) {
                ui_log(L"Moonlight: install failed, retrying in %llu s.", install_wait / 1000);
                next_install = now + install_wait;
                if (install_wait < 600000) install_wait *= 2;
            }
        }

        if (have_pending) {
            PrepResult result = prepare(&pending);
            SecureZeroMemory(pending.password, sizeof(pending.password));
            EnterCriticalSection(&g_lock);
            {
                StreamSlot *s = slot_find(pending.vm_id, FALSE);
                if (s && result == PREP_READY) {
                    s->ready = TRUE;
                    s->pair_wait = 0;
                } else if (s && result == PREP_FAILED) {
                    s->pair_wait = s->pair_wait ? min(s->pair_wait * 2, PAIR_RETRY_MAX_MS) : PAIR_RETRY_MS;
                    s->next_check = GetTickCount64() + s->pair_wait;
                    ui_log(L"Moonlight: retrying \"%s\" in %llu s.", pending.name, s->pair_wait / 1000);
                }
            }
            LeaveCriticalSection(&g_lock);
            if (result == PREP_READY) {
                ui_log(L"Moonlight: \"%s\" is ready to stream.", pending.name);
                if (g_on_ready) g_on_ready(pending.vm_id);
            }
        }

        for (i = 0; i < 4 && g_running; i++) Sleep(500);
    }
    return 0;
}

/* ---- public ---- */

void vm_stream_init(VmStreamReadyFn on_ready)
{
    if (g_thread) return;
    InitializeCriticalSection(&g_lock);
    g_on_ready = on_ready;
    if (!GetEnvironmentVariableW(L"ProgramData", g_base, MAX_PATH))
        wcscpy_s(g_base, MAX_PATH, L"C:\\ProgramData");
    wcscat_s(g_base, MAX_PATH, L"\\AppSandbox");
    swprintf_s(g_dir, MAX_PATH, L"%s\\Moonlight", g_base);
    swprintf_s(g_exe, MAX_PATH, L"%s\\Moonlight.exe", g_dir);
    swprintf_s(g_ini, MAX_PATH, L"%s\\Moonlight Game Streaming Project\\Moonlight.ini", g_dir);
#if defined(_M_X64)
    g_running = TRUE;
    g_thread = CreateThread(NULL, 0, monitor_thread, NULL, 0, NULL);
#endif
}

void vm_stream_shutdown(void)
{
    int i;
    if (!g_thread) return;
    g_running = FALSE;
    WaitForSingleObject(g_thread, 60000);
    CloseHandle(g_thread);
    g_thread = NULL;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < ASB_MAX_VMS; i++) close_slot(&g_slots[i]);
    LeaveCriticalSection(&g_lock);
}

/* Moonlight's bitrate for the mode: the measured 300 Mbps at 5120x2160@120 (~0.23 bit per
   pixel), scaled with the pixel rate. */
static DWORD bitrate_kbps(const VmInstance *v)
{
    ULONGLONG kbps = (ULONGLONG)v->display_width * v->display_height * v->display_refresh * 225 / 1000000;
    if (kbps < 20000) kbps = 20000;
    if (kbps > 500000) kbps = 500000;
    return (DWORD)kbps;
}

static BOOL CALLBACK focus_window(HWND hwnd, LPARAM pid)
{
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == (DWORD)pid && IsWindowVisible(hwnd)) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return FALSE;
    }
    return TRUE;
}

BOOL vm_stream_open(VmInstance *vm)
{
    StreamSlot *s;
    BOOL ok = FALSE;
    if (!vm || !vm->native_display || !g_installed) return FALSE;
    EnterCriticalSection(&g_lock);
    s = slot_find(vm->unique_id, FALSE);
    if (s && s->ready) {
        if (s->moonlight && WaitForSingleObject(s->moonlight, 0) == WAIT_TIMEOUT) {
            EnumWindows(focus_window, (LPARAM)GetProcessId(s->moonlight));
            ok = TRUE;
        } else {
            wchar_t args[256];
            if (s->moonlight) { CloseHandle(s->moonlight); s->moonlight = NULL; }
            swprintf_s(args, ARRAYSIZE(args),
                       L"stream %S Desktop --resolution %lux%lu --fps %lu --bitrate %lu "
                       L"--video-codec HEVC --display-mode fullscreen --capture-system-keys fullscreen",
                       vm->nat_ip, vm->display_width, vm->display_height, vm->display_refresh,
                       bitrate_kbps(vm));
            s->moonlight = start_moonlight(args);
            s->launched = GetTickCount64();
            ok = s->moonlight != NULL;
            if (ok) ui_log(L"Moonlight: streaming \"%s\" at %lux%lu@%lu, %lu kbps.", vm->name,
                           vm->display_width, vm->display_height, vm->display_refresh, bitrate_kbps(vm));
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

BOOL vm_stream_is_open(UINT64 vm_id)
{
    StreamSlot *s;
    BOOL open = FALSE;
    if (!g_thread) return FALSE;
    EnterCriticalSection(&g_lock);
    s = slot_find(vm_id, FALSE);
    if (s && s->moonlight) open = WaitForSingleObject(s->moonlight, 0) == WAIT_TIMEOUT;
    LeaveCriticalSection(&g_lock);
    return open;
}

BOOL vm_stream_is_ready(UINT64 vm_id)
{
    StreamSlot *s;
    BOOL ready = FALSE;
    if (!g_thread) return FALSE;
    EnterCriticalSection(&g_lock);
    s = slot_find(vm_id, FALSE);
    ready = s && s->ready && g_installed;
    LeaveCriticalSection(&g_lock);
    return ready;
}

void vm_stream_close(UINT64 vm_id)
{
    StreamSlot *s;
    if (!g_thread) return;
    EnterCriticalSection(&g_lock);
    s = slot_find(vm_id, FALSE);
    if (s) close_slot(s);
    LeaveCriticalSection(&g_lock);
}
