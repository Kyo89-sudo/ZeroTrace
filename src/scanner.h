// ZeroTrace - disinstallatore stile Revo Uninstaller
// scanner.h : enumerazione programmi, scansione residui, rimozione
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

struct Program {
    std::wstring name, publisher, version, installDate;
    std::wstring installLocation, uninstallString, quietUninstallString, displayIcon;
    std::wstring keyName;      // nome della sottochiave Uninstall
    HKEY         root = nullptr;
    REGSAM       view = 0;     // KEY_WOW64_64KEY / KEY_WOW64_32KEY / 0
    DWORD        sizeKB = 0;
    bool         isMsi = false;
    std::wstring RootName() const;   // "HKLM" / "HKCU"
    std::wstring UninstallKeyPath() const;
};

// L'ordine conta: è anche l'ordine dei gruppi nell'interfaccia
enum class ResKind { RegKey, RegValue, Service, Task, Firewall, PathEntry, Folder, File };

struct Residue {
    ResKind      kind;
    HKEY         root = nullptr;   // registro
    REGSAM       view = 0;
    std::wstring path;             // sottochiave / percorso file / nome servizio / nome task
    std::wstring valueName;        // RegValue / PathEntry / Firewall (id del valore)
    std::wstring extra;            // PathEntry: voce da togliere; Firewall: nome della regola
    std::wstring reason;           // perché è stato trovato
    bool         checked = true;
    std::wstring Display() const;  // testo mostrato all'utente
};

enum class ScanLevel { Safe = 0, Moderate = 1, Advanced = 2 };

using LogFn = std::function<void(const std::wstring&)>;

// Programmi installati (HKLM 64/32 + HKCU)
std::vector<Program> EnumeratePrograms();

// Esegue il disinstallatore e attende la fine di tutti i processi figli.
// Ritorna false se non è stato possibile avviarlo.
bool RunUninstaller(const Program& p, DWORD& exitCode, LogFn log);

// Scansione residui. installDirHint può essere vuoto.
std::vector<Residue> ScanResidues(const Program& p, ScanLevel level, LogFn log);

// Termina processi in esecuzione dalla cartella indicata
int  KillProcessesInFolder(const std::wstring& dir, LogFn log);

// Rimuove gli elementi selezionati. backupDir: dove esportare il registro.
// Ritorna il numero di errori.
int  RemoveResidues(std::vector<Residue>& items, bool useRecycleBin,
                    const std::wstring& backupDir, LogFn log, bool& rebootNeeded);

// Punto di ripristino di sistema (richiede admin e Protezione sistema attiva)
bool CreateRestorePoint(const std::wstring& desc, LogFn log);

// Utilità
std::wstring ResolveInstallDir(const Program& p);
bool IsProtectedPath(const std::wstring& path);
std::wstring Lower(std::wstring s);
bool IsRunningAsAdmin();
