# Read the PE subsystem of an exe without needing vcvars/dumpbin:
# 2 = Windows GUI (no console window), 3 = Windows CUI (console).
param([string[]]$Paths)
foreach ($p in $Paths) {
  $fs = [System.IO.File]::OpenRead($p)
  $br = New-Object System.IO.BinaryReader($fs)
  $fs.Position = 0x3C
  $peOff = $br.ReadInt32()
  $fs.Position = $peOff + 24
  $magic = $br.ReadUInt16()          # 0x20B = PE32+ (x64)
  $fs.Position = $peOff + 24 + 68    # Subsystem field in the optional header
  $sub = $br.ReadUInt16()
  $fs.Close()
  $kind = if ($sub -eq 2) { 'GUI  (no cmd window)' } elseif ($sub -eq 3) { 'CONSOLE (cmd window)' } else { "unknown ($sub)" }
  Write-Host ("{0,-12} magic=0x{1:X4}  {2}" -f (Split-Path $p -Leaf), $magic, $kind)
}
