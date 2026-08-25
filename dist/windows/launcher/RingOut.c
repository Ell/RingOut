/* Ring Out Windows launcher. The package version is recorded in SOURCE.txt.
 *
 * The native equivalent of the RingOut bash script on Linux. First run asks for
 * a GameCube disc image the user already has, runs setup, then starts the
 * runtime. Nothing game-derived ships with the package; everything personal is
 * produced here and stays in this folder.
 *
 * This exists so players double-click a real .exe. The earlier RingOut.cmd had
 * to bypass PowerShell's execution policy, which Windows applies to downloaded
 * .ps1 files, and flashed up a console window on the way through.
 *
 * Built by .github/scripts/package-windows-cross.sh with MinGW-w64:
 *   clang RingOut.c -o RingOut.exe -municode -O2 -lcomdlg32
 */

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t g_here[MAX_PATH];

static void die(const wchar_t *msg)
{
    fwprintf(stderr, L"%ls\n", msg);
    MessageBoxW(NULL, msg, L"Ring Out", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

/* Path to <exe dir>\rest, into out. */
static void here_path(wchar_t *out, size_t n, const wchar_t *rest)
{
    _snwprintf(out, n, L"%ls\\%ls", g_here, rest);
    out[n - 1] = L'\0';
}

static BOOL path_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* A module is g<DISCID>_recomp.dll, built from the user's own disc. */
static BOOL have_module(void)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    here_path(pat, MAX_PATH, L"bin\\g*_recomp.dll");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    FindClose(h);
    return TRUE;
}

static BOOL pick_iso(wchar_t *out, DWORD n)
{
    OPENFILENAMEW ofn;

    ZeroMemory(&ofn, sizeof ofn);
    out[0] = L'\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = NULL;
    /* Only what dolrecomp can actually read. Offering .rvz/.gcz/.gcm led
     * straight to "Disc extraction failed" after the user had already picked a
     * file -- the extractor rejects them by extension. */
    ofn.lpstrFilter = L"GameCube disc images (*.iso;*.wbfs)\0"
                      L"*.iso;*.wbfs\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = out;
    ofn.nMaxFile = n;
    ofn.lpstrTitle = L"Ring Out - select your game disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn);
}

/* Run a command line and wait. Returns its exit code, or -1 if it never started. */
static int run_and_wait(wchar_t *cmdline, const wchar_t *workdir)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = (DWORD)-1;

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    /* CreateProcessW may write to lpCommandLine, so it must be a writable buffer. */
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, workdir, &si, &pi))
        return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static BOOL has_disc_extension(const wchar_t *p)
{
    static const wchar_t *exts[] = { L".iso", L".gcm", L".rvz", L".wbfs", L".gcz" };
    size_t len = wcslen(p), i;

    for (i = 0; i < sizeof exts / sizeof *exts; ++i)
    {
        size_t e = wcslen(exts[i]);
        if (len >= e && _wcsicmp(p + len - e, exts[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t exe[MAX_PATH], userdir[MAX_PATH], game[MAX_PATH];
    wchar_t iso[MAX_PATH];
    wchar_t *cmd;
    size_t cap;
    int i, rc;
    BOOL have_iso_arg = FALSE;
    int iso_arg_index = -1;

    /* Resolve our own folder; the package is relocatable, so nothing may be
     * assumed about the working directory (shortcuts often set it elsewhere). */
    if (!GetModuleFileNameW(NULL, g_here, MAX_PATH))
        die(L"Could not determine the Ring Out folder.");
    {
        wchar_t *slash = wcsrchr(g_here, L'\\');
        if (!slash)
            die(L"Could not determine the Ring Out folder.");
        *slash = L'\0';
    }

    here_path(userdir, MAX_PATH, L"userdata");
    CreateDirectoryW(userdir, NULL);
    here_path(game, MAX_PATH, L"game");
    here_path(exe, MAX_PATH, L"bin\\moderngekko-run.exe");

    /* A disc image given on the command line is consumed here and NOT forwarded
     * to the runtime, which would reject it as an unknown option. */
    iso[0] = L'\0';
    for (i = 1; i < argc; ++i)
    {
        if (!have_iso_arg && has_disc_extension(argv[i]) && path_exists(argv[i]))
        {
            wcsncpy(iso, argv[i], MAX_PATH - 1);
            iso[MAX_PATH - 1] = L'\0';
            have_iso_arg = TRUE;
            iso_arg_index = i;
        }
    }

    if (!have_module() || !path_exists(game))
    {
        wprintf(L"First run: this package contains no game data.\n"
                L"You need a GameCube disc image you already have.\n\n");

        if (!have_iso_arg && !pick_iso(iso, MAX_PATH))
            die(L"No disc image selected. Setup cancelled.");

        {
            wchar_t setup[MAX_PATH];
            here_path(setup, MAX_PATH, L"setup.ps1");

            cap = wcslen(setup) + wcslen(iso) + 128;
            cmd = (wchar_t *)calloc(cap, sizeof *cmd);
            if (!cmd)
                die(L"Out of memory.");
            /* setup.ps1 stays PowerShell: it drives cmake and ninja, which is
             * script work, not launcher work. -ExecutionPolicy Bypass applies to
             * this process only and changes nothing system-wide. */
            _snwprintf(cmd, cap,
                       L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%ls\" \"%ls\"",
                       setup, iso);
            rc = run_and_wait(cmd, g_here);
            free(cmd);

            if (rc == -1)
                die(L"Could not start PowerShell to run setup.");
            if (rc != 0 || !have_module())
                die(L"Setup did not finish. See the messages in the setup window.");
        }
    }

    /* bin\moderngekko-run.exe --user-dir <userdata> [--game <game>] [args...] */
    cap = wcslen(exe) + wcslen(userdir) + wcslen(game) + 64;
    for (i = 1; i < argc; ++i)
    {
        if (i != iso_arg_index)
            cap += wcslen(argv[i]) + 4;
    }
    cmd = (wchar_t *)calloc(cap, sizeof *cmd);
    if (!cmd)
        die(L"Out of memory.");

    if (!path_exists(exe))
        die(L"bin\\moderngekko-run.exe is missing.");

    _snwprintf(cmd, cap, L"\"%ls\" --user-dir \"%ls\"", exe, userdir);
    if (path_exists(game))
    {
        wchar_t frag[MAX_PATH + 16];
        _snwprintf(frag, MAX_PATH + 16, L" --game \"%ls\"", game);
        wcscat(cmd, frag);
    }
    for (i = 1; i < argc; ++i)
    {
        if (i == iso_arg_index)
            continue;
        wcscat(cmd, L" \"");
        wcscat(cmd, argv[i]);
        wcscat(cmd, L"\"");
    }

    rc = run_and_wait(cmd, g_here);
    free(cmd);
    if (rc == -1)
        die(L"Could not start bin\\moderngekko-run.exe.");
    return rc;
}
