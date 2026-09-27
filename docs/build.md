# Build system notes (AGENTS.md §5)

## 5. Build system (Windows x64)

The app is built for **x64 Windows** with **MSVC 2022** (toolset 14.4x) via
`CMakeLists.txt`, driven by `build_win.bat` (finds `vcvars64.bat`, runs `moc`
on the six `Q_OBJECT` headers, configures CMake with the `NMake Makefiles`
generator, then builds with nmake). Everything the app needs is vendored in
`third_party/`, so the build is self-contained:

```
third_party/
  qt/6.7.3/msvc2019_64/     Qt 6.7.3 official msvc2019_64 binary (Qt.io archive)
  opencv/opencv/build/       OpenCV 4.12.0 official windows archive
                              (build/x64/vc16/bin/opencv_world4120.dll + lib +
                               build/include; a single "world" DLL)
  gst/                       GStreamer 1.24.12 from the MSYS2 mingw64 repo
                              (binaries are C-ABI; MSVC links them through the
                               COFF import libs in gst/lib/*.lib):
                              include/   gstreamer-1.0 + glib-2.0 (+glibconfig)
                              lib/       import libs (*.dll.a renamed to .lib)
                              runtime/   core + glib + x264(164+165) + pcre2 +
                                         zlib + zstd + libwinpthread +
                                         libgcc_s_seh-1 + libintl + libiconv +
                                         liborc (+ gcc-libs libstdc++-6)
                              plugins/   all plugin DLLs incl. the 5 the
                                         encoder pipeline needs
  extract/cmake-.../         CMake 3.29.6 (build tool, no install)
```

Assembly notes (how the tree is built on a fresh machine — all downloads must
go through Node.js: system TLS is broken on this box, PowerShell/curl/choco
HTTPS fail with `SEC_E_NO_CREDENTIALS` while Node v24+ works):

- **Qt 6.7.3**: from the Qt online archives (qt.io `6.7.3/msvc2019_64/` 7z
  files: qtbase, qttools). The `6.8.4` directory exists in the repo but is
  EMPTY.
- **OpenCV 4.12.0**: `opencv-4.12.0-windows.exe` is a 7-Zip **SFX** (the
  Inno-style `/VERYSILENT` flags do nothing and it prompts for a directory).
  Extract with the 7-Zip console: `7zr.exe x -y -o<dir>
  opencv-4.12.0-windows.exe` (7zr reads the embedded 7z at its own offset;
  the layout is `<dir>/opencv/build/...`).
- **GStreamer 1.24.12**: the official `gstreamer-1.24.12.tar.xz` on
  gstreamer.freedesktop.org contains only the core (the subprojects are
  separate tarballs — e.g. `gst/isomp4/` is NOT in plugins-base), so building
  the pipeline from source is not the route. The staged set here combines
  MSYS2 **mingw64** packages (gstreamer, plugins-base/good/ugly, glib2,
  pcre2, zlib, zstd, libwinpthread, gcc-libs 16.1.0) with a few **ucrt64**
  packages (gettext-runtime, libiconv, orc, libx264 0.164) — both repos are
  C-ABI and the mix loads cleanly (verified: every staged DLL resolves and
  the encoder pipeline encodes end-to-end). MSYS2 packages
  (`repo.msys2.org/mingw/mingw64/` and `.../mingw/ucrt64/`) ship prebuilt
  64-bit C-ABI DLLs + import libs for all of it:
  `mingw-w64-x86_64-{gstreamer,gstreamer-plugins-base,
  gstreamer-plugins-good,gstreamer-plugins-ugly,glib2,libx264,pcre2,zlib,
  zstd,libwinpthread}-1.24.12` plus the transitive runtime DLLs the staged
  set actually loads: `gettext-runtime` (`libintl-8.dll`), `libiconv`
  (`libiconv-2.dll`), `orc` (`liborc-0.4-0.dll`), and `libx264 0.164`
  (`libx264-164.dll` — the 1.24.12 x264 plugin links the 164 soname, not the
  newer 165; keep both), and `gcc-libs-16.1.0` for
  `libgcc_s_seh-1.dll`/`libstdc++-6.dll` (the gstreamer core imports
  libgcc). Merge the packages into one prefix, rename `lib/*.dll.a` →
  `lib/*.lib` (COFF import libs, linkable by MSVC), and keep
  `lib/glib-2.0/include/glibconfig.h` on the include path (the glib headers
  do `#include <glibconfig.h>` flat).
  `gitlab.freedesktop.org` is behind an Anubis bot-wall (returns challenge
  HTML with HTTP 200) — use the freedesktop *src* tarballs and the GitHub
  `gstreamer/gstreamer` mirror instead.
