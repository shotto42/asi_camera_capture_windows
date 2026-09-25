# Launch an exe, list its top-level windows by class name, flag whether a
# CONSOLE window (ConsoleWindowClass) exists (it exists iff the exe is a
# console-subsystem binary), and optionally BitBlt-capture the main window
# to a BMP. Uses Add-Type P/Invoke (needs FullLanguage, not Constrained).
param(
  [string]$Exe,
  [string[]]$AppArgs = @(),
  [int]$WaitSec = 10,
  [string]$Shot = ""   # if set: save BMP of the largest non-console window
)
$code = @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class W {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int x1, y1, x2, y2; }
  [StructLayout(LayoutKind.Sequential)]
  public struct Bmi { public int size; public int w; public int h; public short planes; public short bpp; public int comp; public int sizeImage; }
  [DllImport("user32.dll", EntryPoint="GetDCW")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("gdi32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
  [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("gdi32.dll")] [return: MarshalAs(UnmanagedType.Bool)] public static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, uint rop);
  [DllImport("gdi32.dll")] public static extern void DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] public static extern void DeleteDC(IntPtr dc);
}
"@
Add-Type -TypeDefinition $code -ReferencedAssemblies System.Drawing

if ($AppArgs -and $AppArgs.Count -gt 0) {
  $proc = Start-Process -FilePath $Exe -ArgumentList ($AppArgs -join ' ') -PassThru
} else {
  $proc = Start-Process -FilePath $Exe -PassThru
}
Start-Sleep -Seconds $WaitSec
if ($proc.HasExited) { Write-Host "process exited early (code $($proc.ExitCode))"; exit 1 }

$wins = New-Object System.Collections.ArrayList
$cb = [W+EnumWindowsProc] { param($h, $p)
  $pid2 = 0
  [void][W]::GetWindowThreadProcessId($h, [ref]$pid2)
  if ($pid2 -eq $proc.Id) {
    $sb = New-Object Text.StringBuilder 256
    [void][W]::GetClassName($h, $sb, 256)
    $r = New-Object W+RECT
    [void][W]::GetWindowRect($h, [ref]$r)
    [void]$wins.Add([pscustomobject]@{ Hwnd = $h; Class = $sb.ToString(); W = ($r.x2 - $r.x1); H = ($r.y2 - $r.y1) })
  }
  return $true
}
[void][W]::EnumWindows($cb, [IntPtr]::Zero)

Write-Host ("windows of {0} (pid {1}):" -f (Split-Path $Exe -Leaf), $proc.Id)
$console = $false
foreach ($w in $wins) {
  $tag = if ($w.Class -eq 'ConsoleWindowClass') { ' <-- CONSOLE WINDOW' } elseif ($w.Class -like 'Qt*') { ' (Qt main)' } else { '' }
  Write-Host ("  {0,-24} {1,5}x{2,-5}{3}" -f $w.Class, $w.W, $w.H, $tag)
  if ($w.Class -eq 'ConsoleWindowClass') { $console = $true }
}

if ($Shot -ne "") {
  $main = $wins | Where-Object { $_.Class -ne 'ConsoleWindowClass' } |
          Sort-Object { $_.W * $_.H } -Descending | Select-Object -First 1
  if ($main) {
    $src = [W]::GetDC($main.Hwnd)
    $dst = [W]::CreateCompatibleDC($src)
    $bmp = [W]::CreateCompatibleBitmap($src, $main.W, $main.H)
    [void][W]::SelectObject($dst, $bmp)
    [void][W]::BitBlt($dst, 0, 0, $main.W, $main.H, $src, 0, 0, 0x00CC0020)  # SRCCOPY
    $img = [System.Drawing.Bitmap]::FromHbitmap($bmp)
    $img.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $img.Dispose()
    [W]::DeleteObject($bmp); [W]::DeleteDC($dst); [W]::ReleaseDC($main.Hwnd, $src)
    Write-Host "captured main window -> $Shot"
  }
}

Stop-Process -Id $proc.Id -Force
if ($console) { Write-Host "RESULT: CONSOLE window present (console-subsystem exe)"; exit 1 }
Write-Host "RESULT: no console window (GUI-subsystem exe)"
exit 0
