@echo off
rem Windows build for the ASI camera app (MSVC 2022 x64, CMake + nmake).
rem Usage: build_win.bat [configure|build|all|package|clean]
setlocal
cd /d "%~dp0"

rem vcvars64.bat: honor an explicit %VCVARS% override, else probe the standard
rem install locations (VS 2022/2026, all editions; BuildTools lives in
rem "Program Files (x86)"). Set VCVARS to override, e.g. for a custom install.
if not defined VCVARS (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files ^(x86^\)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files ^(x86^\)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2026\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2026\Professional\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files\Microsoft Visual Studio\2026\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2026\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files ^(x86^\)\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files ^(x86^\)\Microsoft Visual Studio\2026\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else (
    echo vcvars64.bat not found: install Visual Studio 2022 Build Tools
    echo with the Desktop development with C++ workload, or set VCVARS.
    exit /b 1
  )
)
echo using vcvars: %VCVARS%

set CMAKE=third_party\extract\cmake-3.29.6-windows-x86_64\bin\cmake.exe
if not exist "%CMAKE%" (
  where cmake >nul 2>nul
  if errorlevel 1 (
    echo CMake not found: install CMake from cmake.org/download/ or restore
    echo third_party\extract\cmake-3.29.6-windows-x86_64 - see README.
    exit /b 1
  )
  set CMAKE=cmake
)
echo using cmake: %CMAKE%

set BUILD_DIR=build_win

call "%VCVARS%" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)

set MODE=%1
if "%MODE%"=="" set MODE=all

if "%MODE%"=="configure" goto :do_configure
if "%MODE%"=="build" goto :do_build
if "%MODE%"=="package" goto :do_package
if "%MODE%"=="clean" (
  if exist %BUILD_DIR% rmdir /s /q %BUILD_DIR%
  if exist package_win rmdir /s /q package_win
  echo cleaned & exit /b 0
)
if "%MODE%"=="all" (
  call :do_configure
  if errorlevel 1 exit /b 1
  call :do_build
  exit /b %errorlevel%
)
echo unknown mode: %MODE% (configure^|build^|package^|clean^|all)
exit /b 1

:do_configure
echo === configure ===
rem moc first (CMake's AUTOMOC is off on Windows: the sandbox on this box
rem denies CMake's autogen libuv child-process spawn). The six Q_OBJECT
rem headers are moked into %BUILD_DIR%\moc_*.cpp, which CMake compiles as
rem regular sources.
call :do_moc
if errorlevel 1 exit /b 1
rem CMake try-compile sub-builds stall on this machine (the nested compile/link
rem of a fresh .exe never completes), so every check that uses try_compile is
rem pre-seeded with the known-good MSVC value (verified by hand, docs/build.md section 5):
rem   CMAKE_CXX_COMPILER_WORKS  compiler works (cl + link of a trivial exe)
rem   CMAKE_CXX_ABI_COMPILED    ABI detection (MSVC 19.43 x64)
rem   CMAKE_HAVE_LIBC_PTHREAD   MSVC/UCRT uses native threads, no -lpthread
rem   HAVE_STDATOMIC            Qt6 FindWrapAtomic check; MSVC <atomic> works
rem Generator: NMake Makefiles (MSVC's own nmake, sequential); the Ninja
rem generator's child-process handling stalls on this box (docs/build.md section 5).
"%CMAKE%" -S . -B %BUILD_DIR% -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_CXX_COMPILER_WORKS=TRUE -DCMAKE_CXX_ABI_COMPILED=TRUE -DCMAKE_HAVE_LIBC_PTHREAD=TRUE -DHAVE_STDATOMIC=TRUE
if errorlevel 1 (echo configure FAILED & exit /b 1)
goto :eof

:do_moc
echo === moc: six Q_OBJECT headers ===
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
set MOC=third_party\qt\6.7.3\msvc2019_64\bin\moc.exe
set QTINC=third_party\qt\6.7.3\msvc2019_64\include
for %%H in (camera_worker main_window mode_toggle record_button shutter_button sequence_button) do (
  "%MOC%" -DQT_CORE_LIB -DQT_GUI_LIB -DQT_NO_DEBUG -DQT_WIDGETS_LIB -DUNICODE -DWIN32 -DWIN64 -D_ENABLE_EXTENDED_ALIGNED_STORAGE -D_UNICODE -D_WIN64 -Iinclude -Itests -IASI_SDK -I%QTINC%\QtCore -I%QTINC%\QtGui -I%QTINC%\QtWidgets -o %BUILD_DIR%\moc_%%H.cpp include\%%H.h >nul
  if errorlevel 1 (echo moc FAILED: %%H & exit /b 1)
)
goto :eof

:do_package
rem Self-contained package_win/ folder (the Windows analogue of `make
rem package` on Linux): the executable + every runtime DLL it loads (Qt,
rem OpenCV, ZWO SDK, GStreamer + glib) + the GStreamer encoder plugins
rem (gstreamer-1.0/) + the Qt platform plugins (platforms/) + the docs.
rem Run package_win\camera_app.exe from any CWD - no system Qt/OpenCV/
rem GStreamer or VS runtime installation required.
echo === package ===
rem Ship the GUI-subsystem exe (no cmd window): build_win\gui\camera_app.exe.
rem The console twin at build_win\camera_app.exe stays for headless testing.
if not exist %BUILD_DIR%\gui\camera_app.exe (echo build first: build_win.bat build & exit /b 1)
if exist package_win rmdir /s /q package_win
mkdir package_win
copy /y %BUILD_DIR%\gui\camera_app.exe package_win\ >nul
copy /y %BUILD_DIR%\*.dll package_win\ >nul
xcopy /e /i /q %BUILD_DIR%\gstreamer-1.0 package_win\gstreamer-1.0 >nul
xcopy /e /i /q %BUILD_DIR%\platforms package_win\platforms >nul
copy /y README.md package_win\ >nul
copy /y ASI_SDK\license.txt package_win\ >nul
echo === done: package_win\ ===
goto :eof

:do_build
rem Run nmake directly (the Makefile CMake generated): cmd -> nmake -> cl/link,
rem shallow tree, sequential, MSVC-native. `cmake --build` would add a cmake
rem process in the tree, which stalls on this box.
echo === build ===
pushd %BUILD_DIR%
nmake
popd
if errorlevel 1 (echo build FAILED & exit /b 1)
echo === done: %BUILD_DIR%\camera_app.exe ===
goto :eof
