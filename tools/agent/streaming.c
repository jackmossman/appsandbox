/*
 * streaming.c — Native display streaming in the guest (see streaming.h).
 *
 * Sunshine is GPL-3 and App Sandbox is MIT, so Sunshine is downloaded at runtime rather than
 * shipped: one pinned release, checked against its SHA-256 before anything is extracted.
 *
 * Sunshine runs as the signed-in user, not SYSTEM. Windows Graphics Capture needs a user
 * session, and it is the only capture that works in a GPU-PV guest (Desktop Duplication
 * reports ACCESS_LOST on every output). Its binaries stay admin-only; only its config folder
 * (pairing state, logs, settings the web UI saves) is writable by users.
 *
 * Install:  %ProgramData%\AppSandbox\Sunshine\                  binaries + appsandbox-version.txt
 *           %ProgramData%\AppSandbox\Sunshine\config\           sunshine.conf, state, log
 */

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "streaming.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

#define STREAM_REG_KEY      L"SOFTWARE\\AppSandbox\\Streaming"
#define SUNSHINE_VERSION    "2026.914.233613"
#define SUNSHINE_HOST       L"github.com"
#define WIDEN2(x)           L##x
#define WIDEN(x)            WIDEN2(x)
#define SUNSHINE_PATH       L"/LizardByte/Sunshine/releases/download/v" WIDEN(SUNSHINE_VERSION) L"/Sunshine-Windows-AMD64-lite.zip"
#define SUNSHINE_SHA256     "233008e46f4c0e501a586cbfd6c4fd4a4c0d414a0b5fc7f13c070eb92ec3824b"
#define FIREWALL_RULE       L"App Sandbox Sunshine"

#define LAYOUT_INTERVAL_MS  60000       /* re-check the display layout while signed in */
#define RESPAWN_DELAY_MS    10000       /* least time between Sunshine starts */

#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static StreamLogFn   g_log;
static HANDLE        g_thread;
static volatile BOOL g_running;
static wchar_t       g_subnet[64];
static wchar_t       g_base[MAX_PATH];      /* %ProgramData%\AppSandbox */
static wchar_t       g_dir[MAX_PATH];       /* <base>\Sunshine */
static wchar_t       g_exe[MAX_PATH];       /* <dir>\sunshine.exe */
static HANDLE        g_sunshine;            /* Sunshine we started, or NULL */
static DWORD         g_sunshine_session = 0xFFFFFFFF;
static SRWLOCK       g_subnet_lock = SRWLOCK_INIT;   /* g_subnet, g_new_subnet */
static wchar_t       g_new_subnet[64];      /* from set_ip, applied by the thread; "" = none */
static volatile LONG g_layout_now;          /* the raw viewer asked for a size: apply it next loop */
static BOOL g_scale_done;                   /* host scaling applied (or not wanted) for this boot */

#define VDD_REG_KEY         L"SOFTWARE\\AppSandbox\\VDD"

/* ---- processes ---- */

/* Run a command hidden as SYSTEM and wait for it. cmd must be writable. */
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

/* Run cmd (writable) as the user that token belongs to, on the interactive desktop, in dir.
   proc != NULL: leave it running and return its handle. Otherwise wait up to wait_ms, put its
   stdout in out (when given) and return its exit code in *exit_code. */
