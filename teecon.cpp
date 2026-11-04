// teecon.cpp - run a command inside a ConPTY, relay it to this console,
//                and tee flat text to a log file (-r for the raw VT stream).
//                The log file is the first argument (positional); -a appends,
//                otherwise each run replaces it.
//
    #define TEECON_VERSION L"1.0" 
// (C) 2026 Alan Robinson
//
// Toolchain: WinDDK 7600.16385.1 (VC9 / C++03) from a Windows 7 build environment.
//   (build.exe via the stock one-line makefile + per-project sources file)
//
// Usage:  teecon log.txt -a -r powershell.exe -NoExit
//         teecon log.txt -d C:\work -new cmd.exe /k echo hi
//         teecon run.log -- -weird.exe
//
// The child keeps a real console of its own: color, prompt editing, fullscreen
// programs behave as if launched from Explorer. Every output chunk is written
// to the log as it passes through and the log is flushed on a tick, so the
// transcript body grows while the window is still open.
//
// The log is flat text by default: a small VT parser resolves the cursor moves
// cmd.exe actually emits (backspace corrections, CR-overwrite, arrow edits,
// erase-to-end) into plain lines, so the file reads line-by-line with no
// escape bytes. -r keeps the raw VT stream instead. The console always shows
// the raw stream untouched, either way. Exit code: the child's exit code
// passes through.
//

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// ConPTY needs Windows 10 1809 (build 17763). Target Win10 so the SDK 6.1
// fallbacks below stay honest; the entry points themselves are resolved at
// runtime via GetProcAddress, so this binary still loads pre-1809 and prints
// a clear "unavailable" message instead of failing to start.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

// ---- SDK 6.1 fallbacks (ConPTY is a Win10-1809 API) ---------------------
#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
#define PSEUDOCONSOLE_INHERIT_CURSOR 0x1
typedef VOID *HPCON;
#endif

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
// ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

// ---------------------------------------------------------------- output
// Unicode-safe: WriteConsoleW on a console, UTF-8 bytes when redirected.
static void out(HANDLE h, const wchar_t *fmt, ...) {
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 4095, fmt, ap);
    va_end(ap);
    buf[4095] = 0;

    DWORD n = (DWORD)wcslen(buf), w, mode;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, buf, n, &w, NULL);
    } else {
        char u8[4096 * 4];
        int l = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, u8, (int)sizeof u8, NULL, NULL);
        if (l > 0) WriteFile(h, u8, (DWORD)l, &w, NULL);
    }
}
// windef.h already defines OUT (0x40000000 - a memory-access flag). This file
// wants its own reporting macro, and the build compiles /W3 /WX, so a second
// definition is C4005 -> fatal C2220 -> no .obj at all. Drop theirs first.
#undef OUT
#define OUT(...) out(GetStdHandle(STD_OUTPUT_HANDLE), __VA_ARGS__)
#define ERR(...) out(GetStdHandle(STD_ERROR_HANDLE),  __VA_ARGS__)

