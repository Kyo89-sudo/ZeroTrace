// ZeroTrace Setup - installer Win32 nativo (nessun NSIS/Inno/MSI)
//
//   ZeroTrace-Setup.exe                    procedura guidata di installazione
//   ZeroTrace-Setup.exe /S [/D=percorso]   installazione silenziosa
//   uninstall.exe /uninstall [/S]        disinstallazione (registrata in "App installate")
//
// I file del programma sono incorporati come risorse RCDATA. Il disinstallatore è una
// copia dell'installer: quando parte dalla cartella del programma si copia in %TEMP%,
// si rilancia da lì (così può eliminare la cartella) e alla fine cancella la copia.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <thread>
#include "setup_res.h"

using namespace Gdiplus;

static const wchar_t* kName = L"ZeroTrace";
static const wchar_t* kVersion = L"1.1.0";
static const wchar_t* kPublisher = L"ZeroTrace";
static const wchar_t* kUninstKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZeroTrace";
static const wchar_t* kUninstExe = L"uninstall.exe";

static const wchar_t* kLicense =
    L"ZeroTrace 1.1 - Licenza d'uso\r\n\r\n"
    L"Copyright (c) 2026\r\n\r\n"
    L"Con la presente si concede gratuitamente a chiunque ottenga una copia di questo software "
    L"il permesso di usarlo, copiarlo, modificarlo, unirlo, pubblicarlo e distribuirlo, alle "
    L"seguenti condizioni:\r\n\r\n"
    L"L'avviso di copyright e questo permesso devono essere inclusi in tutte le copie o parti "
    L"sostanziali del software.\r\n\r\n"
    L"IL SOFTWARE VIENE FORNITO \"COSÌ COM'È\", SENZA GARANZIE DI ALCUN TIPO, ESPLICITE O "
    L"IMPLICITE. ZEROTRACE ELIMINA FILE E CHIAVI DI REGISTRO: CONTROLLA SEMPRE GLI ELEMENTI "
    L"SELEZIONATI PRIMA DI RIMUOVERLI. GLI AUTORI NON SONO RESPONSABILI DI DANNI O PERDITE DI "
    L"DATI DERIVANTI DALL'USO DEL SOFTWARE.\r\n\r\n"
    L"Prima di ogni rimozione ZeroTrace salva un backup delle chiavi di registro in "
    L"C:\\ProgramData\\ZeroTrace\\Backup e, se richiesto, crea un punto di ripristino di sistema.";

// ---------------------------------------------------------------- stato
enum { P_WELCOME, P_LICENSE, P_DIR, P_OPTIONS, P_PROGRESS, P_FINISH, U_CONFIRM, U_PROGRESS, U_FINISH };
enum {
    IDC_BACK = 10, IDC_NEXT, IDC_CANCEL, IDC_TEXT, IDC_LIC, IDC_ACCEPT, IDC_DECLINE, IDC_DIR, IDC_BROWSE,
    IDC_SPACE, IDC_DESKTOP, IDC_STARTMENU, IDC_STUB, IDC_STUBDESC, IDC_ACTION, IDC_PROGRESS, IDC_LOG,
    IDC_LAUNCH, IDC_KEEPBACKUP,
};
enum { WM_APP_PROG = WM_APP + 1, WM_APP_LOG, WM_APP_DONE };

static HINSTANCE g_inst;
static HWND g_wnd;
static HFONT g_font, g_fontBold, g_fontBig, g_fontTitle;
static HICON g_icon96;
static HBRUSH g_brWhite, g_brBar, g_brLic;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static bool g_uninstall = false, g_silent = false, g_upgrade = false, g_busy = false, g_ok = false;
static int g_page = P_WELCOME;
static std::wstring g_dir, g_error, g_from;
static bool g_optDesktop = false, g_optStart = true, g_optStub = false, g_delBackups = false;
static std::vector<HWND> g_pageCtl[9];
static HWND g_back, g_next, g_cancel, g_text, g_lic, g_accept, g_decline, g_dirEdit, g_browse, g_space,
            g_desktop, g_start, g_stub, g_stubDesc, g_action, g_prog, g_log, g_launchChk, g_keepChk;

// ---------------------------------------------------------------- utilità
static std::wstring KF(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}
static std::wstring SelfPath() {
    wchar_t b[MAX_PATH * 2]; GetModuleFileNameW(nullptr, b, MAX_PATH * 2); return b;
}
static std::wstring ParentDir(const std::wstring& p) { size_t k = p.find_last_of(L'\\'); return k == std::wstring::npos ? p : p.substr(0, k); }
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h); std::wstring s(n, 0); GetWindowTextW(h, &s[0], n + 1); return s;
}
static std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[2048]; DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &sz) == ERROR_SUCCESS) return buf;
    return L"";
}
static void SetStr(HKEY h, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static void SetDw(HKEY h, const wchar_t* name, DWORD v) { RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }
static bool IsAdmin() {
    BOOL a = FALSE; PSID g = nullptr; SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g)) {
        CheckTokenMembership(nullptr, g, &a); FreeSid(g);
    }
    return a != FALSE;
}
static std::wstring FormatMB(ULONGLONG bytes) {
    wchar_t b[64];
    if (bytes >= 1024ull * 1024 * 1024) swprintf(b, 64, L"%.1f GB", bytes / 1073741824.0);
    else swprintf(b, 64, L"%.1f MB", bytes / 1048576.0);
    return b;
}
static Color GC(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }
static void FillSolid(HDC dc, const RECT& r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH)); }
static void DrawTxt(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ o = SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX); SelectObject(dc, o);
}
static void FillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
    GraphicsPath p; float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90); p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure(); SolidBrush b(c); g.FillPath(&b, &p);
}

