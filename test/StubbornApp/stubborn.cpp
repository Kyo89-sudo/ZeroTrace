// StubbornApp - programma di PROVA "difficile da rimuovere" per testare ZeroTrace.
//
// Non fa nulla di dannoso: crea solo tracce tipiche dei programmi reali che lasciano
// residui (file, cartelle, chiavi di registro, avvio automatico, servizio, attività
// pianificata, associazioni file, componente COM, permessi che negano l'eliminazione,
// file bloccato da un processo in esecuzione) e registra un disinstallatore
// volutamente DIFETTOSO che rimuove solo una piccola parte di ciò che ha installato.
//
//   StubbornApp.exe              finestra di controllo (installa / verifica / ripulisci)
//   StubbornApp.exe /install     installazione silenziosa
//   StubbornApp.exe /uninstall   disinstallatore difettoso (quello registrato in Windows)
//   StubbornApp.exe /purge       rimozione COMPLETA (per ripristinare il PC dopo il test)
//   StubbornApp.exe /status      codice di uscita = numero di tracce ancora presenti
//   StubbornApp.exe /tray        processo residente con icona nell'area di notifica
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include <functional>

static const wchar_t* SVC = L"StubbornAppSvc";
static const wchar_t* TASK = L"\\StubbornApp\\Updater";
static const wchar_t* CLSID_STR = L"{6A4C7C0E-3D5B-4E5F-9B1A-5B7B0D2E9F11}";
static const wchar_t* UNINST = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\StubbornApp";
static const wchar_t* ENVKEY = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
static const wchar_t* FWKEY = L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules";
static const wchar_t* FWID = L"{StubbornApp-Inbound-TCP}";
static const wchar_t* VERBKEY = L"SOFTWARE\\Classes\\*\\shell\\StubbornApp";
static const wchar_t* MUIKEY = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache";

// ------------------------------------------------------------------ percorsi
static std::wstring KF(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}
static std::wstring Dir()        { return KF(FOLDERID_ProgramFiles) + L"\\StubbornApp"; }
static std::wstring Exe()        { return Dir() + L"\\StubbornApp.exe"; }
static std::wstring PData()      { return KF(FOLDERID_ProgramData) + L"\\StubbornSoft"; }
static std::wstring Roaming()    { return KF(FOLDERID_RoamingAppData) + L"\\StubbornSoft"; }
static std::wstring LocalDir()   { return KF(FOLDERID_LocalAppData) + L"\\StubbornApp"; }
static std::wstring MenuDir()    { return KF(FOLDERID_CommonPrograms) + L"\\StubbornApp"; }
static std::wstring DeskLnk()    { return KF(FOLDERID_PublicDesktop) + L"\\StubbornApp.lnk"; }
static std::wstring DeepDll()    { return Dir() + L"\\plugins\\codec\\v3\\x64\\legacy\\compat\\stubcodec.dll"; }
static std::wstring SharedDll()  { return Dir() + L"\\shared\\stubcore.dll"; }

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool KeyExists(HKEY root, const std::wstring& k, REGSAM view = KEY_WOW64_64KEY) {
    HKEY h; if (RegOpenKeyExW(root, k.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h); return true;
}
static bool ValueExists(HKEY root, const std::wstring& k, const std::wstring& v, REGSAM view = KEY_WOW64_64KEY) {
    HKEY h; if (RegOpenKeyExW(root, k.c_str(), 0, KEY_READ | view, &h) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(h, v.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(h); return ok;
}

// ------------------------------------------------------------------ helper
static void WriteText(const std::wstring& path, const std::string& text) {
    SHCreateDirectoryExW(nullptr, path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(h);
}
static void SetStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& val,
                   REGSAM view = KEY_WOW64_64KEY, DWORD type = REG_SZ) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, type, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}
static void SetDw(HKEY root, const std::wstring& key, const wchar_t* name, DWORD v, REGSAM view = KEY_WOW64_64KEY) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(h);
}
static void MakeLink(const std::wstring& lnk, const std::wstring& target, const std::wstring& args) {
    SHCreateDirectoryExW(nullptr, lnk.substr(0, lnk.find_last_of(L'\\')).c_str(), nullptr);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
}
static DWORD RunHidden(const std::wstring& cmd) {
    std::vector<wchar_t> b(cmd.begin(), cmd.end()); b.push_back(0);
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD c = 1; GetExitCodeProcess(pi.hProcess, &c);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return c;
}
static std::wstring Sys(const wchar_t* exe) { return L"\"" + KF(FOLDERID_System) + L"\\" + exe + L"\""; }

// Applica un DACL (SDDL) protetto, senza ereditarietà
static void ApplySddl(const std::wstring& obj, SE_OBJECT_TYPE type, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return;
    BOOL present = FALSE, def = FALSE; PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &def);
    std::vector<wchar_t> n(obj.begin(), obj.end()); n.push_back(0);
    SetNamedSecurityInfoW(n.data(), type, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                          nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
}
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
// Ripristina permessi normali (usato da /purge)
static void ResetAcl(const std::wstring& obj, SE_OBJECT_TYPE type) {
    EnablePriv(SE_TAKE_OWNERSHIP_NAME); EnablePriv(SE_RESTORE_NAME); EnablePriv(SE_BACKUP_NAME);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;GA;;;BA)(A;OICI;GA;;;SY)",
                                                             SDDL_REVISION_1, &sd, nullptr)) return;
    BOOL p, d; PACL dacl = nullptr; PSID owner = nullptr;
    GetSecurityDescriptorDacl(sd, &p, &dacl, &d);
    GetSecurityDescriptorOwner(sd, &owner, &d);
    std::vector<wchar_t> n(obj.begin(), obj.end()); n.push_back(0);
    TreeResetNamedSecurityInfoW(n.data(), type, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                PROTECTED_DACL_SECURITY_INFORMATION, owner, nullptr, dacl, nullptr, FALSE,
                                nullptr, ProgressInvokeNever, nullptr);
    LocalFree(sd);
}