// ---------------------------------------------------------------- ConPTY session
// All three ConPTY entry points are resolved with GetProcAddress so the
// binary still loads on pre-1809 Windows (CpLoad just returns FALSE).

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void    (WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole pfnCreate = NULL;
static PFN_ResizePseudoConsole pfnResize = NULL;
static PFN_ClosePseudoConsole  pfnClose  = NULL;

typedef struct CpSession {
    HPCON  hPC;      // the pseudoconsole
    HANDLE hPtyIn;   // WRITE here -> becomes the child's console input
    HANDLE hPtyOut;  // READ here  <- the child's console output (VT)
    HANDLE hProc;    // child process
    DWORD  pid;
} CpSession;

static BOOL CpLoad(void) {
    HMODULE mod = NULL;
    wchar_t path[MAX_PATH];
    DWORD   n;
    wchar_t *slash;

    if (pfnCreate) return TRUE;

    // Prefer a side-by-side conpty.dll (NuGet package) so behaviour is a
    // property of our build rather than of the target machine's patch
    // level. Fall back to the inbox implementation in kernel32.
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        slash = wcsrchr(path, L'\\');
        if (slash && (size_t)(slash - path) + 12 < MAX_PATH) {
            wcscpy(slash + 1, L"conpty.dll");
            mod = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
    }
    if (!mod) mod = GetModuleHandleW(L"kernel32.dll");
    if (!mod) return FALSE;

    pfnCreate = (PFN_CreatePseudoConsole)(void *)GetProcAddress(mod, "CreatePseudoConsole");
    pfnResize = (PFN_ResizePseudoConsole)(void *)GetProcAddress(mod, "ResizePseudoConsole");
    pfnClose  = (PFN_ClosePseudoConsole)(void *)GetProcAddress(mod, "ClosePseudoConsole");

    if (!pfnCreate || !pfnResize || !pfnClose) {
        pfnCreate = NULL;
        pfnResize = NULL;
        pfnClose  = NULL;
        return FALSE;
    }
    return TRUE;
}

// cmdline MUST be writable (CreateProcessW scribbles on it).
// workdir may be NULL. Returns FALSE + GetLastError() on failure.
static BOOL CpStart(CpSession *s, wchar_t *cmdline, const wchar_t *workdir, COORD size) {
    HANDLE inRead = NULL, inWrite = NULL, outRead = NULL, outWrite = NULL;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T  bytes = 0;
    HRESULT hr;
    DWORD   err = 0;
    BOOL    ok = FALSE;

    ZeroMemory(s, sizeof(*s));
    ZeroMemory(&pi, sizeof(pi));

    if (!CpLoad()) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    if (size.X < 1) size.X = 80;
    if (size.Y < 1) size.Y = 25;

    if (!CreatePipe(&inRead,  &inWrite,  NULL, 0)) goto done;
    if (!CreatePipe(&outRead, &outWrite, NULL, 0)) goto done;

    // hInput  = what the pty READS from -> our write end feeds it
    // hOutput = what the pty WRITES to  -> our read end drains it
    hr = pfnCreate(size, inRead, outWrite, 0, &s->hPC);
    if (FAILED(hr)) {
        SetLastError(HRESULT_CODE(hr) ? (DWORD)HRESULT_CODE(hr) : (DWORD)hr);
        goto done;
    }

    // The pty duplicated what it needs; drop our copies of its ends.
    // Leaving these open means the pipes never break on teardown.
    CloseHandle(inRead);   inRead   = NULL;
    CloseHandle(outWrite); outWrite = NULL;

    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);

    // First call always fails with ERROR_INSUFFICIENT_BUFFER.
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!attrs) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &bytes)) {
        HeapFree(GetProcessHeap(), 0, attrs);
        attrs = NULL;
        goto done;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   s->hPC, sizeof(s->hPC), NULL, NULL))
        goto done;

    si.lpAttributeList = attrs;

    // bInheritHandles = FALSE: the pty is handed over via the attribute
    // list, not via handle inheritance.
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        NULL, workdir, &si.StartupInfo, &pi))
        goto done;

    CloseHandle(pi.hThread);
    s->hProc   = pi.hProcess;
    s->pid     = pi.dwProcessId;
    s->hPtyIn  = inWrite;  inWrite = NULL;
    s->hPtyOut = outRead;  outRead = NULL;
    ok = TRUE;

done:
    if (!ok) err = GetLastError();
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
    if (inRead)   CloseHandle(inRead);
    if (outWrite) CloseHandle(outWrite);
    if (!ok) {
        if (inWrite) CloseHandle(inWrite);
        if (outRead) CloseHandle(outRead);
        if (s->hPC && pfnClose) pfnClose(s->hPC);
        ZeroMemory(s, sizeof(*s));
        SetLastError(err);
    }
    return ok;
}

static BOOL CpResize(CpSession *s, COORD size) {
    HRESULT hr;
    if (!s || !s->hPC || !pfnResize) return FALSE;
    if (size.X < 1 || size.Y < 1) return FALSE;
    hr = pfnResize(s->hPC, size);
    return SUCCEEDED(hr) ? TRUE : FALSE;
}

// Step 1 of teardown. Flushes pending output and breaks hPtyOut, which is
// what makes the reader thread's ReadFile return FALSE.
// The reader thread MUST still be running when you call this or it can
// block forever.
static void CpShutdownPty(CpSession *s) {
    if (s && s->hPC && pfnClose) {
        pfnClose(s->hPC);   // flushes; reader thread must still be alive
        s->hPC = NULL;
    }
}

// Step 2 of teardown. Call only after the reader AND writer threads are
// done touching hPtyOut / hPtyIn.
static void CpFree(CpSession *s) {
    if (!s) return;
    CpShutdownPty(s);
    if (s->hPtyIn)  CloseHandle(s->hPtyIn);
    if (s->hPtyOut) CloseHandle(s->hPtyOut);
    if (s->hProc)   CloseHandle(s->hProc);
    ZeroMemory(s, sizeof(*s));
}

// ---------------------------------------------------------------- console mode save/restore
static HANDLE g_hIn   = INVALID_HANDLE_VALUE;
static HANDLE g_hOut  = INVALID_HANDLE_VALUE;
static DWORD  g_inMode = 0, g_outMode = 0;
static UINT   g_inCP = 0, g_outCP = 0;
static LONG   g_restored = 1;

static void RestoreConsole(void) {
    if (InterlockedExchange(&g_restored, 1) != 0) return;
    SetConsoleMode(g_hIn,  g_inMode);
    SetConsoleMode(g_hOut, g_outMode);
    SetConsoleCP(g_inCP);
    SetConsoleOutputCP(g_outCP);
}

static BOOL WINAPI CtrlHandler(DWORD type) {
    (void)type;
    RestoreConsole();
    return FALSE;   // let the default handler terminate us
}