static BOOL run_as_user(HANDLE token, wchar_t *cmd, const wchar_t *dir, DWORD wait_ms,
                        char *out, int out_size, DWORD *exit_code, HANDLE *proc)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    LPVOID env = NULL;
    BOOL ok;

    if (out) {
        out[0] = '\0';
        if (!CreatePipe(&rd, &wr, &sa, 0)) return FALSE;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    }
    if (!CreateEnvironmentBlock(&env, token, FALSE)) env = NULL;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    if (out) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
    }
    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessAsUserW(token, NULL, cmd, NULL, NULL, out != NULL,
                              CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                              env, dir, &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    if (wr) CloseHandle(wr);
    if (!ok) {
        LOG("Streaming: CreateProcessAsUser failed (%lu): %ls", GetLastError(), cmd);
        if (rd) CloseHandle(rd);
        return FALSE;
    }
    CloseHandle(pi.hThread);

    if (proc) {
        *proc = pi.hProcess;
        if (rd) CloseHandle(rd);
        return TRUE;
    }
    if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    if (rd) {
        DWORD got;
        int pos = 0;
        while (pos < out_size - 1 && ReadFile(rd, out + pos, (DWORD)(out_size - 1 - pos), &got, NULL) && got)
            pos += (int)got;
        out[pos] = '\0';
        while (pos > 0 && (out[pos - 1] == '\r' || out[pos - 1] == '\n' || out[pos - 1] == ' '))
            out[--pos] = '\0';
        CloseHandle(rd);
    }
    if (exit_code && !GetExitCodeProcess(pi.hProcess, exit_code)) *exit_code = GetLastError();
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* End every sunshine.exe started from our install (one left by a previous agent instance would
   hold Sunshine's ports and make each new start exit at once). */
static void kill_stray_sunshine(void)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        HANDLE h;
        if (_wcsicmp(pe.szExeFile, L"sunshine.exe") != 0) continue;
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!h) continue;
        if (QueryFullProcessImageNameW(h, 0, path, &len) && _wcsicmp(path, g_exe) == 0) {
            LOG("Streaming: ending Sunshine PID %lu left running.", pe.th32ProcessID);
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 3000);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

static void stop_sunshine(void)
{
    if (g_sunshine) {
        TerminateProcess(g_sunshine, 0);
        WaitForSingleObject(g_sunshine, 3000);
        CloseHandle(g_sunshine);
        g_sunshine = NULL;
        g_sunshine_session = 0xFFFFFFFF;
        LOG("Streaming: Sunshine stopped.");
    }
}

/* ---- configuration ---- */

static BOOL reg_sz(HKEY key, const wchar_t *name, wchar_t *out, DWORD cch)
{
    DWORD size = cch * sizeof(wchar_t);
    out[0] = L'\0';
    return RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, out, &size) == ERROR_SUCCESS;
}

/* Values end up on command lines: allow only what they need. */
static BOOL only_chars(const wchar_t *s, const wchar_t *allowed)
{
    return s[0] && wcsspn(s, allowed) == wcslen(s);
}

#define ALNUM L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

/* ---- install ---- */

