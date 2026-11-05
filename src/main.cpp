// ZeroTrace - disinstallatore stile Revo Uninstaller (Win32 nativo)
// main.cpp : interfaccia grafica
//
//  ┌──────────────────────────── barra strumenti (icone grandi) ─────── cerca ┐
//  │ navigazione │  elenco programmi (dettagli / icone grandi)                 │
//  │  (filtri e  │                                                             │
//  │  strumenti) ├─────────────────────────────────────────────────────────────┤
//  │             │  pannello dettagli del programma selezionato                │
//  └──────────────────────────── barra di stato ──────────────────────────────┘
#include "scanner.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <thread>
#include <memory>
#include <algorithm>
#include "resource.h"

using namespace Gdiplus;

// ---------------------------------------------------------------- id e messaggi
enum {
    IDC_LIST = 100, IDC_SEARCH, IDC_LEVEL, IDC_RESTORE, IDC_STATUS, IDC_TOOLBAR, IDC_SIDEBAR, IDC_INFO,
    CMD_UNINSTALL = 150, CMD_FORCE, CMD_TRACES, CMD_REFRESH, CMD_VIEW, CMD_OPENDIR, CMD_OPENREG, CMD_COPYNAME,
    CMD_BACKUPS, CMD_APPWIZ,
    IDR_LIST = 200, IDR_LOG, IDR_ALL, IDR_NONE, IDR_DELETE, IDR_CLOSE, IDR_RECYCLE, IDR_INFO, IDR_COUNT,
    IDR_DETAILS, IDR_PROGRESS,
};
enum {
    WM_APP_LISTED = WM_APP + 1,   // lParam = ListResult*
    WM_APP_LOG,                   // lParam = std::wstring*
    WM_APP_SCANNED,               // wParam = 1 dopo rimozione, lParam = std::vector<Residue>*
    WM_APP_REMOVED,               // wParam = errori, lParam = riavvio necessario
    WM_APP_STEP,                  // wParam = fase, lParam = stato
};

// ---------------------------------------------------------------- tema
namespace col {
const COLORREF toolbar = RGB(255, 255, 255), line = RGB(224, 227, 233), sidebar = RGB(243, 245, 249),
               text = RGB(32, 36, 44), muted = RGB(110, 118, 132), accent = RGB(0, 103, 192),
               hover = RGB(229, 240, 251), pressed = RGB(204, 227, 247), navSel = RGB(222, 234, 249),
               info = RGB(250, 251, 253), ok = RGB(28, 160, 90), warn = RGB(230, 120, 20);
}
static Color GC(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

static HINSTANCE g_inst;
static HWND g_main, g_list, g_search, g_level, g_restore, g_status, g_toolbar, g_sidebar, g_info;
static HFONT g_font, g_fontBold, g_fontTitle, g_fontSmall, g_fontBig;
static HIMAGELIST g_imgSmall, g_imgMed, g_imgLarge;
static HICON g_appIcon;
static HBRUSH g_brToolbar, g_brInfo;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static int Max(int a, int b) { return a > b ? a : b; }

struct Entry { Program p; std::wstring dir; int icon = 0; bool orphan = false; };
static std::vector<Entry> g_programs;
static std::vector<int> g_view;
static int g_sortCol = 0;
static bool g_sortAsc = true;
static bool g_iconView = false;

enum Filter { F_ALL, F_RECENT, F_LARGE, F_USER, F_X86, F_ORPHAN, F_COUNT };
static int g_filter = F_ALL;
static int g_counts[F_COUNT];

struct ListResult { std::vector<Entry> entries; std::vector<HICON> small, med, large; };

// ======================================================================
//  Utilità
// ======================================================================
static std::wstring FormatSize(unsigned long long kb) {
    if (!kb) return L"";
    wchar_t b[64];
    if (kb >= 1024ull * 1024) swprintf(b, 64, L"%.2f GB", kb / 1048576.0);
    else if (kb >= 1024) swprintf(b, 64, L"%.1f MB", kb / 1024.0);
    else swprintf(b, 64, L"%llu KB", kb);
    return b;
}
static bool AllDigits(const std::wstring& s) {
    for (wchar_t c : s) if (c < L'0' || c > L'9') return false;
    return !s.empty();
}
static std::wstring FormatDate(const std::wstring& d) {
    if (d.size() == 8 && AllDigits(d)) return d.substr(6, 2) + L"/" + d.substr(4, 2) + L"/" + d.substr(0, 4);
    return d;
}
static int DaysSince(const std::wstring& d) {   // -1 se data sconosciuta
    if (d.size() != 8 || !AllDigits(d)) return -1;
    SYSTEMTIME st{}; st.wYear = (WORD)_wtoi(d.substr(0, 4).c_str());
    st.wMonth = (WORD)_wtoi(d.substr(4, 2).c_str()); st.wDay = (WORD)_wtoi(d.substr(6, 2).c_str());
    FILETIME ft, now;
    if (!SystemTimeToFileTime(&st, &ft)) return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{ { ft.dwLowDateTime, ft.dwHighDateTime } }, b{ { now.dwLowDateTime, now.dwHighDateTime } };
    if (b.QuadPart < a.QuadPart) return 0;
    return (int)((b.QuadPart - a.QuadPart) / (10000000ull * 86400));
}
static std::wstring WhereText(const Program& p) {
    if (p.root == HKEY_CURRENT_USER) return L"Utente corrente";
    return p.view == KEY_WOW64_32KEY ? L"Sistema · 32 bit" : L"Sistema · 64 bit";
}
static std::wstring RegPathOf(const Program& p) {
    if (p.root == HKEY_LOCAL_MACHINE && p.view == KEY_WOW64_32KEY)
        return L"HKLM\\SOFTWARE\\WOW6432Node\\" + p.UninstallKeyPath().substr(9);
    return p.RootName() + L"\\" + p.UninstallKeyPath();
}
static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    return s;
}
static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}
static void OpenRegeditAt(const std::wstring& fullKey) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        std::wstring k = fullKey;
        if (k.rfind(L"HKLM\\", 0) == 0) k = L"HKEY_LOCAL_MACHINE\\" + k.substr(5);
        else if (k.rfind(L"HKCU\\", 0) == 0) k = L"HKEY_CURRENT_USER\\" + k.substr(5);
        k = L"Computer\\" + k;
        RegSetValueExW(h, L"LastKey", 0, REG_SZ, (const BYTE*)k.c_str(), (DWORD)((k.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
    ShellExecuteW(nullptr, L"open", L"regedit.exe", L"/m", nullptr, SW_SHOWNORMAL);
}
static void OpenInExplorer(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
static std::wstring BackupRoot() {
    PWSTR pd = nullptr;
    SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd);
    std::wstring base = pd ? pd : L"C:\\ProgramData";
    if (pd) CoTaskMemFree(pd);
    return base + L"\\ZeroTrace\\Backup";
}
static std::wstring CmdExe(std::wstring c) {
    wchar_t buf[2048];
    if (ExpandEnvironmentStringsW(c.c_str(), buf, 2048)) c = buf;
    size_t a = c.find_first_not_of(L" ");
    if (a == std::wstring::npos) return L"";
    c = c.substr(a);
    if (c[0] == L'"') { size_t q = c.find(L'"', 1); return c.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1); }
    std::wstring l = Lower(c);
    size_t k = l.find(L".exe");
    return k == std::wstring::npos ? c : c.substr(0, k + 4);
}
// icona di un programma a una dimensione precisa
static HICON IconFor(const std::wstring& displayIcon, int size) {
    std::wstring icon = displayIcon;
    int idx = 0;
    size_t c = icon.find_last_of(L',');
    if (c != std::wstring::npos && c > 2) { idx = _wtoi(icon.c_str() + c + 1); icon = icon.substr(0, c); }
    icon.erase(std::remove(icon.begin(), icon.end(), L'"'), icon.end());
    wchar_t buf[2048];
    if (ExpandEnvironmentStringsW(icon.c_str(), buf, 2048)) icon = buf;
    if (icon.empty()) return nullptr;
    HICON h = nullptr;
    UINT id = 0;
    if (PrivateExtractIconsW(icon.c_str(), idx, size, size, &h, &id, 1, 0) == 1 && h) return h;
    return nullptr;
}

// GDI+: rettangolo arrotondato antialiasing
static void FillRound(Graphics& g, const RectF& r, float rad, const Color& c) {
    GraphicsPath p;
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
    SolidBrush b(c);
    g.FillPath(&b, &p);
}
static void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void DrawTxt(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
    SelectObject(dc, o);
}
static int TextW(HDC dc, const std::wstring& s, HFONT f) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{}; GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz.cx;
}
// pittura senza sfarfallio
template <class F> static void Buffered(HWND h, F fn) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, Max(1, rc.right), Max(1, rc.bottom));
    HGDIOBJ old = SelectObject(mem, bmp);
    fn(mem, rc);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