// ---------------------------------------------------------------- file incorporati
struct Payload { int res; const wchar_t* rel; bool optionalStub; };
static const Payload kFiles[] = {
    { IDR_ZEROTRACE, L"ZeroTrace.exe", false },
    { IDR_README,  L"LEGGIMI.txt", false },
    { IDR_STUB,    L"Strumenti\\StubbornApp.exe", true },
};
static bool ExtractRes(int id, const std::wstring& dest, DWORD& written) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(g_inst, r);
    const void* data = LockResource(g);
    DWORD size = SizeofResource(g_inst, r);
    SHCreateDirectoryExW(nullptr, ParentDir(dest).c_str(), nullptr);
    SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
    for (int attempt = 0; attempt < 10; ++attempt) {
        HANDLE h = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            BOOL ok = WriteFile(h, data, size, &written, nullptr);
            CloseHandle(h);
            return ok && written == size;
        }
        Sleep(300);
    }
    return false;
}
static ULONGLONG PayloadSize(bool withStub) {
    ULONGLONG t = 0;
    for (auto& f : kFiles) {
        if (f.optionalStub && !withStub) continue;
        if (HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(f.res), RT_RCDATA)) t += SizeofResource(g_inst, r);
    }
    HANDLE h = CreateFileW(SelfPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { LARGE_INTEGER s; GetFileSizeEx(h, &s); t += s.QuadPart; CloseHandle(h); }
    return t;
}

static void MakeLink(const std::wstring& lnk, const std::wstring& target, const std::wstring& desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(ParentDir(target).c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}
static std::wstring StartLnk()  { return KF(FOLDERID_CommonPrograms) + L"\\ZeroTrace.lnk"; }
static std::wstring StubLnk()   { return KF(FOLDERID_CommonPrograms) + L"\\StubbornApp (prova ZeroTrace).lnk"; }
static std::wstring DeskLnk()   { return KF(FOLDERID_PublicDesktop) + L"\\ZeroTrace.lnk"; }

// chiude ZeroTrace se è in esecuzione dalla cartella di installazione
static int CloseRunning(const std::wstring& dir) {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    std::wstring pre = dir + L"\\";
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t p[MAX_PATH * 2]; DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, p, &len) && _wcsnicmp(p, pre.c_str(), pre.size()) == 0 &&
            _wcsicmp(pe.szExeFile, kUninstExe) != 0) {
            TerminateProcess(h, 0); WaitForSingleObject(h, 4000); ++n;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}
static bool AnyRunning(const std::wstring& dir) {
    bool any = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    std::wstring pre = dir + L"\\";
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !any; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t p[MAX_PATH * 2]; DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, p, &len) && _wcsnicmp(p, pre.c_str(), pre.size()) == 0 &&
            _wcsicmp(pe.szExeFile, kUninstExe) != 0) any = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return any;
}