- **ZWO SDK**: `ASI_SDK/x64/ASICamera2.lib` + `.dll` (vendored).
- **Build tool**: CMake 3.29.6 windows zip (no install; used from
  `third_party/extract/`).

Build:

```
build_win.bat configure   # vcvars64 + moc + CMake (NMake Makefiles generator)
build_win.bat build       # nmake in build_win (direct)
build_win.bat package     # assemble the self-contained package_win/ folder
build_win.bat clean
```

→ **two builds from the same sources**: `build_win/camera_app.exe`
(console subsystem — kept for the headless tests) and
`build_win/gui/camera_app.exe`, the **GUI-subsystem** twin that
`package_win/` ships. CMake target `camera_app_gui` sets
`WIN32_EXECUTABLE` + `LINKER:/ENTRY:mainCRTStartup`, so the plain
`int main(int, char**)` entry works under `/SUBSYSTEM:WINDOWS` and the app
opens **without a command window** (Windows hands every console-subsystem
exe a console; the PE optional-header word at offset +68 — 2 = GUI,
3 = CUI — is what `tools/pesubsys.ps1` checks). `/utf-8` for the µ/°/§ in
the sources. Post-build steps stage the runtime DLLs next to **each** exe:
OpenCV world DLL + ffmpeg videoio, `ASICamera2.dll`, the three Qt DLLs +
the `windows`/`offscreen` platform plugins (`platforms/`), all GStreamer
runtime DLLs, and the 5 encoder plugins into `gstreamer-1.0/` — `main()`
sets `GST_PLUGIN_PATH` to that folder when present (see `src/main.cpp`), so
no system Qt/OpenCV/GStreamer is needed.

**Box-specific quirks (2026-09; the DSH file sandbox on this machine denies
some child-process spawns, which looks like a "build stall"):**

