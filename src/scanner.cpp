// ZeroTrace - disinstallatore stile Revo Uninstaller
// scanner.cpp : enumerazione programmi, scansione residui, rimozione forzata
#include "scanner.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <set>
#include <map>
#include <algorithm>
#include <cwctype>

// ============================================================================
//  Utilità stringhe / percorsi
// ============================================================================
std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n\"");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n\"");
    return s.substr(a, b - a + 1);
}
static bool IEq(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
static bool IStarts(const std::wstring& s, const std::wstring& pre) {
    return s.size() >= pre.size() && _wcsnicmp(s.c_str(), pre.c_str(), pre.size()) == 0;
}
// Solo lettere e cifre minuscole: "Stubborn App" == "stubbornapp"
static std::wstring Norm(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) if (iswalnum(c)) r += (wchar_t)towlower(c);
    return r;
}
static std::wstring Expand(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[4096];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 4096);
    return (n && n < 4096) ? std::wstring(buf) : s;
}
static std::wstring StripSlash(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}
static std::wstring FullPath(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr);
    std::wstring r = (n && n < MAX_PATH * 4) ? buf : p;
    std::replace(r.begin(), r.end(), L'/', L'\\');
    return StripSlash(r);
}
static std::wstring ParentDir(const std::wstring& p) {
    size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? L"" : p.substr(0, k);
}
static std::wstring LeafName(const std::wstring& p) {
    size_t k = p.find_last_of(L'\\');
    return k == std::wstring::npos ? p : p.substr(k + 1);
}
static bool PathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return StripSlash(r);
}
// Il percorso "dir" compare dentro "text" come percorso completo (non come prefisso
// di un'altra cartella: "C:\App" non deve combaciare con "C:\AppData").
static bool PathBoundary(wchar_t c) {
    return c == 0 || c == L'\\' || c == L'"' || c == L' ' || c == L';' || c == L',' || c == L'/' || c == L'|';
}
static bool ContainsPathN(const wchar_t* t, size_t n, const std::wstring& dirLower) {
    size_t m = dirLower.size();
    if (m < 6 || n < m) return false;
    const wchar_t first = dirLower[0];
    for (size_t i = 0; i + m <= n; ++i) {
        if ((wchar_t)towlower(t[i]) != first) continue;
        size_t k = 1;
        while (k < m && (wchar_t)towlower(t[i + k]) == dirLower[k]) ++k;
        if (k == m && PathBoundary(i + m < n ? t[i + m] : 0)) return true;
    }
    return false;
}
// Estrae il percorso dell'eseguibile da una riga di comando
static std::wstring ExeFromCommand(std::wstring cmd) {
    cmd = Trim(Expand(cmd));
    if (cmd.empty()) return L"";
    // DisplayIcon tipo "C:\x\app.exe,0"
    if (!cmd.empty() && cmd[0] == L'"') {
        size_t q = cmd.find(L'"', 1);
        return q == std::wstring::npos ? cmd.substr(1) : cmd.substr(1, q - 1);
    }
    std::wstring l = Lower(cmd);
    size_t k = l.find(L".exe");
    if (k != std::wstring::npos) return cmd.substr(0, k + 4);
    size_t c = cmd.find(L',');
    if (c != std::wstring::npos) cmd = cmd.substr(0, c);
    return Trim(cmd);
}

// ============================================================================
//  Registro
// ============================================================================
static std::wstring RegStr(HKEY h, const wchar_t* name) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(h, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return L"";
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return L"";
    std::vector<BYTE> buf(size + 4, 0);
    if (RegQueryValueExW(h, name, nullptr, &type, buf.data(), &size) != ERROR_SUCCESS) return L"";
    std::wstring s(reinterpret_cast<wchar_t*>(buf.data()));
    return type == REG_EXPAND_SZ ? Expand(s) : s;
}
static DWORD RegDw(HKEY h, const wchar_t* name) {
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegQueryValueExW(h, name, nullptr, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS || type != REG_DWORD) return 0;
    return v;
}
static HKEY RegOpen(HKEY root, REGSAM view, const std::wstring& path, REGSAM acc = KEY_READ) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, acc | view, &h) != ERROR_SUCCESS) return nullptr;
    return h;
}
static bool RegKeyExists(HKEY root, REGSAM view, const std::wstring& path) {
    HKEY h = RegOpen(root, view, path);
    if (h) { RegCloseKey(h); return true; }
    return false;
}
static std::vector<std::wstring> RegSubkeys(HKEY h) {
    std::vector<std::wstring> r;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = 512;
        LONG e = RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e == ERROR_SUCCESS) r.push_back(name);
        else if (e != ERROR_MORE_DATA) break;
    }
    return r;
}
static std::wstring RootName(HKEY root) {
    if (root == HKEY_LOCAL_MACHINE) return L"HKLM";
    if (root == HKEY_CURRENT_USER) return L"HKCU";
    if (root == HKEY_CLASSES_ROOT) return L"HKCR";
    if (root == HKEY_USERS) return L"HKU";
    return L"HK?";
}
// Percorso "reale" visualizzato (con WOW6432Node per la vista a 32 bit)
static std::wstring ShownRegPath(HKEY root, REGSAM view, const std::wstring& path) {
    std::wstring p = path;
    if (root == HKEY_LOCAL_MACHINE && view == KEY_WOW64_32KEY && IStarts(p, L"SOFTWARE\\") &&
        !IStarts(p, L"SOFTWARE\\Classes"))
        p = L"SOFTWARE\\WOW6432Node\\" + p.substr(9);
    return RootName(root) + L"\\" + p;
}

std::wstring Program::RootName() const { return ::RootName(root); }
std::wstring Program::UninstallKeyPath() const {
    return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + keyName;
}

std::wstring Residue::Display() const {
    switch (kind) {
    case ResKind::RegKey:   return ShownRegPath(root, view, path);
    case ResKind::RegValue: return ShownRegPath(root, view, path) + L"  →  " +
                                   (valueName.empty() ? L"(Predefinito)" : valueName);
    case ResKind::Service:  return L"Servizio: " + path;
    case ResKind::Task:     return L"Attività pianificata: " + path;
    case ResKind::Firewall: return L"Regola firewall: " + extra;
    case ResKind::PathEntry: return std::wstring(root == HKEY_LOCAL_MACHINE ? L"PATH di sistema" : L"PATH dell'utente") +
                                    L":  " + extra;
    default:                return path;
    }
}