// With ENABLE_PROCESSED_INPUT off, Ctrl+C arrives as byte 0x03 on stdin and
// is forwarded into the pty, which turns it into a real CTRL_C_EVENT for
// the child. This handler only covers Ctrl+Break / close / logoff.
static BOOL SetupConsole(void) {
    BOOL haveIn, haveOut;

    g_hIn  = GetStdHandle(STD_INPUT_HANDLE);
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    haveIn  = GetConsoleMode(g_hIn,  &g_inMode);
    haveOut = GetConsoleMode(g_hOut, &g_outMode);
    g_inCP  = GetConsoleCP();
    g_outCP = GetConsoleOutputCP();

    if (!haveIn && !haveOut) return FALSE;  // fully redirected: no modes

    g_restored = 0;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // ConPTY speaks UTF-8 in both directions.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (haveOut)
        SetConsoleMode(g_hOut, g_outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    if (haveIn)
        SetConsoleMode(g_hIn, (g_inMode & ~(ENABLE_LINE_INPUT
                                          | ENABLE_ECHO_INPUT
                                          | ENABLE_PROCESSED_INPUT))
                              | ENABLE_VIRTUAL_TERMINAL_INPUT);
    return TRUE;
}

// ---------------------------------------------------------------- helpers
static BOOL WriteAll(HANDLE h, const BYTE *p, DWORD n) {
    DWORD w;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return TRUE;
    while (n) {
        if (!WriteFile(h, p, n, &w, NULL) || w == 0) return FALSE;
        p += w;
        n -= w;
    }
    return TRUE;
}

static COORD ConsoleSize(HANDLE hOut) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD c;
    c.X = 80;
    c.Y = 25;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        c.X = (SHORT)(csbi.srWindow.Right  - csbi.srWindow.Left + 1);
        c.Y = (SHORT)(csbi.srWindow.Bottom - csbi.srWindow.Top  + 1);
        if (c.X < 1) c.X = 80;
        if (c.Y < 1) c.Y = 25;
    }
    return c;
}

// ---------------------------------------------------------------- threads
#define OUT_BUF 8192
#define IN_BUF  1024
#define TICK_MS 200

typedef struct {
    HANDLE ptyOut;  // read from pty
    HANDLE conOut;  // write to our console (raw stream, always)
    HANDLE log;     // tee target, may be NULL
    BOOL   raw;     // TRUE: log the raw VT stream; FALSE: flat text via VtFeed
} OutCtx;

// ---------------------------------------------------------------- VT flat-text filter
// One line buffer + one column index, fed UTF-8 bytes, flushed on LF.
// Covers cmd.exe interactive line editing: BS/Left/Right/Home/End moves,
// insert/erase/delete at the cursor (IC/X/DCH), erase-to-start/end/all (K),
// CR-overwrite, column/row moves (G/H/f/d/E/F), history repaints (A/B via
// the CUP/EL that follows). Everything else (SGR colors, title sets, shell
// integration strings, focus/keyboard privates, show/hide) is consumed and
// dropped. History recall is deliberately passive: Up/Down only reposition,
// the repaint carries the recalled text; fullscreen addressing smears.
// Non-ASCII UTF-8 bytes pass through untouched; column counts bytes, which
// only misplaces overwrites inside CJK-wide runs - accepted, cmd.exe output
// here is ASCII. Full-screen programs smear; out of contract, use -r for those.
// Invalid/unfinished sequences at chunk end carry over via the held state.

#define VT_LINE 4096
#define VT_MAX_PARAMS 8

typedef struct {
    BYTE line[VT_LINE];  // current line bytes
    size_t len;          // valid bytes in line
    size_t col;          // cursor column (byte offset into line)
    int row;             // last CUP row (1-based; 0 = unseen, then carried)
    BYTE seq[64];        // in-progress escape sequence bytes
    size_t seqlen;
} VtState;

static BOOL VtEmitLine(VtState *v, HANDLE log) {
    // Trim trailing spaces the erase dance leaves behind, then LF.
    while (v->len && (v->line[v->len - 1] == ' ' || v->line[v->len - 1] == '\t'))
        v->len--;
    {
        BYTE nl = '\n';
        if (v->len && !WriteAll(log, v->line, (DWORD)v->len)) return FALSE;
        if (!WriteAll(log, &nl, 1)) return FALSE;
    }
    v->len = 0;
    v->col = 0;
    if (v->row) v->row++;  // LF moves down one row; carry it so the next
                           // CUP can tell a new prompt from a same-line edit
    return TRUE;
}

static void VtPut(VtState *v, BYTE b) {
    if (v->col < v->len) {
        v->line[v->col++] = b;  // overwrite at cursor (arrow/CR/CSI-G flow)
    } else {
        while (v->len < v->col && v->len < VT_LINE - 1)
            v->line[v->len++] = ' ';
        if (v->len < VT_LINE - 1) {
            v->line[v->len++] = b;
            v->col = v->len;
        }
    }
}

