param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing
$source=[Drawing.Image]::FromFile((Join-Path $root 'icon/icon.png'))
try {
    $frames=@()
    foreach($size in @(16,24,32,48,64,128,256)) {
        $bitmap=New-Object Drawing.Bitmap($size,$size)
        $g=[Drawing.Graphics]::FromImage($bitmap)
        try {
            $g.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.DrawImage($source,0,0,$size,$size)
            $stream=New-Object IO.MemoryStream
            $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
            $frames+=,@{Size=$size;Bytes=$stream.ToArray()}
            $stream.Dispose()
            if($size -eq 64) { $bitmap.Save((Join-Path $root 'src/ui/web/app-icon.png'),[Drawing.Imaging.ImageFormat]::Png) }
            if($size -eq 128) { $bitmap.Save((Join-Path $root 'src/ui/web/app-icon@2x.png'),[Drawing.Imaging.ImageFormat]::Png) }
        } finally { $g.Dispose(); $bitmap.Dispose() }
    }
    $ico=[IO.File]::Create((Join-Path $root 'src/ui/app.ico'))
    $writer=New-Object IO.BinaryWriter($ico)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
        $offset=6+16*$frames.Count
        foreach($frame in $frames) {
            $s=if($frame.Size -eq 256){0}else{$frame.Size}
            $writer.Write([byte]$s); $writer.Write([byte]$s); $writer.Write([byte]0); $writer.Write([byte]0)
            $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
            $offset+=$frame.Bytes.Length
        }
        foreach($frame in $frames){$writer.Write([byte[]]$frame.Bytes)}
    } finally { $writer.Dispose(); $ico.Dispose() }
} finally { $source.Dispose() }
Write-Output 'PASS regenerated executable and web icons from icon/icon.png'
