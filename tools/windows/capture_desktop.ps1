$ErrorActionPreference = 'Stop'
for ($attempt = 0; $attempt -lt 60; $attempt++) {
    if (Get-Process explorer -ErrorAction SilentlyContinue) { break }
    Start-Sleep -Seconds 1
}
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { throw 'Explorer did not start' }
Start-Sleep -Seconds 5
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$image = New-Object -TypeName System.Drawing.Bitmap -ArgumentList $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($image)
try {
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $image.Save('C:\PCEMU\DESKTOP.PNG', [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "PCEMU_DESKTOP_CAPTURE_OK user=$env:USERNAME width=$($bounds.Width) height=$($bounds.Height)"
} finally {
    $graphics.Dispose()
    $image.Dispose()
}