- CMake **try-compile sub-builds stall** — the nested `cmake → make → link`
  of a fresh `.exe` never completes. Workaround: `build_win.bat` pre-seeds
  every try-compile-based check with the known-good MSVC value
  (`CMAKE_CXX_COMPILER_WORKS`, `CMAKE_CXX_ABI_COMPILED`,
  `CMAKE_HAVE_LIBC_PTHREAD`, and Qt6's `HAVE_STDATOMIC`).
- **Ninja's child spawn stalls** (a `ninja → cl` edge never starts) while
  `nmake → cl` is fine — so the generator is **NMake Makefiles** and the
  build runs `nmake` directly in `build_win/`.
- **CMake's AUTOMOC cannot spawn `moc`** — `cmake -E cmake_autogen` dies with
  `libuv process spawn failed: operation not permitted`. So the six Q_OBJECT
  headers are moked by `build_win.bat` itself into `build_win/moc_*.cpp`,
  `CMAKE_AUTOMOC` is off, and those files are regular sources.
- `dbghlp.lib` no longer ships in the SDK — `CaptureStackBackTrace` links
  against the SDK's `DbgHelp` (`dbghelp.dll`).
- `QPixmap::save(name)` with an extension-less name fails on the Windows Qt
  build, so `--uishot` saves the window shot with an explicit `PNG` format.

On a machine without these sandbox quirks, plain
`cmake -S . -B build_win -G Ninja` + `cmake --build build_win` works.

Headless verification (no camera needed):

```
build_win\camera_app.exe -platform offscreen --sertest --frametest --capstest
build_win\camera_app.exe -platform offscreen --colourtest --wbtest --fpstest
build_win\camera_app.exe --smoke    # needs a connected camera
```

(The headless GUI-mode tests need `-platform offscreen` + the `platforms/`
folder; a real desktop run needs no flag.)

`tests/gst_encode_test.cpp` is a standalone probe that runs the app's exact
H.264→MP4 pipeline (BGR appsrc → videoconvert → x264enc baseline →
mp4mux → filesink) on synthetic frames and validates the resulting MP4
(`ftyp` box + size); it is built separately (own `main()`), e.g.:

```
cl /O2 /MD /EHsc /std:c++17 /utf-8 -DUNICODE -D_UNICODE ^
   -I third_party\gst\include\gstreamer-1.0 -I third_party\gst\include\glib-2.0 ^
   -I third_party\gst\lib\glib-2.0\include tests\gst_encode_test.cpp ^
   /Fe:build_win\gst_encode_test.exe ^
   /link third_party\gst\lib\libgstreamer-1.0.dll.lib ^
         third_party\gst\lib\libgstapp-1.0.dll.lib ^
         third_party\gst\lib\libglib-2.0.dll.lib ^
         third_party\gst\lib\libgobject-2.0.dll.lib
```

**Self-contained `package_win/`** (via `build_win.bat package`): `camera_app.exe` + every runtime DLL it loads
(Qt6Core/Gui/Widgets, `opencv_world4120.dll` + ffmpeg videoio,
`ASICamera2.dll`, the whole GStreamer runtime set incl. libintl/libiconv/
liborc + x264 164/165), the encoder plugins in `gstreamer-1.0/`, the Qt
platform plugins in `platforms/`, `README.md`, and the ZWO `license.txt`.
It runs from any CWD — no system Qt/OpenCV/GStreamer installation required
(the machine needs the standard VC++ runtime in `System32`, as any MSVC app
does). True single-file embedding is not possible with dynamically linked
Qt/OpenCV/GStreamer; the folder is the self-contained deliverable. Note that
`package_win/camera_app.exe`
is the **GUI-subsystem** build (see above); the console twin stays in
`build_win/` for the headless suites.

**`.msi` installer (`build_msi.bat`, WiX Toolset 3.14.1).** The same
`package_win/` tree is packaged into `dist/camera_app-1.1.0-x64.msi`. The
toolset lives in `third_party/wix/` (candle/light/heat/dark + the extension
DLLs), extracted from the community release `wix3141rtm` — GitHub org
**`wixtoolset`** (the `wix3` org 404s), asset `wix314-binaries.zip`;
`tools/dlwix.mjs` downloads it. Pipeline:

```
heat   dir package_win -dr INSTALLFOLDER -cg PkgFiles -gg -sreg -scom -srd -nocomp -var var.SourceDir -nologo -out dist\wix\package_files.wxs
candle -nologo -arch x64 -dSourceDir=%CD%\package_win -out dist\wix\ dist\wix\camera_app.wxs dist\wix\package_files.wxs
light  -nologo -sval -cultures:en-US dist\wix\camera_app.wixobj dist\wix\package_files.wixobj -out dist\camera_app-1.1.0-x64.msi
```

Product "ASI Camera Capture" 1.1.0.0, `InstallScope=perUser` (installs to
`%LOCALAPPDATA%\ASI Camera Capture`, **no UAC**), fixed `UpgradeCode
A42DE0D7-C7AB-4042-B913-8EEAE27BBF30` + `MajorUpgrade` for upgrades, 54
components with heat-generated stable GUIDs, files flat under the install
dir (+ `gstreamer-1.0/`, `platforms/`). The ZWO driver is **not** bundled —
it stays the separate per-machine signed install (README, Prerequisites step 2).

The `Product/@Id` is a **fixed literal GUID**
(`E5829535-BC75-4B62-A790-D78A3FC89459`), never `Id="*"`: a regenerated code
per rebuild makes every rebuilt MSI a *different* product — installing a
rebuild next to the old one leaves **two** "ASI Camera Capture" entries in
*Add or remove programs* (and a leftover per-user product registration can
make a fresh install of the same version abort with 1638 "product is
installed per-user"). Keep the literal for the product's life; the
`UpgradeCode` is what upgrades key on.
Flag notes that took real debugging:

- `-cg <name>` (NOT `-gg`) wraps the components in the ComponentGroup the
  Feature references; `-gg` only writes the GUIDs *now* (without it,
  group members come out `Guid="PUT-GUID-HERE"` → CNDL0040).
- `-srd` suppresses the root directory element; without it heat nests the
  whole tree one level down in a `package_win/` directory.
- `-var var.SourceDir` **with the `var.` prefix**: heat's default
  `Source="SourceDir\..."` placeholders only become valid
  `$(var.SourceDir)\...` preprocessor refs that way (a bare `$(SourceDir)`
  is ill-formed → CNDL0149); candle defines it with `-dSourceDir=`.
- No custom `<UI>`: the binaries zip does not ship `wixui.wxi`
  (WixStandardUI), and the v3 source tree never checks the `.wxi` in
  either, so the MSI carries no UI tables and msiexec shows its standard
  dialogs; silent `/qn` install/uninstall works regardless. Double-clicking
  the `.msi` therefore shows only a brief msiexec progress box and "closes
  right away" — that is the install finishing (a few seconds for 54
  per-user files), not a failure; verify via the install dir or *Add or
  remove programs*. (WiX v4 has a `<LaunchApp>` element; in v3
  launch-after-install needs a WixUtil custom action.)
- `-sval` on light skips the ICE consistency checks: they run through the
  Windows Installer service (msihost) and a restricted/CI session blocks
  that access (LGHT0217) even with the service running. Drop `-sval` on a
  normal desktop.
- candle's `-out` takes a **directory** for multiple sources (it writes one
  `.wixobj` per input), and light needs **both** `.wixobj` files.