// Parse and apply one complete CSI sequence held in v->seq. Returns FALSE
// if a log write failed (row-change flush); unknown/unsupported sequences
// are dropped (the narrow-strip rule).
static BOOL VtApplyCsi(VtState *v, HANDLE log) {
    int params[VT_MAX_PARAMS];
    int np = 0;
    size_t i;
    int cur = -1;  // -1 = no digits yet for this param
    BYTE final = 0;
    int n;

    if (v->seqlen < 3 || v->seq[0] != 0x1B || v->seq[1] != '[') return TRUE;
    for (i = 2; i < v->seqlen; i++) {
        BYTE b = v->seq[i];
        if (b >= '0' && b <= '9') {
            if (cur < 0) cur = 0;
            cur = cur * 10 + (b - '0');
        } else if (b == ';') {
            if (np < VT_MAX_PARAMS) params[np++] = (cur < 0) ? 0 : cur;
            cur = -1;
        } else if (b == '?' || b == '>' || b == '!' || b == ' ') {
            // private prefixes / intermediates: keep scanning, drop them
        } else {
            final = b;  // first non-param byte ends the sequence
            break;
        }
    }
    if (!final) return TRUE;
    if (np < VT_MAX_PARAMS) params[np++] = (cur < 0) ? 0 : cur;
    n = params[0];

    switch (final) {
    case '@':  // ICH: insert blanks (CSI n @), default 1
        {
            size_t k, ins;
            if (n < 1) n = 1;
            ins = (size_t)n;
            if (v->col > v->len) v->col = v->len;
            // Push the tail right to make room, then blank the gap.
            if (ins > VT_LINE - 1 - v->len)
                ins = VT_LINE - 1 - v->len;
            for (k = v->len; k > v->col; k--)
                v->line[k + ins - 1] = v->line[k - 1];
            v->len += ins;
            for (k = 0; k < ins; k++)
                v->line[v->col + k] = ' ';
        }
        break;
    case 'D':  // CUB: cursor back (CSI n D / Left, default 1)
        if (n < 1) n = 1;
        v->col = (v->col > (size_t)n) ? v->col - (size_t)n : 0;
        break;
    case 'C':  // CUF: cursor forward (CSI n C / Right, default 1)
        if (n < 1) n = 1;
        v->col += (size_t)n;
        if (v->col > v->len) v->col = v->len;
        break;
    case 'A':  // CUU: cursor up (history) - leave the flat line alone
    case 'B':  // CUD: cursor down (history) - a history recall repaints
               // the whole input line; the child follows with CUP/EL,
               // which is where the flat line actually updates
        break;
    case 'H':  // CUP: row;col (CSI n;m H) - a new row starts a new line
    case 'f':  // HVP: same shape as CUP (no screen model; rows are lines)
        {
            int row1 = params[0];
            int col1 = (np >= 2) ? params[1] : 1;
            if (row1 < 1) row1 = 1;
            if (col1 < 1) col1 = 1;
            if (v->row && row1 != v->row && (v->len || v->col)) {
                if (!VtEmitLine(v, log)) return FALSE;
            }
            v->row = row1;
            v->col = (size_t)(col1 - 1);
            if (v->col > v->len) v->col = v->len;
        }
        break;
    case 'E':  // CNL: line down + column 0 (CR LF in one sequence)
        if (n < 1) n = 1;
        while (n-- > 0) {
            if (!VtEmitLine(v, log)) return FALSE;
        }
        break;
    case 'F':  // CPL: line up + column 0 - a same-screen move, drop the
               // positioning; the repaint that follows carries the text
        break;
    case 'G':  // CHA: column absolute (CSI n G / Home+col, default 1)
        // Home is CSI 1 G: col 0, keep the flat line. Mid-line CHA moves
        // the cursor; Home+text+LF reprints the whole input line, so LF
        // carries the final content (the jk/jkopjojj case: echo kept jk,
        // the shell's own error line carried jkopjojj).
        if (n < 1) n = 1;
        v->col = (size_t)(n - 1);
        if (v->col > v->len) v->col = v->len;
        break;
    case 'd':  // VPA: row absolute (CSI n d) - same rule as CUP
        if (n < 1) n = 1;
        if (v->row && n != v->row && (v->len || v->col)) {
            if (!VtEmitLine(v, log)) return FALSE;
        }
        v->row = n;
        v->col = 0;
        break;
    case 'X':  // ECH: erase n chars at cursor (CSI n X), default 1
        // Blanks in place, no shift (trailing blanks trim at emit).
        if (n < 1) n = 1;
        {
            size_t k, cnt = (size_t)n;
            if (v->col > v->len) v->col = v->len;
            for (k = 0; k < cnt && v->col + k < v->len; k++)
                v->line[v->col + k] = ' ';
        }
        break;
    case 'P':  // DCH: delete n chars at cursor (CSI n P), default 1
        if (n < 1) n = 1;
        {
            size_t k, cnt = (size_t)n;
            if (v->col + cnt < v->len) {
                size_t tail = v->len - (v->col + cnt);
                for (k = 0; k < tail; k++)
                    v->line[v->col + k] = v->line[v->col + cnt + k];
                v->len -= cnt;
            } else if (v->col < v->len) {
                v->len = v->col;
            }
        }
        break;
    case 'K':  // EL: erase in line (CSI n K) - modes 0/1/2 supported
        if (n == 0) { if (v->col < v->len) v->len = v->col; }
        else if (n == 1) {
            // Erase from start through the cursor: blank then drop.
            size_t k;
            for (k = 0; k < v->col && k < v->len; k++)
                v->line[k] = ' ';
            if (v->col < v->len) {
                size_t tail = v->len - v->col;
                for (k = 0; k < tail; k++)
                    v->line[k] = v->line[v->col + k];
                v->len = tail;
                v->col = 0;
            } else {
                v->len = 0;
                v->col = 0;
            }
        }
        else if (n == 2) { v->len = 0; v->col = 0; }
        break;
    case 'J':  // ED: erase in display - a clear starts a fresh screenful
        if (n == 2 || n == 3) { v->len = 0; v->col = 0; }
        break;
    default:
        break;  // SGR 'm', show/hide 'h'/'l', etc: dropped
    }
    return TRUE;
}

