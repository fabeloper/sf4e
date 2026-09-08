# Draws the SF4Enhanced application icon: an ink-black roundel with a red
# band and a bold "IV", in the lobby's own style, and writes it as a
# PNG-compressed multi-size .ico. Original artwork; replace
# src/launcher/app.ico with any icon you prefer.
Add-Type -AssemblyName System.Drawing
$repo = Split-Path -Parent $PSScriptRoot

function DrawEmblem([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList @($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)
    [float]$s = $size
    [float]$r = $s * 0.18

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0.0, 0.0, ($r * 2), ($r * 2), 180.0, 90.0)
    $path.AddArc(($s - $r * 2), 0.0, ($r * 2), ($r * 2), 270.0, 90.0)
    $path.AddArc(($s - $r * 2), ($s - $r * 2), ($r * 2), ($r * 2), 0.0, 90.0)
    $path.AddArc(0.0, ($s - $r * 2), ($r * 2), ($r * 2), 90.0, 90.0)
    $path.CloseFigure()
    $ink = New-Object System.Drawing.SolidBrush -ArgumentList @([System.Drawing.Color]::FromArgb(255, 10, 10, 13))
    $g.FillPath($ink, $path)
    $g.SetClip($path)

    # Red diagonal band.
    $red = New-Object System.Drawing.SolidBrush -ArgumentList @([System.Drawing.Color]::FromArgb(255, 200, 16, 46))
    $p1 = New-Object System.Drawing.PointF -ArgumentList @([float](-$s * 0.1), [float]($s * 0.80))
    $p2 = New-Object System.Drawing.PointF -ArgumentList @([float]($s * 1.1), [float]($s * 0.20))
    $p3 = New-Object System.Drawing.PointF -ArgumentList @([float]($s * 1.1), [float]($s * 0.56))
    $p4 = New-Object System.Drawing.PointF -ArgumentList @([float](-$s * 0.1), [float]($s * 1.16))
    [System.Drawing.PointF[]]$pts = @($p1, $p2, $p3, $p4)
    $g.FillPolygon($red, $pts)

    # An ink stroke across it.
    $pen = New-Object System.Drawing.Pen -ArgumentList @([System.Drawing.Color]::FromArgb(230, 0, 0, 0), [float]($s * 0.06))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawLine($pen, [float]($s * 0.04), [float]($s * 0.64), [float]($s * 0.96), [float]($s * 0.38))

    # The mark.
    [float]$fontSize = $s * 0.60
    $family = New-Object System.Drawing.FontFamily -ArgumentList @('Impact')
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $rect = New-Object System.Drawing.RectangleF -ArgumentList @([float]0, [float](-$s * 0.05), [float]$s, [float]$s)
    $tp = New-Object System.Drawing.Drawing2D.GraphicsPath
    $tp.AddString('IV', $family, [int][System.Drawing.FontStyle]::Regular, $fontSize, $rect, $sf)
    $outline = New-Object System.Drawing.Pen -ArgumentList @([System.Drawing.Color]::Black, [float]($s * 0.05))
    $outline.LineJoin = 'Round'
    $g.DrawPath($outline, $tp)
    $g.FillPath([System.Drawing.Brushes]::White, $tp)

    $gold = New-Object System.Drawing.SolidBrush -ArgumentList @([System.Drawing.Color]::FromArgb(255, 242, 193, 78))
    $g.FillRectangle($gold, [float]($s * 0.30), [float]($s * 0.80), [float]($s * 0.40), [float]($s * 0.05))

    $g.ResetClip()
    $g.Dispose()
    return $bmp
}

$sizes = @(256, 64, 48, 32, 16)
$entries = @()
foreach ($sz in $sizes) {
    $b = DrawEmblem $sz
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += ,@{ size = $sz; bytes = $ms.ToArray() }
    if ($sz -eq 256) { $b.Save("$repo\images\app-icon-preview.png", [System.Drawing.Imaging.ImageFormat]::Png) }
    $b.Dispose()
}

$ico = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter -ArgumentList @($ico)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.size -ge 256) { 0 } else { $e.size }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$e.bytes.Length); $w.Write([uint32]$offset)
    $offset += $e.bytes.Length
}
foreach ($e in $entries) { $w.Write($e.bytes) }
$w.Flush()
[IO.File]::WriteAllBytes("$repo\src\launcher\app.ico", $ico.ToArray())
Write-Host "wrote $repo\src\launcher\app.ico ($($ico.Length) bytes) and images\app-icon-preview.png"