static BOOL sunshine_installed(void)
{
    wchar_t marker[MAX_PATH];
    char ver[64] = "";
    FILE *f = NULL;
    swprintf_s(marker, MAX_PATH, L"%s\\appsandbox-version.txt", g_dir);
    if (GetFileAttributesW(g_exe) == INVALID_FILE_ATTRIBUTES || _wfopen_s(&f, marker, L"r") != 0 || !f)
        return FALSE;
    fgets(ver, sizeof(ver), f);
    fclose(f);
    ver[strcspn(ver, "\r\n")] = '\0';
    return strcmp(ver, SUNSHINE_VERSION) == 0;
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

    s = WinHttpOpen(L"AppSandbox-Agent", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) goto done;
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    c = WinHttpConnect(s, SUNSHINE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!c) goto done;
    r = WinHttpOpenRequest(c, L"GET", SUNSHINE_PATH, NULL, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r || !WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r, NULL)) {
        LOG("Streaming: download request failed (%lu).", GetLastError());
        goto done;
    }
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        LOG("Streaming: download returned HTTP %lu.", status);
        goto done;
    }

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) != 0)
        goto done;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LOG("Streaming: cannot create %ls (%lu).", path, GetLastError());
        goto done;
    }
    for (;;) {
        DWORD written;
        if (!g_running) goto done;
        if (!WinHttpReadData(r, buf, sizeof(buf), &got)) {
            LOG("Streaming: download interrupted after %llu bytes (%lu).", total, GetLastError());
            goto done;
        }
        if (got == 0) break;
        if (!WriteFile(f, buf, got, &written, NULL) || written != got) goto done;
        BCryptHashData(hash, buf, got, 0);
        total += got;
    }
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    for (int i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    ok = strcmp(hex, SUNSHINE_SHA256) == 0;
    LOG("Streaming: downloaded %llu bytes, SHA-256 %s (%s).", total, hex, ok ? "matches" : "DOES NOT MATCH");

done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static void remove_tree(const wchar_t *path)
{
    wchar_t cmd[MAX_PATH + 64];
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    swprintf_s(cmd, MAX_PATH + 64, L"cmd.exe /c rmdir /s /q \"%s\"", path);
    run_hidden(cmd, 60000);
}

/* Config folder: writable by users (Sunshine runs as the user), with our settings on first
   install only, so changes made in Sunshine's web UI are kept. */
static void prepare_config(void)
{
    wchar_t cfg[MAX_PATH], conf[MAX_PATH], cmd[MAX_PATH + 96];
    FILE *f = NULL;

    swprintf_s(cfg, MAX_PATH, L"%s\\config", g_dir);
    CreateDirectoryW(cfg, NULL);
    swprintf_s(cmd, MAX_PATH + 96, L"icacls \"%s\" /grant *S-1-5-32-545:(OI)(CI)M", cfg);
    run_hidden(cmd, 30000);

    swprintf_s(conf, MAX_PATH, L"%s\\sunshine.conf", cfg);
    if (GetFileAttributesW(conf) != INVALID_FILE_ATTRIBUTES) return;
    if (_wfopen_s(&f, conf, L"w") != 0 || !f) {
        LOG("Streaming: cannot write %ls.", conf);
        return;
    }
    fprintf(f, "# Written by the App Sandbox agent at install. Sunshine's web UI may change it.\n"
               "capture = wgc\n"
               "origin_web_ui_allowed = lan\n"
               "min_log_level = info\n"
               "log_path = %ls\\sunshine.log\n", cfg);
    fclose(f);
}

static BOOL install_sunshine(void)
{
    wchar_t zip[MAX_PATH], staging[MAX_PATH], extracted[MAX_PATH], old[MAX_PATH], cmd[3 * MAX_PATH];
    wchar_t sys[MAX_PATH], check[MAX_PATH];
    BOOL had_old = FALSE;
    FILE *f = NULL;
    DWORD rc;

    swprintf_s(zip, MAX_PATH, L"%s\\sunshine-%S.zip", g_base, SUNSHINE_VERSION);
    swprintf_s(staging, MAX_PATH, L"%s\\Sunshine.new", g_base);
    swprintf_s(extracted, MAX_PATH, L"%s\\Sunshine", staging);
    swprintf_s(old, MAX_PATH, L"%s\\Sunshine.old", g_base);
    CreateDirectoryW(g_base, NULL);

    LOG("Streaming: installing Sunshine %s.", SUNSHINE_VERSION);
    if (!download_verified(zip)) {
        DeleteFileW(zip);
        return FALSE;
    }

    remove_tree(staging);
    CreateDirectoryW(staging, NULL);
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf_s(cmd, 3 * MAX_PATH, L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"", sys, zip, staging);
    rc = run_hidden(cmd, 300000);
    DeleteFileW(zip);
    swprintf_s(check, MAX_PATH, L"%s\\sunshine.exe", extracted);
    if (rc != 0 || GetFileAttributesW(check) == INVALID_FILE_ATTRIBUTES) {
        LOG("Streaming: extracting failed (tar exit %lu).", rc);
        remove_tree(staging);
        return FALSE;
    }

    /* Swap the new release in, keeping the old one's config (pairing state, settings). */
    kill_stray_sunshine();
    remove_tree(old);
    if (GetFileAttributesW(g_dir) != INVALID_FILE_ATTRIBUTES) {
        if (!MoveFileExW(g_dir, old, 0)) {
            LOG("Streaming: cannot move the old install aside (%lu).", GetLastError());
            remove_tree(staging);
            return FALSE;
        }
        had_old = TRUE;
    }
    if (!MoveFileExW(extracted, g_dir, 0)) {
        LOG("Streaming: cannot move the new install in place (%lu).", GetLastError());
        if (had_old) MoveFileExW(old, g_dir, 0);
        remove_tree(staging);
        return FALSE;
    }
    remove_tree(staging);
    if (had_old) {
        wchar_t from[MAX_PATH], to[MAX_PATH];
        swprintf_s(from, MAX_PATH, L"%s\\config", old);
        swprintf_s(to, MAX_PATH, L"%s\\config", g_dir);
        if (GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES && !MoveFileExW(from, to, 0))
            LOG("Streaming: cannot carry the old config over (%lu).", GetLastError());
        remove_tree(old);
    }

    prepare_config();

    swprintf_s(check, MAX_PATH, L"%s\\appsandbox-version.txt", g_dir);
    if (_wfopen_s(&f, check, L"w") == 0 && f) {
        fprintf(f, "%s\n", SUNSHINE_VERSION);
        fclose(f);
    }
    LOG("Streaming: Sunshine %s installed in %ls.", SUNSHINE_VERSION, g_dir);
    return TRUE;
}

static BOOL wait_service_running(const wchar_t *name, DWORD timeout_ms)
{
    ULONGLONG end = GetTickCount64() + timeout_ms;
    BOOL running = FALSE;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, name, SERVICE_QUERY_STATUS) : NULL;

    while (svc && g_running) {
        SERVICE_STATUS ss;
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
            running = TRUE;
            break;
        }
        if (GetTickCount64() >= end) break;
        Sleep(1000);
    }
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    return running;
}