// ======================================================================
//  Elenco programmi
// ======================================================================
static bool PassFilter(const Entry& e, int f) {
    switch (f) {
    case F_RECENT: { int d = DaysSince(e.p.installDate); return d >= 0 && d <= 30; }
    case F_LARGE:  return e.p.sizeKB >= 500 * 1024;
    case F_USER:   return e.p.root == HKEY_CURRENT_USER;
    case F_X86:    return e.p.root == HKEY_LOCAL_MACHINE && e.p.view == KEY_WOW64_32KEY;
    case F_ORPHAN: return e.orphan;
    default:       return true;
    }
}
static Entry* SelectedEntry() {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0 || i >= (int)g_view.size()) return nullptr;
    return &g_programs[g_view[i]];
}
static void UpdateStatus() {
    unsigned long long total = 0;
    for (int i : g_view) total += g_programs[i].p.sizeKB;
    std::wstring a = L"  " + std::to_wstring(g_view.size()) + L" programmi visualizzati";
    std::wstring b = L"  Spazio stimato: " + (total ? FormatSize(total) : std::wstring(L"—"));
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)a.c_str());
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)b.c_str());
}
static void RebuildView() {
    std::wstring q = Lower(GetText(g_search));
    std::wstring keepKey;
    if (Entry* e = SelectedEntry()) keepKey = e->p.keyName + RegPathOf(e->p);
    for (int f = 0; f < F_COUNT; ++f) g_counts[f] = 0;
    g_view.clear();
    for (int i = 0; i < (int)g_programs.size(); ++i) {
        auto& e = g_programs[i];
        for (int f = 0; f < F_COUNT; ++f) if (PassFilter(e, f)) ++g_counts[f];
        if (!PassFilter(e, g_filter)) continue;
        if (q.empty() || Lower(e.p.name).find(q) != std::wstring::npos || Lower(e.p.publisher).find(q) != std::wstring::npos)
            g_view.push_back(i);
    }
    std::stable_sort(g_view.begin(), g_view.end(), [](int a, int b) {
        const Program &x = g_programs[a].p, &y = g_programs[b].p;
        int c = 0;
        switch (g_sortCol) {
        case 0: c = _wcsicmp(x.name.c_str(), y.name.c_str()); break;
        case 1: c = _wcsicmp(x.publisher.c_str(), y.publisher.c_str()); break;
        case 2: c = _wcsicmp(x.version.c_str(), y.version.c_str()); break;
        case 3: c = x.sizeKB < y.sizeKB ? -1 : x.sizeKB > y.sizeKB; break;
        case 4: c = _wcsicmp(x.installDate.c_str(), y.installDate.c_str()); break;
        case 5: c = _wcsicmp(WhereText(x).c_str(), WhereText(y).c_str()); break;
        }
        return g_sortAsc ? c < 0 : c > 0;
    });
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemCountEx(g_list, g_view.size(), 0);
    for (size_t i = 0; i < g_view.size() && !keepKey.empty(); ++i) {
        auto& p = g_programs[g_view[i]].p;
        if (p.keyName + RegPathOf(p) == keepKey) {
            ListView_SetItemState(g_list, (int)i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, (int)i, FALSE);
            break;
        }
    }
    InvalidateRect(g_list, nullptr, FALSE);
    InvalidateRect(g_sidebar, nullptr, FALSE);
    InvalidateRect(g_info, nullptr, FALSE);
    InvalidateRect(g_toolbar, nullptr, FALSE);
    UpdateStatus();
}

static void LoadProgramsAsync() {
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)L"  Lettura dei programmi installati...");
    int sm = S(16), md = S(32), lg = S(48);
    std::thread([sm, md, lg] {
        auto* res = new ListResult;
        for (auto& p : EnumeratePrograms()) {
            Entry e; e.p = p;
            e.dir = ResolveInstallDir(p);
            std::wstring exe = CmdExe(p.uninstallString);
            std::wstring le = Lower(exe);
            e.orphan = p.uninstallString.empty() ||
                       (le.find(L"msiexec") == std::wstring::npos && le.find(L"rundll32") == std::wstring::npos &&
                        GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES);
            std::wstring src = p.displayIcon;
            if (src.empty() && !e.orphan && le.find(L"msiexec") == std::wstring::npos) src = exe;
            HICON s = IconFor(src, sm), m = IconFor(src, md), l = IconFor(src, lg);
            if (s && m && l) {
                e.icon = (int)res->small.size() + 1;
                res->small.push_back(s); res->med.push_back(m); res->large.push_back(l);
            } else { if (s) DestroyIcon(s); if (m) DestroyIcon(m); if (l) DestroyIcon(l); }
            res->entries.push_back(std::move(e));
        }
        PostMessageW(g_main, WM_APP_LISTED, 0, (LPARAM)res);
    }).detach();
}

// ======================================================================
//  Metriche (tutto il layout deriva dalla dimensione reale dei caratteri)
// ======================================================================
static int g_lineH = 16, g_bigH = 22, g_titleH = 20;
static int FontHeight(HFONT f) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(dc, f);
    TEXTMETRICW tm{}; GetTextMetricsW(dc, &tm);
    SelectObject(dc, o); ReleaseDC(nullptr, dc);
    return tm.tmHeight;
}
static int TextWidth(const std::wstring& s, HFONT f) {
    HDC dc = GetDC(nullptr);
    int w = TextW(dc, s, f);
    ReleaseDC(nullptr, dc);
    return w;
}

// ======================================================================
//  Barra strumenti (una riga; diventa "solo icone" se manca spazio)
// ======================================================================
struct TBtn { int cmd; const wchar_t* text; const wchar_t* tip; int icon; bool needsSel; bool sepAfter; HICON h; RECT rc; };
static TBtn g_tb[] = {
    { CMD_UNINSTALL, L"Disinstalla",     L"Disinstalla il programma e rimuovi i residui (Invio)", IDI_TB_UNINSTALL, true,  false },
    { CMD_FORCE,     L"Forza rimozione", L"Rimuovi senza eseguire il disinstallatore (Maiusc+Canc)", IDI_TB_FORCE, true,  true  },
    { CMD_TRACES,    L"Da cartella",     L"Cerca e rimuovi le tracce di un programma partendo dalla sua cartella", IDI_TB_TRACES, false, true },
    { CMD_REFRESH,   L"Aggiorna",        L"Ricarica l'elenco dei programmi (F5)", IDI_TB_REFRESH, false, false },
    { CMD_VIEW,      L"Vista",           L"Alterna righe compatte / righe grandi", IDI_TB_VIEW, false, true },
    { CMD_OPENDIR,   L"Cartella",        L"Apri la cartella di installazione", IDI_TB_FOLDER, true, false },
    { CMD_OPENREG,   L"Registro",        L"Apri la voce di disinstallazione nell'Editor del Registro", IDI_TB_REGISTRY, true, false },
};
static const int kTbCount = sizeof(g_tb) / sizeof(g_tb[0]);
static int g_tbHot = -1, g_tbDown = -1;
static bool g_tbTracking = false, g_tbCompact = false, g_showLevelLabel = true;
static RECT g_levelLabel;
static HWND g_tips;

static int ToolbarHeight() { return S(6) + S(24) + S(3) + g_lineH + S(7); }

static bool TbEnabled(const TBtn& b) {
    if (!b.needsSel) return true;
    Entry* e = SelectedEntry();
    if (!e) return false;
    if (b.cmd == CMD_OPENDIR) return !e->dir.empty();
    return true;
}
static int TbHit(POINT pt) {
    for (int i = 0; i < kTbCount; ++i) if (PtInRect(&g_tb[i].rc, pt)) return i;
    return -1;
}

static void ToolbarLayout(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    int th = ToolbarHeight();
    // ---- area destra: [Scansione][combo]  [x Punto di ripristino]  [cerca......]
    int lblW = TextWidth(L"Scansione", g_font) + S(6);
    int comboW = TextWidth(L"Moderata", g_font) + S(40);
    int chkW = TextWidth(L"Punto di ripristino", g_font) + S(26);
    int searchMin = S(140), searchMax = S(240), gap = S(12), m = S(10);
    auto rightNeeded = [&](int search) { return lblW + comboW + gap + chkW + gap + search + m; };
    // ---- pulsanti: con etichetta oppure solo icona
    auto layoutButtons = [&](bool compact) {
        int x = S(6);
        for (auto& b : g_tb) {
            int w = compact ? S(40) : TextWidth(b.text, g_font) + S(18);
            if (!compact && w < S(54)) w = S(54);
            b.rc = { x, S(3), x + w, th - S(3) };
            x += w + S(2);
            if (b.sepAfter) x += S(11);
        }
        return x;
    };
    g_tbCompact = false;
    int end = layoutButtons(false);
    if (end + rightNeeded(searchMin) > rc.right) { g_tbCompact = true; end = layoutButtons(true); }
    int avail = rc.right - end;
    int search = avail - rightNeeded(0);
    if (search > searchMax) search = searchMax;
    g_showLevelLabel = true;
    if (search < searchMin) { g_showLevelLabel = false; search += lblW; lblW = 0; }
    if (search < S(90)) search = S(90);
    int x = rc.right - m - search;
    int editH = g_lineH + S(10);
    MoveWindow(g_search, x, (th - editH) / 2, search, editH, TRUE);
    x -= gap + chkW;
    MoveWindow(g_restore, x, (th - S(22)) / 2, chkW, S(22), TRUE);
    x -= gap + comboW;
    MoveWindow(g_level, x, 0, comboW, S(200), TRUE);
    RECT cr; GetWindowRect(g_level, &cr);
    int ch = cr.bottom - cr.top;
    MoveWindow(g_level, x, (th - ch) / 2, comboW, S(200), TRUE);
    g_levelLabel = { x - lblW, 0, x - S(6), th };
    ShowWindow(g_restore, end + rightNeeded(0) - lblW < rc.right ? SW_SHOW : SW_HIDE);
    // suggerimenti sui pulsanti
    for (int i = 0; i < kTbCount; ++i) {
        TOOLINFOW ti{ sizeof(ti) }; ti.hwnd = h; ti.uId = i; ti.rect = g_tb[i].rc;
        SendMessageW(g_tips, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

static void DrawToolbarIcon(HDC dc, Graphics& g, const TBtn& b, int ix, int iy, int is, bool en) {
    (void)g;
    if (!b.h) return;
    if (en) { DrawIconEx(dc, ix, iy, b.h, is, is, 0, nullptr, DI_NORMAL); return; }
    // icona "spenta": grigia e semitrasparente
    HDC tmp = CreateCompatibleDC(dc);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), is, -is, 1, 32 };
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ o = SelectObject(tmp, hb);
    memset(bits, 0, (size_t)is * is * 4);
    DrawIconEx(tmp, 0, 0, b.h, is, is, 0, nullptr, DI_NORMAL);
    BYTE* px = (BYTE*)bits;
    for (int k = 0; k < is * is; ++k, px += 4) {
        int gray = (px[2] * 30 + px[1] * 59 + px[0] * 11) / 100;
        int a = px[3] * 40 / 100, v = gray * 40 / 100;
        px[0] = px[1] = px[2] = (BYTE)(v > a ? a : v); px[3] = (BYTE)a;
    }
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, ix, iy, is, is, tmp, 0, 0, is, is, bf);
    SelectObject(tmp, o);
    DeleteObject(hb); DeleteDC(tmp);
}

