Add-Type -AssemblyName System.Drawing

$out = Join-Path (Split-Path -Parent $PSScriptRoot) "src\icon.ico"
$sizes = @(16, 20, 24, 32, 40, 48, 64)
$images = @()

foreach ($n in $sizes) {
  $ss = 8
  $S = $n * $ss
  $bmp = New-Object System.Drawing.Bitmap $S, $S
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  # A pulse trace: flat, spike up, dip down, flat.
  $points = @(
    [System.Drawing.PointF]::new($S * 0.08, $S * 0.55),
    [System.Drawing.PointF]::new($S * 0.30, $S * 0.55),
    [System.Drawing.PointF]::new($S * 0.39, $S * 0.25),
    [System.Drawing.PointF]::new($S * 0.54, $S * 0.84),
    [System.Drawing.PointF]::new($S * 0.63, $S * 0.46),
    [System.Drawing.PointF]::new($S * 0.72, $S * 0.55),
    [System.Drawing.PointF]::new($S * 0.92, $S * 0.55)
  )
  $col = [System.Drawing.Color]::FromArgb(255, 43, 143, 230)
  $pen = New-Object System.Drawing.Pen $col, ($S * 0.115)
  $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
  $g.DrawLines($pen, $points)
  $g.Dispose(); $pen.Dispose()

  $small = New-Object System.Drawing.Bitmap $n, $n
  $g2 = [System.Drawing.Graphics]::FromImage($small)
  $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g2.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g2.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g2.Clear([System.Drawing.Color]::Transparent)
  $rect = New-Object System.Drawing.Rectangle 0, 0, $n, $n
  $g2.DrawImage($bmp, $rect)
  $g2.Dispose(); $bmp.Dispose()
  $images += $small
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms

$entries = @()
foreach ($img in $images) {
  $n = $img.Width
  $data = New-Object System.IO.MemoryStream
  $dw = New-Object System.IO.BinaryWriter $data
  $dw.Write([UInt32]40)
  $dw.Write([Int32]$n)
  $dw.Write([Int32]($n * 2))
  $dw.Write([UInt16]1)
  $dw.Write([UInt16]32)
  $dw.Write([UInt32]0)
  $dw.Write([UInt32]($n * $n * 4))
  $dw.Write([Int32]0); $dw.Write([Int32]0)
  $dw.Write([UInt32]0); $dw.Write([UInt32]0)

  for ($y = $n - 1; $y -ge 0; $y--) {
    for ($x = 0; $x -lt $n; $x++) {
      $c = $img.GetPixel($x, $y)
      $dw.Write([byte]$c.B); $dw.Write([byte]$c.G); $dw.Write([byte]$c.R); $dw.Write([byte]$c.A)
    }
  }
  $rowBytes = [Math]::Floor(($n + 31) / 32) * 4
  $dw.Write((New-Object byte[] ($rowBytes * $n)))
  $dw.Flush()
  $entries += [PSCustomObject]@{ Size = $n; Bytes = $data.ToArray() }
}

$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$entries.Count)
$offset = 6 + 16 * $entries.Count
foreach ($e in $entries) {
  $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
  $bw.Write([byte]$dim); $bw.Write([byte]$dim)
  $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([UInt16]1); $bw.Write([UInt16]32)
  $bw.Write([UInt32]$e.Bytes.Length)
  $bw.Write([UInt32]$offset)
  $offset += $e.Bytes.Length
}
foreach ($e in $entries) { $bw.Write($e.Bytes) }
$bw.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
$bw.Dispose(); $ms.Dispose()
foreach ($img in $images) { $img.Dispose() }
"wrote $out ($((Get-Item $out).Length) bytes)"
