param(
  [ValidateRange(256, 8192)]
  [int]$Size = 2048,
  [string]$OutputPath = (Join-Path $PSScriptRoot "..\assets\tokmon-brand-2048.png")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$absoluteOutput = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory(
  [System.IO.Path]::GetDirectoryName($absoluteOutput)) | Out-Null

$bitmap = [System.Drawing.Bitmap]::new(
  $Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$pen = $null
$brush = $null
try {
  $graphics.Clear([System.Drawing.Color]::Transparent)
  $graphics.SmoothingMode =
    [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $graphics.PixelOffsetMode =
    [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $graphics.CompositingQuality =
    [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

  $scale = $Size / 32.0
  $ink = [System.Drawing.Color]::FromArgb(255, 92, 95, 102)
  $pen = [System.Drawing.Pen]::new($ink, [single](1.8 * $scale))
  $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $brush = [System.Drawing.SolidBrush]::new($ink)

  # This is the same 32-unit vector geometry used by Tokmon's native icon.
  $graphics.DrawLine($pen, [single](11 * $scale), [single](10.5 * $scale),
    [single](11 * $scale), [single](21.5 * $scale))
  $graphics.DrawLine($pen, [single](13.5 * $scale), [single](16 * $scale),
    [single](17.5 * $scale), [single](16 * $scale))
  foreach ($node in @(@(11, 8), @(20, 16), @(11, 24))) {
    $radius = 2.5 * $scale
    $graphics.FillEllipse($brush,
      [single]($node[0] * $scale - $radius),
      [single]($node[1] * $scale - $radius),
      [single](2 * $radius), [single](2 * $radius))
  }

  $bitmap.SetResolution(300, 300)
  $bitmap.Save($absoluteOutput, [System.Drawing.Imaging.ImageFormat]::Png)
  Write-Output $absoluteOutput
}
finally {
  if ($brush) { $brush.Dispose() }
  if ($pen) { $pen.Dispose() }
  $graphics.Dispose()
  $bitmap.Dispose()
}