/* Inbound rule for Sunshine, limited to the host's NAT subnet. Replaced on every start so the
   rule always matches the current configuration. Early in boot a delete can be lost while the
   firewall service is still starting (the add then made a duplicate), so the old rule is
   confirmed gone before the new one is added. */
static void apply_firewall(void)
{
    wchar_t cmd[2 * MAX_PATH + 256];
    DWORD rc;
    int tries;

    if (!wait_service_running(L"MpsSvc", 120000)) {
        LOG("Streaming: firewall service not running; rule not set.");
        return;
    }
    for (tries = 0; tries < 30 && g_running; tries++) {
        swprintf_s(cmd, 2 * MAX_PATH + 256, L"netsh advfirewall firewall delete rule name=\"%s\"", FIREWALL_RULE);
        run_hidden(cmd, 30000);
        swprintf_s(cmd, 2 * MAX_PATH + 256, L"netsh advfirewall firewall show rule name=\"%s\"", FIREWALL_RULE);
        if (run_hidden(cmd, 30000) == 1)     /* "No rules match the specified criteria." */
            break;
        Sleep(2000);
    }
    if (tries == 30 || !g_running) {
        LOG("Streaming: cannot replace the firewall rule; leaving the existing one.");
        return;
    }
    if (!only_chars(g_subnet, L"0123456789./")) {
        LOG("Streaming: no valid AllowedSubnet, Sunshine stays unreachable from the network.");
        return;
    }
    swprintf_s(cmd, 2 * MAX_PATH + 256,
               L"netsh advfirewall firewall add rule name=\"%s\" dir=in action=allow program=\"%s\" "
               L"remoteip=%s profile=any enable=yes", FIREWALL_RULE, g_exe, g_subnet);
    rc = run_hidden(cmd, 30000);
    LOG("Streaming: firewall rule for %ls %s (netsh exit %lu).", g_subnet, rc == 0 ? "set" : "FAILED", rc);
}

/* ---- session ---- */

