// Test automatico (console): installa StubbornApp, simula il disinstallatore
// difettoso, scansiona i residui con ZeroTrace e li rimuove, poi verifica.
//   selftest.exe <percorso di StubbornApp.exe>
#include "../src/scanner.h"
#include <cstdio>
#include <io.h>
#include <fcntl.h>

static void Out(const std::wstring& s) { wprintf(L"%ls\n", s.c_str()); fflush(stdout); }

static DWORD Run(const std::wstring& cmd) {
    std::vector<wchar_t> b(cmd.begin(), cmd.end()); b.push_back(0);
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, b.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD c; GetExitCodeProcess(pi.hProcess, &c);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return c;
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc < 2) { Out(L"uso: selftest <StubbornApp.exe>"); return 2; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto log = [](const std::wstring& s) { Out(L"  | " + s); };

    Out(L"== 1. Installazione StubbornApp");
    Run(L"\"" + std::wstring(argv[1]) + L"\" /install");
    Out(L"Tracce create: " + std::to_wstring(Run(L"\"" + std::wstring(argv[1]) + L"\" /status")));
    Sleep(1500);

    Program prog; bool found = false;
    for (auto& p : EnumeratePrograms())
        if (p.name.find(L"StubbornApp") != std::wstring::npos) { prog = p; found = true; }
    if (!found) { Out(L"ERRORE: StubbornApp non compare tra i programmi installati"); return 1; }
    Out(L"Trovato: " + prog.name + L" | " + prog.publisher + L" | " + prog.uninstallString);
    Out(L"Cartella risolta: " + ResolveInstallDir(prog));

    Out(L"== 2. Disinstallatore difettoso (simulato: rimuove solo la voce Uninstall e readme)");
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, prog.UninstallKeyPath().c_str(), KEY_WOW64_64KEY, 0);
    DeleteFileW((ResolveInstallDir(prog) + L"\\readme.txt").c_str());

    for (int lvl = 0; lvl < 3; ++lvl) {
        const wchar_t* n[] = { L"Sicura", L"Moderata", L"Avanzata" };
        auto r = ScanResidues(prog, (ScanLevel)lvl, [](const std::wstring&) {});
        Out(std::wstring(L"== 3. Scansione ") + n[lvl] + L": " + std::to_wstring(r.size()) + L" elementi");
        if (lvl == 2) for (auto& x : r) Out(L"   [" + x.reason + L"] " + x.Display());
    }

    auto items = ScanResidues(prog, ScanLevel::Advanced, log);
    Out(L"== 4. Termina processi + rimozione");
    KillProcessesInFolder(ResolveInstallDir(prog), log);
    bool reboot = false;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    int err = RemoveResidues(items, false, std::wstring(tmp) + L"zerotrace_backup", log, reboot);
    Out(L"Errori: " + std::to_wstring(err) + L"  riavvio necessario: " + (reboot ? L"sì" : L"no"));

    DWORD left = Run(L"\"" + std::wstring(argv[1]) + L"\" /status");
    Out(L"Tracce ancora presenti secondo StubbornApp: " + std::to_wstring(left));
    auto again = ScanResidues(prog, ScanLevel::Advanced, [](const std::wstring&) {});
    Out(L"== 5. Nuova scansione dopo la rimozione: " + std::to_wstring(again.size()) + L" elementi");
    for (auto& x : again) Out(L"   RESTA: " + x.Display());
    return again.empty() && err == 0 && left == 0 ? 0 : 1;
}