// ---- variabile PATH di sistema: aggiunge/rimuove una voce
static std::wstring ReadPath() {
    HKEY h; std::wstring r;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENVKEY, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS) return r;
    DWORD sz = 0, type = 0;
    RegQueryValueExW(h, L"Path", nullptr, &type, nullptr, &sz);
    std::vector<wchar_t> b(sz / 2 + 2, 0);
    if (RegQueryValueExW(h, L"Path", nullptr, &type, (BYTE*)b.data(), &sz) == ERROR_SUCCESS) r = b.data();
    RegCloseKey(h);
    return r;
}
static void WritePath(const std::wstring& v) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENVKEY, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return;
    RegSetValueExW(h, L"Path", 0, REG_EXPAND_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2));
    RegCloseKey(h);
    DWORD_PTR res;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 2000, &res);
}
static std::wstring PathSeg() { return Dir() + L"\\bin"; }
static bool PathHasSeg() {
    std::wstring p = L";" + ReadPath() + L";", s = L";" + PathSeg() + L";";
    for (auto& c : p) c = (wchar_t)towlower(c);
    for (auto& c : s) c = (wchar_t)towlower(c);
    return p.find(s) != std::wstring::npos;
}

static bool FirewallRulePresent();

// ============================================================ installazione
static std::vector<std::wstring> g_log;