/* Transparency effects off, once per user (appsandbox-displays --no-transparency). */
static void apply_transparency_default(HANDLE token)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32], *slash;
    char out[64];
    DWORD code = 0;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe, MAX_PATH, L"appsandbox-displays.exe");
    swprintf_s(cmd, MAX_PATH + 32, L"\"%s\" --no-transparency", exe);
    if (run_as_user(token, cmd, NULL, 15000, out, sizeof(out), &code, NULL) && strcmp(out, "kept") != 0)
        LOG("Streaming: transparency effects default: %s (exit %lu).", out[0] ? out : "(no output)", code);
}

/* Make the App Sandbox display the only one, primary, at the raw viewer's size (registry
   ViewerWidth/ViewerHeight, which the VDD offers as a mode) or else native. FALSE on error;
   *native says whether the display is now at its native mode. */
static BOOL apply_layout(HANDLE token, BOOL *native)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 48], size[32] = L"", *slash;
    char out[256];
    DWORD code = 0, w = 0, h = 0, len = sizeof(DWORD);
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, VDD_REG_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegGetValueW(key, NULL, L"ViewerWidth", RRF_RT_REG_DWORD, NULL, &w, &len) == ERROR_SUCCESS &&
            (len = sizeof(DWORD), RegGetValueW(key, NULL, L"ViewerHeight", RRF_RT_REG_DWORD, NULL, &h, &len)) == ERROR_SUCCESS)
            swprintf_s(size, 32, L" %lux%lu", w, h);
        RegCloseKey(key);
    }
    *native = size[0] == L'\0';
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe, MAX_PATH, L"appsandbox-displays.exe");
    swprintf_s(cmd, MAX_PATH + 48, L"\"%s\" --layout%s", exe, size);
    if (!run_as_user(token, cmd, NULL, 15000, out, sizeof(out), &code, NULL)) return FALSE;
    if (strncmp(out, "ok ", 3) != 0)
        LOG("Streaming: display layout%ls: %s (exit %lu).", size, out[0] ? out : "(no output)", code);
    return code == 0;
}

/* The host monitor's scaling (registry Streaming\Scale, written at provisioning), once per user
   (appsandbox-displays --scale). Windows keeps scaling as steps from the scale it recommends for
   the current mode, so it is set only at the native mode, the one the host value is for. */
static void apply_scale_default(HANDLE token)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32], *slash;
    char out[64];
    DWORD scale = 0, len = sizeof(scale), code = 1;

    if (g_scale_done) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, STREAM_REG_KEY, L"Scale", RRF_RT_REG_DWORD, NULL, &scale, &len) != ERROR_SUCCESS ||
        scale < 100 || scale > 500) {
        g_scale_done = TRUE;
        return;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe, MAX_PATH, L"appsandbox-displays.exe");
    swprintf_s(cmd, MAX_PATH + 32, L"\"%s\" --scale %lu", exe, scale);
    if (!run_as_user(token, cmd, NULL, 15000, out, sizeof(out), &code, NULL)) return;
    if (code == 0) g_scale_done = TRUE;
    if (strcmp(out, "kept") != 0)
        LOG("Streaming: display scaling default %lu%%: %s (exit %lu).", scale, out[0] ? out : "(no output)", code);
}

/* Apply Sunshine web UI credentials waiting in the registry, then delete the password. Run as
   the user, like Sunshine itself, so nothing in the user-writable config runs as SYSTEM. */