**Verified 2026-09-25:** all headless suites pass offscreen
(`--sertest/--frametest/--capstest/--colourtest/--wbtest/--fpstest`),
`gst_encode_test` writes a valid MP4 (90 frames @ 30 fps, `ftyp` present),
and the GUI opens on a real desktop (verified by capturing the live window
surface — `QWidget::grab()` screenshots show tofu boxes on this box, a Qt
offscreen-repaint quirk; the on-screen window renders all text correctly).
The capture helper is `tools/capwin2.exe` (native BitBlt window grabber,
`tools/capwin2.cpp`, compiled with `cl /O2 /EHsc ... gdi32.lib user32.lib`
after vcvars64): on this box managed P/Invoke cannot resolve
`GetDC`/`GetWindowDC` from user32, so the native binary is the working
screenshot path (window class `Qt673QWindowIcon`); `tools/pesubsys.ps1`
checks a PE's subsystem (GUI vs console) and `tools/winctl.ps1` lists an
exe's top-level windows and flags any `ConsoleWindowClass`.
The GUI-subsystem exe (`package_win/` and `build_win/gui/`) opens with **no
console window** (window enumeration: no `ConsoleWindowClass`, Qt main
window 1456×1043) and its live preview renders a real camera image. The MSI
was verified end-to-end: `dark` decompiles all 54 files with the flat
layout; `msiexec /i dist\camera_app-1.1.0-x64.msi /qn` installs to
`%LOCALAPPDATA%\ASI Camera Capture` (exit 0, Add/Remove entry,
GUI-subsystem exe), the installed app shows the live preview and passes
`--capstest`, and `msiexec /x /qn` removes files and product state.
Re-verified the same day after pinning `Product/@Id` (a regenerated code had
orphaned an Add/Remove entry and made a same-version reinstall abort with
1638): fresh install exit 0 → 54 files, exactly one Add/Remove entry;
uninstall exit 0 → zero files, zero entries.
