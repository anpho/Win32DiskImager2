@echo off
setlocal

rem Uses any MinGW-w64 g++ on PATH; falls back to the llvm-mingw copy installed
rem by NS Port Builder on this machine.
if not defined CXX set CXX=g++
if not defined RC  set RC=windres
set "MINGW=%LOCALAPPDATA%\NS Port Builder\tools\llvm-mingw-20261020\bin"
if exist "%MINGW%\x86_64-w64-mingw32-g++.exe" set "PATH=%MINGW%;%PATH%"

%RC% --codepage=65001 -I. Win32DiskImager.rc -O coff -o resources.o || exit /b 1
%CXX% -municode -mwindows -O2 -s -DUNICODE -D_UNICODE -static ^
    main.cpp resources.o -o Win32DiskImager.exe ^
    -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lbcrypt -ladvapi32 -lgdi32 -luser32 || exit /b 1
del resources.o 2>nul
echo Built %CD%\Win32DiskImager.exe
