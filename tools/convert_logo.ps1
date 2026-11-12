Add-Type -AssemblyName System.Drawing

$inputPath = "C:/Users/azese/.gemini/antigravity/brain/8fe5eae0-b57b-46d2-8ece-fe50d8c6a425/.user_uploaded/media_1794338317768_eba0a14f.jpg"
$repoRoot = (Get-Item $PSScriptRoot).Parent.FullName

Write-Host "Repo root: $repoRoot"
$sourceImg = [System.Drawing.Bitmap]::FromFile($inputPath)

# Function to resize bitmap with high quality bicubic
function Resize-Bitmap($src, $w, $h) {
    $dest = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($dest)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($src, 0, 0, $w, $h)
    $g.Dispose()
    return $dest
}

# 1. Save 256x256 PNG for default theme icon & Eglantine.png
$bmp256 = Resize-Bitmap $sourceImg 256 256
$themeIconPath = Join-Path $repoRoot "dist\qt_themes\default\icons\256x256\eden.png"
$bmp256.Save($themeIconPath, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "Saved: $themeIconPath"

$eglantinePng = Join-Path $repoRoot "dist\Eglantine.png"
$bmp256.Save($eglantinePng, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "Saved: $eglantinePng"

$daliaPng = Join-Path $repoRoot "dist\dalia.png"
$bmp256.Save($daliaPng, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "Saved: $daliaPng"

# 2. Save 256x256 BMP for yuzu_cmd
$bmpFile = Join-Path $repoRoot "dist\eden.bmp"
$bmp256.Save($bmpFile, [System.Drawing.Imaging.ImageFormat]::Bmp)
Write-Host "Saved: $bmpFile"

# 3. Save profile jpg
$profileJpg = Join-Path $repoRoot "dist\eden_profile.jpg"
$bmp256.Save($profileJpg, [System.Drawing.Imaging.ImageFormat]::Jpeg)
Write-Host "Saved: $profileJpg"

# Function to extract DIB (device-independent bitmap) data for standard Windows ICO frames
function Get-DibBytes($resizedBmp) {
    $w = $resizedBmp.Width
    $h = $resizedBmp.Height
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    # BITMAPINFOHEADER (40 bytes)
    $bw.Write([uint32]40)          # biSize
    $bw.Write([int32]$w)           # biWidth
    $bw.Write([int32]($h * 2))     # biHeight (doubled: XOR mask + AND mask)
    $bw.Write([uint16]1)           # biPlanes
    $bw.Write([uint16]32)          # biBitCount (32-bit BGRA)
    $bw.Write([uint32]0)           # biCompression (BI_RGB)
    $bw.Write([uint32]($w * $h * 4)) # biSizeImage
    $bw.Write([int32]0)            # biXPelsPerMeter
    $bw.Write([int32]0)            # biYPelsPerMeter
    $bw.Write([uint32]0)           # biClrUsed
    $bw.Write([uint32]0)           # biClrImportant

    # Pixel data (bottom-up, BGRA)
    for ($y = $h - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $w; $x++) {
            $pixel = $resizedBmp.GetPixel($x, $y)
            $bw.Write([byte]$pixel.B)
            $bw.Write([byte]$pixel.G)
            $bw.Write([byte]$pixel.R)
            $bw.Write([byte]$pixel.A)
        }
    }

    # 1-bit AND mask (bottom-up, padded to 32-bit/4-byte boundary)
    $rowBytes = [Math]::Ceiling($w / 32.0) * 4
    $andRow = New-Object byte[] $rowBytes
    for ($y = 0; $y -lt $h; $y++) {
        $bw.Write($andRow)
    }

    $bw.Flush()
    $bytes = $ms.ToArray()
    $bw.Close()
    $ms.Dispose()
    return $bytes
}

# 4. Generate multi-resolution .ico containing 16, 24, 32, 48, 64, 128 (DIB) and 256 (PNG) frames
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$iconFrames = @()

foreach ($sz in $sizes) {
    $resized = Resize-Bitmap $sourceImg $sz $sz
    if ($sz -lt 256) {
        # Standard Win32 DIB format for explorer and shell compatibility
        $frameBytes = Get-DibBytes $resized
    } else {
        # PNG format for 256x256 high-dpi frame
        $ms = New-Object System.IO.MemoryStream
        $resized.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $frameBytes = $ms.ToArray()
        $ms.Dispose()
    }
    $resized.Dispose()
    
    $iconFrames += [PSCustomObject]@{
        Width = $sz
        Height = $sz
        Bytes = $frameBytes
    }
}

$icoPath = Join-Path $repoRoot "dist\eden.ico"
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)

# ICONDIR header
$bw.Write([uint16]0) # Reserved
$bw.Write([uint16]1) # Type 1 = ICO
$bw.Write([uint16]$iconFrames.Count) # Image count

$offset = 6 + ($iconFrames.Count * 16)

# Write ICONDIRENTRY for each
foreach ($frame in $iconFrames) {
    $wByte = if ($frame.Width -ge 256) { [byte]0 } else { [byte]$frame.Width }
    $hByte = if ($frame.Height -ge 256) { [byte]0 } else { [byte]$frame.Height }
    $bw.Write($wByte)       # Width
    $bw.Write($hByte)       # Height
    $bw.Write([byte]0)      # Colors
    $bw.Write([byte]0)      # Reserved
    $bw.Write([uint16]1)    # Color planes
    $bw.Write([uint16]32)   # Bits per pixel
    $bw.Write([uint32]$frame.Bytes.Length) # Size in bytes
    $bw.Write([uint32]$offset)            # Image offset
    $offset += $frame.Bytes.Length
}

# Write PNG data for each
foreach ($frame in $iconFrames) {
    $bw.Write($frame.Bytes)
}

$bw.Flush()
$bw.Close()
$fs.Close()

Write-Host "Saved multi-resolution ICO: $icoPath"

# Also save dist/dalia.ico
Copy-Item $icoPath (Join-Path $repoRoot "dist\dalia.ico") -Force
Write-Host "Saved dist\dalia.ico"

$bmp256.Dispose()
$sourceImg.Dispose()
Write-Host "Icon generation completed successfully!"

