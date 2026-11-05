# MinGW-w64: da Linux "make", da MSYS2 "make PREFIX="
PREFIX  ?= x86_64-w64-mingw32-
CXX     := $(PREFIX)g++
WINDRES := $(PREFIX)windres
FLAGS = -std=c++17 -O2 -s -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Wall -Wno-missing-field-initializers
LIBS  = -static -lcomctl32 -lshell32 -lshlwapi -lole32 -luuid -ladvapi32 -luxtheme -lgdi32 -lgdiplus -lmsimg32

all: bin/ZeroTrace.exe bin/StubbornApp.exe bin/selftest.exe bin/ZeroTrace-Setup-1.1.exe

bin:
	mkdir -p bin

bin/zerotrace_res.o: src/ZeroTrace.rc src/ZeroTrace.manifest src/ZeroTrace.ico | bin
	$(WINDRES) $< -O coff -o $@

bin/stub_res.o: test/StubbornApp/StubbornApp.rc test/StubbornApp/StubbornApp.manifest | bin
	$(WINDRES) $< -O coff -o $@

bin/ZeroTrace.exe: src/main.cpp src/scanner.cpp src/scanner.h bin/zerotrace_res.o
	$(CXX) $(FLAGS) -mwindows src/main.cpp src/scanner.cpp bin/zerotrace_res.o -o $@ $(LIBS)

bin/StubbornApp.exe: test/StubbornApp/stubborn.cpp bin/stub_res.o
	$(CXX) $(FLAGS) -mwindows $< bin/stub_res.o -o $@ $(LIBS)

bin/selftest.exe: test/selftest.cpp src/scanner.cpp src/scanner.h
	$(CXX) $(FLAGS) test/selftest.cpp src/scanner.cpp -o $@ $(LIBS)

bin/setup_res.o: setup/setup.rc setup/setup_res.h bin/ZeroTrace.exe bin/StubbornApp.exe README.md src/ZeroTrace.ico
	$(WINDRES) -I setup $< -O coff -o $@

bin/ZeroTrace-Setup-1.1.exe: setup/setup.cpp setup/setup_res.h bin/setup_res.o
	$(CXX) $(FLAGS) -mwindows setup/setup.cpp bin/setup_res.o -o $@ $(LIBS)

clean:
	rm -rf bin