// Feed one chunk; returns FALSE if a log write failed (caller breaks).
static BOOL VtFeed(VtState *v, const BYTE *p, DWORD n, HANDLE log) {
    DWORD i;
    for (i = 0; i < n; i++) {
        BYTE b = p[i];
        if (v->seqlen) {
            // Inside an escape sequence. The '[' / ']' introducer itself
            // must NOT close the sequence: ESC [ is 2 bytes, the CSI body
            // follows; ESC ] opens an OSC string that runs to BEL or ST.
            if (v->seqlen < ARRAYSIZE(v->seq) - 1) {
                v->seq[v->seqlen++] = b;
            } else {
                v->seqlen = 0;  // overflow: lose one byte, catch the next ESC
                if (b == 0x1B) { v->seq[0] = b; v->seqlen = 1; }
                continue;
            }
            if (v->seqlen == 2) {
                BYTE s1 = v->seq[1];
                if (s1 == '[' || s1 == ']' || s1 == 'P' ||
                    s1 == 'X' || s1 == '^' || s1 == '_' ||
                    s1 == '(' || s1 == ')' || s1 == '#') {
                    continue;  // multi-byte sequence: keep accumulating
                }
                // Other ESC + single final (M, =, >, 7, 8, c...): dropped.
                v->seqlen = 0;
                continue;
            }
            {
                BYTE s1 = v->seq[1];
                if (s1 == '[') {
                    // CSI: params/intermediates accumulate; a byte in
                    // 0x40-0x7E after the introducer ends it.
                    if (b >= 0x40 && b <= 0x7E) {
                        if (!VtApplyCsi(v, log)) return FALSE;
                        v->seqlen = 0;
                    }
                } else if (s1 == ']' || s1 == 'P' || s1 == 'X' ||
                           s1 == '^' || s1 == '_') {
                    // OSC/DCS/SOS/PM/APC strings: BEL or ST (ESC \) ends.
                    if (b == 0x07) {
                        v->seqlen = 0;
                    } else if (b == '\\' && v->seqlen >= 2 &&
                               v->seq[v->seqlen - 2] == 0x1B) {
                        v->seqlen = 0;
                    }
                    // Otherwise keep accumulating: ordinary finals inside
                    // a string (like the ';' or 'h' in 9001;...h) are data.
                } else {
                    // Charset selects (ESC ( X) and ESC # X: 3 bytes total.
                    v->seqlen = 0;
                }
            }
            continue;
        }
        switch (b) {
        case 0x1B:  // ESC: start a sequence
            v->seq[0] = b;
            v->seqlen = 1;
            break;
        case '\r':  // CR: rewind column; CR LF handled by the LF arm
            v->col = 0;
            break;
        case '\n':  // LF: flush the line, row advances if known
            if (!VtEmitLine(v, log)) return FALSE;
            break;
        case '\b':  // BS: non-destructive step left (Left arrow emits this;
            if (v->col > 0) v->col--;  // the BS-SP-BS erase still works: SP
            break;                    // blanks, trailing blanks trim at emit)
        case '\t':  // TAB: advance to next multiple of 8
            do { VtPut(v, ' '); } while ((v->col % 8) != 0);
            break;
        default:
            if (b < 0x20) break;  // BEL, SO/SI, XON/XOFF...: dropped
            VtPut(v, b);          // printable + UTF-8 continuation bytes
            break;
        }
    }
    return TRUE;
}

