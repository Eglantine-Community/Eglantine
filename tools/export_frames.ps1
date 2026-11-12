Add-Type -AssemblyName System.Drawing

$repoRoot = (Get-Item $PSScriptRoot).Parent.FullName
$srcPath = Join-Path $repoRoot "dist\dalia.png"

$src = [System.Drawing.Bitmap]::FromFile($srcPath)

function Resize-Image($bmp, $w, $h, $outPath) {
    $dest = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($dest)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($bmp, 0, 0, $w, $h)
    $dest.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose()
    $dest.Dispose()
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
foreach ($s in $sizes) {
    $outPath = Join-Path $repoRoot "dist\icon_$s.png"
    Resize-Image $src $s $s $outPath
    Write-Host "Exported icon_$s.png"
}

$src.Dispose()