// ============================================================================
//  Programmi installati
// ============================================================================
std::vector<Program> EnumeratePrograms() {
    std::vector<Program> out;
    std::set<std::wstring> seen;
    struct Loc { HKEY root; REGSAM view; };
    const Loc locs[] = { {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY},
                         {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
                         {HKEY_CURRENT_USER, 0} };
    const std::wstring base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (auto& l : locs) {
        HKEY h = RegOpen(l.root, l.view, base);
        if (!h) continue;
        for (auto& sub : RegSubkeys(h)) {
            HKEY k = RegOpen(l.root, l.view, base + L"\\" + sub);
            if (!k) continue;
            Program p;
            p.name = RegStr(k, L"DisplayName");
            bool skip = p.name.empty() || RegDw(k, L"SystemComponent") == 1 ||
                        !RegStr(k, L"ParentKeyName").empty();
            std::wstring rt = Lower(RegStr(k, L"ReleaseType"));
            if (rt == L"update" || rt == L"hotfix" || rt == L"security update") skip = true;
            if (!skip) {
                p.publisher = RegStr(k, L"Publisher");
                p.version = RegStr(k, L"DisplayVersion");
                p.installDate = RegStr(k, L"InstallDate");
                p.installLocation = RegStr(k, L"InstallLocation");
                p.uninstallString = RegStr(k, L"UninstallString");
                p.quietUninstallString = RegStr(k, L"QuietUninstallString");
                p.displayIcon = RegStr(k, L"DisplayIcon");
                p.sizeKB = RegDw(k, L"EstimatedSize");
                p.isMsi = RegDw(k, L"WindowsInstaller") == 1;
                p.keyName = sub;
                p.root = l.root;
                p.view = l.view;
                // Le chiavi condivise appaiono in entrambe le viste su alcuni sistemi
                std::wstring id = Lower(p.name + L"|" + p.version + L"|" + sub + L"|" + RootName(l.root));
                if (seen.insert(id).second) out.push_back(std::move(p));
            }
            RegCloseKey(k);
        }
        RegCloseKey(h);
    }
    std::sort(out.begin(), out.end(), [](const Program& a, const Program& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

// ============================================================================
//  Percorsi protetti: non verranno MAI eliminati
// ============================================================================
static std::vector<std::wstring> ProtectedRoots() {
    static std::vector<std::wstring> roots;
    if (!roots.empty()) return roots;
    const KNOWNFOLDERID* ids[] = {
        &FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
        &FOLDERID_ProgramFilesCommon, &FOLDERID_ProgramFilesCommonX86, &FOLDERID_ProgramData,
        &FOLDERID_RoamingAppData, &FOLDERID_LocalAppData, &FOLDERID_LocalAppDataLow,
        &FOLDERID_Profile, &FOLDERID_UserProfiles, &FOLDERID_Public, &FOLDERID_Desktop,
        &FOLDERID_PublicDesktop, &FOLDERID_Documents, &FOLDERID_Downloads,
        &FOLDERID_Programs, &FOLDERID_CommonPrograms, &FOLDERID_StartMenu,
        &FOLDERID_CommonStartMenu, &FOLDERID_Startup, &FOLDERID_CommonStartup,
        &FOLDERID_System, &FOLDERID_SystemX86 };
    for (auto id : ids) { auto p = KnownFolder(*id); if (!p.empty()) roots.push_back(Lower(p)); }
    // sottocartelle critiche
    std::vector<std::wstring> extra;
    for (auto& r : roots) {
        extra.push_back(r + L"\\microsoft");
        extra.push_back(r + L"\\windowsapps");
        extra.push_back(r + L"\\packages");
        extra.push_back(r + L"\\temp");
        extra.push_back(r + L"\\package cache");
        extra.push_back(r + L"\\windows defender");
        extra.push_back(r + L"\\internet explorer");
        extra.push_back(r + L"\\common files");
    }
    roots.insert(roots.end(), extra.begin(), extra.end());
    return roots;
}
bool IsProtectedPath(const std::wstring& path) {
    std::wstring p = Lower(FullPath(path));
    if (p.size() <= 3) return true;                          // radice di unità
    if (std::count(p.begin(), p.end(), L'\\') < 2) return true; // es. C:\Users
    std::wstring win = Lower(KnownFolder(FOLDERID_Windows));
    if (!win.empty() && (p == win || IStarts(p, win + L"\\"))) return true;
    for (auto& r : ProtectedRoots()) {
        if (p == r) return true;
        if (IStarts(r, p + L"\\")) return true;              // antenato di una radice protetta
    }
    return false;
}

std::wstring ResolveInstallDir(const Program& p) {
    auto ok = [](const std::wstring& d) {
        if (d.size() < 4 || IsProtectedPath(d)) return false;
        std::wstring l = Lower(d);
        if (l.find(L"\\package cache") != std::wstring::npos) return false;
        if (l.find(L"\\installer\\") != std::wstring::npos) return false;
        if (l.find(L"\\temp") != std::wstring::npos) return false;
        return true;
    };
    std::wstring d = FullPath(Trim(Expand(p.installLocation)));
    if (ok(d)) return d;
    std::wstring icon = ExeFromCommand(p.displayIcon);
    if (!icon.empty()) { d = ParentDir(FullPath(icon)); if (ok(d)) return d; }
    std::wstring exe = ExeFromCommand(p.uninstallString);
    std::wstring le = Lower(LeafName(exe));
    if (!exe.empty() && le != L"msiexec.exe" && le != L"rundll32.exe") {
        d = ParentDir(FullPath(exe));
        // alcuni disinstallatori stanno in una sottocartella "uninstall"
        std::wstring leaf = Lower(LeafName(d));
        if (leaf == L"uninstall" || leaf == L"uninst" || leaf == L"_uninst") d = ParentDir(d);
        if (ok(d)) return d;
    }
    return L"";
}

// ============================================================================
//  Esecuzione del disinstallatore
// ============================================================================
static void SplitCommand(const std::wstring& cmd, std::wstring& file, std::wstring& args) {
    std::wstring raw = Expand(cmd);
    size_t a = raw.find_first_not_of(L" \t");
    if (a == std::wstring::npos) { file.clear(); args.clear(); return; }
    raw = raw.substr(a);
    if (raw[0] == L'"') {
        size_t q = raw.find(L'"', 1);
        file = raw.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1);
        args = q == std::wstring::npos ? L"" : raw.substr(q + 1);
    } else {
        std::wstring l = Lower(raw);
        size_t k = l.find(L".exe");
        if (k != std::wstring::npos) { file = raw.substr(0, k + 4); args = raw.substr(k + 4); }
        else { size_t s = raw.find(L' '); file = raw.substr(0, s); args = s == std::wstring::npos ? L"" : raw.substr(s); }
    }
    size_t b = args.find_first_not_of(L" ");
    args = b == std::wstring::npos ? L"" : args.substr(b);
}

bool RunUninstaller(const Program& p, DWORD& exitCode, LogFn log) {
    exitCode = 0;
    std::wstring cmd = Expand(p.uninstallString);
    if (Trim(cmd).empty()) { log(L"Nessun comando di disinstallazione registrato."); return false; }
    // MSI: /I{GUID} apre la manutenzione, /X{GUID} disinstalla
    std::wstring lc = Lower(cmd);
    if (lc.find(L"msiexec") != std::wstring::npos) {
        size_t k = lc.find(L"/i{");
        if (k != std::wstring::npos) cmd[k + 1] = L'X';
        k = lc.find(L"/i {");
        if (k != std::wstring::npos) cmd[k + 1] = L'X';
    }
    log(L"Avvio: " + cmd);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    HANDLE proc = nullptr;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    bool inJob = false;
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                       nullptr, nullptr, &si, &pi)) {
        if (job && AssignProcessToJobObject(job, pi.hProcess)) inJob = true;
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
    } else {
        DWORD err = GetLastError();
        std::wstring file, args;
        SplitCommand(cmd, file, args);
        SHELLEXECUTEINFOW se{ sizeof(se) };
        se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        se.lpVerb = err == ERROR_ELEVATION_REQUIRED ? L"runas" : nullptr;
        se.lpFile = file.c_str();
        se.lpParameters = args.empty() ? nullptr : args.c_str();
        se.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&se) || !se.hProcess) {
            log(L"Impossibile avviare il disinstallatore (errore " + std::to_wstring(GetLastError()) + L").");
            if (job) CloseHandle(job);
            return false;
        }
        proc = se.hProcess;
        if (job && AssignProcessToJobObject(job, proc)) inJob = true;
    }

    log(L"In attesa del termine del disinstallatore (e dei processi figli)...");
    WaitForSingleObject(proc, INFINITE);
    GetExitCodeProcess(proc, &exitCode);
    CloseHandle(proc);
    if (inJob) {
        // Molti disinstallatori (NSIS, Inno) si copiano in %TEMP% e rilanciano:
        // il processo iniziale termina subito, il vero lavoro lo fa il figlio.
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai{};
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ai, sizeof(ai), nullptr))
                break;
            if (ai.ActiveProcesses == 0) break;
            Sleep(400);
        }
    }
    if (job) CloseHandle(job);
    log(L"Disinstallatore terminato (codice " + std::to_wstring(exitCode) + L").");
    return true;
}

// ============================================================================
//  Scansione residui
// ============================================================================
static const wchar_t* kStopWords[] = {
    L"microsoft", L"windows", L"software", L"program", L"programs", L"application", L"app",
    L"apps", L"setup", L"update", L"updater", L"tools", L"tool", L"common", L"system", L"driver",
    L"drivers", L"runtime", L"x64", L"x86", L"64bit", L"32bit", L"inc", L"corp", L"corporation",
    L"llc", L"ltd", L"gmbh", L"the", L"free", L"pro", L"edition", L"classes", L"policies",
    L"wow6432node", L"clients", L"intel", L"nvidia", L"amd", L"default", L"user", L"data",
    L"local", L"roaming", L"temp", L"cache", L"plugins", L"bin", L"version", L"install",
    L"installer", L"uninstall", L"service", L"services", L"client", L"server", L"player" };

static bool GoodCandidate(const std::wstring& s) {
    std::wstring n = Norm(s);
    if (n.size() < 4) return false;
    for (auto w : kStopWords) if (n == w) return false;
    bool digits = std::all_of(n.begin(), n.end(), [](wchar_t c) { return iswdigit(c); });
    return !digits;
}
static std::wstring CleanName(std::wstring s) {
    // rimuove "(x64)", "[...]"
    for (wchar_t open : { L'(', L'[' }) {
        wchar_t close = open == L'(' ? L')' : L']';
        size_t a;
        while ((a = s.find(open)) != std::wstring::npos) {
            size_t b = s.find(close, a);
            s.erase(a, b == std::wstring::npos ? std::wstring::npos : b - a + 1);
        }
    }
    // rimuove token di versione ("1.2.3", "v2", "2024")
    std::wstring out, tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        bool hasDigit = std::any_of(tok.begin(), tok.end(), [](wchar_t c) { return iswdigit(c); });
        bool versiony = hasDigit && (tok.find(L'.') != std::wstring::npos ||
                        std::all_of(tok.begin(), tok.end(), [](wchar_t c) { return iswdigit(c) || c == L'.'; }) ||
                        ((tok[0] == L'v' || tok[0] == L'V') && tok.size() > 1 && iswdigit(tok[1])));
        std::wstring lt = Lower(tok);
        if (!versiony && lt != L"x64" && lt != L"x86" && lt != L"64-bit" && lt != L"32-bit" && lt != L"-") {
            if (!out.empty()) out += L' ';
            out += tok;
        }
        tok.clear();
    };
    for (wchar_t c : s) { if (c == L' ' || c == L'\t') flush(); else tok += c; }
    flush();
    return Trim(out);
}
static std::wstring CleanPublisher(std::wstring s) {
    s = CleanName(s);
    const wchar_t* suf[] = { L", Inc.", L", Inc", L" Inc.", L" Inc", L" Ltd.", L" Ltd", L" LLC", L" GmbH",
                             L" Corporation", L" Corp.", L" Corp", L" Co.", L" S.r.l.", L" Srl",
                             L" Limited", L" S.A.", L" AB", L" B.V.", L"," , L"." };
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto x : suf) {
            std::wstring sx = x;
            if (s.size() > sx.size() && IEq(s.substr(s.size() - sx.size()), sx)) {
                s.erase(s.size() - sx.size()); s = Trim(s); changed = true;
            }
        }
    }
    return s;
}
static bool IsGuid(const std::wstring& s) {
    return s.size() >= 38 && s[0] == L'{' && s[37] == L'}';
}