// ---- migrazione dal vecchio nome (PurgeIt 1.0)
static const wchar_t* kOldKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PurgeIt";
static std::wstring OldInstallDir() { return RegStr(HKEY_LOCAL_MACHINE, kOldKey, L"InstallLocation"); }
static void Log(const std::wstring& s);
static void MigrateFromPurgeIt() {
    std::wstring old = OldInstallDir();
    if (old.empty()) return;
    Log(L"Rimozione della versione precedente (PurgeIt) da " + old);
    CloseRunning(old);
    for (const wchar_t* f : { L"PurgeIt.exe", L"LEGGIMI.txt", L"Strumenti\\StubbornApp.exe", L"uninstall.exe" }) {
        std::wstring p = old + L"\\" + f;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(p.c_str()) && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    RemoveDirectoryW((old + L"\\Strumenti").c_str());
    RemoveDirectoryW(old.c_str());
    for (auto l : { KF(FOLDERID_CommonPrograms) + L"\\PurgeIt.lnk", KF(FOLDERID_CommonPrograms) + L"\\StubbornApp (prova PurgeIt).lnk",
                    KF(FOLDERID_PublicDesktop) + L"\\PurgeIt.lnk" })
        DeleteFileW(l.c_str());
    // i backup del registro passano alla cartella di ZeroTrace
    std::wstring ob = KF(FOLDERID_ProgramData) + L"\\PurgeIt", nb = KF(FOLDERID_ProgramData) + L"\\ZeroTrace";
    if (GetFileAttributesW(ob.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(nb.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (MoveFileW(ob.c_str(), nb.c_str())) Log(L"Backup del registro spostati in " + nb);
    }
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kOldKey, KEY_WOW64_64KEY, 0);
}

// ================================================================ installazione
static void Post(UINT m, WPARAM w, const std::wstring* s = nullptr) {
    if (g_wnd) PostMessageW(g_wnd, m, w, (LPARAM)s); else delete s;
}
static void Log(const std::wstring& s) { Post(WM_APP_LOG, 0, new std::wstring(s)); }

static bool DoInstall() {
    std::wstring dir = g_dir;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Post(WM_APP_PROG, 2);
    if (CloseRunning(dir)) Log(L"Chiuso ZeroTrace in esecuzione.");
    MigrateFromPurgeIt();
    Log(L"Creazione cartella " + dir);
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_error = L"Impossibile creare la cartella di destinazione.";
        return false;
    }
    int total = 0, step = 0;
    for (auto& f : kFiles) if (!f.optionalStub || g_optStub) ++total;
    total += 3;
    DWORD bytes = 0;
    ULONGLONG sum = 0;
    for (auto& f : kFiles) {
        std::wstring dest = dir + L"\\" + f.rel;
        if (f.optionalStub && !g_optStub) {   // componente deselezionato: rimuove versioni precedenti
            if (DeleteFileW(dest.c_str())) RemoveDirectoryW(ParentDir(dest).c_str());
            continue;
        }
        Log(L"Estrazione " + std::wstring(f.rel));
        if (!ExtractRes(f.res, dest, bytes)) { g_error = L"Impossibile scrivere " + dest + L" (file in uso?)."; return false; }
        sum += bytes;
        Post(WM_APP_PROG, 5 + 70 * (++step) / total);
    }
    // disinstallatore = copia dell'installer
    std::wstring un = dir + L"\\" + kUninstExe;
    Log(L"Creazione del programma di disinstallazione");
    SetFileAttributesW(un.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (_wcsicmp(SelfPath().c_str(), un.c_str()) != 0 && !CopyFileW(SelfPath().c_str(), un.c_str(), FALSE)) {
        g_error = L"Impossibile creare " + un + L".";
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(un.c_str(), GetFileExInfoStandard, &fa)) sum += fa.nFileSizeLow;
    Post(WM_APP_PROG, 80);

    Log(L"Collegamenti");
    std::wstring exe = dir + L"\\ZeroTrace.exe";
    if (g_optStart) MakeLink(StartLnk(), exe, L"Disinstallazione completa dei programmi"); else DeleteFileW(StartLnk().c_str());
    if (g_optDesktop) MakeLink(DeskLnk(), exe, L"Disinstallazione completa dei programmi"); else DeleteFileW(DeskLnk().c_str());
    if (g_optStub && g_optStart) MakeLink(StubLnk(), dir + L"\\Strumenti\\StubbornApp.exe", L"Programma di prova per ZeroTrace");
    else DeleteFileW(StubLnk().c_str());
    Post(WM_APP_PROG, 88);

    Log(L"Registrazione in App installate");
    HKEY h;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t date[16]; swprintf(date, 16, L"%04d%02d%02d", t.wYear, t.wMonth, t.wDay);
        SetStr(h, L"DisplayName", kName);
        SetStr(h, L"DisplayVersion", kVersion);
        SetStr(h, L"Publisher", kPublisher);
        SetStr(h, L"InstallLocation", dir);
        SetStr(h, L"InstallDate", date);
        SetStr(h, L"DisplayIcon", exe + L",0");
        SetStr(h, L"UninstallString", L"\"" + un + L"\" /uninstall");
        SetStr(h, L"QuietUninstallString", L"\"" + un + L"\" /uninstall /S");
        SetStr(h, L"Comments", L"Disinstallazione completa dei programmi con rimozione dei residui");
        SetDw(h, L"EstimatedSize", (DWORD)(sum / 1024));
        SetDw(h, L"NoModify", 1);
        SetDw(h, L"NoRepair", 1);
        SetDw(h, L"Options", (g_optDesktop ? 1 : 0) | (g_optStart ? 2 : 0) | (g_optStub ? 4 : 0));
        RegCloseKey(h);
    } else {
        g_error = L"Impossibile scrivere nel registro (servono i privilegi di amministratore).";
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Post(WM_APP_PROG, 100);
    Log(L"Installazione completata.");
    CoUninitialize();
    return true;
}

static void DeleteTree(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) DeleteTree(p + L"\\" + fd.cFileName); }
            while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        RemoveDirectoryW(p.c_str());
    } else DeleteFileW(p.c_str());
}

