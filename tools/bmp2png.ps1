param([string]$In, [string]$Out)
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile((Resolve-Path $In).Path)
$img.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host ("saved {0}x{1} -> {2}" -f $img.Width, $img.Height, $Out)
$img.Dispose()