static void apply_credentials(HANDLE token)
{
    wchar_t user[64], pass[128], cmd[MAX_PATH + 256];
    char out[512];
    DWORD code = 1;
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STREAM_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    reg_sz(key, L"User", user, 64);
    reg_sz(key, L"Password", pass, 128);
    RegCloseKey(key);
    if (!pass[0]) return;
    if (!only_chars(user, ALNUM) || !only_chars(pass, ALNUM)) {
        LOG("Streaming: User/Password must be letters and digits; credentials not applied.");
        return;
    }
    swprintf_s(cmd, MAX_PATH + 256, L"\"%s\" --creds %s %s", g_exe, user, pass);
    SecureZeroMemory(pass, sizeof(pass));
    run_as_user(token, cmd, g_dir, 30000, out, sizeof(out), &code, NULL);
    SecureZeroMemory(cmd, sizeof(cmd));
    if (code == 0) {
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, STREAM_REG_KEY, L"Password");
        LOG("Streaming: Sunshine credentials set for '%ls'.", user);
    } else {
        LOG("Streaming: setting Sunshine credentials failed (exit %lu): %s", code, out);
    }
}

static void start_sunshine(HANDLE token, DWORD session)
{
    wchar_t cmd[MAX_PATH + 4];
    swprintf_s(cmd, MAX_PATH + 4, L"\"%s\"", g_exe);
    if (run_as_user(token, cmd, g_dir, 0, NULL, 0, NULL, &g_sunshine)) {
        g_sunshine_session = session;
        LOG("Streaming: Sunshine started (PID %lu, session %lu).", GetProcessId(g_sunshine), session);
    }
}

static DWORD WINAPI streaming_thread(LPVOID param)
{
    ULONGLONG next_install = 0, install_wait = 30000, next_start = 0, last_layout = 0;
    BOOL ready;
    (void)param;

    LOG("Streaming: enabled (Sunshine %s in %ls).", SUNSHINE_VERSION, g_dir);
    apply_firewall();
    kill_stray_sunshine();
    ready = sunshine_installed();

    while (g_running) {
        ULONGLONG now = GetTickCount64();
        wchar_t subnet[64] = L"";

        AcquireSRWLockExclusive(&g_subnet_lock);
        if (g_new_subnet[0]) {
            wcscpy_s(subnet, 64, g_new_subnet);
            wcscpy_s(g_subnet, 64, g_new_subnet);
            g_new_subnet[0] = L'\0';
        }
        ReleaseSRWLockExclusive(&g_subnet_lock);
        if (subnet[0]) {
            LOG("Streaming: NAT subnet is now %ls.", subnet);
            RegSetKeyValueW(HKEY_LOCAL_MACHINE, STREAM_REG_KEY, L"AllowedSubnet", REG_SZ, subnet,
                            (DWORD)((wcslen(subnet) + 1) * sizeof(wchar_t)));
            apply_firewall();
        }

        if (!ready) {
            /* First boot: the network may not be up yet, so retry with a growing wait. */
            if (now >= next_install) {
                ready = install_sunshine();
                if (!ready) {
                    LOG("Streaming: install failed, retrying in %llu s.", install_wait / 1000);
                    next_install = now + install_wait;
                    if (install_wait < 600000) install_wait *= 2;
                }
            }
        } else {
            DWORD session = WTSGetActiveConsoleSessionId();
            HANDLE token = NULL;
            BOOL alive = g_sunshine && WaitForSingleObject(g_sunshine, 0) == WAIT_TIMEOUT;

            if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &token)) {
                if (g_sunshine) {
                    LOG("Streaming: nobody signed in.");
                    stop_sunshine();
                }
            } else {
                if (alive && session != g_sunshine_session) {
                    LOG("Streaming: console session changed %lu -> %lu.", g_sunshine_session, session);
                    stop_sunshine();
                    alive = FALSE;
                }
                if (!alive) {
                    if (g_sunshine) {
                        DWORD code = 0;
                        GetExitCodeProcess(g_sunshine, &code);
                        LOG("Streaming: Sunshine exited (code %lu).", code);
                        CloseHandle(g_sunshine);
                        g_sunshine = NULL;
                    }
                    if (now >= next_start) {
                        BOOL native;
                        apply_transparency_default(token);
                        if (apply_layout(token, &native) && native)
                            apply_scale_default(token);
                        apply_credentials(token);
                        start_sunshine(token, session);
                        last_layout = now;
                        next_start = now + RESPAWN_DELAY_MS;
                    }
                } else if (g_layout_now || now - last_layout >= LAYOUT_INTERVAL_MS) {
                    /* A new raw viewer size waits for the VDD to offer it (it polls once a
                       second), so a failed switch is retried on the next loop. */
                    BOOL requested = InterlockedExchange(&g_layout_now, 0) != 0, native;
                    if (!apply_layout(token, &native)) {
                        if (requested) InterlockedExchange(&g_layout_now, 1);
                    } else if (native) {
                        apply_scale_default(token);
                    }
                    last_layout = now;
                }
                CloseHandle(token);
            }
        }

        for (int i = 0; i < 4 && g_running; i++)
            Sleep(500);
    }

    stop_sunshine();
    LOG("Streaming: stopped.");
    return 0;
}