static bool DoUninstall() {
    std::wstring dir = g_dir;
    Post(WM_APP_PROG, 5);
    if (CloseRunning(dir)) Log(L"Chiuso ZeroTrace in esecuzione.");
    Log(L"Rimozione collegamenti");
    DeleteFileW(StartLnk().c_str());
    DeleteFileW(StubLnk().c_str());
    DeleteFileW(DeskLnk().c_str());
    Post(WM_APP_PROG, 20);
    int step = 0;
    for (auto& f : kFiles) {
        std::wstring p = dir + L"\\" + f.rel;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        Log(L"Eliminazione " + std::wstring(f.rel));
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(p.c_str())) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        Post(WM_APP_PROG, 20 + 15 * ++step);
    }
    RemoveDirectoryW((dir + L"\\Strumenti").c_str());
    // il disinstallatore originale: il processo genitore potrebbe non essere ancora terminato
    std::wstring un = dir + L"\\" + kUninstExe;
    for (int i = 0; i < 25 && GetFileAttributesW(un.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        SetFileAttributesW(un.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(un.c_str())) break;
        Sleep(200);
    }
    if (GetFileAttributesW(un.c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(un.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!RemoveDirectoryW(dir.c_str())) {
        // restano solo file non nostri? li lascia, ma segnala
        WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        bool empty = true;
        if (f != INVALID_HANDLE_VALUE) {
            do if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = false; while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        if (!empty) Log(L"La cartella " + dir + L" contiene altri file e non è stata eliminata.");
        else MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    Post(WM_APP_PROG, 85);
    if (g_delBackups) {
        std::wstring b = KF(FOLDERID_ProgramData) + L"\\ZeroTrace";
        Log(L"Eliminazione backup del registro in " + b);
        DeleteTree(b);
    }
    Log(L"Rimozione dal registro");
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, KEY_WOW64_64KEY, 0);
    Post(WM_APP_PROG, 100);
    Log(L"Disinstallazione completata.");
    return true;
}

// rimuove la copia temporanea del disinstallatore dopo la chiusura
static void ScheduleSelfDelete() {
    std::wstring self = SelfPath();
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    if (_wcsnicmp(self.c_str(), tmp, wcslen(tmp)) != 0) return;
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + self + L"\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    std::vector<wchar_t> b(cmd.begin(), cmd.end()); b.push_back(0);
    if (CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS |
                       CREATE_BREAKAWAY_FROM_JOB, nullptr, tmp, &si, &pi) ||
        CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, tmp, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else {
        MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

// ================================================================ interfaccia
static const wchar_t* kInstSteps[] = { L"Benvenuto", L"Licenza", L"Destinazione", L"Componenti", L"Installazione", L"Completato" };
static const wchar_t* kUnSteps[] = { L"Conferma", L"Rimozione", L"Completato" };
static int SidebarW() { return S(196); }
static int BarH() { return S(56); }

static void PageTitle(std::wstring& t, std::wstring& sub) {
    switch (g_page) {
    case P_WELCOME:  t = g_upgrade ? L"Aggiornamento di ZeroTrace" : L"Benvenuto in ZeroTrace";
                     sub = L"Installazione guidata della versione " + std::wstring(kVersion); break;
    case P_LICENSE:  t = L"Contratto di licenza"; sub = L"Leggi le condizioni d'uso prima di continuare."; break;
    case P_DIR:      t = L"Cartella di destinazione"; sub = L"Scegli dove installare ZeroTrace."; break;
    case P_OPTIONS:  t = L"Componenti e collegamenti"; sub = L"Scegli cosa installare."; break;
    case P_PROGRESS: t = L"Installazione in corso"; sub = L"Attendi mentre ZeroTrace viene installato."; break;
    case P_FINISH:   t = g_ok ? L"Installazione completata" : L"Installazione non riuscita";
                     sub = g_ok ? L"ZeroTrace è pronto all'uso." : L"Nessuna modifica permanente è stata completata."; break;
    case U_CONFIRM:  t = L"Disinstallazione di ZeroTrace"; sub = L"ZeroTrace verrà rimosso da questo computer."; break;
    case U_PROGRESS: t = L"Rimozione in corso"; sub = L"Attendi mentre ZeroTrace viene rimosso."; break;
    case U_FINISH:   t = L"Disinstallazione completata"; sub = L"ZeroTrace è stato rimosso dal computer."; break;
    }
}

static void UpdateSpace() {
    std::wstring d = GetText(g_dirEdit);
    while (d.size() > 3 && GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES) d = ParentDir(d);
    ULARGE_INTEGER freeB{};
    std::wstring s = L"Spazio richiesto: " + FormatMB(PayloadSize(g_optStub));
    if (GetDiskFreeSpaceExW(d.c_str(), &freeB, nullptr, nullptr)) s += L"      Spazio disponibile: " + FormatMB(freeB.QuadPart);
    SetWindowTextW(g_space, s.c_str());
}

static void ShowPage(int p) {
    g_page = p;
    for (auto& v : g_pageCtl) for (HWND h : v) ShowWindow(h, SW_HIDE);
    for (HWND h : g_pageCtl[p]) ShowWindow(h, SW_SHOW);
    bool installing = p == P_PROGRESS || p == U_PROGRESS;
    bool finish = p == P_FINISH || p == U_FINISH;
    ShowWindow(g_back, (p == P_WELCOME || p == U_CONFIRM || installing || finish) ? SW_HIDE : SW_SHOW);
    ShowWindow(g_cancel, finish ? SW_HIDE : SW_SHOW);
    EnableWindow(g_cancel, !installing);
    const wchar_t* nt = L"Avanti";
    if (p == P_OPTIONS) nt = g_upgrade ? L"Aggiorna" : L"Installa";
    if (p == U_CONFIRM) nt = L"Disinstalla";
    if (finish) nt = L"Fine";
    SetWindowTextW(g_next, nt);
    EnableWindow(g_next, !installing && (p != P_LICENSE || SendMessageW(g_accept, BM_GETCHECK, 0, 0) == BST_CHECKED));
    ShowWindow(g_next, installing ? SW_HIDE : SW_SHOW);
    if (p == P_WELCOME) {
        std::wstring t = g_upgrade
            ? L"È già installata una versione di ZeroTrace in:\r\n" + g_dir +
              L"\r\n\r\nLa procedura la aggiornerà mantenendo le tue impostazioni e i backup del registro.\r\n\r\n"
              L"Se ZeroTrace è aperto verrà chiuso automaticamente."
            : std::wstring(L"ZeroTrace rimuove completamente i programmi: esegue il loro disinstallatore e poi elimina "
              L"tutti i residui rimasti, come file, cartelle, chiavi di registro, servizi, attività pianificate "
              L"e voci di avvio automatico.\r\n\r\n"
              L"Questa procedura installerà ZeroTrace " ) + kVersion + L" sul computer.\r\n\r\n" +
              (OldInstallDir().empty() ? L"" : L"La versione precedente (PurgeIt) verrà rimossa automaticamente; "
                                               L"i backup del registro verranno conservati.\r\n\r\n") +
              L"Fai clic su Avanti per continuare oppure su Annulla per uscire.";
        SetWindowTextW(g_text, t.c_str());
    }
    if (p == P_FINISH) {
        std::wstring t = g_ok ? L"ZeroTrace è stato installato in:\r\n" + g_dir +
                                L"\r\n\r\nLo trovi nel menu Start. Per rimuoverlo usa \"App installate\" nelle Impostazioni di Windows."
                              : L"Si è verificato un errore:\r\n\r\n" + g_error;
        SetWindowTextW(g_text, t.c_str());
        ShowWindow(g_text, SW_SHOW);
        ShowWindow(g_launchChk, g_ok ? SW_SHOW : SW_HIDE);
    }
    if (p == U_CONFIRM) {
        SetWindowTextW(g_text, (L"Verranno rimossi il programma, i collegamenti e la voce in \"App installate\".\r\n\r\n"
                                L"Cartella: " + g_dir + L"\r\n\r\n"
                                L"I backup del registro creati durante le disinstallazioni si trovano in "
                                L"C:\\ProgramData\\ZeroTrace. Puoi conservarli o eliminarli.").c_str());
    }
    if (p == U_FINISH) SetWindowTextW(g_text, L"ZeroTrace è stato rimosso dal computer.\r\n\r\nGrazie per averlo usato.");
    if (p == P_DIR) UpdateSpace();
    RECT rc; GetClientRect(g_wnd, &rc);
    InvalidateRect(g_wnd, nullptr, FALSE);
    if (p == P_DIR) SetFocus(g_dirEdit);
    else if (IsWindowEnabled(g_next) && IsWindowVisible(g_next)) SetFocus(g_next);
}

static void StartWork() {
    g_busy = true;
    ShowPage(g_uninstall ? U_PROGRESS : P_PROGRESS);
    std::thread([] {
        bool ok = g_uninstall ? DoUninstall() : DoInstall();
        Post(WM_APP_DONE, ok);
    }).detach();
}

static void BrowseDir() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Cartella di installazione di ZeroTrace");
    if (SUCCEEDED(d->Show(g_wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                std::wstring s = p;
                CoTaskMemFree(p);
                // se la cartella scelta non è già "ZeroTrace", ne crea una sottocartella
                std::wstring leaf = s.substr(s.find_last_of(L'\\') + 1);
                if (_wcsicmp(leaf.c_str(), L"ZeroTrace") != 0) s += (s.back() == L'\\' ? L"" : L"\\") + std::wstring(L"ZeroTrace");
                SetWindowTextW(g_dirEdit, s.c_str());
            }
            it->Release();
        }
    }
    d->Release();
}

static bool ValidateDir() {
    std::wstring d = GetText(g_dirEdit);
    while (!d.empty() && (d.back() == L'\\' || d.back() == L' ')) d.pop_back();
    if (d.size() < 4 || d[1] != L':' || d[2] != L'\\') {
        MessageBoxW(g_wnd, L"Inserisci un percorso completo, ad esempio C:\\Program Files\\ZeroTrace.", L"ZeroTrace Setup", MB_ICONWARNING);
        return false;
    }
    std::wstring win = KF(FOLDERID_Windows);
    if (_wcsnicmp(d.c_str(), win.c_str(), win.size()) == 0) {
        MessageBoxW(g_wnd, L"Non è possibile installare nella cartella di Windows.", L"ZeroTrace Setup", MB_ICONWARNING);
        return false;
    }
    g_dir = d;
    return true;
}

static void Next() {
    switch (g_page) {
    case P_WELCOME: ShowPage(P_LICENSE); break;
    case P_LICENSE: ShowPage(P_DIR); break;
    case P_DIR: if (ValidateDir()) ShowPage(P_OPTIONS); break;
    case P_OPTIONS:
        g_optDesktop = SendMessageW(g_desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_optStart = SendMessageW(g_start, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_optStub = SendMessageW(g_stub, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (AnyRunning(g_dir) &&
            MessageBoxW(g_wnd, L"ZeroTrace è in esecuzione e verrà chiuso per completare l'installazione.\nContinuare?",
                        L"ZeroTrace Setup", MB_ICONQUESTION | MB_OKCANCEL) != IDOK) break;
        StartWork();
        break;
    case U_CONFIRM:
        if (AnyRunning(g_dir) &&
            MessageBoxW(g_wnd, L"ZeroTrace è in esecuzione e verrà chiuso per completare la disinstallazione.\nContinuare?",
                        L"ZeroTrace Setup", MB_ICONQUESTION | MB_OKCANCEL) != IDOK) break;
        g_delBackups = SendMessageW(g_keepChk, BM_GETCHECK, 0, 0) == BST_CHECKED;
        StartWork();
        break;
    case P_FINISH:
        if (g_ok && SendMessageW(g_launchChk, BM_GETCHECK, 0, 0) == BST_CHECKED)
            ShellExecuteW(nullptr, L"open", (g_dir + L"\\ZeroTrace.exe").c_str(), nullptr, g_dir.c_str(), SW_SHOWNORMAL);
        DestroyWindow(g_wnd);
        break;
    case U_FINISH:
        DestroyWindow(g_wnd);
        break;
    }
}
static void Back() {
    if (g_page == P_LICENSE) ShowPage(P_WELCOME);
    else if (g_page == P_DIR) ShowPage(P_LICENSE);
    else if (g_page == P_OPTIONS) ShowPage(P_DIR);
}

static HWND Ctl(const wchar_t* cls, const wchar_t* txt, DWORD st, int id, int page, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, txt, WS_CHILD | st, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (page >= 0) g_pageCtl[page].push_back(h);
    return h;
}

static void Layout() {
    RECT rc; GetClientRect(g_wnd, &rc);
    int x = SidebarW() + S(28), w = rc.right - x - S(28), y = S(96);
    int contentBottom = rc.bottom - BarH() - S(16);
    MoveWindow(g_text, x, y, w, S(150), TRUE);
    MoveWindow(g_lic, x, y, w, contentBottom - y - S(60), TRUE);
    MoveWindow(g_accept, x, contentBottom - S(52), w, S(22), TRUE);
    MoveWindow(g_decline, x, contentBottom - S(26), w, S(22), TRUE);
    MoveWindow(g_dirEdit, x, y + S(28), w - S(100), S(26), TRUE);
    MoveWindow(g_browse, x + w - S(92), y + S(27), S(92), S(28), TRUE);
    MoveWindow(g_space, x, y + S(70), w, S(20), TRUE);
    MoveWindow(g_start, x, y + S(4), w, S(24), TRUE);
    MoveWindow(g_desktop, x, y + S(34), w, S(24), TRUE);
    MoveWindow(g_stub, x, y + S(78), w, S(24), TRUE);
    MoveWindow(g_stubDesc, x + S(22), y + S(104), w - S(22), S(54), TRUE);
    MoveWindow(g_action, x, y + S(4), w, S(20), TRUE);
    MoveWindow(g_prog, x, y + S(30), w, S(16), TRUE);
    MoveWindow(g_log, x, y + S(60), w, contentBottom - y - S(60), TRUE);
    MoveWindow(g_launchChk, x, y + S(160), w, S(24), TRUE);
    MoveWindow(g_keepChk, x, y + S(160), w, S(24), TRUE);
    int by = rc.bottom - BarH() + (BarH() - S(30)) / 2;
    MoveWindow(g_cancel, rc.right - S(20) - S(96), by, S(96), S(30), TRUE);
    MoveWindow(g_next, rc.right - S(20) - S(96) - S(10) - S(112), by, S(112), S(30), TRUE);
    MoveWindow(g_back, rc.right - S(20) - S(96) - S(10) - S(112) - S(8) - S(96), by, S(96), S(30), TRUE);
}

static void Paint(HDC dc, RECT rc) {
    FillSolid(dc, rc, RGB(255, 255, 255));
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // barra laterale: gradiente nei colori dell'icona
    int sw = SidebarW();
    LinearGradientBrush br(Point(0, 0), Point(sw, rc.bottom), GC(RGB(84, 176, 255)), GC(RGB(64, 72, 222)));
    g.FillRectangle(&br, 0, 0, sw, rc.bottom);
    SolidBrush glow(Color(40, 255, 255, 255));
    g.FillEllipse(&glow, -sw / 2, -sw / 2, sw * 3 / 2, sw);
    int is = S(88);
    DrawIconEx(dc, (sw - is) / 2, S(30), g_icon96, is, is, 0, nullptr, DI_NORMAL);
    RECT t = { 0, S(30) + is + S(10), sw, S(30) + is + S(38) };
    DrawTxt(dc, kName, t, g_fontTitle, RGB(255, 255, 255), DT_CENTER | DT_SINGLELINE);
    t.top = t.bottom; t.bottom += S(20);
    DrawTxt(dc, std::wstring(L"Versione ") + kVersion, t, g_font, RGB(220, 232, 255), DT_CENTER | DT_SINGLELINE);
    // elenco fasi
    const wchar_t** steps = g_uninstall ? kUnSteps : kInstSteps;
    int n = g_uninstall ? 3 : 6;
    int cur = g_uninstall ? g_page - U_CONFIRM : g_page;
    int y = t.bottom + S(30);
    for (int i = 0; i < n; ++i) {
        int cy = y + i * S(32) + S(10);
        int r = S(5);
        bool done = i < cur, now = i == cur;
        SolidBrush dot(now ? Color(255, 255, 255, 255) : done ? Color(220, 255, 255, 255) : Color(90, 255, 255, 255));
        if (now) { SolidBrush halo(Color(60, 255, 255, 255)); g.FillEllipse(&halo, S(24) - r - S(4), cy - r - S(4), 2 * r + S(8), 2 * r + S(8)); }
        g.FillEllipse(&dot, S(24) - r, cy - r, 2 * r, 2 * r);
        if (i < n - 1) {
            Pen p(Color(done ? 200 : 80, 255, 255, 255), (REAL)S(1));
            g.DrawLine(&p, S(24), cy + r + S(4), S(24), cy + S(32) - r - S(4));
        }
        RECT lr = { S(40), cy - S(10), sw - S(10), cy + S(10) };
        DrawTxt(dc, steps[i], lr, now ? g_fontBold : g_font, now ? RGB(255, 255, 255) : done ? RGB(230, 238, 255) : RGB(190, 205, 245),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // intestazione della pagina
    std::wstring title, sub;
    PageTitle(title, sub);
    RECT tr = { sw + S(28), S(24), rc.right - S(24), S(56) };
    DrawTxt(dc, title, tr, g_fontBig, RGB(28, 32, 40), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    tr.top = S(56); tr.bottom = S(78);
    DrawTxt(dc, sub, tr, g_font, RGB(110, 118, 132), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (g_page == P_DIR) {
        RECT lr = { sw + S(28), S(96), rc.right - S(24), S(118) };
        DrawTxt(dc, L"ZeroTrace verrà installato nella cartella seguente:", lr, g_font, RGB(28, 32, 40), DT_LEFT | DT_SINGLELINE);
    }
    if (g_page == P_OPTIONS) {
        RECT lr = { sw + S(28), S(96) + S(58), rc.right - S(24), S(96) + S(76) };
        RECT ln = { sw + S(28), S(96) + S(66), rc.right - S(28), S(96) + S(67) };
        FillSolid(dc, ln, RGB(232, 235, 240));
        (void)lr;
    }
    // barra inferiore
    RECT bar = { sw, rc.bottom - BarH(), rc.right, rc.bottom };
    FillSolid(dc, bar, RGB(247, 248, 250));
    RECT line = { sw, bar.top, rc.right, bar.top + 1 };
    FillSolid(dc, line, RGB(226, 229, 235));
    RECT note = { sw + S(20), bar.top, rc.right - S(360), bar.bottom };
    DrawTxt(dc, IsAdmin() ? L"" : L"Servono i privilegi di amministratore", note, g_font, RGB(200, 90, 30), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        g_back = Ctl(L"BUTTON", L"< Indietro", BS_PUSHBUTTON | WS_TABSTOP, IDC_BACK, -1);
        g_next = Ctl(L"BUTTON", L"Avanti", BS_OWNERDRAW | WS_TABSTOP, IDC_NEXT, -1);
        g_cancel = Ctl(L"BUTTON", L"Annulla", BS_PUSHBUTTON | WS_TABSTOP, IDC_CANCEL, -1);
        g_text = Ctl(L"STATIC", L"", SS_LEFT, IDC_TEXT, P_WELCOME);
        g_pageCtl[P_FINISH].push_back(g_text); g_pageCtl[U_CONFIRM].push_back(g_text); g_pageCtl[U_FINISH].push_back(g_text);
        g_lic = Ctl(L"EDIT", kLicense, ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, IDC_LIC, P_LICENSE, WS_EX_CLIENTEDGE);
        g_accept = Ctl(L"BUTTON", L"Accetto le condizioni del contratto di licenza", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, IDC_ACCEPT, P_LICENSE);
        g_decline = Ctl(L"BUTTON", L"Non accetto", BS_AUTORADIOBUTTON | WS_TABSTOP, IDC_DECLINE, P_LICENSE);
        SendMessageW(g_decline, BM_SETCHECK, BST_CHECKED, 0);
        g_dirEdit = Ctl(L"EDIT", g_dir.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, IDC_DIR, P_DIR, WS_EX_CLIENTEDGE);
        g_browse = Ctl(L"BUTTON", L"Sfoglia…", BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE, P_DIR);
        g_space = Ctl(L"STATIC", L"", SS_LEFT, IDC_SPACE, P_DIR);
        g_start = Ctl(L"BUTTON", L"Collegamento nel menu Start", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_STARTMENU, P_OPTIONS);
        g_desktop = Ctl(L"BUTTON", L"Collegamento sul Desktop", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DESKTOP, P_OPTIONS);
        g_stub = Ctl(L"BUTTON", L"Programma di prova StubbornApp", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_STUB, P_OPTIONS);
        g_stubDesc = Ctl(L"STATIC", L"Un programma innocuo che lascia tracce difficili da rimuovere, utile per verificare "
                                    L"come lavora ZeroTrace. Viene solo copiato nella cartella Strumenti: non installa nulla "
                                    L"finché non lo avvii.", SS_LEFT, IDC_STUBDESC, P_OPTIONS);
        SendMessageW(g_start, BM_SETCHECK, g_optStart ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_desktop, BM_SETCHECK, g_optDesktop ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_stub, BM_SETCHECK, g_optStub ? BST_CHECKED : BST_UNCHECKED, 0);
        g_action = Ctl(L"STATIC", L"Preparazione...", SS_LEFT | SS_ENDELLIPSIS, IDC_ACTION, P_PROGRESS);
        g_prog = Ctl(PROGRESS_CLASSW, L"", 0, IDC_PROGRESS, P_PROGRESS);
        SendMessageW(g_prog, PBM_SETRANGE32, 0, 100);
        g_log = Ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, IDC_LOG, P_PROGRESS, WS_EX_CLIENTEDGE);
        for (HWND c : { g_action, g_prog, g_log }) g_pageCtl[U_PROGRESS].push_back(c);
        g_launchChk = Ctl(L"BUTTON", L"Avvia ZeroTrace", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_LAUNCH, P_FINISH);
        SendMessageW(g_launchChk, BM_SETCHECK, BST_CHECKED, 0);
        g_keepChk = Ctl(L"BUTTON", L"Elimina anche i backup del registro (C:\\ProgramData\\ZeroTrace)", BS_AUTOCHECKBOX | WS_TABSTOP,
                        IDC_KEEPBACKUP, U_CONFIRM);
        Layout();
        ShowPage(g_uninstall ? U_CONFIRM : P_WELCOME);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ o = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, o); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        HWND c = (HWND)l;
        if (c == g_lic) { SetBkColor(dc, RGB(248, 249, 251)); return (LRESULT)g_brLic; }
        if (c == g_log) { SetBkColor(dc, RGB(248, 249, 251)); SetTextColor(dc, RGB(80, 88, 100)); return (LRESULT)g_brLic; }
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, (c == g_space || c == g_stubDesc) ? RGB(110, 118, 132) : RGB(32, 36, 44));
        return (LRESULT)g_brWhite;
    }
    case WM_CTLCOLORBTN: return (LRESULT)g_brBar;
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlID != IDC_NEXT) break;
        RECT r = di->rcItem;
        FillSolid(di->hDC, r, RGB(247, 248, 250));
        bool en = !(di->itemState & ODS_DISABLED), down = (di->itemState & ODS_SELECTED) != 0;
        Graphics g(di->hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        COLORREF c = !en ? RGB(176, 190, 210) : down ? RGB(0, 84, 160) : RGB(0, 103, 192);
        FillRound(g, RectF(0.5f, 0.5f, (REAL)(r.right - 1), (REAL)(r.bottom - 1)), (REAL)S(5), GC(c));
        std::wstring t = GetText(di->hwndItem);
        DrawTxt(di->hDC, t, r, g_fontBold, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (di->itemState & ODS_FOCUS) { InflateRect(&r, -S(3), -S(3)); DrawFocusRect(di->hDC, &r); }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_NEXT: case IDOK: if (IsWindowEnabled(g_next) && IsWindowVisible(g_next)) Next(); return 0;
        case IDC_BACK: Back(); return 0;
        case IDC_CANCEL: case IDCANCEL: SendMessageW(h, WM_CLOSE, 0, 0); return 0;
        case IDC_BROWSE: BrowseDir(); return 0;
        case IDC_ACCEPT: case IDC_DECLINE:
            EnableWindow(g_next, SendMessageW(g_accept, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InvalidateRect(g_next, nullptr, FALSE);
            return 0;
        case IDC_DIR: if (HIWORD(w) == EN_CHANGE) UpdateSpace(); return 0;
        case IDC_STUB: g_optStub = SendMessageW(g_stub, BM_GETCHECK, 0, 0) == BST_CHECKED; return 0;
        }
        break;
    case WM_APP_PROG: SendMessageW(g_prog, PBM_SETPOS, w, 0); return 0;
    case WM_APP_LOG: {
        std::wstring* s = (std::wstring*)l;
        SetWindowTextW(g_action, s->c_str());
        int len = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, len, len);
        std::wstring line = *s + L"\r\n";
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
        delete s;
        return 0;
    }
    case WM_APP_DONE:
        g_busy = false;
        g_ok = w != 0;
        Sleep(300);
        ShowPage(g_uninstall ? U_FINISH : P_FINISH);
        return 0;
    case WM_CLOSE:
        if (g_busy) return 0;
        if (g_page != P_FINISH && g_page != U_FINISH &&
            MessageBoxW(h, g_uninstall ? L"Annullare la disinstallazione?" : L"Annullare l'installazione di ZeroTrace?",
                        L"ZeroTrace Setup", MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: g_wnd = nullptr; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================================================================ avvio
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdline, int show) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // argomenti: /S, /uninstall, /from="percorso", /D=percorso (ultimo, può contenere spazi, come NSIS)
    std::wstring cmd = cmdline ? cmdline : L"";
    std::wstring dArg;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"/S" || a == L"/s") g_silent = true;
            else if (_wcsicmp(a.c_str(), L"/uninstall") == 0) g_uninstall = true;
            else if (_wcsnicmp(a.c_str(), L"/from=", 6) == 0) g_from = a.substr(6);
            else if (_wcsnicmp(a.c_str(), L"/D=", 3) == 0) {
                dArg = a.substr(3);
                for (int j = i + 1; j < argc; ++j) dArg += std::wstring(L" ") + argv[j];
                break;
            }
        }
        if (argv) LocalFree(argv);
    }
    while (!dArg.empty() && (dArg.back() == L'"' || dArg.back() == L' ' || dArg.back() == L'\\')) dArg.pop_back();
    if (!dArg.empty() && dArg[0] == L'"') dArg.erase(0, 1);
    while (!g_from.empty() && g_from.back() == L'\\') g_from.pop_back();

    if (!IsAdmin()) {   // il manifest lo richiede già; per sicurezza rilancia elevato
        ShellExecuteW(nullptr, L"runas", SelfPath().c_str(), cmd.c_str(), nullptr, SW_SHOWNORMAL);
        return 0;
    }

    std::wstring existing = RegStr(HKEY_LOCAL_MACHINE, kUninstKey, L"InstallLocation");
    if (g_uninstall) {
        g_dir = !g_from.empty() ? g_from : !existing.empty() ? existing : ParentDir(SelfPath());
        // se partiamo dalla cartella del programma: copia in %TEMP% e rilancio
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        std::wstring self = SelfPath();
        if (g_from.empty() && _wcsnicmp(self.c_str(), tmp, wcslen(tmp)) != 0) {
            wchar_t name[64]; swprintf(name, 64, L"ZeroTrace-uninstall-%lu.exe", GetCurrentProcessId());
            std::wstring copy = std::wstring(tmp) + name;
            if (CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
                std::wstring args = L"/uninstall /from=\"" + g_dir + L"\"" + (g_silent ? L" /S" : L"");
                std::wstring full = L"\"" + copy + L"\" " + args;
                std::vector<wchar_t> b(full.begin(), full.end()); b.push_back(0);
                STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi)) {
                    // termina subito: la copia deve poter eliminare uninstall.exe.
                    // (resta nello stesso Job: chi attende i processi figli, come ZeroTrace, attende anche lei)
                    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    return 0;
                }
            }
        }
    } else {
        g_upgrade = !existing.empty();
        g_dir = !dArg.empty() ? dArg : g_upgrade ? existing : KF(FOLDERID_ProgramFiles) + L"\\ZeroTrace";
        if (g_upgrade) {   // ripropone le scelte precedenti
            DWORD opt = 2, sz = sizeof(opt);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, kUninstKey, L"Options", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &opt, &sz) == ERROR_SUCCESS) {
                g_optDesktop = opt & 1; g_optStart = (opt & 2) != 0; g_optStub = (opt & 4) != 0;
            }
        }
    }

    if (g_silent) {
        bool ok = g_uninstall ? DoUninstall() : DoInstall();
        if (g_uninstall) ScheduleSelfDelete();
        return ok ? 0 : 1;
    }

    // ---- interfaccia
    GdiplusStartupInput gsi; ULONG_PTR gtok; GdiplusStartup(&gtok, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD; g_fontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 17 / 10; g_fontBig = CreateFontIndirectW(&lf);
    lf.lfHeight = ncm.lfMessageFont.lfHeight * 3 / 2; lf.lfWeight = FW_BOLD; g_fontTitle = CreateFontIndirectW(&lf);
    g_brWhite = CreateSolidBrush(RGB(255, 255, 255));
    g_brBar = CreateSolidBrush(RGB(247, 248, 250));
    g_brLic = CreateSolidBrush(RGB(248, 249, 251));
    g_icon96 = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(88), S(88), 0);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"ZeroTraceSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(700), S(470) };
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    std::wstring title = g_uninstall ? L"Disinstallazione di ZeroTrace" : L"Installazione di ZeroTrace " + std::wstring(kVersion);
    HWND w = CreateWindowExW(0, L"ZeroTraceSetup", title.c_str(), style,
                             wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh,
                             nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_wnd && IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (g_uninstall) ScheduleSelfDelete();   // solo se è la copia temporanea
    GdiplusShutdown(gtok);
    return g_ok ? 0 : 1;
}
