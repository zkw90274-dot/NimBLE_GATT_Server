# Regenerates src\NimBleImuHost\app.ico (teal rounded tile + white board + orange nose, echoing the
# 3D attitude view). The ico is a binary asset, so its source of truth is this script: draw at 256 px
# with System.Drawing, downscale, and pack the PNGs into an ICO container (PNG entries, Vista+).
#
#   powershell -ExecutionPolicy Bypass -File scripts\make-icon.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\make-icon.ps1 -Preview ..\artifacts\ui\icon.png
#
# Keep this file ASCII-only, like capture-ui.ps1 (see docs/host-app.md pitfall 8).
param(
    [string]$Out = '',
    [string]$Preview = ''
)

Add-Type -AssemblyName System.Drawing

if (-not $Out) { $Out = Join-Path $PSScriptRoot '..\src\NimBleImuHost\app.ico' }

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, $r * 2, $r * 2, 180, 90)
    $p.AddArc($x + $w - $r * 2, $y, $r * 2, $r * 2, 270, 90)
    $p.AddArc($x + $w - $r * 2, $y + $h - $r * 2, $r * 2, $r * 2, 0, 90)
    $p.AddArc($x, $y + $h - $r * 2, $r * 2, $r * 2, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $s = $size / 256.0

    $bg = New-RoundedRect ([float](8 * $s)) ([float](8 * $s)) ([float](240 * $s)) ([float](240 * $s)) ([float](56 * $s))
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 16, 165, 148))), $bg)

    # The board + nose echo the 3D attitude view: white plate, orange head, slight bank angle.
    $state = $g.Save()
    $g.TranslateTransform([float](128 * $s), [float](132 * $s))
    $g.RotateTransform(-14)
    $g.TranslateTransform([float](-128 * $s), [float](-132 * $s))
    $board = New-RoundedRect ([float](52 * $s)) ([float](110 * $s)) ([float](152 * $s)) ([float](46 * $s)) ([float](14 * $s))
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)), $board)
    $nose = New-RoundedRect ([float](168 * $s)) ([float](119 * $s)) ([float](28 * $s)) ([float](28 * $s)) ([float](9 * $s))
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 122, 69))), $nose)
    $g.Restore($state)

    $g.Dispose()
    return $bmp
}

function ConvertTo-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return $ms.ToArray()
}

$sizes = @(256, 48, 32, 16)
$pngs = New-Object System.Collections.Generic.List[object]
foreach ($sz in $sizes) {
    if ($sz -eq 256) { $bmp = New-IconBitmap 256 }
    else {
        $big = New-IconBitmap 256
        $bmp = New-Object System.Drawing.Bitmap($sz, $sz)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = 'HighQualityBicubic'
        $g.SmoothingMode = 'AntiAlias'
        $g.DrawImage($big, 0, 0, $sz, $sz)
        $g.Dispose()
        $big.Dispose()
    }
    $pngs.Add([byte[]](ConvertTo-PngBytes $bmp))
    if ($Preview -and $sz -eq 256) { $bmp.Save($Preview, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}
"png sizes: $(($pngs | ForEach-Object { $_.Length }) -join ', ')"

$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
[void]$w.Write([uint16]0)
[void]$w.Write([uint16]1)
[void]$w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    [void]$w.Write([byte]$dim)
    [void]$w.Write([byte]$dim)
    [void]$w.Write([byte]0)
    [void]$w.Write([byte]0)
    [void]$w.Write([uint16]1)
    [void]$w.Write([uint16]32)
    [void]$w.Write([uint32]$pngs[$i].Length)
    [void]$w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
for ($i = 0; $i -lt $sizes.Count; $i++) { [void]$w.Write($pngs[$i]) }
$w.Flush()
[IO.File]::WriteAllBytes([IO.Path]::GetFullPath($Out), $ms.ToArray())
"wrote $Out ($($ms.Length) bytes)"