static void Install() {
    std::wstring dir = Dir(), exe = Exe();
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);

    // ---- file nella cartella del programma
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (_wcsicmp(self, exe.c_str()) != 0) CopyFileW(self, exe.c_str(), FALSE);
    WriteText(dir + L"\\readme.txt", "StubbornApp 3.2 - programma di prova per ZeroTrace.\r\n");
    WriteText(dir + L"\\config\\app.cfg", "[main]\r\nupdates=1\r\n");
    WriteText(DeepDll(), "MZ fake plugin");
    WriteText(SharedDll(), "MZ fake shared dll");
    // file di sola lettura + nascosto + di sistema
    std::wstring lic = dir + L"\\data\\license.dat";
    WriteText(lic, "LICENSE-KEY-0000-SIMULAZIONE");
    SetFileAttributesW(lic.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    SetFileAttributesW((dir + L"\\data").c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN);
    // cartella con ACL che NEGA l'eliminazione a Everyone (anche agli amministratori)
    WriteText(dir + L"\\protected\\core.bin", "core binary");
    WriteText(dir + L"\\protected\\keys\\master.key", "secret");
    const wchar_t* denyDel = L"D:P(D;OICI;0x10040;;;WD)(A;OICI;0x1200a9;;;WD)";
    ApplySddl(dir + L"\\protected\\keys\\master.key", SE_FILE_OBJECT, denyDel);
    ApplySddl(dir + L"\\protected\\keys", SE_FILE_OBJECT, denyDel);
    ApplySddl(dir + L"\\protected\\core.bin", SE_FILE_OBJECT, denyDel);
    ApplySddl(dir + L"\\protected", SE_FILE_OBJECT, denyDel);
    WriteText(dir + L"\\data\\session.lock", "");   // verrà tenuto aperto dal processo /tray
    g_log.push_back(L"File in " + dir);

    // ---- dati sparsi nel profilo e in ProgramData
    WriteText(PData() + L"\\StubbornApp\\cache\\cache.db", "cache");
    WriteText(PData() + L"\\StubbornApp\\cache\\thumbs\\0001.bin", "thumb");
    WriteText(Roaming() + L"\\StubbornApp\\settings.ini", "[ui]\r\ntheme=dark\r\n");
    WriteText(LocalDir() + L"\\logs\\app.log", "avvio\r\n");
    g_log.push_back(L"Dati in ProgramData, AppData\\Roaming, AppData\\Local");

    // ---- collegamenti
    MakeLink(MenuDir() + L"\\StubbornApp.lnk", exe, L"");
    MakeLink(MenuDir() + L"\\Disinstalla StubbornApp.lnk", exe, L"/uninstall");
    MakeLink(DeskLnk(), exe, L"");
    g_log.push_back(L"Collegamenti nel menu Start e sul Desktop pubblico");

    // ---- registro: chiavi del programma (64 e 32 bit, macchina e utente)
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\StubbornSoft\\StubbornApp", L"InstallDir", dir);
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\StubbornSoft\\StubbornApp", L"Version", L"3.2.1");
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\StubbornSoft\\StubbornApp\\Licensing", L"Serial", L"0000-SIM");
    // sottochiave con ACL che nega DELETE a Everyone
    ApplySddl(L"MACHINE\\SOFTWARE\\StubbornSoft\\StubbornApp\\Licensing", SE_REGISTRY_KEY, L"D:P(D;CI;0x10000;;;WD)(A;CI;KR;;;WD)");
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\StubbornSoft\\StubbornApp", L"InstallDir", dir, KEY_WOW64_32KEY);
    SetStr(HKEY_CURRENT_USER, L"Software\\StubbornSoft\\StubbornApp\\Settings", L"Language", L"it-IT", 0);
    SetStr(HKEY_CURRENT_USER, L"Software\\StubbornSoft\\StubbornApp\\Recent", L"File1", L"C:\\esempio.stub", 0);
    SetDw(HKEY_CURRENT_USER, L"Software\\StubbornApp", L"FirstRun", 0, 0);

    // ---- avvio automatico
    SetStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"StubbornApp Tray",
           L"\"" + exe + L"\" /tray", 0);
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"StubbornUpdater",
           L"\"" + exe + L"\" /noop", KEY_WOW64_32KEY);

    // ---- voce di disinstallazione (con disinstallatore difettoso)
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t date[16]; swprintf(date, 16, L"%04d%02d%02d", t.wYear, t.wMonth, t.wDay);
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"DisplayName", L"StubbornApp 3.2 (programma di prova)");
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"Publisher", L"StubbornSoft Inc.");
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"DisplayVersion", L"3.2.1");
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"InstallLocation", dir);
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"InstallDate", date);
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"DisplayIcon", exe + L",0");
    SetStr(HKEY_LOCAL_MACHINE, UNINST, L"UninstallString", L"\"" + exe + L"\" /uninstall");
    SetDw(HKEY_LOCAL_MACHINE, UNINST, L"EstimatedSize", 48 * 1024);
    SetDw(HKEY_LOCAL_MACHINE, UNINST, L"NoModify", 1);
    SetDw(HKEY_LOCAL_MACHINE, UNINST, L"NoRepair", 1);

    // ---- App Paths, associazione file .stub, componente COM
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\StubbornApp.exe", nullptr, exe);
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\StubbornApp.exe", L"Path", dir);
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\.stub", nullptr, L"StubbornApp.Document");
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\.stub\\OpenWithProgids", L"StubbornApp.Document", L"");
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\StubbornApp.Document", nullptr, L"Documento StubbornApp");
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\StubbornApp.Document\\DefaultIcon", nullptr, exe + L",0");
    SetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\StubbornApp.Document\\shell\\open\\command", nullptr,
           L"\"" + exe + L"\" \"%1\"");
    std::wstring clsid = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + CLSID_STR;
    SetStr(HKEY_LOCAL_MACHINE, clsid, nullptr, L"StubbornApp Codec");
    SetStr(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", nullptr, DeepDll());
    SetStr(HKEY_LOCAL_MACHINE, clsid + L"\\InprocServer32", L"ThreadingModel", L"Both");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    // ---- tracce "nascoste" in chiavi di Windows (trovate solo con scansione Avanzata)
    SetDw(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SharedDLLs", SharedDll().c_str(), 1);
    {
        HKEY h; BYTE data[4] = { 1, 0, 0, 0 };
        if (RegCreateKeyExW(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store",
                0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(h, exe.c_str(), 0, REG_BINARY, data, 4);
            RegCloseKey(h);
        }
    }
    g_log.push_back(L"Registro: chiavi programma, Run, Uninstall, App Paths, .stub, CLSID, SharedDLLs");

    // ---- voce nella variabile PATH di sistema
    WriteText(PathSeg() + L"\\stub-cli.cmd", "@echo StubbornApp CLI\r\n");
    if (!PathHasSeg()) { std::wstring p = ReadPath(); if (!p.empty() && p.back() != L';') p += L';'; WritePath(p + PathSeg()); }
    // ---- comando "Apri con StubbornApp" nel menu contestuale di tutti i file
    SetStr(HKEY_LOCAL_MACHINE, VERBKEY, nullptr, L"Apri con StubbornApp");
    SetStr(HKEY_LOCAL_MACHINE, VERBKEY, L"Icon", exe + L",0");
    SetStr(HKEY_LOCAL_MACHINE, std::wstring(VERBKEY) + L"\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
    // ---- regola del firewall (in entrata, porta 47001, solo per il programma)
    RunHidden(Sys(L"netsh.exe") + L" advfirewall firewall add rule name=\"StubbornApp Sync\" dir=in action=allow program=\"" +
              exe + L"\" protocol=TCP localport=47001");
    if (!FirewallRulePresent())   // se netsh non è disponibile scrive la regola direttamente
        SetStr(HKEY_LOCAL_MACHINE, FWKEY, FWID, L"v2.30|Action=Allow|Active=TRUE|Dir=In|Protocol=6|LPort=47001|App=" + exe +
               L"|Name=StubbornApp Sync|", 0);
    // ---- cache dei nomi applicazione (MuiCache), come la crea Explorer
    SetStr(HKEY_CURRENT_USER, MUIKEY, (exe + L".FriendlyAppName").c_str(), L"StubbornApp", 0);
    g_log.push_back(L"PATH di sistema, menu contestuale, regola firewall, MuiCache");

    // ---- servizio (avvio manuale: non viene mai avviato)
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)) {
        std::wstring bin = L"\"" + exe + L"\" /service";
        SC_HANDLE s = CreateServiceW(scm, SVC, L"StubbornApp Update Service (prova)", SERVICE_ALL_ACCESS,
                                     SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                                     bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (s) {
            SERVICE_DESCRIPTIONW d{ (LPWSTR)L"Servizio fittizio creato da StubbornApp per testare ZeroTrace." };
            ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
            CloseServiceHandle(s);
        }
        CloseServiceHandle(scm);
        g_log.push_back(L"Servizio " + std::wstring(SVC));
    }

    // ---- attività pianificata (all'accesso esegue "/noop", che termina subito)
    RunHidden(Sys(L"schtasks.exe") + L" /create /tn \"" + TASK + L"\" /tr \"\\\"" + exe +
              L"\\\" /noop\" /sc onlogon /rl limited /f");
    g_log.push_back(L"Attività pianificata " + std::wstring(TASK));

    // ---- processo residente che tiene bloccato un file
    ShellExecuteW(nullptr, L"open", exe.c_str(), L"/tray", dir.c_str(), SW_HIDE);
    g_log.push_back(L"Processo residente avviato (icona nell'area di notifica)");
}

// ================================================== disinstallatore difettoso
// Come molti disinstallatori reali (NSIS/Inno): si copia in %TEMP%, rilancia la
// copia e termina subito. La copia rimuove solo una piccola parte dei file.
static bool IsAdmin() {
    BOOL admin = FALSE; PSID grp = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(nullptr, grp, &admin); FreeSid(grp);
    }
    return admin != FALSE;
}
static int BrokenUninstallStage1() {
    wchar_t self[MAX_PATH], tmp[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    GetTempPathW(MAX_PATH, tmp);
    std::wstring copy = std::wstring(tmp) + L"Au_StubbornApp.exe";
    CopyFileW(self, copy.c_str(), FALSE);
    ShellExecuteW(nullptr, IsAdmin() ? L"open" : L"runas", copy.c_str(), L"/uninstall2", tmp, SW_SHOWNORMAL);
    return 0;   // termina subito: il lavoro lo fa la copia
}
static int BrokenUninstallStage2() {
    if (MessageBoxW(nullptr, L"Rimuovere StubbornApp 3.2 e tutti i suoi componenti?",
                    L"Disinstallazione di StubbornApp", MB_ICONQUESTION | MB_YESNO) != IDYES) return 1;
    Sleep(1500);   // "sta lavorando..."
    DeleteFileW((Dir() + L"\\readme.txt").c_str());
    DeleteFileW((Dir() + L"\\config\\app.cfg").c_str());
    RemoveDirectoryW((Dir() + L"\\config").c_str());
    DeleteFileW(DeskLnk().c_str());
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, UNINST, KEY_WOW64_64KEY, 0);
    // tutto il resto viene "dimenticato"...
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    MessageBoxW(nullptr, L"StubbornApp è stato rimosso correttamente dal computer.",
                L"Disinstallazione di StubbornApp", MB_ICONINFORMATION);
    return 0;
}

// ============================================================ verifica stato
static bool FirewallRulePresent() {
    HKEY h;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FWKEY, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    bool found = false;
    std::wstring needle = L"App=" + Exe() + L"|";
    for (auto& c : needle) c = (wchar_t)towlower(c);
    std::vector<wchar_t> name(16384); std::vector<BYTE> data(16384);
    for (DWORD i = 0; !found; ++i) {
        DWORD nl = 16384, dl = 16384, type = 0;
        LONG e = RegEnumValueW(h, i, name.data(), &nl, nullptr, &type, data.data(), &dl);
        if (e == ERROR_NO_MORE_ITEMS) break;
        if (e != ERROR_SUCCESS || type != REG_SZ) continue;
        std::wstring v((wchar_t*)data.data());
        for (auto& c : v) c = (wchar_t)towlower(c);
        found = v.find(needle) != std::wstring::npos;
    }
    RegCloseKey(h);
    return found;
}

struct Check { std::wstring label; std::function<bool()> present; };
static std::vector<Check> Checks() {
    auto HKLM = HKEY_LOCAL_MACHINE; auto HKCU = HKEY_CURRENT_USER;
    std::wstring exe = Exe();
    return {
        { L"Cartella Programmi\\StubbornApp", [] { return Exists(Dir()); } },
        { L"  file bloccato data\\session.lock", [] { return Exists(Dir() + L"\\data\\session.lock"); } },
        { L"  file sola lettura/nascosto data\\license.dat", [] { return Exists(Dir() + L"\\data\\license.dat"); } },
        { L"  cartella con ACL 'nega eliminazione' protected\\", [] { return Exists(Dir() + L"\\protected"); } },
        { L"  plugin in percorso profondo", [] { return Exists(DeepDll()); } },
        { L"ProgramData\\StubbornSoft", [] { return Exists(PData()); } },
        { L"AppData\\Roaming\\StubbornSoft", [] { return Exists(Roaming()); } },
        { L"AppData\\Local\\StubbornApp", [] { return Exists(LocalDir()); } },
        { L"Menu Start\\StubbornApp", [] { return Exists(MenuDir()); } },
        { L"Collegamento sul Desktop pubblico", [] { return Exists(DeskLnk()); } },
        { L"HKLM\\SOFTWARE\\StubbornSoft (64 bit)", [=] { return KeyExists(HKLM, L"SOFTWARE\\StubbornSoft"); } },
        { L"  sottochiave Licensing con ACL 'nega DELETE'", [=] { return KeyExists(HKLM, L"SOFTWARE\\StubbornSoft\\StubbornApp\\Licensing"); } },
        { L"HKLM\\SOFTWARE\\WOW6432Node\\StubbornSoft (32 bit)", [=] { return KeyExists(HKLM, L"SOFTWARE\\StubbornSoft", KEY_WOW64_32KEY); } },
        { L"HKCU\\Software\\StubbornSoft", [=] { return KeyExists(HKCU, L"Software\\StubbornSoft", 0); } },
        { L"HKCU\\Software\\StubbornApp", [=] { return KeyExists(HKCU, L"Software\\StubbornApp", 0); } },
        { L"Avvio automatico HKCU\\...\\Run", [=] { return ValueExists(HKCU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"StubbornApp Tray", 0); } },
        { L"Avvio automatico HKLM\\...\\Run (32 bit)", [=] { return ValueExists(HKLM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"StubbornUpdater", KEY_WOW64_32KEY); } },
        { L"Voce di disinstallazione", [=] { return KeyExists(HKLM, UNINST); } },
        { L"App Paths\\StubbornApp.exe", [=] { return KeyExists(HKLM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\StubbornApp.exe"); } },
        { L"Associazione .stub", [=] { return KeyExists(HKLM, L"SOFTWARE\\Classes\\.stub"); } },
        { L"ProgID StubbornApp.Document", [=] { return KeyExists(HKLM, L"SOFTWARE\\Classes\\StubbornApp.Document"); } },
        { L"Componente COM (CLSID)  [Avanzata]", [=] { return KeyExists(HKLM, std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + CLSID_STR); } },
        { L"Valore SharedDLLs  [Avanzata]", [=] { return ValueExists(HKLM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SharedDLLs", SharedDll()); } },
        { L"Valore Compatibility Assistant  [Avanzata]", [=] { return ValueExists(HKCU,
              L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store", exe, 0); } },
        { L"Voce nella variabile PATH di sistema", [] { return PathHasSeg(); } },
        { L"Comando nel menu contestuale dei file", [=] { return KeyExists(HKLM, VERBKEY); } },
        { L"Regola firewall \"StubbornApp Sync\"", [] { return FirewallRulePresent(); } },
        { L"Valore MuiCache  [Moderata]", [=] { return ValueExists(HKCU, MUIKEY, exe + L".FriendlyAppName", 0); } },
        { L"Servizio StubbornAppSvc", [] { return KeyExists(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\StubbornAppSvc", 0); } },
        { L"Attività pianificata \\StubbornApp\\Updater", [] { return Exists(KF(FOLDERID_System) + L"\\Tasks\\StubbornApp\\Updater"); } },
    };
}

// ===================================================== pulizia totale (reset)
static void KillTray() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId() || _wcsicmp(pe.szExeFile, L"StubbornApp.exe")) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
            TerminateProcess(h, 0); WaitForSingleObject(h, 3000); CloseHandle(h);
        }
    }
    CloseHandle(snap);
}
static void DelTree(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) DelTree(p + L"\\" + fd.cFileName); }
            while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        RemoveDirectoryW(p.c_str());
    } else DeleteFileW(p.c_str());
}
static void Purge(bool deleteSelf) {
    KillTray();
    if (Exists(Dir() + L"\\protected")) ResetAcl(Dir() + L"\\protected", SE_FILE_OBJECT);
    if (KeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\StubbornSoft\\StubbornApp\\Licensing"))
        ResetAcl(L"MACHINE\\SOFTWARE\\StubbornSoft\\StubbornApp\\Licensing", SE_REGISTRY_KEY);
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    bool runningFromDir = _wcsicmp(self, Exe().c_str()) == 0;
    for (auto& p : { PData(), Roaming(), LocalDir(), MenuDir(), DeskLnk() }) DelTree(p);
    DelTree(Dir());
    if (runningFromDir && deleteSelf) {
        MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MoveFileExW(Dir().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    for (REGSAM v : { KEY_WOW64_64KEY, KEY_WOW64_32KEY }) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_ALL_ACCESS | v, &h) == ERROR_SUCCESS) {
            RegDeleteTreeW(h, L"StubbornSoft");
            RegDeleteKeyExW(h, L"StubbornSoft", v, 0);
            RegCloseKey(h);
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\StubbornSoft");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\StubbornApp");
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"StubbornApp Tray");
    {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                          KEY_SET_VALUE | KEY_WOW64_32KEY, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, L"StubbornUpdater"); RegCloseKey(h);
        }
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, UNINST);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, UNINST);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\StubbornApp.exe");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\StubbornApp.exe");
    for (auto k : { L"SOFTWARE\\Classes\\.stub", L"SOFTWARE\\Classes\\StubbornApp.Document" }) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, k); RegDeleteKeyW(HKEY_LOCAL_MACHINE, k);
    }
    std::wstring clsid = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + CLSID_STR;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid.c_str()); RegDeleteKeyW(HKEY_LOCAL_MACHINE, clsid.c_str());
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\SharedDLLs", SharedDll().c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store",
                       Exe().c_str());
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS)) {
        if (SC_HANDLE s = OpenServiceW(scm, SVC, DELETE)) { DeleteService(s); CloseServiceHandle(s); }
        CloseServiceHandle(scm);
    }
    RunHidden(Sys(L"schtasks.exe") + L" /delete /tn \"" + TASK + L"\" /f");
    // PATH, menu contestuale, firewall, MuiCache
    if (PathHasSeg()) {
        std::wstring p = ReadPath(), out, seg = PathSeg();
        size_t pos = 0;
        while (pos <= p.size()) {
            size_t sc = p.find(L';', pos);
            std::wstring s = p.substr(pos, sc == std::wstring::npos ? std::wstring::npos : sc - pos);
            if (!s.empty() && _wcsicmp(s.c_str(), seg.c_str()) != 0) { if (!out.empty()) out += L';'; out += s; }
            if (sc == std::wstring::npos) break;
            pos = sc + 1;
        }
        WritePath(out);
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, VERBKEY); RegDeleteKeyW(HKEY_LOCAL_MACHINE, VERBKEY);
    RunHidden(Sys(L"netsh.exe") + L" advfirewall firewall delete rule name=\"StubbornApp Sync\"");
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, FWKEY, FWID);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, MUIKEY, (Exe() + L".FriendlyAppName").c_str());
    // cartella vuota "StubbornApp" in Utilità di pianificazione
    RemoveDirectoryW((KF(FOLDERID_System) + L"\\Tasks\\StubbornApp").c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// ======================================================= modalità /tray
static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP + 1 && (l == WM_RBUTTONUP || l == WM_LBUTTONDBLCLK)) {
        if (MessageBoxW(h, L"StubbornApp (programma di prova) è in esecuzione e tiene bloccato\n"
                           L"il file data\\session.lock.\n\nChiudere il processo?", L"StubbornApp",
                        MB_ICONQUESTION | MB_YESNO) == IDYES) DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY) {
        NOTIFYICONDATAW nid{ sizeof(nid) }; nid.hWnd = h; nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static int TrayMode(HINSTANCE inst) {
    // blocca un file: non condivisibile, non eliminabile finché il processo vive
    HANDLE lock = CreateFileW((Dir() + L"\\data\\session.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = TrayProc; wc.hInstance = inst; wc.lpszClassName = L"StubbornTray";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"StubbornTray", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = h; nid.uID = 1; nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wcscpy(nid.szTip, L"StubbornApp (programma di prova)");
    Shell_NotifyIconW(NIM_ADD, &nid);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) DispatchMessageW(&msg);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    return 0;
}

// =================================================== finestra di controllo
static HWND g_lv, g_wnd;
static HFONT g_font;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void RefreshStatus() {
    ListView_DeleteAllItems(g_lv);
    auto checks = Checks();
    int present = 0;
    for (int i = 0; i < (int)checks.size(); ++i) {
        bool p = checks[i].present();
        present += p;
        LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (LPWSTR)checks[i].label.c_str();
        int idx = ListView_InsertItem(g_lv, &it);
        ListView_SetItemText(g_lv, idx, 1, (LPWSTR)(p ? L"● PRESENTE" : L"○ rimosso"));
    }
    std::wstring t = L"StubbornApp - " + std::to_wstring(present) + L"/" + std::to_wstring(checks.size()) + L" tracce presenti";
    SetWindowTextW(g_wnd, t.c_str());
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int id, int x, int y, int cw, int ch) {
            HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st, S(x), S(y), S(cw), S(ch), h, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
        };
        mk(L"STATIC", L"Programma di PROVA per ZeroTrace: installa tracce difficili da rimuovere e un disinstallatore difettoso.",
           SS_LEFT, 0, 10, 10, 640, 20);
        mk(L"BUTTON", L"1. Installa", BS_PUSHBUTTON, 1, 10, 36, 120, 30);
        mk(L"BUTTON", L"2. Disinstallatore difettoso", BS_PUSHBUTTON, 2, 136, 36, 190, 30);
        mk(L"BUTTON", L"Verifica tracce", BS_PUSHBUTTON, 3, 332, 36, 120, 30);
        mk(L"BUTTON", L"Ripulisci tutto (reset)", BS_PUSHBUTTON, 4, 458, 36, 170, 30);
        g_lv = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOSORTHEADER | WS_BORDER, 5, 10, 76, 620, 470);
        SetWindowTheme(g_lv, L"Explorer", nullptr);
        ListView_SetExtendedListViewStyle(g_lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)L"Traccia"; c.cx = S(470); ListView_InsertColumn(g_lv, 0, &c);
        c.pszText = (LPWSTR)L"Stato"; c.cx = S(120); ListView_InsertColumn(g_lv, 1, &c);
        RefreshStatus();
        return 0;
    }
    case WM_COMMAND: {
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        switch (LOWORD(w)) {
        case 1:
            if (Exists(Dir())) { MessageBoxW(h, L"StubbornApp risulta già installato. Usa prima \"Ripulisci tutto\".", L"StubbornApp", MB_ICONINFORMATION); break; }
            g_log.clear(); Install(); Sleep(500); RefreshStatus();
            {
                std::wstring s = L"Installazione simulata completata:\n\n";
                for (auto& x : g_log) s += L"• " + x + L"\n";
                s += L"\nOra apri ZeroTrace e disinstalla \"StubbornApp 3.2 (programma di prova)\".";
                MessageBoxW(h, s.c_str(), L"StubbornApp", MB_ICONINFORMATION);
            }
            break;
        case 2:
            ShellExecuteW(h, L"open", Exe().c_str(), L"/uninstall", nullptr, SW_SHOWNORMAL);
            break;
        case 3: RefreshStatus(); break;
        case 4:
            if (MessageBoxW(h, L"Rimuovere completamente tutte le tracce di StubbornApp?", L"StubbornApp", MB_ICONQUESTION | MB_YESNO) == IDYES) {
                Purge(true); Sleep(300); RefreshStatus();
            }
            break;
        }
        return 0;
    }
    case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE && g_lv) RefreshStatus(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring a = cmd ? cmd : L"";
    auto has = [&](const wchar_t* s) { return a.find(s) != std::wstring::npos; };
    if (has(L"/uninstall2")) return BrokenUninstallStage2();
    if (has(L"/uninstall"))  return BrokenUninstallStage1();
    if (has(L"/tray"))       return TrayMode(inst);
    if (has(L"/noop") || has(L"/service")) return 0;
    if (has(L"/status")) {   // per i test automatici: codice di uscita = tracce ancora presenti (0 = tutto pulito)
        int n = 0;
        std::string rep;
        for (auto& c : Checks())
            if (c.present()) {
                ++n;
                int len = WideCharToMultiByte(CP_UTF8, 0, c.label.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string u(len > 0 ? len - 1 : 0, 0);
                WideCharToMultiByte(CP_UTF8, 0, c.label.c_str(), -1, &u[0], len, nullptr, nullptr);
                rep += u + "\r\n";
            }
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        WriteText(std::wstring(tmp) + L"stubbornapp_status.txt", rep);   // elenco delle tracce presenti
        return n;
    }
    if (!IsAdmin()) {   // install, purge e finestra richiedono privilegi amministrativi
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        ShellExecuteW(nullptr, L"runas", self, a.c_str(), nullptr, SW_SHOWNORMAL);
        return 0;
    }
    if (has(L"/install"))    { Install(); return 0; }
    if (has(L"/purge"))      { Purge(true); return 0; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSW wc{}; wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = L"StubbornMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    RECT r{ 0, 0, S(640), S(556) };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowW(L"StubbornMain", L"StubbornApp", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(g_wnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(g_wnd, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