static LRESULT CALLBACK ToolbarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                 0, 0, 0, 0, h, nullptr, g_inst, nullptr);
        SendMessageW(g_tips, TTM_SETMAXTIPWIDTH, 0, S(320));
        for (int i = 0; i < kTbCount; ++i) {
            TOOLINFOW ti{ sizeof(ti) };
            ti.uFlags = TTF_SUBCLASS; ti.hwnd = h; ti.uId = i; ti.lpszText = (LPWSTR)g_tb[i].tip;
            SendMessageW(g_tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: ToolbarLayout(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT:
        Buffered(h, [&](HDC dc, RECT rc) {
            FillSolid(dc, rc, col::toolbar);
            RECT ln = { 0, rc.bottom - 1, rc.right, rc.bottom };
            FillSolid(dc, ln, col::line);
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            int is = S(24);
            for (int i = 0; i < kTbCount; ++i) {
                auto& b = g_tb[i];
                bool en = TbEnabled(b);
                if (en && (i == g_tbHot || i == g_tbDown)) {
                    RectF r((REAL)b.rc.left, (REAL)b.rc.top, (REAL)(b.rc.right - b.rc.left), (REAL)(b.rc.bottom - b.rc.top));
                    FillRound(g, r, (REAL)S(5), GC(i == g_tbDown ? col::pressed : col::hover));
                }
                int ix = (b.rc.left + b.rc.right - is) / 2;
                int iy = g_tbCompact ? (b.rc.top + b.rc.bottom - is) / 2 : b.rc.top + S(4);
                DrawToolbarIcon(dc, g, b, ix, iy, is, en);
                if (!g_tbCompact) {
                    RECT tr = { b.rc.left, iy + is + S(2), b.rc.right, b.rc.bottom };
                    DrawTxt(dc, b.text, tr, g_font, en ? col::text : RGB(165, 170, 180), DT_CENTER | DT_TOP | DT_SINGLELINE);
                }
                if (b.sepAfter) {
                    RECT sp = { b.rc.right + S(6), b.rc.top + S(8), b.rc.right + S(7), b.rc.bottom - S(8) };
                    FillSolid(dc, sp, col::line);
                }
            }
            if (g_showLevelLabel)
                DrawTxt(dc, L"Scansione", g_levelLabel, g_font, col::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        });
        return 0;
    case WM_MOUSEMOVE: {
        if (!g_tbTracking) { TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); g_tbTracking = true; }
        int hit = TbHit({ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (hit != g_tbHot) { g_tbHot = hit; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE: g_tbTracking = false; g_tbHot = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        g_tbDown = TbHit({ GET_X_LPARAM(l), GET_Y_LPARAM(l) });
        if (g_tbDown >= 0) { SetCapture(h); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        int was = g_tbDown;
        g_tbDown = -1;
        ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
        if (was >= 0 && was == TbHit({ GET_X_LPARAM(l), GET_Y_LPARAM(l) }) && TbEnabled(g_tb[was]))
            PostMessageW(g_main, WM_COMMAND, g_tb[was].cmd, 0);
        return 0;
    }
    case WM_COMMAND: return SendMessageW(g_main, m, w, l);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)w, col::toolbar);
        return (LRESULT)g_brToolbar;
    }
    return DefWindowProcW(h, m, w, l);
}

// ======================================================================
//  Barra laterale di navigazione (larghezza calcolata sui testi)
// ======================================================================
struct NavItem { const wchar_t* text; int filter; int cmd; bool header; RECT rc; };
static NavItem g_nav[] = {
    { L"PROGRAMMI", -1, 0, true },
    { L"Tutti i programmi", F_ALL, 0, false },
    { L"Installati di recente", F_RECENT, 0, false },
    { L"Grandi dimensioni", F_LARGE, 0, false },
    { L"Solo utente corrente", F_USER, 0, false },
    { L"Applicazioni a 32 bit", F_X86, 0, false },
    { L"Voci orfane", F_ORPHAN, 0, false },
    { L"STRUMENTI", -1, 0, true },
    { L"Tracce da cartella…", -1, CMD_TRACES, false },
    { L"Backup del registro", -1, CMD_BACKUPS, false },
    { L"Programmi e funzionalità", -1, CMD_APPWIZ, false },
};
static const int kNavCount = sizeof(g_nav) / sizeof(g_nav[0]);
static int g_navHot = -1, g_sideW = 190;
static bool g_navTracking = false;

static void SidebarLayout() {
    int maxW = 0;
    for (auto& n : g_nav) if (!n.header) { int w = TextWidth(n.text, g_fontBold); if (w > maxW) maxW = w; }
    int countW = TextWidth(L"0000", g_font);
    g_sideW = maxW + countW + S(6 + 10 + 12 + 8 + 6);
    if (g_sideW < S(150)) g_sideW = S(150);
    if (g_sideW > S(240)) g_sideW = S(240);
    int itemH = g_lineH + S(12), y = S(8);
    for (auto& n : g_nav) {
        if (n.header) { if (y > S(16)) y += S(8); n.rc = { 0, y, g_sideW, y + g_lineH + S(4) }; y += g_lineH + S(6); }
        else { n.rc = { S(6), y, g_sideW - S(6), y + itemH }; y += itemH + S(2); }
    }
}
static LRESULT CALLBACK SidebarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        Buffered(h, [&](HDC dc, RECT rc) {
            FillSolid(dc, rc, col::sidebar);
            RECT ln = { rc.right - 1, 0, rc.right, rc.bottom };
            FillSolid(dc, ln, col::line);
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            for (int i = 0; i < kNavCount; ++i) {
                auto& n = g_nav[i];
                RECT r = n.rc;
                if (n.header) {
                    r.left = S(16);
                    DrawTxt(dc, n.text, r, g_fontSmall, col::muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    continue;
                }
                bool sel = n.filter >= 0 && n.filter == g_filter;
                if (sel || i == g_navHot) {
                    RectF rf((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top));
                    FillRound(g, rf, (REAL)S(5), GC(sel ? col::navSel : RGB(232, 237, 245)));
                    if (sel) FillRound(g, RectF((REAL)r.left, (REAL)(r.top + S(6)), (REAL)S(3), (REAL)(r.bottom - r.top - S(12))),
                                       (REAL)S(1), GC(col::accent));
                }
                int countW = n.filter >= 0 ? TextWidth(L"0000", g_font) + S(8) : 0;
                RECT tr = { r.left + S(10), r.top, r.right - countW - S(4), r.bottom };
                DrawTxt(dc, n.text, tr, sel ? g_fontBold : g_font, sel ? col::accent : col::text,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (n.filter >= 0) {
                    RECT cr = { r.right - countW, r.top, r.right - S(8), r.bottom };
                    DrawTxt(dc, std::to_wstring(g_counts[n.filter]), cr, g_font, col::muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
            // marchio in fondo, solo se c'è spazio
            int is = S(32);
            int y = rc.bottom - is - S(14);
            if (y > g_nav[kNavCount - 1].rc.bottom + S(16)) {
                DrawIconEx(dc, S(14), y, g_appIcon, is, is, 0, nullptr, DI_NORMAL);
                RECT tr = { S(14) + is + S(8), y - S(1), rc.right - S(6), y + is / 2 + S(2) };
                DrawTxt(dc, L"ZeroTrace", tr, g_fontBold, col::text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
                tr.top = y + is / 2 + S(1); tr.bottom = y + is + S(2);
                DrawTxt(dc, L"Versione 1.1", tr, g_fontSmall, col::muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        });
        return 0;
    case WM_MOUSEMOVE: {
        if (!g_navTracking) { TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); g_navTracking = true; }
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int hit = -1;
        for (int i = 0; i < kNavCount; ++i) if (!g_nav[i].header && PtInRect(&g_nav[i].rc, pt)) hit = i;
        if (hit != g_navHot) { g_navHot = hit; InvalidateRect(h, nullptr, FALSE); }
        SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR: if (g_navHot >= 0) return TRUE; break;
    case WM_MOUSELEAVE: g_navTracking = false; g_navHot = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
        if (g_navHot >= 0) {
            auto& n = g_nav[g_navHot];
            if (n.filter >= 0) { g_filter = n.filter; RebuildView(); }
            else if (n.cmd) PostMessageW(g_main, WM_COMMAND, n.cmd, 0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ======================================================================
//  Pannello dettagli (altezza calcolata sui caratteri)
// ======================================================================
static int InfoHeight() { return S(10) + g_bigH + g_lineH + S(8) + 3 * (g_lineH + S(2)) + S(10); }
static LRESULT CALLBACK InfoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        Buffered(h, [&](HDC dc, RECT rc) {
            FillSolid(dc, rc, col::info);
            RECT ln = { 0, 0, rc.right, 1 };
            FillSolid(dc, ln, col::line);
            Entry* e = SelectedEntry();
            if (!e) {
                DrawTxt(dc, L"Seleziona un programma per vederne i dettagli. Doppio clic o Invio per disinstallarlo.",
                        rc, g_font, col::muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                return;
            }
            int pad = S(10), is = S(40);
            if (g_imgLarge) ImageList_DrawEx(g_imgLarge, e->icon, dc, pad + S(4), pad, is, is, CLR_NONE, CLR_NONE, ILD_TRANSPARENT);
            int x = pad + S(4) + is + S(12), y = pad - S(2);
            RECT t = { x, y, rc.right - pad, y + g_bigH };
            DrawTxt(dc, e->p.name, t, g_fontBig, col::text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            std::wstring sub = e->p.publisher.empty() ? L"Editore sconosciuto" : e->p.publisher;
            if (!e->p.version.empty()) sub += L"   ·   versione " + e->p.version;
            if (e->orphan) sub += L"   ·   disinstallatore mancante: usa Forza rimozione";
            t = { x, y + g_bigH, rc.right - pad, y + g_bigH + g_lineH };
            DrawTxt(dc, sub, t, g_font, e->orphan ? col::warn : col::muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            struct KV { const wchar_t* k; std::wstring v; };
            KV left[] = { { L"Installato il", FormatDate(e->p.installDate) }, { L"Dimensione", FormatSize(e->p.sizeKB) },
                          { L"Tipo", WhereText(e->p) } };
            KV right[] = { { L"Cartella", e->dir }, { L"Disinstallatore", e->p.uninstallString },
                           { L"Registro", RegPathOf(e->p) } };
            int kw1 = 0, kw2 = 0;
            for (auto& k : left) { int ww = TextWidth(k.k, g_font); if (ww > kw1) kw1 = ww; }
            for (auto& k : right) { int ww = TextWidth(k.k, g_font); if (ww > kw2) kw2 = ww; }
            kw1 += S(12); kw2 += S(12);
            int col1 = kw1 + TextWidth(L"Sistema · 64 bit", g_font) + S(24);   // larghezza della prima colonna
            int lh = g_lineH + S(2), y0 = y + g_bigH + g_lineH + S(8);
            for (int i = 0; i < 3; ++i) {
                int ty = y0 + i * lh;
                RECT k = { x, ty, x + kw1, ty + lh };
                DrawTxt(dc, left[i].k, k, g_font, col::muted, DT_LEFT | DT_SINGLELINE);
                RECT v = { x + kw1, ty, x + col1, ty + lh };
                DrawTxt(dc, left[i].v.empty() ? L"—" : left[i].v, v, g_font, col::text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
                RECT k2 = { x + col1, ty, x + col1 + kw2, ty + lh };
                DrawTxt(dc, right[i].k, k2, g_font, col::muted, DT_LEFT | DT_SINGLELINE);
                RECT v2 = { k2.right, ty, rc.right - pad, ty + lh };
                DrawTxt(dc, right[i].v.empty() ? L"—" : right[i].v, v2, g_font, col::text,
                        DT_LEFT | DT_SINGLELINE | (i == 0 ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
            }
        });
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
// ======================================================================
//  Finestra di avanzamento / risultati
// ======================================================================
enum StepState { ST_WAIT, ST_RUN, ST_DONE, ST_SKIP, ST_FAIL };
static const wchar_t* kSteps[] = { L"Punto di ripristino", L"Disinstallatore", L"Scansione residui", L"Rimozione" };

struct ResultsState {
    HWND hwnd = nullptr, list, log, info, count, progress, bAll, bNone, bDel, bClose, recycle, bDetails;
    Program prog;
    std::wstring installDir;
    std::vector<Residue> items;
    int steps[4] = { ST_WAIT, ST_WAIT, ST_WAIT, ST_WAIT };
    HICON icon = nullptr;
    bool busy = true, removed = false, showLog = false;
};
static ResultsState* g_res = nullptr;
static int HeadTop() { int a = g_bigH + g_lineH, b = S(40); return S(12) + (a > b ? a : b); }
static int ResHeaderH() { return HeadTop() + S(12) + S(22) + S(12); }

static void ResLog(const std::wstring& s) {
    int len = GetWindowTextLengthW(g_res->log);
    SendMessageW(g_res->log, EM_SETSEL, len, len);
    std::wstring line = s + L"\r\n";
    SendMessageW(g_res->log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}
static LogFn MakePostLog(HWND target) {
    return [target](const std::wstring& s) { PostMessageW(target, WM_APP_LOG, 0, (LPARAM)new std::wstring(s)); };
}
static void PostStep(HWND target, int step, int state) { PostMessageW(target, WM_APP_STEP, step, state); }

static void ResUpdateCount() {
    int n = ListView_GetItemCount(g_res->list), c = 0;
    for (int i = 0; i < n; ++i) c += ListView_GetCheckState(g_res->list, i) != FALSE;
    std::wstring s = n ? std::to_wstring(c) + L" di " + std::to_wstring(n) + L" selezionati" : L"";
    SetWindowTextW(g_res->count, s.c_str());
    EnableWindow(g_res->bDel, !g_res->busy && c > 0);
}
static void ResSetBusy(bool busy) {
    g_res->busy = busy;
    EnableWindow(g_res->bAll, !busy);
    EnableWindow(g_res->bNone, !busy);
    EnableWindow(g_res->bClose, !busy);
    EnableWindow(g_res->recycle, !busy);
    ShowWindow(g_res->progress, busy ? SW_SHOW : SW_HIDE);
    SendMessageW(g_res->progress, PBM_SETMARQUEE, busy, 30);
    ResUpdateCount();
}

static void ResFill() {
    HWND lv = g_res->list;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    ListView_RemoveAllGroups(lv);
    int cnt[4] = {};
    for (auto& r : g_res->items)
        ++cnt[(r.kind == ResKind::RegKey || r.kind == ResKind::RegValue) ? 1 :
              (r.kind == ResKind::Service || r.kind == ResKind::Task || r.kind == ResKind::Firewall || r.kind == ResKind::PathEntry) ? 2 : 3];
    std::wstring names[4] = { L"", L"Registro di sistema", L"Servizi, attività, firewall e PATH", L"File e cartelle" };
    for (int gid = 1; gid <= 3; ++gid) {
        if (!cnt[gid]) continue;
        std::wstring hdr = names[gid] + L"  (" + std::to_wstring(cnt[gid]) + L")";
        LVGROUP gr{ sizeof(gr) };
        gr.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        gr.pszHeader = (LPWSTR)hdr.c_str();
        gr.iGroupId = gid;
        gr.stateMask = LVGS_COLLAPSIBLE; gr.state = LVGS_COLLAPSIBLE;
        ListView_InsertGroup(lv, -1, &gr);
    }
    for (int i = 0; i < (int)g_res->items.size(); ++i) {
        auto& r = g_res->items[i];
        int gid = (r.kind == ResKind::RegKey || r.kind == ResKind::RegValue) ? 1 :
                  (r.kind == ResKind::Service || r.kind == ResKind::Task || r.kind == ResKind::Firewall || r.kind == ResKind::PathEntry) ? 2 : 3;
        std::wstring disp = r.Display();
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID | LVIF_IMAGE;
        it.iItem = i; it.pszText = (LPWSTR)disp.c_str(); it.lParam = i; it.iGroupId = gid; it.iImage = (int)r.kind;
        int idx = ListView_InsertItem(lv, &it);
        ListView_SetItemText(lv, idx, 1, (LPWSTR)r.reason.c_str());
        ListView_SetCheckState(lv, idx, r.checked);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
    std::wstring h;
    if (g_res->items.empty())
        h = g_res->removed ? L"Tutti gli elementi selezionati sono stati rimossi."
                           : L"Nessun residuo trovato: il programma è stato rimosso completamente.";
    else if (g_res->removed)
        h = L"Attenzione: " + std::to_wstring(g_res->items.size()) + L" elementi non sono stati rimossi (dettagli nel registro operazioni).";
    else
        h = L"Trovati " + std::to_wstring(g_res->items.size()) +
            L" elementi residui. Controlla la selezione, poi premi \"Elimina selezionati\". Doppio clic per aprire un elemento.";
    SetWindowTextW(g_res->info, h.c_str());
    ResUpdateCount();
}

static void ResLayout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom, m = S(12), bh = g_lineH + S(14), g = S(6);
    auto bw = [&](HWND h, HFONT f) { return TextWidth(GetText(h), f) + S(28); };
    int y = ResHeaderH();
    MoveWindow(g_res->progress, 0, y - S(3), W, S(3), TRUE);
    MoveWindow(g_res->info, m, y + S(8), W - 2 * m, g_lineH + S(2), TRUE);
    int rowY = y + S(8) + g_lineH + S(8);
    int x = m, w1 = bw(g_res->bAll, g_font), w2 = bw(g_res->bNone, g_font);
    MoveWindow(g_res->bAll, x, rowY, w1, bh, TRUE); x += w1 + g;
    MoveWindow(g_res->bNone, x, rowY, w2, bh, TRUE); x += w2 + S(12);
    int wd = TextWidth(L"Nascondi registro operazioni", g_font) + S(28);
    MoveWindow(g_res->bDetails, W - m - wd, rowY, wd, bh, TRUE);
    MoveWindow(g_res->count, x, rowY + (bh - g_lineH) / 2, W - m - wd - x - S(8), g_lineH + S(2), TRUE);
    int listTop = rowY + bh + S(8);
    int bottomBar = H - m - bh;
    int logH = g_res->showLog ? (H / 4 > S(110) ? H / 4 : S(110)) : 0;
    int listBottom = bottomBar - m - (logH ? logH + S(6) : 0);
    MoveWindow(g_res->list, m, listTop, W - 2 * m, listBottom - listTop, TRUE);
    ShowWindow(g_res->log, g_res->showLog ? SW_SHOW : SW_HIDE);
    if (logH) MoveWindow(g_res->log, m, listBottom + S(6), W - 2 * m, logH, TRUE);
    int wc = bw(g_res->bClose, g_font), wdel = TextWidth(L"Elimina selezionati", g_fontBold) + S(32);
    MoveWindow(g_res->bClose, W - m - wc, bottomBar, wc, bh, TRUE);
    MoveWindow(g_res->bDel, W - m - wc - g - wdel, bottomBar, wdel, bh, TRUE);
    MoveWindow(g_res->recycle, m, bottomBar + (bh - S(20)) / 2, TextWidth(GetText(g_res->recycle), g_font) + S(26), S(20), TRUE);
    int lw = W - 2 * m - GetSystemMetrics(SM_CXVSCROLL) - S(6);
    ListView_SetColumnWidth(g_res->list, 0, lw * 62 / 100);
    ListView_SetColumnWidth(g_res->list, 1, lw - lw * 62 / 100);
    RECT hr = { 0, 0, W, y };
    InvalidateRect(hwnd, &hr, FALSE);
}

static void PaintResHeader(HDC dc, RECT rc) {
    RECT hr = { 0, 0, rc.right, ResHeaderH() };
    // fascia con leggero gradiente
    {
        Graphics g(dc);
        LinearGradientBrush br(Point(0, 0), Point(0, hr.bottom), GC(RGB(255, 255, 255)), GC(RGB(242, 246, 252)));
        g.FillRectangle(&br, 0, 0, hr.right, hr.bottom);
    }
    RECT ln = { 0, hr.bottom - 1, hr.right, hr.bottom };
    FillSolid(dc, ln, col::line);
    int pad = S(12), is = S(40);
    DrawIconEx(dc, pad + S(2), pad, g_res->icon ? g_res->icon : g_appIcon, is, is, 0, nullptr, DI_NORMAL);
    RECT t = { pad + S(2) + is + S(12), pad - S(1), rc.right - pad, pad + g_bigH };
    DrawTxt(dc, g_res->prog.name, t, g_fontBig, col::text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    t.top = t.bottom; t.bottom = t.top + g_lineH + S(2);
    std::wstring sub = g_res->installDir.empty() ? std::wstring(L"Cartella di installazione sconosciuta") : g_res->installDir;
    DrawTxt(dc, sub, t, g_font, col::muted, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);

    // indicatore delle fasi
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    int r = S(10), y = HeadTop() + S(12) + S(11);
    int x0 = pad + S(4), avail = rc.right - 2 * pad - S(8);
    int stepW = avail / 4;
    for (int i = 0; i < 4; ++i) {
        int cx = x0 + i * stepW + r;
        int st = g_res->steps[i];
        COLORREF c = st == ST_DONE ? col::ok : st == ST_RUN ? col::accent : st == ST_FAIL ? col::warn : RGB(196, 202, 212);
        std::wstring lbl = kSteps[i];
        if (st == ST_SKIP) lbl += L" (saltato)";
        HFONT lf = st == ST_RUN ? g_fontBold : g_font;
        int lblEnd = cx + r + S(8) + TextW(dc, lbl, lf);
        if (i < 3) {   // connettore dopo l'etichetta fino al cerchio successivo
            int nx = x0 + (i + 1) * stepW + r;
            if (nx - r - S(8) > lblEnd + S(10)) {
                Pen p(GC(st == ST_DONE ? col::ok : RGB(214, 219, 227)), (REAL)S(2));
                if (st == ST_SKIP) p.SetDashStyle(DashStyleDash);
                g.DrawLine(&p, lblEnd + S(10), y, nx - r - S(8), y);
            }
        }
        if (st == ST_WAIT || st == ST_SKIP) {
            Pen p(GC(c), (REAL)S(2));
            g.DrawEllipse(&p, cx - r, y - r, 2 * r, 2 * r);
        } else {
            if (st == ST_RUN) {
                SolidBrush halo(GC(c, 50));
                g.FillEllipse(&halo, cx - r - S(4), y - r - S(4), 2 * r + S(8), 2 * r + S(8));
            }
            SolidBrush b(GC(c));
            g.FillEllipse(&b, cx - r, y - r, 2 * r, 2 * r);
        }
        Pen gp(GC(st == ST_WAIT || st == ST_SKIP ? col::muted : RGB(255, 255, 255)), (REAL)S(2));
        gp.SetStartCap(LineCapRound); gp.SetEndCap(LineCapRound); gp.SetLineJoin(LineJoinRound);
        REAL fx = (REAL)cx, fy = (REAL)y, u = (REAL)r / 11.0f;
        if (st == ST_DONE) {          // spunta
            PointF pts[] = { PointF(fx - 5 * u, fy), PointF(fx - 1.5f * u, fy + 4 * u), PointF(fx + 5.5f * u, fy - 4 * u) };
            g.DrawLines(&gp, pts, 3);
        } else if (st == ST_SKIP) {   // trattino
            g.DrawLine(&gp, fx - 4 * u, fy, fx + 4 * u, fy);
        } else if (st == ST_FAIL) {   // punto esclamativo
            g.DrawLine(&gp, fx, fy - 5 * u, fx, fy + 1 * u);
            SolidBrush wb(GC(RGB(255, 255, 255)));
            g.FillEllipse(&wb, fx - 1.3f * u, fy + 3.2f * u, 2.6f * u, 2.6f * u);
        } else {
            RECT gr = { cx - r, y - r, cx + r, y + r };
            DrawTxt(dc, std::to_wstring(i + 1), gr, g_fontBold, st == ST_RUN ? RGB(255, 255, 255) : col::muted,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT lr = { cx + r + S(8), y - S(10), x0 + (i + 1) * stepW - S(4), y + S(10) };
        DrawTxt(dc, lbl, lr, lf, st == ST_WAIT || st == ST_SKIP ? col::muted : col::text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static std::wstring BackupDirFor(const std::wstring& name) {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t ts[64];
    swprintf(ts, 64, L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring safe;
    for (wchar_t c : name) safe += (wcschr(L"\\/:*?\"<>|", c) ? L'_' : c);
    if (safe.size() > 60) safe.resize(60);
    return BackupRoot() + L"\\" + safe + L"_" + ts;
}

static void ResDelete() {
    auto& st = *g_res;
    int n = 0;
    for (int i = 0; i < ListView_GetItemCount(st.list); ++i) {
        LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = i;
        ListView_GetItem(st.list, &it);
        bool c = ListView_GetCheckState(st.list, i) != FALSE;
        st.items[it.lParam].checked = c;
        n += c;
    }
    if (!n) return;
    std::wstring msg = L"Eliminare " + std::to_wstring(n) + L" elementi?\n\n"
                       L"Le chiavi di registro verranno salvate in un backup (.reg) prima dell'eliminazione.";
    if (MessageBoxW(st.hwnd, msg.c_str(), L"Conferma eliminazione", MB_ICONWARNING | MB_YESNO) != IDYES) return;
    bool kill = false;
    for (auto& r : st.items)
        if (r.checked && r.kind == ResKind::Folder && !st.installDir.empty() && _wcsicmp(r.path.c_str(), st.installDir.c_str()) == 0)
            kill = true;
    ResSetBusy(true);
    st.steps[3] = ST_RUN;
    InvalidateRect(st.hwnd, nullptr, FALSE);
    bool recycle = SendMessageW(st.recycle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    auto items = std::make_shared<std::vector<Residue>>(st.items);
    std::wstring dir = st.installDir, backup = BackupDirFor(st.prog.name);
    HWND target = st.hwnd;
    std::thread([items, recycle, dir, backup, target, kill] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto log = MakePostLog(target);
        if (kill) KillProcessesInFolder(dir, log);
        bool reboot = false;
        int err = RemoveResidues(*items, recycle, backup, log, reboot);
        auto* left = new std::vector<Residue>;
        for (auto& r : *items) if (r.checked) left->push_back(r);
        PostStep(target, 3, err ? ST_FAIL : ST_DONE);
        PostMessageW(target, WM_APP_SCANNED, 1, (LPARAM)left);
        PostMessageW(target, WM_APP_REMOVED, err, reboot);
        CoUninitialize();
    }).detach();
}

static LRESULT CALLBACK ResultsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_res->hwnd = hwnd;
        g_res->progress = MakeCtl(PROGRESS_CLASSW, L"", PBS_MARQUEE, hwnd, IDR_PROGRESS);
        g_res->info = MakeCtl(L"STATIC", L"Operazione in corso...", SS_LEFT | SS_ENDELLIPSIS, hwnd, IDR_INFO);
        SendMessageW(g_res->info, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
        g_res->bAll = MakeCtl(L"BUTTON", L"Seleziona tutto", BS_PUSHBUTTON | WS_TABSTOP, hwnd, IDR_ALL);
        g_res->bNone = MakeCtl(L"BUTTON", L"Deseleziona tutto", BS_PUSHBUTTON | WS_TABSTOP, hwnd, IDR_NONE);
        g_res->count = MakeCtl(L"STATIC", L"", SS_LEFT, hwnd, IDR_COUNT);
        g_res->bDetails = MakeCtl(L"BUTTON", L"Mostra registro operazioni", BS_PUSHBUTTON | WS_TABSTOP, hwnd, IDR_DETAILS);
        g_res->list = MakeCtl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP,
                              hwnd, IDR_LIST, WS_EX_CLIENTEDGE);
        SetWindowTheme(g_res->list, L"Explorer", nullptr);
        ListView_SetExtendedListViewStyle(g_res->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
        ListView_EnableGroupView(g_res->list, TRUE);
        HIMAGELIST il = ImageList_Create(S(16), S(16), ILC_COLOR32 | ILC_MASK, 8, 0);
        struct IconSrc { const wchar_t* dll; int id; };
        // un'icona per ogni ResKind, nello stesso ordine dell'enum
        const IconSrc src[] = { {L"regedit.exe", 0}, {L"regedit.exe", 1}, {L"shell32.dll", 316},
                                {L"shell32.dll", 21}, {L"shell32.dll", 47}, {L"shell32.dll", 165},
                                {L"shell32.dll", 3}, {L"shell32.dll", 0} };
        for (auto& s : src) {
            HICON hi = nullptr; UINT id;
            if (PrivateExtractIconsW(s.dll, s.id, S(16), S(16), &hi, &id, 1, 0) != 1 || !hi)
                PrivateExtractIconsW(s.dll, 0, S(16), S(16), &hi, &id, 1, 0);
            if (!hi) hi = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
            ImageList_AddIcon(il, hi);
            DestroyIcon(hi);
        }
        ListView_SetImageList(g_res->list, il, LVSIL_SMALL);
        LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)L"Elemento"; c.cx = S(560); ListView_InsertColumn(g_res->list, 0, &c);
        c.pszText = (LPWSTR)L"Motivo"; c.cx = S(240); ListView_InsertColumn(g_res->list, 1, &c);
        g_res->log = MakeCtl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | ES_NOHIDESEL,
                             hwnd, IDR_LOG, WS_EX_CLIENTEDGE);
        SendMessageW(g_res->log, EM_SETLIMITTEXT, 0, 0);
        g_res->recycle = MakeCtl(L"BUTTON", L"Sposta i file nel Cestino", BS_AUTOCHECKBOX | WS_TABSTOP, hwnd, IDR_RECYCLE);
        SendMessageW(g_res->recycle, BM_SETCHECK, BST_CHECKED, 0);
        g_res->bDel = MakeCtl(L"BUTTON", L"Elimina selezionati", BS_OWNERDRAW | WS_TABSTOP, hwnd, IDR_DELETE);
        g_res->bClose = MakeCtl(L"BUTTON", L"Chiudi", BS_PUSHBUTTON | WS_TABSTOP, hwnd, IDR_CLOSE);
        ResSetBusy(true);
        ResLayout(hwnd);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        rc.top = ResHeaderH();
        FillSolid((HDC)wp, rc, RGB(255, 255, 255));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int hh = ResHeaderH();
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, hh);
        HGDIOBJ o = SelectObject(mem, bmp);
        PaintResHeader(mem, rc);
        BitBlt(dc, 0, 0, rc.right, hh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, o); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(255, 255, 255));
        if ((HWND)lp == g_res->count) SetTextColor(dc, col::muted);
        if ((HWND)lp == g_res->info) SetTextColor(dc, g_res->removed && !g_res->items.empty() ? col::warn : col::text);
        if ((HWND)lp == g_res->log) { SetBkColor(dc, RGB(248, 249, 251)); return (LRESULT)g_brInfo; }
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_CTLCOLORBTN: return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlID != IDR_DELETE) break;
        bool en = !(di->itemState & ODS_DISABLED), down = (di->itemState & ODS_SELECTED) != 0;
        RECT r = di->rcItem;
        FillSolid(di->hDC, r, RGB(255, 255, 255));
        Graphics g(di->hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        COLORREF c = !en ? RGB(190, 200, 214) : down ? RGB(0, 84, 160) : col::accent;
        FillRound(g, RectF(0.5f, 0.5f, (REAL)(r.right - 1), (REAL)(r.bottom - 1)), (REAL)S(5), GC(c));
        DrawTxt(di->hDC, L"Elimina selezionati", r, g_fontBold, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (di->itemState & ODS_FOCUS) { InflateRect(&r, -S(3), -S(3)); DrawFocusRect(di->hDC, &r); }
        return TRUE;
    }
    case WM_SIZE: ResLayout(hwnd); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = { S(560), S(400) }; return 0;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        ResLog(*s);
        return 0;
    }
    case WM_APP_STEP:
        if (wp < 4) g_res->steps[wp] = (int)lp;
        { RECT hr; GetClientRect(hwnd, &hr); hr.bottom = ResHeaderH(); InvalidateRect(hwnd, &hr, FALSE); }
        return 0;
    case WM_APP_SCANNED: {
        std::unique_ptr<std::vector<Residue>> v((std::vector<Residue>*)lp);
        g_res->items = *v;
        g_res->removed = wp == 1;
        ResFill();
        if (wp == 0) ResSetBusy(false);
        return 0;
    }
    case WM_APP_REMOVED: {
        ResSetBusy(false);
        int err = (int)wp;
        std::wstring m = err == 0 ? L"Rimozione completata." :
                         L"Rimozione completata con " + std::to_wstring(err) + L" errori.\nGli elementi non rimossi restano nell'elenco.";
        if (lp) {
            m += L"\n\nAlcuni file erano in uso e verranno eliminati al prossimo riavvio.\nRiavviare ora?";
            if (MessageBoxW(hwnd, m.c_str(), L"ZeroTrace", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                HANDLE tok; TOKEN_PRIVILEGES tp{ 1 };
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
                    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
                    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
                    CloseHandle(tok);
                }
                ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED);
            }
        } else if (err) {
            MessageBoxW(hwnd, m.c_str(), L"ZeroTrace", MB_ICONWARNING);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDR_ALL: case IDR_NONE:
            for (int i = 0; i < ListView_GetItemCount(g_res->list); ++i)
                ListView_SetCheckState(g_res->list, i, LOWORD(wp) == IDR_ALL);
            ResUpdateCount();
            return 0;
        case IDR_DETAILS:
            g_res->showLog = !g_res->showLog;
            SetWindowTextW(g_res->bDetails, g_res->showLog ? L"Nascondi registro operazioni" : L"Mostra registro operazioni");
            ResLayout(hwnd);
            return 0;
        case IDR_DELETE: ResDelete(); return 0;
        case IDR_CLOSE: case IDCANCEL: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->idFrom != IDR_LIST) break;
        if (nh->code == LVN_ITEMCHANGED && (((NMLISTVIEW*)lp)->uChanged & LVIF_STATE)) ResUpdateCount();
        if (nh->code == NM_DBLCLK) {
            int i = ListView_GetNextItem(g_res->list, -1, LVNI_SELECTED);
            if (i >= 0) {
                LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = i;
                ListView_GetItem(g_res->list, &it);
                auto& r = g_res->items[it.lParam];
                if (r.kind == ResKind::File || r.kind == ResKind::Folder) OpenInExplorer(r.path);
                else if (r.kind == ResKind::RegKey) OpenRegeditAt(r.Display());
                else if (r.kind == ResKind::RegValue) { std::wstring d = r.Display(); OpenRegeditAt(d.substr(0, d.find(L"  →  "))); }
            }
        }
        break;
    }
    case WM_CLOSE:
        if (g_res->busy) {
            MessageBoxW(hwnd, L"Attendi il termine dell'operazione in corso.", L"ZeroTrace", MB_ICONINFORMATION);
            return 0;
        }
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        HIMAGELIST il = ListView_GetImageList(g_res->list, LVSIL_SMALL);
        if (il) ImageList_Destroy(il);
        if (g_res->icon) DestroyIcon(g_res->icon);
        delete g_res; g_res = nullptr;
        LoadProgramsAsync();
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// (punto di ripristino) -> (disinstallatore) -> scansione
static void StartUninstall(const Program& prog, bool runUninstaller) {
    if (g_res) return;
    ScanLevel level = (ScanLevel)SendMessageW(g_level, CB_GETCURSEL, 0, 0);
    bool restore = SendMessageW(g_restore, BM_GETCHECK, 0, 0) == BST_CHECKED;

    g_res = new ResultsState;
    g_res->prog = prog;
    g_res->installDir = ResolveInstallDir(prog);   // letta PRIMA: il disinstallatore cancella la voce
    g_res->icon = IconFor(prog.displayIcon, S(48));
    if (!restore) g_res->steps[0] = ST_SKIP;
    if (!runUninstaller) g_res->steps[1] = ST_SKIP;
    std::wstring title = L"ZeroTrace - " + prog.name;
    RECT mr; GetWindowRect(g_main, &mr);
    HWND w = CreateWindowExW(0, L"ZeroTraceResults", title.c_str(), WS_OVERLAPPEDWINDOW,
                             mr.left + S(40), mr.top + S(30), (mr.right - mr.left) - S(80) > S(900) ? S(900) : (mr.right - mr.left) - S(80),
                             (mr.bottom - mr.top) - S(60) > S(620) ? S(620) : (mr.bottom - mr.top) - S(60), g_main, nullptr, g_inst, nullptr);
    EnableWindow(g_main, FALSE);
    ShowWindow(w, SW_SHOW);

    HWND target = w;
    std::thread([prog, runUninstaller, level, restore, target] {
        auto log = MakePostLog(target);
        if (restore) {
            PostStep(target, 0, ST_RUN);
            PostStep(target, 0, CreateRestorePoint(L"ZeroTrace: rimozione " + prog.name, log) ? ST_DONE : ST_FAIL);
        }
        if (runUninstaller) {
            PostStep(target, 1, ST_RUN);
            DWORD code = 0;
            log(L"── Disinstallatore del programma ──");
            bool ok = RunUninstaller(prog, code, log);
            if (!ok) log(L"Il disinstallatore non è disponibile: procedo con la rimozione forzata.");
            PostStep(target, 1, ok ? ST_DONE : ST_FAIL);
            Sleep(1000);   // lascia a Explorer/servizi il tempo di rilasciare i file
        } else {
            log(L"Rimozione forzata: il disinstallatore non viene eseguito.");
        }
        PostStep(target, 2, ST_RUN);
        const wchar_t* lv[] = { L"Sicura", L"Moderata", L"Avanzata" };
        log(std::wstring(L"── Scansione residui (modalità ") + lv[(int)level] + L") ──");
        auto* res = new std::vector<Residue>(ScanResidues(prog, level, log));
        PostStep(target, 2, ST_DONE);
        PostMessageW(target, WM_APP_SCANNED, 0, (LPARAM)res);
    }).detach();
}

static void TracesFromFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opt; dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Scegli la cartella del programma da rimuovere");
    std::wstring path;
    if (SUCCEEDED(dlg->Show(g_main))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    dlg->Release();
    if (path.empty()) return;
    if (IsProtectedPath(path)) {
        MessageBoxW(g_main, L"Questa è una cartella di sistema protetta e non può essere usata.", L"ZeroTrace", MB_ICONERROR);
        return;
    }
    Program p;
    p.installLocation = path;
    p.name = path.substr(path.find_last_of(L'\\') + 1);
    std::wstring m = L"Cercare e rimuovere tutte le tracce di \"" + p.name + L"\"?\n\n" + path;
    if (MessageBoxW(g_main, m.c_str(), L"ZeroTrace", MB_ICONQUESTION | MB_YESNO) == IDYES)
        StartUninstall(p, false);
}

// ======================================================================
//  Finestra principale
// ======================================================================
static void SetIconView(bool on) {
    // "icone grandi": stesso elenco dettagli con righe alte e icone da 32 px (come Revo)
    g_iconView = on;
    ListView_SetImageList(g_list, on ? g_imgMed : g_imgSmall, LVSIL_SMALL);
    InvalidateRect(g_list, nullptr, TRUE);
}

// colonne proporzionali alla larghezza disponibile (niente barra di scorrimento orizzontale)
static void FitColumns() {
    RECT r; GetClientRect(g_list, &r);
    int avail = r.right - GetSystemMetrics(SM_CXVSCROLL) - S(2);
    if (avail < S(200)) return;
    static const int pct[6] = { 34, 20, 12, 10, 11, 13 };
    int used = 0;
    for (int i = 0; i < 6; ++i) {
        int w = i == 5 ? avail - used : avail * pct[i] / 100;
        ListView_SetColumnWidth(g_list, i, w);
        used += w;
    }
}

static void MainLayout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr; GetWindowRect(g_status, &sr);
    int sh = sr.bottom - sr.top;
    int th = ToolbarHeight(), sw = g_sideW, ih = InfoHeight();
    MoveWindow(g_toolbar, 0, 0, rc.right, th, TRUE);
    MoveWindow(g_sidebar, 0, th, sw, rc.bottom - th - sh, TRUE);
    MoveWindow(g_list, sw, th, rc.right - sw, rc.bottom - th - sh - ih, TRUE);
    MoveWindow(g_info, sw, rc.bottom - sh - ih, rc.right - sw, ih, TRUE);
    FitColumns();
    int p0 = TextWidth(L"0000 programmi visualizzati", g_font) + S(24);
    int parts[] = { p0, p0 + TextWidth(L"Spazio stimato: 999.99 GB", g_font) + S(24), -1 };
    SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
}

static void ShowContextMenu(HWND hwnd, POINT pt) {
    Entry* e = SelectedEntry();
    if (!e) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_UNINSTALL, L"&Disinstalla\tInvio");
    AppendMenuW(m, MF_STRING, CMD_FORCE, L"&Forza rimozione\tMaiusc+Canc");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (e->dir.empty() ? MF_GRAYED : 0), CMD_OPENDIR, L"Apri &cartella di installazione");
    AppendMenuW(m, MF_STRING, CMD_OPENREG, L"Apri voce nel &Registro");
    AppendMenuW(m, MF_STRING, CMD_COPYNAME, L"Copia &nome");
    SetMenuDefaultItem(m, CMD_UNINSTALL, FALSE);
    if (pt.x == -1) {
        RECT r; int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        ListView_GetItemRect(g_list, i, &r, LVIR_LABEL);
        pt = { r.left, r.bottom }; ClientToScreen(g_list, &pt);
    }
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static void DoUninstall(bool force) {
    Entry* e = SelectedEntry();
    if (!e) return;
    if (!force && e->orphan) {
        int r = MessageBoxW(g_main, (L"Il disinstallatore di \"" + e->p.name + L"\" non esiste più.\n\n"
                                     L"Procedere con la rimozione forzata (ricerca e rimozione diretta dei residui)?").c_str(),
                            L"ZeroTrace", MB_ICONWARNING | MB_YESNO);
        if (r != IDYES) return;
        StartUninstall(e->p, false);
        return;
    }
    std::wstring m;
    if (force)
        m = L"Rimozione forzata di \"" + e->p.name + L"\".\n\nIl disinstallatore del programma NON verrà eseguito: "
            L"ZeroTrace cercherà e rimuoverà direttamente file, cartelle e chiavi di registro.\n\nContinuare?";
    else
        m = L"Disinstallare \"" + e->p.name + L"\"?\n\nVerrà avviato il disinstallatore del programma; al termine "
            L"ZeroTrace cercherà i residui rimasti.";
    if (MessageBoxW(g_main, m.c_str(), L"ZeroTrace", (force ? MB_ICONWARNING : MB_ICONQUESTION) | MB_YESNO) != IDYES) return;
    StartUninstall(e->p, !force);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_toolbar = CreateWindowExW(0, L"ZeroTraceToolbar", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0,
                                    hwnd, (HMENU)IDC_TOOLBAR, g_inst, nullptr);
        for (auto& b : g_tb) b.h = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(b.icon), IMAGE_ICON, S(24), S(24), 0);
        g_search = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, g_toolbar, IDC_SEARCH, WS_EX_CLIENTEDGE);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Cerca per nome o editore  (Ctrl+F)");
        g_level = MakeCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, g_toolbar, IDC_LEVEL);
        for (auto s : { L"Sicura", L"Moderata", L"Avanzata" }) SendMessageW(g_level, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(g_level, CB_SETCURSEL, 1, 0);
        g_restore = MakeCtl(L"BUTTON", L"Punto di ripristino", BS_AUTOCHECKBOX | WS_TABSTOP, g_toolbar, IDC_RESTORE);

        g_sidebar = CreateWindowExW(0, L"ZeroTraceSidebar", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                    (HMENU)IDC_SIDEBAR, g_inst, nullptr);
        SidebarLayout();
        g_info = CreateWindowExW(0, L"ZeroTraceInfo", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                 (HMENU)IDC_INFO, g_inst, nullptr);

        g_list = MakeCtl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA | WS_TABSTOP, hwnd, IDC_LIST);
        SetWindowTheme(g_list, L"Explorer", nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        g_imgSmall = ImageList_Create(S(16), S(16), ILC_COLOR32 | ILC_MASK, 256, 64);
        g_imgMed = ImageList_Create(S(32), S(32), ILC_COLOR32 | ILC_MASK, 256, 64);
        g_imgLarge = ImageList_Create(S(48), S(48), ILC_COLOR32 | ILC_MASK, 256, 64);
        ListView_SetImageList(g_list, g_imgSmall, LVSIL_SMALL);
        struct Col { const wchar_t* t; int w; int fmt; };
        const Col cols[] = { {L"Nome", 320, LVCFMT_LEFT}, {L"Editore", 190, LVCFMT_LEFT}, {L"Versione", 110, LVCFMT_LEFT},
                             {L"Dimensione", 90, LVCFMT_RIGHT}, {L"Installato il", 95, LVCFMT_LEFT}, {L"Tipo", 120, LVCFMT_LEFT} };
        for (int i = 0; i < 6; ++i) {
            LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.pszText = (LPWSTR)cols[i].t; c.cx = S(cols[i].w); c.fmt = cols[i].fmt;
            ListView_InsertColumn(g_list, i, &c);
        }
        {   // freccia di ordinamento iniziale
            HDITEMW hd{}; hd.mask = HDI_FORMAT;
            HWND hdr = ListView_GetHeader(g_list);
            Header_GetItem(hdr, 0, &hd); hd.fmt |= HDF_SORTUP; Header_SetItem(hdr, 0, &hd);
        }
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                   hwnd, (HMENU)IDC_STATUS, g_inst, nullptr);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
        MainLayout(hwnd);
        SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)(IsRunningAsAdmin() ? L"  ● Amministratore"
                                                         : L"  ● Senza privilegi amministrativi: alcuni residui non saranno rimovibili"));
        LoadProgramsAsync();
        SetFocus(g_list);
        return 0;
    }
    case WM_SIZE: MainLayout(hwnd); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = { S(640), S(420) }; return 0;
    case WM_APP_LISTED: {
        std::unique_ptr<ListResult> r((ListResult*)lp);
        ImageList_RemoveAll(g_imgSmall);
        ImageList_RemoveAll(g_imgMed);
        ImageList_RemoveAll(g_imgLarge);
        HIMAGELIST lists[] = { g_imgSmall, g_imgMed, g_imgLarge };
        int sizes[] = { S(16), S(32), S(48) };
        for (int k = 0; k < 3; ++k) {   // indice 0 = icona generica
            HICON d = IconFor(L"shell32.dll,2", sizes[k]);
            ImageList_AddIcon(lists[k], d ? d : LoadIconW(nullptr, IDI_APPLICATION));
            if (d) DestroyIcon(d);
        }
        for (size_t i = 0; i < r->small.size(); ++i) {
            ImageList_AddIcon(g_imgSmall, r->small[i]); DestroyIcon(r->small[i]);
            ImageList_AddIcon(g_imgMed, r->med[i]); DestroyIcon(r->med[i]);
            ImageList_AddIcon(g_imgLarge, r->large[i]); DestroyIcon(r->large[i]);
        }
        g_programs = std::move(r->entries);
        RebuildView();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CMD_UNINSTALL: DoUninstall(false); return 0;
        case CMD_FORCE: DoUninstall(true); return 0;
        case CMD_TRACES: TracesFromFolder(); return 0;
        case CMD_REFRESH: LoadProgramsAsync(); return 0;
        case CMD_VIEW: SetIconView(!g_iconView); return 0;
        case CMD_BACKUPS: {
            std::wstring b = BackupRoot();
            SHCreateDirectoryExW(nullptr, b.c_str(), nullptr);
            OpenInExplorer(b);
            return 0;
        }
        case CMD_APPWIZ: ShellExecuteW(nullptr, L"open", L"control.exe", L"appwiz.cpl", nullptr, SW_SHOWNORMAL); return 0;
        case IDC_SEARCH: if (HIWORD(wp) == EN_CHANGE) RebuildView(); return 0;
        case CMD_OPENDIR: if (Entry* e = SelectedEntry()) OpenInExplorer(e->dir); return 0;
        case CMD_OPENREG: if (Entry* e = SelectedEntry()) OpenRegeditAt(RegPathOf(e->p)); return 0;
        case CMD_COPYNAME:
            if (Entry* e = SelectedEntry()) {
                if (OpenClipboard(hwnd)) {
                    EmptyClipboard();
                    size_t bytes = (e->p.name.size() + 1) * sizeof(wchar_t);
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    memcpy(GlobalLock(g), e->p.name.c_str(), bytes); GlobalUnlock(g);
                    SetClipboardData(CF_UNICODETEXT, g);
                    CloseClipboard();
                }
            }
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->idFrom != IDC_LIST) break;
        switch (nh->code) {
        case LVN_GETDISPINFOW: {
            auto* di = (NMLVDISPINFOW*)lp;
            if (di->item.iItem < 0 || di->item.iItem >= (int)g_view.size()) return 0;
            auto& e = g_programs[g_view[di->item.iItem]];
            static std::wstring tmp;
            if (di->item.mask & LVIF_TEXT) {
                switch (di->item.iSubItem) {
                case 0: tmp = e.p.name; break;
                case 1: tmp = e.p.publisher; break;
                case 2: tmp = e.p.version; break;
                case 3: tmp = FormatSize(e.p.sizeKB); break;
                case 4: tmp = FormatDate(e.p.installDate); break;
                case 5: tmp = WhereText(e.p); break;
                }
                lstrcpynW(di->item.pszText, tmp.c_str(), di->item.cchTextMax);
            }
            if (di->item.mask & LVIF_IMAGE) di->item.iImage = e.icon;
            return 0;
        }
        case LVN_ITEMCHANGED:
            if (((NMLISTVIEW*)lp)->uChanged & LVIF_STATE) {
                InvalidateRect(g_info, nullptr, FALSE);
                InvalidateRect(g_toolbar, nullptr, FALSE);
            }
            return 0;
        case NM_CUSTOMDRAW: {   // voci orfane in arancione
            auto* cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                size_t i = cd->nmcd.dwItemSpec;
                if (i < g_view.size() && g_programs[g_view[i]].orphan) cd->clrText = col::warn;
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        case LVN_COLUMNCLICK: {
            int c = ((NMLISTVIEW*)lp)->iSubItem;
            if (c == g_sortCol) g_sortAsc = !g_sortAsc; else { g_sortCol = c; g_sortAsc = true; }
            HWND hdr = ListView_GetHeader(g_list);
            for (int i = 0; i < 6; ++i) {
                HDITEMW h{}; h.mask = HDI_FORMAT;
                Header_GetItem(hdr, i, &h);
                h.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
                if (i == g_sortCol) h.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
                Header_SetItem(hdr, i, &h);
            }
            RebuildView();
            return 0;
        }
        case LVN_ODFINDITEMW: {
            auto* fi = (NMLVFINDITEMW*)lp;
            if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || g_view.empty()) return -1;
            size_t n = wcslen(fi->lvfi.psz);
            for (size_t k = 0; k < g_view.size(); ++k) {
                size_t i = (fi->iStart + k) % g_view.size();
                if (_wcsnicmp(g_programs[g_view[i]].p.name.c_str(), fi->lvfi.psz, n) == 0) return (LRESULT)i;
            }
            return -1;
        }
        case NM_DBLCLK: if (SelectedEntry()) DoUninstall(false); return 0;
        case LVN_KEYDOWN: {
            WORD k = ((NMLVKEYDOWN*)lp)->wVKey;
            if (k == VK_RETURN) DoUninstall(false);
            else if (k == VK_DELETE) DoUninstall(GetKeyState(VK_SHIFT) < 0);
            else if (k == VK_F5) LoadProgramsAsync();
            else if (k == 'F' && GetKeyState(VK_CONTROL) < 0) SetFocus(g_search);
            return 0;
        }
        }
        break;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_list) { ShowContextMenu(hwnd, { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi;
    ULONG_PTR gtok;
    GdiplusStartup(&gtok, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD; g_fontBold = CreateFontIndirectW(&lf);
    LOGFONTW lt = ncm.lfMessageFont; lt.lfHeight = lt.lfHeight * 13 / 10; lt.lfWeight = FW_SEMIBOLD;
    g_fontTitle = CreateFontIndirectW(&lt);
    LOGFONTW lb = ncm.lfMessageFont; lb.lfHeight = lb.lfHeight * 3 / 2; lb.lfWeight = FW_SEMIBOLD;
    g_fontBig = CreateFontIndirectW(&lb);
    LOGFONTW ls = ncm.lfMessageFont; ls.lfHeight = ls.lfHeight * 9 / 10; ls.lfWeight = FW_SEMIBOLD;
    g_fontSmall = CreateFontIndirectW(&ls);
    g_lineH = FontHeight(g_font);
    g_bigH = FontHeight(g_fontBig);
    g_titleH = FontHeight(g_fontTitle);
    g_brToolbar = CreateSolidBrush(col::toolbar);
    g_brInfo = CreateSolidBrush(RGB(248, 249, 251));

    g_appIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(48), S(48), 0);
    HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON sm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = big; wc.hIconSm = sm;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpfnWndProc = MainProc;    wc.lpszClassName = L"ZeroTraceMain";    RegisterClassExW(&wc);
    wc.lpfnWndProc = ResultsProc; wc.lpszClassName = L"ZeroTraceResults"; RegisterClassExW(&wc);
    wc.hbrBackground = nullptr;
    wc.style = CS_HREDRAW | CS_VREDRAW;   // i pannelli disegnati a mano si ridisegnano interi
    wc.lpfnWndProc = ToolbarProc; wc.lpszClassName = L"ZeroTraceToolbar"; RegisterClassExW(&wc);
    wc.lpfnWndProc = SidebarProc; wc.lpszClassName = L"ZeroTraceSidebar"; RegisterClassExW(&wc);
    wc.lpfnWndProc = InfoProc;    wc.lpszClassName = L"ZeroTraceInfo";    RegisterClassExW(&wc);

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = S(1040), wh = S(660);
    if (ww > (wa.right - wa.left) * 92 / 100) ww = (wa.right - wa.left) * 92 / 100;
    if (wh > (wa.bottom - wa.top) * 92 / 100) wh = (wa.bottom - wa.top) * 92 / 100;
    HWND w = CreateWindowExW(0, L"ZeroTraceMain", L"ZeroTrace — Disinstallazione completa", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh,
                             nullptr, nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        HWND dlgTarget = g_res && IsChild(g_res->hwnd, m.hwnd) ? g_res->hwnd : nullptr;
        if (dlgTarget && IsDialogMessageW(dlgTarget, &m)) continue;
        if (m.message == WM_KEYDOWN && m.wParam == 'F' && GetKeyState(VK_CONTROL) < 0 && !g_res) { SetFocus(g_search); continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    GdiplusShutdown(gtok);
    CoUninitialize();
    return 0;
}
