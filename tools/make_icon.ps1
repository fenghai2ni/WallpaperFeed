# 生成 WallpaperFeed 托盘/应用图标（多尺寸 PNG 压缩 ICO）
Add-Type -AssemblyName System.Drawing

$outPath = 'D:\豆包工作\壁纸\src\res\app.ico'
$outDir = Split-Path $outPath
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$S = 512
$bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.Clear([System.Drawing.Color]::Transparent)

# 圆角矩形底
$radius = 112
$gp = New-Object System.Drawing.Drawing2D.GraphicsPath
$gp.AddArc(0, 0, 2 * $radius, 2 * $radius, 180, 90)
$gp.AddArc($S - 2 * $radius, 0, 2 * $radius, 2 * $radius, 270, 90)
$gp.AddArc($S - 2 * $radius, $S - 2 * $radius, 2 * $radius, 2 * $radius, 0, 90)
$gp.AddArc(0, $S - 2 * $radius, 2 * $radius, 2 * $radius, 90, 90)
$gp.CloseFigure()

$bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    [System.Drawing.Point]::new(0, 0),
    [System.Drawing.Point]::new($S, $S),
    [System.Drawing.Color]::FromArgb(255, 11, 34, 68),
    [System.Drawing.Color]::FromArgb(255, 38, 138, 210))
$g.FillPath($bg, $gp)
$g.SetClip($gp)

# 天光
$glow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(70, 255, 205, 128))
$g.FillEllipse($glow, 236, -20, 320, 320)

# 落日
$sun = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 200, 108))
$g.FillEllipse($sun, 318, 78, 104, 104)

# 远山
$farPts = @(
    [System.Drawing.Point]::new(0, 374),
    [System.Drawing.Point]::new(148, 248),
    [System.Drawing.Point]::new(244, 332),
    [System.Drawing.Point]::new(332, 230),
    [System.Drawing.Point]::new(432, 328),
    [System.Drawing.Point]::new(512, 288),
    [System.Drawing.Point]::new(512, 512),
    [System.Drawing.Point]::new(0, 512)
)
$far = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 15, 46, 88))
$g.FillPolygon($far, $farPts)

# 近山
$nearPts = @(
    [System.Drawing.Point]::new(0, 436),
    [System.Drawing.Point]::new(116, 340),
    [System.Drawing.Point]::new(252, 450),
    [System.Drawing.Point]::new(332, 380),
    [System.Drawing.Point]::new(432, 460),
    [System.Drawing.Point]::new(512, 424),
    [System.Drawing.Point]::new(512, 512),
    [System.Drawing.Point]::new(0, 512)
)
$near = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 219, 238, 255))
$g.FillPolygon($near, $nearPts)

# 底部的水面
$base = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 10, 30, 62))
$g.FillRectangle($base, 0, 470, 512, 42)

$g.ResetClip()

# 描边提升质感
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(60, 255, 255, 255), 5)
$g.DrawPath($pen, $gp)

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$entries = @()
foreach ($sz in $sizes) {
    $thumb = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $tg = [System.Drawing.Graphics]::FromImage($thumb)
    $tg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $tg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $tg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $tg.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $tg.Clear([System.Drawing.Color]::Transparent)
    $tg.DrawImage($bmp, 0, 0, $sz, $sz)
    $tg.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $thumb.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $entries += [pscustomobject]@{ Size = $sz; Data = $ms.ToArray() }
    $ms.Dispose()
    $thumb.Dispose()
}

$fs = [System.IO.File]::Create($outPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$entries.Count)

$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
    $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
    $bw.Write([byte]$dim)
    $bw.Write([byte]$dim)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$e.Data.Length)
    $bw.Write([uint32]$offset)
    $offset += $e.Data.Length
}
foreach ($e in $entries) { $bw.Write([byte[]]$e.Data) }
$bw.Close()

$g.Dispose()
$bmp.Dispose()

Write-Output ("ICO 已生成: {0} ({1} bytes)" -f $outPath, (Get-Item $outPath).Length)
