@echo off
rem Windows installer (MSI) build for the ASI camera app (WiX Toolset 3.14.1).
rem Usage: build_msi.bat
rem   1. harvests package_win\ (run build_win.bat package first)
rem   2. compiles dist\wix\camera_app.wxs + the harvested file list
rem   3. links dist\camera_app-1.0.0-x64.msi (per-user install, no admin needed)
setlocal
cd /d "%~dp0"

if not exist package_win\camera_app.exe (
  echo package_win\ missing: run build_win.bat package first
  exit /b 1
)
if not exist third_party\wix\candle.exe (
  echo WiX missing: extract third_party\dl\wix314-binaries.zip into third_party\wix
  exit /b 1
)
if not exist dist\wix mkdir dist\wix

echo === harvest (heat dir package_win) ===
rem heat flag notes:
rem   -srd                -> do NOT emit the root directory as an element, so the
rem                           files install flat under -dr INSTALLFOLDER (no
rem                           "package_win" nesting level)
rem   -var var.SourceDir  -> File Source="$(var.SourceDir)\..." (candle defines
rem                           it with -dSourceDir)
rem   -cg PkgFiles        -> wraps every component in <ComponentGroup Id="PkgFiles">
rem   -gg                 -> write the component GUIDs into the .wxs NOW (heat
rem                           leaves PUT-GUID-HERE for group members otherwise);
rem                           stable for a given file set, so upgrades of the
rem                           same version reuse the components
third_party\wix\heat.exe dir package_win -dr INSTALLFOLDER -cg PkgFiles -gg -sreg -scom -srd -nocomp -var var.SourceDir -nologo -out dist\wix\package_files.wxs
if errorlevel 1 (echo harvest FAILED & exit /b 1)

echo === compile (candle) ===
rem -out is a DIRECTORY (candle refuses one output file for two sources):
rem it writes dist\wix\camera_app.wixobj + dist\wix\package_files.wixobj.
third_party\wix\candle.exe -nologo -arch x64 -dSourceDir=%CD%\package_win -out dist\wix\ dist\wix\camera_app.wxs dist\wix\package_files.wxs
if errorlevel 1 (echo compile FAILED & exit /b 1)

echo === link (light) ===
rem No custom UI (see the note in camera_app.wxs): msiexec provides the
rem standard Windows Installer dialogs.
rem -sval: the ICE consistency checks run through the Windows Installer
rem service (msihost); in a restricted/CI session that access is blocked and
rem every ICE aborts the link (LGHT0217) even though the service is running.
rem The install is simple (per-user, fixed file set, no custom
rem actions/services/registry), so skip light's MSI validation here. Drop
rem -sval on a normal desktop and let the ICEs run.
third_party\wix\light.exe -nologo -sval -cultures:en-US dist\wix\camera_app.wixobj dist\wix\package_files.wixobj -out dist\camera_app-1.0.0-x64.msi
if errorlevel 1 (echo link FAILED & exit /b 1)

echo === done: dist\camera_app-1.0.0-x64.msi ===
goto :eof