static DWORD WINAPI OutputThread(LPVOID arg) {
    OutCtx *c = (OutCtx *)arg;
    BYTE buf[OUT_BUF];
    VtState vt;
    DWORD n;

    ZeroMemory(&vt, sizeof(vt));

    for (;;) {
        if (!ReadFile(c->ptyOut, buf, OUT_BUF, &n, NULL) || n == 0)
            break;  // pty closed -> ERROR_BROKEN_PIPE
        // Record-then-display: the WriteFile result still gates the echo
        // (never show output we failed to record), but the expensive disk
        // commit stays off the keystroke path - the tick thread flushes
        // within 200 ms, which is the agent-visibility guarantee.
        if (c->log) {
            BOOL ok;
            if (c->raw) {
                ok = WriteAll(c->log, buf, n);
            } else {
                ok = VtFeed(&vt, buf, n, c->log);
            }
            if (!ok) break;
        }
        if (!WriteAll(c->conOut, buf, n))
            break;
    }
    return 0;
}

typedef struct {
    HANDLE conIn;   // read from our console
    HANDLE ptyIn;   // write into pty
} InCtx;

static DWORD WINAPI InputThread(LPVOID arg) {
    InCtx *c = (InCtx *)arg;
    BYTE buf[IN_BUF];
    DWORD n;

    for (;;) {
        if (!ReadFile(c->conIn, buf, IN_BUF, &n, NULL) || n == 0)
            break;
        if (!WriteAll(c->ptyIn, buf, n))
            break;
    }
    return 0;
}

typedef struct {
    CpSession *sess;
    HANDLE     conOut;
    HANDLE     log;
    HANDLE     quit;
    COORD      last;
} TickCtx;

// There is no SIGWINCH on Windows, and ENABLE_VIRTUAL_TERMINAL_INPUT means
// we read bytes rather than WINDOW_BUFFER_SIZE_EVENT records, so resize has
// to be polled. Same thread flushes the log so the hot path stays a plain
// WriteFile. (OutputThread never flushes; this tick is what keeps a reader
// with an open handle fresh, within TICK_MS.)
static DWORD WINAPI TickThread(LPVOID arg) {
    TickCtx *c = (TickCtx *)arg;
    COORD cur;

    while (WaitForSingleObject(c->quit, TICK_MS) == WAIT_TIMEOUT) {
        cur = ConsoleSize(c->conOut);
        if (cur.X != c->last.X || cur.Y != c->last.Y) {
            c->last = cur;
            CpResize(c->sess, cur);  // triggers a full child repaint
        }
        if (c->log) FlushFileBuffers(c->log);
    }
    if (c->log) FlushFileBuffers(c->log);
    return 0;
}

// ---------------------------------------------------------------- log file
static HANDLE OpenLog(const wchar_t *path, const wchar_t *cmdline, BOOL append, BOOL hdr, BOOL raw) {
    HANDLE h;
    SYSTEMTIME st;
    char chdr[1024];
    int  n;

    // Default is replace (CREATE_ALWAYS): the file holds this run only, so
    // the reader starts at line 1. -a appends (OPEN_ALWAYS) for history.
    if (append) {
        h = CreateFileW(path, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        h = CreateFileW(path, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) return NULL;

    // -H only: one header line, no leading blank. Default is no header, so
    // the first log line is the child's own first line.
    if (!hdr) return h;
    GetLocalTime(&st);
    n = _snprintf(chdr, sizeof(chdr),
                  "=== teecon %ls %04d-%02d-%02d %02d:%02d:%02d pid=%lu "
                  "cmd=%ls ===%ls",
                  TEECON_VERSION,
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond,
                  (unsigned long)GetCurrentProcessId(), cmdline,
                  raw ? L"\r\n" : L"\n");  // match the body: raw=CRLF, flat=LF
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(chdr)) n = (int)sizeof(chdr) - 1;
    if (n > 0) WriteAll(h, (const BYTE *)chdr, (DWORD)n);
    return h;
}

// Relaunch ourselves in a brand new console window, Win+R style. The env
// marker avoids having to edit the command line (and avoids a loop).
static BOOL RelaunchNewConsole(void) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *cl;
    size_t n;
    BOOL ok;

    if (GetEnvironmentVariableW(L"TEECON_CHILD", NULL, 0) != 0) return FALSE;
    if (!SetEnvironmentVariableW(L"TEECON_CHILD", L"1")) return FALSE;

    n = wcslen(GetCommandLineW()) + 1;
    cl = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(wchar_t));
    if (!cl) return FALSE;
    wcscpy(cl, GetCommandLineW());

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    ok = CreateProcessW(NULL, cl, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL, &si, &pi);
    HeapFree(GetProcessHeap(), 0, cl);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok;
}