void streaming_start(StreamLogFn log)
{
    DWORD enabled = 0, size = sizeof(enabled);
    HKEY key;

    g_log = log;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STREAM_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;
    RegGetValueW(key, NULL, L"Enabled", RRF_RT_REG_DWORD, NULL, &enabled, &size);
    reg_sz(key, L"AllowedSubnet", g_subnet, 64);
    RegCloseKey(key);
    if (!enabled) return;

#if !defined(_M_X64)
    LOG("Streaming: only x64 guests are supported; not starting.");
    return;
#else
    if (!GetEnvironmentVariableW(L"ProgramData", g_base, MAX_PATH))
        wcscpy_s(g_base, MAX_PATH, L"C:\\ProgramData");
    wcscat_s(g_base, MAX_PATH, L"\\AppSandbox");
    swprintf_s(g_dir, MAX_PATH, L"%s\\Sunshine", g_base);
    swprintf_s(g_exe, MAX_PATH, L"%s\\sunshine.exe", g_dir);

    g_running = TRUE;
    g_thread = CreateThread(NULL, 0, streaming_thread, NULL, 0, NULL);
    if (!g_thread) {
        g_running = FALSE;
        LOG("Streaming: cannot start thread (%lu).", GetLastError());
    }
#endif
}

void streaming_viewer_size(unsigned width, unsigned height)
{
    HKEY key;
    if (!g_thread) return;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, VDD_REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    if (width && height) {
        DWORD w = width, h = height;
        RegSetValueExW(key, L"ViewerWidth", 0, REG_DWORD, (const BYTE *)&w, sizeof(w));
        RegSetValueExW(key, L"ViewerHeight", 0, REG_DWORD, (const BYTE *)&h, sizeof(h));
        LOG("Streaming: raw viewer size %ux%u.", width, height);
    } else {
        RegDeleteValueW(key, L"ViewerWidth");
        RegDeleteValueW(key, L"ViewerHeight");
        LOG("Streaming: native size (Moonlight).");
    }
    RegCloseKey(key);
    InterlockedExchange(&g_layout_now, 1);
}

void streaming_network_changed(const char *ip, int prefix)
{
    unsigned a, b, c, d;
    DWORD net;
    wchar_t subnet[64];

    if (!g_thread || prefix < 8 || prefix > 30 || sscanf_s(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255)
        return;
    net = ((a << 24) | (b << 16) | (c << 8) | d) & (0xFFFFFFFFu << (32 - prefix));
    swprintf_s(subnet, 64, L"%lu.%lu.%lu.%lu/%d", net >> 24, (net >> 16) & 255, (net >> 8) & 255,
               net & 255, prefix);
    AcquireSRWLockExclusive(&g_subnet_lock);
    if (wcscmp(subnet, g_subnet) != 0)
        wcscpy_s(g_new_subnet, 64, subnet);
    ReleaseSRWLockExclusive(&g_subnet_lock);
}

void streaming_stop(void)
{
    if (!g_thread) return;
    g_running = FALSE;
    WaitForSingleObject(g_thread, 15000);
    CloseHandle(g_thread);
    g_thread = NULL;
}
