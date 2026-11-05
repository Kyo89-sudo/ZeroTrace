@echo off
rem Compilazione con Visual Studio (aprire "x64 Native Tools Command Prompt for VS")
setlocal
if not exist bin mkdir bin
set FLAGS=/nologo /O2 /EHsc /std:c++17 /utf-8 /MT /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
set LIBS=comctl32.lib shell32.lib shlwapi.lib ole32.lib uuid.lib advapi32.lib uxtheme.lib gdi32.lib gdiplus.lib msimg32.lib user32.lib

rc /nologo /fo bin\ZeroTrace.res src\ZeroTrace.rc || goto :err
cl %FLAGS% /Fobin\ /Febin\ZeroTrace.exe src\main.cpp src\scanner.cpp bin\ZeroTrace.res %LIBS% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO || goto :err

rc /nologo /fo bin\StubbornApp.res test\StubbornApp\StubbornApp.rc || goto :err
cl %FLAGS% /Fobin\ /Febin\StubbornApp.exe test\StubbornApp\stubborn.cpp bin\StubbornApp.res %LIBS% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO || goto :err

cl %FLAGS% /Fobin\ /Febin\selftest.exe test\selftest.cpp src\scanner.cpp %LIBS% /link /SUBSYSTEM:CONSOLE || goto :err

rem installer (incorpora i file appena compilati)
rc /nologo /I . /I setup /fo bin\setup.res setup\setup.rc || goto :err
cl %FLAGS% /Fobin\ /Febin\ZeroTrace-Setup-1.1.exe setup\setup.cpp bin\setup.res %LIBS% /link /SUBSYSTEM:WINDOWS /MANIFEST:NO || goto :err

del bin\*.obj bin\*.res 2>nul
echo.
echo Compilazione completata: bin\ZeroTrace.exe, bin\StubbornApp.exe, bin\selftest.exe, bin\ZeroTrace-Setup-1.1.exe
exit /b 0
:err
echo ERRORE di compilazione
exit /b 1