// ---------------------------------------------------------------- usage
static void usage(void) {
    OUT(L"teecon %ls\n", TEECON_VERSION);
    OUT(L"Runs a command in its own interactive console, saving the results to a log file simultaneously.\n");
    OUT(L"Usage: teecon <logfile> [flags] [[--] <command>]\n");
    OUT(L"  <logfile>   transcript file (replace; first line is the child output)\n");
    OUT(L"  -a          append to the log instead of replacing it\n");
    OUT(L"  -r          log the raw VT stream instead of flat text\n");
    OUT(L"  -H          write one header line at the top of the log\n");
    OUT(L"  -d <dir>    working directory for the child\n");
    OUT(L"  -new        relaunch in a brand-new console window\n");
    OUT(L"  --          optional end-of-options (only needed if command starts with -)\n");
    OUT(L"  -h, -?      show this help\n");
    OUT(L"Example: teecon log.txt -a -r powershell.exe -NoExit\n");
    OUT(L"Example: teecon log.txt -d c:\\users cmd.exe /c dir\n");
}

// ---------------------------------------------------------------- main
// Raw command line: skip the program name; the FIRST token is the log file,
// then flags, and everything from the command position onward is the child
// command verbatim (so cmd /K echo A & echo B & exit concatenates into one
// transcript). Spaces preserved, quotes optional throughout.
int main() {
    CpSession sess;
    OutCtx  oc;
    InCtx   ic;
    TickCtx tc;
    HANDLE  thOut = NULL, thIn = NULL, thTick = NULL, quit = NULL;
    HANDLE  log = NULL;
    wchar_t cmdline[32768];
    wchar_t logPath[MAX_PATH * 2];
    wchar_t workDir[MAX_PATH * 2];
    const wchar_t *workDirP = NULL;
    BOOL    wantNew = FALSE;
    BOOL    wantAppend = FALSE;
    BOOL    wantRaw = FALSE;
    BOOL    wantHdr = FALSE;
    BOOL    haveWork = FALSE;
    DWORD   code = 1;
    wchar_t *p, *s;

    ZeroMemory(&sess, sizeof(sess));

    // Skip our own program name.
    p = GetCommandLineW();
    if (*p == L'"') { p++; while (*p && *p != L'"') p++; if (*p) p++; }
    else            { while (*p && *p != L' ' && *p != L'\t') p++; }
    while (*p == L' ' || *p == L'\t') p++;

    // First token is the log file (required, positional). No -f/-l.
    logPath[0] = 0;
    workDir[0] = 0;
    cmdline[0] = 0;
    s = p;
    while (*s == L' ' || *s == L'\t') s++;
    if (!*s) { ERR(L"teecon: need a log file path.\n"); usage(); return 2; }
    p = logPath;
    if (*s == L'"') {
        s++;
        while (*s && *s != L'"' && (size_t)(p - logPath) + 1 < ARRAYSIZE(logPath))
            *p++ = *s++;
        if (*s == L'"') s++;
    } else {
        while (*s && *s != L' ' && *s != L'\t' &&
               (size_t)(p - logPath) + 1 < ARRAYSIZE(logPath))
            *p++ = *s++;
    }
    *p = 0;
    if (!logPath[0]) { ERR(L"teecon: need a log file path.\n"); usage(); return 2; }

    // Parse flags; the first non-flag token starts the child command.
    for (;;) {
        while (*s == L' ' || *s == L'\t') s++;
        if (*s != L'-' && *s != L'/') break;  // command starts here
        if ((s[1] == L'a' || s[1] == L'A') &&
            (s[2] == 0 || s[2] == L' ' || s[2] == L'\t')) {
            wantAppend = TRUE;
            s += 2;
        } else if ((s[1] == L'r' || s[1] == L'R') &&
                   (s[2] == 0 || s[2] == L' ' || s[2] == L'\t')) {
            wantRaw = TRUE;
            s += 2;
        } else if (s[1] == L'H' &&
                   (s[2] == 0 || s[2] == L' ' || s[2] == L'\t')) {
            wantHdr = TRUE;
            s += 2;
        } else if ((s[1] == L'd' || s[1] == L'D') &&
                   (s[2] == 0 || s[2] == L' ' || s[2] == L'\t' || s[2] == L':')) {
            s += 2;
            if (*s == L':') s++;
            while (*s == L' ' || *s == L'\t') s++;
            p = workDir;
            if (*s == L'"') {
                s++;
                while (*s && *s != L'"' && (size_t)(p - workDir) + 1 < ARRAYSIZE(workDir))
                    *p++ = *s++;
                if (*s == L'"') s++;
            } else {
                while (*s && *s != L' ' && *s != L'\t' &&
                       (size_t)(p - workDir) + 1 < ARRAYSIZE(workDir))
                    *p++ = *s++;
            }
            *p = 0;
            if (!workDir[0]) { ERR(L"teecon: -d needs a working directory.\n"); usage(); return 2; }
            haveWork = TRUE;
        } else if ((s[1] == L'n' || s[1] == L'N') &&
                   (s[2] == L'e' || s[2] == L'E') &&
                   (s[3] == L'w' || s[3] == L'W') &&
                   (s[4] == 0 || s[4] == L' ' || s[4] == L'\t')) {
            wantNew = TRUE;
            s += 4;
        } else if (s[1] == L'-' &&
                   (s[2] == 0 || s[2] == L' ' || s[2] == L'\t')) {
            s += 2;  // lone "--": rest of the line is the command
            while (*s == L' ' || *s == L'\t') s++;
            break;
        } else if ((s[1] == L'h' || s[1] == L'?') &&
                   (s[2] == 0 || s[2] == L' ' || s[2] == L'\t')) {
            usage();
            return 0;
        } else {
            break;  // not a flag: command starts here
        }
    }
    while (*s == L' ' || *s == L'\t') s++;

    // Strip one layer of wrapping quotes around the whole command, if any.
    if (*s == L'"') {
        size_t sl = wcslen(s);
        if (sl >= 2 && s[sl - 1] == L'"') {
            s[sl - 1] = 0;
            s++;
            while (*s == L' ' || *s == L'\t') s++;
        }
    }

    if (!*s) {
        wcscpy(cmdline, L"cmd.exe");
    } else {
        if (wcslen(s) >= ARRAYSIZE(cmdline)) { ERR(L"teecon: command too long.\n"); return 2; }
        wcscpy(cmdline, s);
        // Trim trailing whitespace the shell would ignore.
        {
            size_t n = wcslen(cmdline);
            while (n && (cmdline[n - 1] == L' ' || cmdline[n - 1] == L'\t')) cmdline[--n] = 0;
        }
        if (!cmdline[0]) wcscpy(cmdline, L"cmd.exe");
    }

    if (haveWork) workDirP = workDir;

    if (wantNew && RelaunchNewConsole())
        return 0;

    if (!CpLoad()) {
        ERR(L"teecon: ConPTY unavailable (needs Windows 10 1809 / build 17763, "
            L"or drop conpty.dll next to this exe).\n");
        return 1;
    }

    if (!SetupConsole())
        ERR(L"teecon: warning: stdio is not a console, running without raw mode.\n");

    log = OpenLog(logPath, cmdline, wantAppend, wantHdr, wantRaw);
    if (!log) {
        ERR(L"teecon: warning: cannot open log %ls (%lu).\n", logPath, GetLastError());
    }

    if (!CpStart(&sess, cmdline, workDirP, ConsoleSize(g_hOut))) {
        ERR(L"teecon: failed to start '%ls' (%lu).\n", cmdline, GetLastError());
        goto cleanup;
    }

    quit = CreateEventW(NULL, TRUE, FALSE, NULL);

    oc.ptyOut = sess.hPtyOut;
    oc.conOut = g_hOut;
    oc.log    = log;
    oc.raw    = wantRaw;

    ic.conIn  = g_hIn;
    ic.ptyIn  = sess.hPtyIn;

    tc.sess   = &sess;
    tc.conOut = g_hOut;
    tc.log    = log;
    tc.quit   = quit;
    tc.last   = ConsoleSize(g_hOut);

    thOut  = CreateThread(NULL, 0, OutputThread, &oc, 0, NULL);
    thIn   = CreateThread(NULL, 0, InputThread,  &ic, 0, NULL);
    thTick = CreateThread(NULL, 0, TickThread,   &tc, 0, NULL);
    if (!thOut || !thIn || !thTick) {
        ERR(L"teecon: CreateThread failed (%lu).\n", GetLastError());
        goto cleanup;
    }

    WaitForSingleObject(sess.hProc, INFINITE);
    if (!GetExitCodeProcess(sess.hProc, &code)) code = 1;

    // Teardown order matters.
    // 1. stop the ticker so nothing calls ResizePseudoConsole on a dead pty
    // 2. ClosePseudoConsole flushes remaining output and breaks ptyOut --
    //    the reader MUST still be alive here or this can hang
    // 3. join the reader once its ReadFile has failed
    // 4. unblock the input thread, then release the pipe handles
    if (quit) SetEvent(quit);
    if (thTick) WaitForSingleObject(thTick, 2000);

    CpShutdownPty(&sess);
    if (thOut) WaitForSingleObject(thOut, 5000);

    if (thIn) {
        CancelSynchronousIo(thIn);  // best effort; console reads are stubborn
        WaitForSingleObject(thIn, 500);
        // No TerminateThread: let process teardown reap a still-blocked reader.
    }

cleanup:
    CpFree(&sess);
    if (thOut)  CloseHandle(thOut);
    if (thIn)   CloseHandle(thIn);
    if (thTick) CloseHandle(thTick);
    if (quit)   CloseHandle(quit);
    if (log) {
        FlushFileBuffers(log);
        CloseHandle(log);
    }
    RestoreConsole();
    return (int)code;
}