namespace {
struct Scanner {
    const Program& p;
    ScanLevel level;
    LogFn log;
    std::wstring dir;                       // cartella di installazione
    std::vector<std::wstring> names;        // nomi candidati (prodotto)
    std::wstring publisher;                 // editore pulito
    std::vector<Residue> out;
    std::set<std::wstring> keys;            // dedup
    std::set<std::wstring> progIds;         // ProgID trovati (minuscolo)
    std::set<std::wstring> clsids;          // CLSID trovati (minuscolo, con graffe)
    std::set<std::wstring> selKeys;         // chiavi già selezionate: "root|view|percorso" minuscolo
    std::vector<std::wstring> normNames;    // nomi normalizzati, calcolati una volta
    std::wstring dirLower;
    std::vector<wchar_t> vnBuf = std::vector<wchar_t>(16384);
    std::vector<BYTE> dataBuf = std::vector<BYTE>(65536);

    Scanner(const Program& pp, ScanLevel l, LogFn lg) : p(pp), level(l), log(lg) {}

    bool NameMatch(const std::wstring& s) const {
        std::wstring n = Norm(s);
        if (n.empty()) return false;
        for (auto& c : normNames) if (c == n) return true;
        return false;
    }
    bool InDir(const std::wstring& text) const {
        return !dirLower.empty() && ContainsPathN(text.c_str(), text.size(), dirLower);
    }
    static std::wstring KeyId(HKEY root, REGSAM view, const std::wstring& path) {
        return RootName(root) + L"|" + std::to_wstring(root == HKEY_CURRENT_USER ? 0 : view) + L"|" + Lower(path);
    }
    void Add(Residue r) {
        std::wstring k = std::to_wstring((int)r.kind) + L"|" + RootName(r.root) + L"|" +
                         std::to_wstring(r.view) + L"|" + Lower(r.path) + L"|" + Lower(r.valueName);
        if (r.kind == ResKind::Folder || r.kind == ResKind::File) {
            if (IsProtectedPath(r.path)) { log(L"  (saltato, percorso protetto) " + r.path); return; }
            k = L"F|" + Lower(r.path);
        }
        if (keys.insert(k).second) out.push_back(std::move(r));
    }
    void AddKey(HKEY root, REGSAM view, const std::wstring& path, const std::wstring& why) {
        Residue r{ ResKind::RegKey }; r.root = root; r.view = view; r.path = path; r.reason = why;
        selKeys.insert(KeyId(root, view, path));
        Add(r);
    }
    void AddValue(HKEY root, REGSAM view, const std::wstring& path, const std::wstring& val, const std::wstring& why) {
        Residue r{ ResKind::RegValue }; r.root = root; r.view = view; r.path = path; r.valueName = val; r.reason = why;
        Add(r);
    }
    void AddPath(const std::wstring& path, const std::wstring& why) {
        Residue r{ IsDir(path) ? ResKind::Folder : ResKind::File };
        r.path = FullPath(path); r.reason = why;
        Add(r);
    }

    // ------------------------------------------------------------------ nomi
    void BuildNames() {
        auto add = [&](const std::wstring& s) {
            std::wstring t = Trim(s);
            if (!GoodCandidate(t)) return;
            for (auto& n : names) if (Norm(n) == Norm(t)) return;
            names.push_back(t);
        };
        std::wstring cn = CleanName(p.name);
        add(cn);
        if (!IsGuid(p.keyName)) { add(p.keyName); add(CleanName(p.keyName)); }
        if (!dir.empty()) add(LeafName(dir));
        if (level == ScanLevel::Advanced) {
            size_t sp = cn.find(L' ');
            if (sp != std::wstring::npos && sp >= 5) add(cn.substr(0, sp));
        }
        publisher = CleanPublisher(p.publisher);
        if (!GoodCandidate(publisher)) publisher.clear();
        std::wstring s = L"Nomi cercati: ";
        for (auto& n : names) s += L"\"" + n + L"\" ";
        if (!publisher.empty()) s += L" | editore: \"" + publisher + L"\"";
        log(s);
        log(L"Cartella di installazione: " + (dir.empty() ? std::wstring(L"(sconosciuta)") : dir));
        for (auto& n : names) normNames.push_back(Norm(n));
        dirLower = Lower(dir);
    }

    // -------------------------------------------------------------- registro
    struct Loc { HKEY root; REGSAM view; const wchar_t* soft; };
    std::vector<Loc> SoftLocs() const {
        return { {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"SOFTWARE"},
                 {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"SOFTWARE"},
                 {HKEY_CURRENT_USER, 0, L"Software"} };
    }

    void ScanUninstallKey() {
        if (!p.keyName.empty() && p.root && RegKeyExists(p.root, p.view, p.UninstallKeyPath()))
            AddKey(p.root, p.view, p.UninstallKeyPath(), L"Voce di disinstallazione");
        // stessa voce nell'altra vista / in HKCU
        for (auto& l : SoftLocs()) {
            std::wstring k = std::wstring(l.soft) + L"\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
            HKEY h = RegOpen(l.root, l.view, k);
            if (!h) continue;
            for (auto& s : RegSubkeys(h)) {
                bool hit = IEq(s, p.keyName) && !IsGuid(s);
                if (!hit && NameMatch(s)) hit = true;
                if (!hit) {
                    HKEY sk = RegOpen(l.root, l.view, k + L"\\" + s);
                    if (sk) {
                        std::wstring dn = RegStr(sk, L"DisplayName");
                        std::wstring il = RegStr(sk, L"InstallLocation");
                        if (IEq(dn, p.name) && (p.publisher.empty() || IEq(RegStr(sk, L"Publisher"), p.publisher))) hit = true;
                        if (!dir.empty() && !il.empty() && IEq(FullPath(il), dir)) hit = true;
                        RegCloseKey(sk);
                    }
                }
                if (hit) AddKey(l.root, l.view, k + L"\\" + s, L"Voce di disinstallazione");
            }
            RegCloseKey(h);
        }
    }

    void ScanSoftwareKeys() {
        for (auto& l : SoftLocs()) {
            std::wstring soft = l.soft;
            for (auto& n : names) {
                if (RegKeyExists(l.root, l.view, soft + L"\\" + n))
                    AddKey(l.root, l.view, soft + L"\\" + n, L"Chiave del programma");
                if (!publisher.empty() && RegKeyExists(l.root, l.view, soft + L"\\" + publisher + L"\\" + n))
                    AddKey(l.root, l.view, soft + L"\\" + publisher + L"\\" + n, L"Chiave del programma (editore)");
            }
            if (level == ScanLevel::Safe) continue;
            // Moderata: SOFTWARE\<qualsiasi>\<nome> e confronto "fuzzy"
            HKEY h = RegOpen(l.root, l.view, soft);
            if (!h) continue;
            for (auto& s1 : RegSubkeys(h)) {
                std::wstring ls = Lower(s1);
                if (ls == L"microsoft" || ls == L"classes" || ls == L"policies" || ls == L"wow6432node" ||
                    ls == L"clients" || ls == L"registeredapplications") continue;
                if (NameMatch(s1)) { AddKey(l.root, l.view, soft + L"\\" + s1, L"Chiave con nome corrispondente"); continue; }
                HKEY h2 = RegOpen(l.root, l.view, soft + L"\\" + s1);
                if (!h2) continue;
                for (auto& s2 : RegSubkeys(h2))
                    if (NameMatch(s2)) AddKey(l.root, l.view, soft + L"\\" + s1 + L"\\" + s2, L"Chiave con nome corrispondente");
                RegCloseKey(h2);
            }
            RegCloseKey(h);
        }
    }

    // Se tutte le sottochiavi della chiave dell'editore verranno rimosse, rimuove anche l'editore
    void ScanPublisherContainers() {
        if (publisher.empty()) return;
        for (auto& l : SoftLocs()) {
            std::wstring pk = std::wstring(l.soft) + L"\\" + publisher;
            HKEY h = RegOpen(l.root, l.view, pk);
            if (!h) continue;
            DWORD nVals = 0;
            RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &nVals, nullptr, nullptr, nullptr, nullptr);
            auto subs = RegSubkeys(h);
            RegCloseKey(h);
            if (nVals > 0 || subs.empty()) continue;
            bool all = true;
            for (auto& s : subs) {
                std::wstring full = Lower(pk + L"\\" + s);
                bool found = false;
                for (auto& r : out)
                    if (r.kind == ResKind::RegKey && r.root == l.root && r.view == l.view && Lower(r.path) == full) found = true;
                if (!found) { all = false; break; }
            }
            if (all) AddKey(l.root, l.view, pk, L"Chiave dell'editore (resterebbe vuota)");
        }
    }

    void ScanRunKeys() {
        std::vector<std::wstring> hitNames;
        for (auto& l : SoftLocs()) {
            for (const wchar_t* sub : { L"\\Microsoft\\Windows\\CurrentVersion\\Run",
                                        L"\\Microsoft\\Windows\\CurrentVersion\\RunOnce" }) {
                std::wstring k = std::wstring(l.soft) + sub;
                HKEY h = RegOpen(l.root, l.view, k);
                if (!h) continue;
                wchar_t vn[512];
                for (DWORD i = 0;; ++i) {
                    DWORD len = 512;
                    if (RegEnumValueW(h, i, vn, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                    std::wstring data = RegStr(h, vn);
                    if ((!dir.empty() && InDir(Expand(data))) || NameMatch(vn)) {
                        AddValue(l.root, l.view, k, vn, L"Avvio automatico");
                        hitNames.push_back(vn);
                    }
                }
                RegCloseKey(h);
            }
        }
        // StartupApproved (stato abilitato/disabilitato in Gestione attività)
        for (auto& l : SoftLocs()) {
            for (const wchar_t* sub : { L"\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                                        L"\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32" }) {
                std::wstring k = std::wstring(l.soft) + sub;
                HKEY h = RegOpen(l.root, l.view, k);
                if (!h) continue;
                for (auto& n : hitNames) {
                    if (RegQueryValueExW(h, n.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
                        AddValue(l.root, l.view, k, n, L"Stato avvio automatico");
                }
                RegCloseKey(h);
            }
        }
    }

    void ScanAppPaths() {
        if (dir.empty()) return;
        for (auto& l : SoftLocs()) {
            std::wstring k = std::wstring(l.soft) + L"\\Microsoft\\Windows\\CurrentVersion\\App Paths";
            HKEY h = RegOpen(l.root, l.view, k);
            if (!h) continue;
            for (auto& s : RegSubkeys(h)) {
                HKEY sk = RegOpen(l.root, l.view, k + L"\\" + s);
                if (!sk) continue;
                if (InDir(RegStr(sk, nullptr)) || InDir(RegStr(sk, L"Path")))
                    AddKey(l.root, l.view, k + L"\\" + s, L"Percorso applicazione (App Paths)");
                RegCloseKey(sk);
            }
            RegCloseKey(h);
        }
    }

    void ScanClasses() {
        struct CL { HKEY root; const wchar_t* path; };
        const CL cls[] = { {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes"}, {HKEY_CURRENT_USER, L"Software\\Classes"} };
        for (auto& c : cls) {
            HKEY h = RegOpen(c.root, KEY_WOW64_64KEY, c.path);
            if (!h) continue;
            auto subs = RegSubkeys(h);
            RegCloseKey(h);
            std::wstring base = c.path;
            // 1) ProgID il cui comando punta alla cartella del programma
            for (auto& s : subs) {
                if (s.empty() || s[0] == L'.' || IEq(s, L"CLSID") || IEq(s, L"Interface") || IEq(s, L"TypeLib") ||
                    IEq(s, L"Installer") || IEq(s, L"*") || IEq(s, L"Directory") || IEq(s, L"Folder") ||
                    IEq(s, L"AllFilesystemObjects") || IEq(s, L"Drive")) continue;
                bool hit = NameMatch(s) && level != ScanLevel::Safe;
                if (!hit && !dir.empty()) {
                    for (const wchar_t* sub : { L"\\shell\\open\\command", L"\\DefaultIcon" }) {
                        HKEY k = RegOpen(c.root, KEY_WOW64_64KEY, base + L"\\" + s + sub);
                        if (k) { if (InDir(RegStr(k, nullptr))) hit = true; RegCloseKey(k); }
                        if (hit) break;
                    }
                }
                if (hit) {
                    AddKey(c.root, KEY_WOW64_64KEY, base + L"\\" + s, L"Tipo di file / ProgID");
                    progIds.insert(Lower(s));
                }
            }
            // 2) estensioni che puntano ai ProgID trovati
            for (auto& s : subs) {
                if (s.empty() || s[0] != L'.') continue;
                std::wstring kp = base + L"\\" + s;
                HKEY k = RegOpen(c.root, KEY_WOW64_64KEY, kp);
                if (!k) continue;
                std::wstring def = Lower(RegStr(k, nullptr));
                bool ours = !def.empty() && progIds.count(def);
                // l'estensione è "solo nostra" se ogni gestore registrato appartiene al programma
                bool foreign = false;
                std::vector<std::wstring> ourOpenWith;
                for (auto& sub : RegSubkeys(k)) {
                    if (IEq(sub, L"OpenWithProgids")) {
                        HKEY ow = RegOpen(c.root, KEY_WOW64_64KEY, kp + L"\\OpenWithProgids");
                        if (!ow) continue;
                        for (DWORD i = 0;; ++i) {
                            DWORD len = (DWORD)vnBuf.size();
                            if (RegEnumValueW(ow, i, vnBuf.data(), &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                            std::wstring vn(vnBuf.data(), len);
                            if (progIds.count(Lower(vn))) ourOpenWith.push_back(vn); else foreign = true;
                        }
                        RegCloseKey(ow);
                    } else if (!(ours && IEq(sub, L"ShellNew"))) {
                        foreign = true;   // altri gestori (OpenWithList, shellex, ...) di altri programmi
                    }
                }
                if (!def.empty() && !ours) foreign = true;
                if ((ours || !ourOpenWith.empty()) && !foreign) {
                    AddKey(c.root, KEY_WOW64_64KEY, kp, L"Estensione registrata solo dal programma");
                } else {
                    if (ours) AddValue(c.root, KEY_WOW64_64KEY, kp, L"", L"Associazione estensione");
                    for (auto& vn : ourOpenWith) AddValue(c.root, KEY_WOW64_64KEY, kp + L"\\OpenWithProgids", vn, L"Apri con");
                }
                RegCloseKey(k);
            }
            // 3) Moderata/Avanzata: oggetti COM (CLSID) registrati dalla cartella del programma
            if (level != ScanLevel::Safe && !dir.empty()) {
                for (REGSAM v : { KEY_WOW64_64KEY, KEY_WOW64_32KEY }) {
                    if (c.root != HKEY_LOCAL_MACHINE && v == KEY_WOW64_32KEY) continue;
                    HKEY ck = RegOpen(c.root, v, base + L"\\CLSID");
                    if (!ck) continue;
                    for (auto& id : RegSubkeys(ck)) {
                        for (const wchar_t* srv : { L"\\InprocServer32", L"\\LocalServer32" }) {
                            HKEY sk = RegOpen(c.root, v, base + L"\\CLSID\\" + id + srv);
                            if (!sk) continue;
                            bool hit = InDir(RegStr(sk, nullptr));
                            RegCloseKey(sk);
                            if (hit) {
                                AddKey(c.root, v, base + L"\\CLSID\\" + id, L"Componente COM");
                                clsids.insert(Lower(id));
                                break;
                            }
                        }
                    }
                    RegCloseKey(ck);
                }
            }
        }
    }

    void ScanServices() {
        const std::wstring base = L"SYSTEM\\CurrentControlSet\\Services";
        HKEY h = RegOpen(HKEY_LOCAL_MACHINE, 0, base);
        if (!h) return;
        for (auto& s : RegSubkeys(h)) {
            HKEY k = RegOpen(HKEY_LOCAL_MACHINE, 0, base + L"\\" + s);
            if (!k) continue;
            std::wstring img = Expand(RegStr(k, L"ImagePath"));
            RegCloseKey(k);
            if ((!dir.empty() && InDir(img)) || (level != ScanLevel::Safe && NameMatch(s) && !img.empty())) {
                Residue r{ ResKind::Service }; r.path = s; r.reason = L"Servizio: " + img;
                Add(r);
            }
        }
        RegCloseKey(h);
    }

    // Avanzata: valori ovunque in Software che contengono la cartella del programma
    // (MuiCache, Compatibility Assistant, SharedDLLs, liste MRU, ecc.)
    size_t visited = 0;
    void DeepValues(HKEY root, REGSAM view, const std::wstring& path, int depth) {
        if (depth > 9 || visited > 400000) return;
        std::wstring lp = Lower(path);
        static const wchar_t* skip[] = { L"\\classes", L"\\microsoft\\windows\\currentversion\\installer",
                                          L"\\microsoft\\cryptography", L"\\microsoft\\windows nt\\currentversion\\winlogon",
                                          L"\\policies", L"\\microsoft\\windows\\currentversion\\uninstall",
                                          L"\\wow6432node" };   // la vista a 32 bit viene visitata a parte
        for (auto s : skip) {
            std::wstring ss = s;
            if (lp.size() >= ss.size() && lp.compare(lp.size() - ss.size(), ss.size(), ss) == 0) return;
        }
        if (selKeys.count(KeyId(root, view, path))) return;   // chiave già selezionata per intero
        HKEY h = RegOpen(root, view, path);
        if (!h) return;
        ++visited;
        for (DWORD i = 0;; ++i) {
            DWORD nl = (DWORD)vnBuf.size(), dl = (DWORD)dataBuf.size(), type = 0;
            LONG e = RegEnumValueW(h, i, vnBuf.data(), &nl, nullptr, &type, dataBuf.data(), &dl);
            if (e == ERROR_NO_MORE_ITEMS) break;
            if (e != ERROR_SUCCESS && e != ERROR_MORE_DATA) break;
            bool hit = ContainsPathN(vnBuf.data(), nl, dirLower);
            if (!hit && e == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)) {
                const wchar_t* t = reinterpret_cast<const wchar_t*>(dataBuf.data());
                size_t n = dl / sizeof(wchar_t);
                hit = ContainsPathN(t, n, dirLower);
                if (!hit && type == REG_EXPAND_SZ && wmemchr(t, L'%', n)) hit = InDir(Expand(std::wstring(t, wcsnlen(t, n))));
            }
            if (hit) AddValue(root, view, path, std::wstring(vnBuf.data(), nl), L"Valore che fa riferimento alla cartella");
        }
        auto subs = RegSubkeys(h);
        RegCloseKey(h);
        for (auto& s : subs) DeepValues(root, view, path + L"\\" + s, depth + 1);
    }

    // ------------------------------------------------------------------- file
    std::vector<std::wstring> FileBases() const {
        std::vector<std::wstring> b;
        for (auto id : { &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData,
                         &FOLDERID_RoamingAppData, &FOLDERID_LocalAppData, &FOLDERID_LocalAppDataLow,
                         &FOLDERID_ProgramFilesCommon, &FOLDERID_ProgramFilesCommonX86 }) {
            auto p = KnownFolder(*id);
            if (!p.empty() && std::find_if(b.begin(), b.end(), [&](auto& x) { return IEq(x, p); }) == b.end())
                b.push_back(p);
        }
        return b;
    }

    void ScanFolders() {
        if (!dir.empty() && PathExists(dir)) AddPath(dir, L"Cartella di installazione");
        for (auto& base : FileBases()) {
            for (auto& n : names) {
                if (PathExists(base + L"\\" + n)) AddPath(base + L"\\" + n, L"Cartella dati del programma");
                if (!publisher.empty() && PathExists(base + L"\\" + publisher + L"\\" + n))
                    AddPath(base + L"\\" + publisher + L"\\" + n, L"Cartella dati (editore)");
            }
            if (level == ScanLevel::Safe) continue;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((base + L"\\*").c_str(), &fd);
            if (f == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                std::wstring s1 = fd.cFileName, ls = Lower(s1);
                if (ls == L"microsoft" || ls == L"packages" || ls == L"temp" || ls == L"windowsapps" ||
                    ls == L"common files" || ls == L"package cache") continue;
                if (NameMatch(s1)) { AddPath(base + L"\\" + s1, L"Cartella con nome corrispondente"); continue; }
                WIN32_FIND_DATAW fd2;
                HANDLE f2 = FindFirstFileW((base + L"\\" + s1 + L"\\*").c_str(), &fd2);
                if (f2 == INVALID_HANDLE_VALUE) continue;
                do {
                    if ((fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd2.cFileName[0] != L'.' &&
                        !(fd2.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && NameMatch(fd2.cFileName))
                        AddPath(base + L"\\" + s1 + L"\\" + fd2.cFileName, L"Cartella con nome corrispondente");
                } while (FindNextFileW(f2, &fd2));
                FindClose(f2);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        // cartella dell'editore che resterebbe vuota
        if (!publisher.empty()) {
            for (auto& base : FileBases()) {
                std::wstring pd = base + L"\\" + publisher;
                if (!IsDir(pd)) continue;
                bool all = true, any = false;
                WIN32_FIND_DATAW fd;
                HANDLE f = FindFirstFileW((pd + L"\\*").c_str(), &fd);
                if (f == INVALID_HANDLE_VALUE) continue;
                do {
                    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                    any = true;
                    std::wstring child = Lower(pd + L"\\" + fd.cFileName);
                    bool found = false;
                    for (auto& r : out) if ((r.kind == ResKind::Folder || r.kind == ResKind::File) && Lower(r.path) == child) found = true;
                    if (!found) { all = false; break; }
                } while (FindNextFileW(f, &fd));
                FindClose(f);
                if (all && any) AddPath(pd, L"Cartella dell'editore (resterebbe vuota)");
            }
        }
    }

    std::wstring ResolveLnk(const std::wstring& lnk) {
        std::wstring target;
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)))
            return target;
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH * 2] = {};
                if (SUCCEEDED(sl->GetPath(buf, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) target = Expand(buf);
            }
            pf->Release();
        }
        sl->Release();
        return target;
    }

    void ScanShortcutsIn(const std::wstring& folder, int depth, bool allowFolders) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((folder + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.cFileName[0] == L'.') continue;
            std::wstring full = folder + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (allowFolders && NameMatch(fd.cFileName)) { AddPath(full, L"Cartella menu Start"); continue; }
                if (depth > 0) ScanShortcutsIn(full, depth - 1, allowFolders);
                continue;
            }
            std::wstring ln = Lower(fd.cFileName);
            if (ln.size() < 4 || ln.substr(ln.size() - 4) != L".lnk") continue;
            std::wstring target = ResolveLnk(full);
            std::wstring stem = std::wstring(fd.cFileName).substr(0, wcslen(fd.cFileName) - 4);
            bool hit = !dir.empty() && InDir(target);
            if (!hit && NameMatch(stem) && (target.empty() || !PathExists(target))) hit = true; // collegamento rotto
            if (hit) AddPath(full, L"Collegamento → " + (target.empty() ? std::wstring(L"?") : target));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }

    void ScanShortcuts() {
        for (auto id : { &FOLDERID_CommonPrograms, &FOLDERID_Programs })
            ScanShortcutsIn(KnownFolder(*id), 3, true);
        for (auto id : { &FOLDERID_Desktop, &FOLDERID_PublicDesktop, &FOLDERID_Startup, &FOLDERID_CommonStartup })
            ScanShortcutsIn(KnownFolder(*id), 0, false);
        if (level == ScanLevel::Advanced) {
            std::wstring ql = KnownFolder(FOLDERID_RoamingAppData) + L"\\Microsoft\\Internet Explorer\\Quick Launch";
            ScanShortcutsIn(ql, 3, false);
        }
    }

    // Attività pianificate: file XML in %windir%\System32\Tasks
    void ScanTasksIn(const std::wstring& root, const std::wstring& rel) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((root + rel + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.cFileName[0] == L'.') continue;
            std::wstring r = rel + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!IEq(fd.cFileName, L"Microsoft")) ScanTasksIn(root, r);
                continue;
            }
            bool hit = NameMatch(fd.cFileName);
            if (!hit && !dir.empty() && fd.nFileSizeLow < 1024 * 1024) {
                HANDLE h = CreateFileW((root + r).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    std::vector<BYTE> b(fd.nFileSizeLow + 2, 0);
                    DWORD rd = 0;
                    ReadFile(h, b.data(), fd.nFileSizeLow, &rd, nullptr);
                    CloseHandle(h);
                    std::wstring text;
                    if (rd >= 2 && b[0] == 0xFF && b[1] == 0xFE) text.assign((wchar_t*)(b.data() + 2), (rd - 2) / 2);
                    else { int n = MultiByteToWideChar(CP_UTF8, 0, (char*)b.data(), rd, nullptr, 0);
                           text.resize(n); MultiByteToWideChar(CP_UTF8, 0, (char*)b.data(), rd, &text[0], n); }
                    // i percorsi nell'XML possono essere tra entità &quot;
                    size_t q; while ((q = text.find(L"&quot;")) != std::wstring::npos) text.replace(q, 6, L"\"");
                    hit = InDir(Expand(text));
                }
            }
            if (hit) { Residue res{ ResKind::Task }; res.path = r; res.reason = L"Attività pianificata"; Add(res); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }

    // Elimina elementi contenuti in altri già selezionati
    void Prune() {
        std::vector<Residue> keep;
        for (auto& r : out) {
            bool inside = false;
            for (auto& o : out) {
                if (&o == &r) continue;
                if ((r.kind == ResKind::Folder || r.kind == ResKind::File) && o.kind == ResKind::Folder &&
                    IStarts(r.path, o.path + L"\\")) inside = true;
                if ((r.kind == ResKind::RegKey || r.kind == ResKind::RegValue) && o.kind == ResKind::RegKey &&
                    o.root == r.root && (o.view == r.view || r.root == HKEY_CURRENT_USER) &&
                    (IStarts(r.path, o.path + L"\\") || (r.kind == ResKind::RegValue && IEq(r.path, o.path))))
                    inside = true;
                if (inside) break;
            }
            if (!inside) keep.push_back(r);
        }
        // HKLM 64/32: alcune chiavi non sono reindirizzate e compaiono due volte
        std::vector<Residue> res;
        for (auto& r : keep) {
            bool dup = false;
            if (r.root == HKEY_LOCAL_MACHINE && r.view == KEY_WOW64_32KEY && (r.kind == ResKind::RegKey || r.kind == ResKind::RegValue)) {
                // confronto contenuto: se la vista 32 apre la stessa chiave della 64 (chiavi condivise)
                std::wstring lp = Lower(r.path);
                if (!IStarts(lp, L"software\\") || IStarts(lp, L"software\\classes")) {
                    for (auto& o : keep) if (o.root == r.root && o.view == KEY_WOW64_64KEY && o.kind == r.kind &&
                                              IEq(o.path, r.path) && IEq(o.valueName, r.valueName)) dup = true;
                }
            }
            if (!dup) res.push_back(r);
        }
        out.swap(res);
    }


    // ---------------------------------------------------- variabile PATH
    void ScanPathEnv() {
        if (dir.empty()) return;
        struct E { HKEY root; const wchar_t* key; };
        const E envs[] = { {HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"},
                           {HKEY_CURRENT_USER, L"Environment"} };
        for (auto& e : envs) {
            HKEY h = RegOpen(e.root, 0, e.key);
            if (!h) continue;
            DWORD type = 0, size = 0;
            if (RegQueryValueExW(h, L"Path", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size) {
                std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
                RegQueryValueExW(h, L"Path", nullptr, &type, (BYTE*)buf.data(), &size);
                std::wstring all(buf.data()), seg;
                size_t pos = 0;
                while (pos <= all.size()) {
                    size_t sc = all.find(L';', pos);
                    seg = all.substr(pos, sc == std::wstring::npos ? std::wstring::npos : sc - pos);
                    std::wstring ex = StripSlash(Trim(Expand(seg)));
                    if (!ex.empty() && (IEq(ex, dir) || IStarts(ex, dir + L"\\"))) {
                        Residue r{ ResKind::PathEntry };
                        r.root = e.root; r.path = e.key; r.valueName = L"Path"; r.extra = seg;
                        r.reason = L"Voce aggiunta alla variabile PATH";
                        Add(r);
                    }
                    if (sc == std::wstring::npos) break;
                    pos = sc + 1;
                }
            }
            RegCloseKey(h);
        }
    }

    // ---------------------------------------------------- regole firewall
    void ScanFirewall() {
        if (dir.empty()) return;
        const std::wstring k = L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules";
        HKEY h = RegOpen(HKEY_LOCAL_MACHINE, 0, k);
        if (!h) return;
        for (DWORD i = 0;; ++i) {
            DWORD nl = (DWORD)vnBuf.size(), dl = (DWORD)dataBuf.size(), type = 0;
            LONG e = RegEnumValueW(h, i, vnBuf.data(), &nl, nullptr, &type, dataBuf.data(), &dl);
            if (e == ERROR_NO_MORE_ITEMS) break;
            if (e != ERROR_SUCCESS || type != REG_SZ) continue;
            std::wstring rule((wchar_t*)dataBuf.data(), wcsnlen((wchar_t*)dataBuf.data(), dl / sizeof(wchar_t)));
            // formato: v2.30|Action=Allow|Dir=In|App=C:\...\x.exe|Name=...|
            auto field = [&](const wchar_t* f) {
                std::wstring key = std::wstring(L"|") + f + L"=";
                size_t a = Lower(rule).find(Lower(key));
                if (a == std::wstring::npos) return std::wstring();
                a += key.size();
                size_t b = rule.find(L'|', a);
                return rule.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
            };
            std::wstring app = Expand(field(L"App"));
            if (!InDir(app)) continue;
            std::wstring name = field(L"Name");
            Residue r{ ResKind::Firewall };
            r.root = HKEY_LOCAL_MACHINE; r.path = k; r.valueName = std::wstring(vnBuf.data(), nl);
            r.extra = name.empty() || name[0] == L'@' ? LeafName(app) : name;
            r.reason = (Lower(field(L"Dir")) == L"out" ? L"Regola in uscita per " : L"Regola in entrata per ") + LeafName(app);
            Add(r);
        }
        RegCloseKey(h);
    }

    // ------------------------------------- menu contestuale e estensioni della shell
    void ScanShell() {
        const wchar_t* targets[] = { L"*", L"Directory", L"Directory\\Background", L"Folder", L"Drive",
                                     L"AllFilesystemObjects", L"DesktopBackground", L"lnkfile", L"exefile" };
        struct CL { HKEY root; const wchar_t* path; };
        const CL cls[] = { {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes"}, {HKEY_CURRENT_USER, L"Software\\Classes"} };
        for (auto& c : cls) {
            for (auto t : targets) {
                std::wstring base = std::wstring(c.path) + L"\\" + t;
                // comandi (verbi): shell\<verbo>\command
                if (HKEY h = RegOpen(c.root, KEY_WOW64_64KEY, base + L"\\shell")) {
                    for (auto& v : RegSubkeys(h)) {
                        std::wstring vk = base + L"\\shell\\" + v;
                        bool hit = false;
                        if (HKEY ck = RegOpen(c.root, KEY_WOW64_64KEY, vk + L"\\command")) { hit = InDir(RegStr(ck, nullptr)); RegCloseKey(ck); }
                        if (!hit) if (HKEY ik = RegOpen(c.root, KEY_WOW64_64KEY, vk)) { hit = InDir(RegStr(ik, L"Icon")); RegCloseKey(ik); }
                        if (hit) AddKey(c.root, KEY_WOW64_64KEY, vk, L"Comando nel menu contestuale");
                    }
                    RegCloseKey(h);
                }
                // gestori: shellex\ContextMenuHandlers|DragDropHandlers|PropertySheetHandlers\<nome>
                for (const wchar_t* hx : { L"ContextMenuHandlers", L"DragDropHandlers", L"PropertySheetHandlers" }) {
                    std::wstring hk = base + L"\\shellex\\" + hx;
                    HKEY h = RegOpen(c.root, KEY_WOW64_64KEY, hk);
                    if (!h) continue;
                    for (auto& n : RegSubkeys(h)) {
                        std::wstring id = Lower(n);
                        if (HKEY sk = RegOpen(c.root, KEY_WOW64_64KEY, hk + L"\\" + n)) {
                            std::wstring d = Lower(RegStr(sk, nullptr));
                            if (!d.empty()) id = d;
                            RegCloseKey(sk);
                        }
                        if (clsids.count(id) || (level != ScanLevel::Safe && NameMatch(n)))
                            AddKey(c.root, KEY_WOW64_64KEY, hk + L"\\" + n, L"Estensione del menu contestuale");
                    }
                    RegCloseKey(h);
                }
            }
        }
        if (clsids.empty()) return;
        // sovrapposizioni icone ed estensioni approvate
        for (REGSAM v : { KEY_WOW64_64KEY, KEY_WOW64_32KEY }) {
            std::wstring ov = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";
            if (HKEY h = RegOpen(HKEY_LOCAL_MACHINE, v, ov)) {
                for (auto& n : RegSubkeys(h)) {
                    HKEY sk = RegOpen(HKEY_LOCAL_MACHINE, v, ov + L"\\" + n);
                    if (!sk) continue;
                    if (clsids.count(Lower(RegStr(sk, nullptr)))) AddKey(HKEY_LOCAL_MACHINE, v, ov + L"\\" + n, L"Sovrapposizione icone");
                    RegCloseKey(sk);
                }
                RegCloseKey(h);
            }
            std::wstring ap = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
            if (HKEY h = RegOpen(HKEY_LOCAL_MACHINE, v, ap)) {
                for (auto& id : clsids)
                    for (DWORD i = 0;; ++i) {
                        DWORD nl = (DWORD)vnBuf.size();
                        if (RegEnumValueW(h, i, vnBuf.data(), &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                        if (Lower(std::wstring(vnBuf.data(), nl)) == id) { AddValue(HKEY_LOCAL_MACHINE, v, ap, std::wstring(vnBuf.data(), nl), L"Estensione shell approvata"); break; }
                    }
                RegCloseKey(h);
            }
        }
    }

    // --------------------- cache di Windows con percorsi dei programmi (nomi dei valori = percorso)
    void ScanCaches() {
        if (dir.empty()) return;
        struct C { HKEY root; const wchar_t* key; const wchar_t* why; };
        const C caches[] = {
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store", L"Cache Assistente compatibilità" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", L"Impostazioni di compatibilità" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers", L"Impostazioni di compatibilità" },
            { HKEY_CURRENT_USER, L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache", L"Cache nomi applicazioni (MuiCache)" },
            { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SharedDLLs", L"Contatore DLL condivise" },
            { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FeatureUsage\\AppSwitched", L"Cronologia uso applicazioni" },
        };
        for (auto& c : caches) {
            HKEY h = RegOpen(c.root, c.root == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0, c.key);
            if (!h) continue;
            for (DWORD i = 0;; ++i) {
                DWORD nl = (DWORD)vnBuf.size();
                LONG e = RegEnumValueW(h, i, vnBuf.data(), &nl, nullptr, nullptr, nullptr, nullptr);
                if (e == ERROR_NO_MORE_ITEMS) break;
                if (e == ERROR_SUCCESS && ContainsPathN(vnBuf.data(), nl, dirLower))
                    AddValue(c.root, c.root == HKEY_LOCAL_MACHINE ? KEY_WOW64_64KEY : 0, c.key, std::wstring(vnBuf.data(), nl), c.why);
            }
            RegCloseKey(h);
        }
    }

    // --------------------- cartella temporanea del programma
    void ScanTemp() {
        wchar_t tmp[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, tmp)) return;
        std::wstring t = StripSlash(tmp);
        for (auto& n : names)
            if (IsDir(t + L"\\" + n)) AddPath(t + L"\\" + n, L"File temporanei del programma");
    }

    std::vector<Residue> Run() {
        ULONGLONG t0 = GetTickCount64();
        dir = ResolveInstallDir(p);
        BuildNames();
        log(L"Scansione registro...");
        ScanUninstallKey();
        ScanSoftwareKeys();
        ScanRunKeys();
        ScanAppPaths();
        ScanServices();
        if (level != ScanLevel::Safe || !dir.empty()) ScanClasses();
        log(L"Scansione PATH, firewall e menu contestuale...");
        ScanPathEnv();
        ScanFirewall();
        ScanShell();
        if (level != ScanLevel::Safe) ScanCaches();
        log(L"Scansione file e cartelle...");
        ScanFolders();
        ScanShortcuts();
        if (level != ScanLevel::Safe) ScanTemp();
        log(L"Scansione attività pianificate...");
        std::wstring tasks = KnownFolder(FOLDERID_System) + L"\\Tasks";
        ScanTasksIn(tasks, L"");
        ScanPublisherContainers();
        if (level == ScanLevel::Advanced && !dir.empty()) {
            log(L"Scansione avanzata dei valori di registro (può richiedere qualche secondo)...");
            DeepValues(HKEY_CURRENT_USER, 0, L"Software", 0);
            DeepValues(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"SOFTWARE", 0);
            DeepValues(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"SOFTWARE", 0);
        }
        Prune();
        std::stable_sort(out.begin(), out.end(), [](const Residue& a, const Residue& b) { return (int)a.kind < (int)b.kind; });
        wchar_t el[32]; swprintf(el, 32, L"%.1f", (GetTickCount64() - t0) / 1000.0);
        log(L"Trovati " + std::to_wstring(out.size()) + L" elementi residui in " + el + L" s.");
        return out;
    }
};
} // namespace

std::vector<Residue> ScanResidues(const Program& p, ScanLevel level, LogFn log) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Scanner s(p, level, log);
    auto r = s.Run();
    if (SUCCEEDED(hr)) CoUninitialize();
    return r;
}

// ============================================================================
//  Rimozione
// ============================================================================
static void EnablePriv(const wchar_t* name) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{ 1 };
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    }
    CloseHandle(tok);
}
static void EnableRecoveryPrivileges() {
    static bool done = false;
    if (done) return;
    EnablePriv(SE_TAKE_OWNERSHIP_NAME);
    EnablePriv(SE_RESTORE_NAME);
    EnablePriv(SE_BACKUP_NAME);
    EnablePriv(SE_SECURITY_NAME);
    done = true;
}

// Prende possesso dell'oggetto (e di tutto il sottoalbero) e assegna controllo
// completo ad Administrators e SYSTEM. Usato quando un ACL "nega" l'eliminazione.
static bool TakeOwnershipTree(const std::wstring& objName, SE_OBJECT_TYPE type, LogFn log) {
    EnableRecoveryPrivileges();
    PSID admins = nullptr, system = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins);
    AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system);
    EXPLICIT_ACCESSW ea[2]{};
    for (int i = 0; i < 2; ++i) {
        ea[i].grfAccessPermissions = GENERIC_ALL;
        ea[i].grfAccessMode = SET_ACCESS;
        ea[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[i].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        ea[i].Trustee.ptstrName = (LPWSTR)(i == 0 ? admins : system);
    }
    PACL acl = nullptr;
    SetEntriesInAclW(2, ea, nullptr, &acl);
    std::vector<wchar_t> name(objName.begin(), objName.end());
    name.push_back(0);
    DWORD e = TreeResetNamedSecurityInfoW(name.data(), type,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        admins, nullptr, acl, nullptr, FALSE, nullptr, ProgressInvokeNever, nullptr);
    if (acl) LocalFree(acl);
    if (admins) FreeSid(admins);
    if (system) FreeSid(system);
    if (e != ERROR_SUCCESS) log(L"    impossibile reimpostare i permessi (errore " + std::to_wstring(e) + L")");
    else log(L"    permessi reimpostati (proprietario: Administrators)");
    return e == ERROR_SUCCESS;
}

static void ClearAttributesTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
        SetFileAttributesW(path.c_str(), (a & FILE_ATTRIBUTE_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        ClearAttributesTree(path + L"\\" + fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Eliminazione definitiva; non segue i junction / link simbolici
static void DeleteTreePermanent(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { DeleteFileW(path.c_str()); return; }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                DeleteTreePermanent(path + L"\\" + fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    RemoveDirectoryW(path.c_str());
}

static int ScheduleDeleteOnReboot(const std::wstring& path) {
    int n = 0;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                n += ScheduleDeleteOnReboot(path + L"\\" + fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) ++n;
    return n;
}

static bool RecycleOrDelete(const std::wstring& path, bool recycle) {
    if (recycle) {
        std::vector<wchar_t> from(path.begin(), path.end());
        from.push_back(0); from.push_back(0);
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.data();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);
        if (!PathExists(path)) return true;
    }
    DeleteTreePermanent(path);
    return !PathExists(path);
}

static std::wstring RegExePath() {
    return KnownFolder(FOLDERID_System) + L"\\reg.exe";
}
static DWORD RunHidden(const std::wstring& cmdline) {
    std::vector<wchar_t> b(cmdline.begin(), cmdline.end());
    b.push_back(0);
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code;
}

static std::wstring SecObjName(HKEY root, const std::wstring& path) {
    return (root == HKEY_LOCAL_MACHINE ? L"MACHINE\\" : L"CURRENT_USER\\") + path;
}

static bool DeleteRegKey(const Residue& r, LogFn log) {
    std::wstring parent = ParentDir(r.path), leaf = LeafName(r.path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        HKEY h = nullptr;
        LONG e = RegOpenKeyExW(r.root, parent.c_str(), 0, KEY_ALL_ACCESS | r.view, &h);
        if (e == ERROR_SUCCESS) {
            e = RegDeleteTreeW(h, leaf.c_str());
            if (e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND) {
                LONG e2 = RegDeleteKeyExW(h, leaf.c_str(), r.view, 0);
                if (e2 == ERROR_SUCCESS || e2 == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS; else e = e2;
            }
            RegCloseKey(h);
        }
        if (e == ERROR_SUCCESS || !RegKeyExists(r.root, r.view, r.path)) return true;
        if (e == ERROR_ACCESS_DENIED && attempt == 0) {
            log(L"    accesso negato, provo a prendere possesso della chiave...");
            TakeOwnershipTree(SecObjName(r.root, r.path),
                              r.view == KEY_WOW64_32KEY ? SE_REGISTRY_WOW64_32KEY : SE_REGISTRY_KEY, log);
            continue;
        }
        log(L"    errore " + std::to_wstring(e));
        return false;
    }
    return false;
}

static bool DeleteService(const std::wstring& name, LogFn log, bool& reboot) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { log(L"    impossibile aprire Gestione servizi"); return false; }
    SC_HANDLE s = OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!s) { DWORD e = GetLastError(); CloseServiceHandle(scm); return e == ERROR_SERVICE_DOES_NOT_EXIST; }
    SERVICE_STATUS st{};
    if (QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED) {
        log(L"    arresto del servizio...");
        ControlService(s, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 40 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(250);
    }
    bool ok = ::DeleteService(s) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    if (ok && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED) reboot = true;
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

int KillProcessesInFolder(const std::wstring& dir, LogFn log) {
    if (dir.empty()) return 0;
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2]; DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &len) && IStarts(path, dir + L"\\")) {
            if (TerminateProcess(h, 1)) {
                WaitForSingleObject(h, 3000);
                log(L"Terminato processo: " + std::wstring(pe.szExeFile) + L" (PID " + std::to_wstring(pe.th32ProcessID) + L")");
                ++n;
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

int RemoveResidues(std::vector<Residue>& items, bool useRecycleBin, const std::wstring& backupDir,
                   LogFn log, bool& rebootNeeded) {
    int errors = 0;
    rebootNeeded = false;
    // 1) backup del registro
    bool anyReg = false;
    auto isReg = [](ResKind k) { return k == ResKind::RegKey || k == ResKind::RegValue || k == ResKind::PathEntry || k == ResKind::Firewall; };
    for (auto& r : items) if (r.checked && isReg(r.kind)) anyReg = true;
    if (anyReg && !backupDir.empty()) {
        SHCreateDirectoryExW(nullptr, backupDir.c_str(), nullptr);
        std::set<std::wstring> done;
        int idx = 0;
        for (auto& r : items) {
            if (!r.checked || !isReg(r.kind)) continue;
            std::wstring key = RootName(r.root) + L"\\" + r.path;
            std::wstring id = key + L"|" + std::to_wstring(r.view);
            if (!done.insert(Lower(id)).second) continue;
            std::wstring file = backupDir + L"\\" + std::to_wstring(++idx) + L".reg";
            std::wstring cmd = L"\"" + RegExePath() + L"\" export \"" + key + L"\" \"" + file + L"\" /y" +
                               (r.view == KEY_WOW64_32KEY ? L" /reg:32" : L" /reg:64");
            RunHidden(cmd);
        }
        log(L"Backup del registro salvato in: " + backupDir);
    }
    // 2) ordine: servizi, attività, valori, chiavi, file, cartelle
    const ResKind order[] = { ResKind::Service, ResKind::Task, ResKind::Firewall, ResKind::PathEntry,
                              ResKind::RegValue, ResKind::RegKey, ResKind::File, ResKind::Folder };
    bool envChanged = false;
    for (ResKind k : order) {
        for (auto& r : items) {
            if (!r.checked || r.kind != k) continue;
            log(L"Rimozione: " + r.Display());
            bool ok = false;
            switch (k) {
            case ResKind::Service:
                ok = DeleteService(r.path, log, rebootNeeded);
                break;
            case ResKind::Task: {
                std::wstring tn = r.path;
                DWORD c = RunHidden(L"\"" + KnownFolder(FOLDERID_System) + L"\\schtasks.exe\" /delete /tn \"" + tn + L"\" /f");
                ok = c == 0;
                if (!ok) {   // fallback: rimuove direttamente il file XML
                    std::wstring f = KnownFolder(FOLDERID_System) + L"\\Tasks" + tn;
                    ok = DeleteFileW(f.c_str()) || !PathExists(f);
                }
                break;
            }
            case ResKind::Firewall: {
                // netsh aggiorna anche il servizio firewall in esecuzione; il registro da solo no
                DWORD c = RunHidden(L"\"" + KnownFolder(FOLDERID_System) + L"\\netsh.exe\" advfirewall firewall delete rule name=\"" +
                                    r.extra + L"\"");
                HKEY h = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, r.path.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
                    LONG e = RegDeleteValueW(h, r.valueName.c_str());
                    ok = c == 0 || e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND;
                    RegCloseKey(h);
                } else ok = c == 0;
                break;
            }
            case ResKind::PathEntry: {
                HKEY h = nullptr;
                if (RegOpenKeyExW(r.root, r.path.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &h) != ERROR_SUCCESS) break;
                DWORD type = REG_EXPAND_SZ, size = 0;
                RegQueryValueExW(h, L"Path", nullptr, &type, nullptr, &size);
                std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
                if (RegQueryValueExW(h, L"Path", nullptr, &type, (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
                    std::wstring all(buf.data()), result;
                    size_t pos = 0;
                    while (pos <= all.size()) {   // ricompone il valore senza la voce
                        size_t sc = all.find(L';', pos);
                        std::wstring seg = all.substr(pos, sc == std::wstring::npos ? std::wstring::npos : sc - pos);
                        if (!IEq(seg, r.extra) && !seg.empty()) { if (!result.empty()) result += L';'; result += seg; }
                        if (sc == std::wstring::npos) break;
                        pos = sc + 1;
                    }
                    ok = RegSetValueExW(h, L"Path", 0, type, (const BYTE*)result.c_str(),
                                        (DWORD)((result.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
                    envChanged |= ok;
                } else ok = true;
                RegCloseKey(h);
                break;
            }
            case ResKind::RegValue: {
                HKEY h = nullptr;
                LONG e = RegOpenKeyExW(r.root, r.path.c_str(), 0, KEY_SET_VALUE | r.view, &h);
                if (e == ERROR_SUCCESS) {
                    e = RegDeleteValueW(h, r.valueName.empty() ? nullptr : r.valueName.c_str());
                    RegCloseKey(h);
                }
                ok = e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND;
                break;
            }
            case ResKind::RegKey:
                ok = DeleteRegKey(r, log);
                break;
            case ResKind::File:
            case ResKind::Folder: {
                if (IsProtectedPath(r.path)) { log(L"    percorso protetto, ignorato"); ok = true; break; }
                if (!PathExists(r.path)) { ok = true; break; }
                ClearAttributesTree(r.path);
                ok = RecycleOrDelete(r.path, useRecycleBin);
                if (!ok) {
                    log(L"    eliminazione non riuscita, reimposto proprietario e permessi...");
                    TakeOwnershipTree(r.path, SE_FILE_OBJECT, log);
                    ClearAttributesTree(r.path);
                    DeleteTreePermanent(r.path);
                    ok = !PathExists(r.path);
                }
                if (!ok) {
                    int n = ScheduleDeleteOnReboot(r.path);
                    if (n > 0) {
                        log(L"    file in uso: " + std::to_wstring(n) + L" elementi verranno eliminati al riavvio");
                        rebootNeeded = true;
                        ok = true;
                    }
                }
                break;
            }
            }
            if (ok) log(L"    OK");
            else { log(L"    NON RIMOSSO"); ++errors; }
            r.checked = !ok;   // gli elementi falliti restano selezionati
        }
    }
    if (envChanged) {   // avvisa Explorer e i programmi aperti che le variabili d'ambiente sono cambiate
        DWORD_PTR res;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, &res);
    }
    return errors;
}

// ============================================================================
//  Punto di ripristino
// ============================================================================
#pragma pack(push, 1)
struct RP_INFOW { DWORD dwEventType; DWORD dwRestorePtType; INT64 llSequenceNumber; WCHAR szDescription[256]; };
struct RP_STATUS { DWORD nStatus; INT64 llSequenceNumber; };
#pragma pack(pop)
typedef BOOL(WINAPI* PFN_SRSetRestorePointW)(RP_INFOW*, RP_STATUS*);

bool CreateRestorePoint(const std::wstring& desc, LogFn log) {
    HMODULE m = LoadLibraryW(L"srclient.dll");
    if (!m) { log(L"Ripristino configurazione di sistema non disponibile."); return false; }
    auto fn = (PFN_SRSetRestorePointW)GetProcAddress(m, "SRSetRestorePointW");
    bool ok = false;
    if (fn) {
        RP_INFOW info{};
        info.dwEventType = 100;      // BEGIN_SYSTEM_CHANGE
        info.dwRestorePtType = 1;    // APPLICATION_UNINSTALL
        wcsncpy(info.szDescription, desc.c_str(), 63);
        RP_STATUS st{};
        log(L"Creazione punto di ripristino...");
        if (fn(&info, &st)) {
            info.dwEventType = 101;  // END_SYSTEM_CHANGE
            info.llSequenceNumber = st.llSequenceNumber;
            fn(&info, &st);
            ok = true;
            log(L"Punto di ripristino creato.");
        } else {
            log(L"Punto di ripristino non creato (Protezione sistema disattivata? codice " +
                std::to_wstring(st.nStatus) + L").");
        }
    }
    FreeLibrary(m);
    return ok;
}

bool IsRunningAsAdmin() {
    BOOL admin = FALSE;
    PSID grp = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(nullptr, grp, &admin);
        FreeSid(grp);
    }
    return admin != FALSE;
}
