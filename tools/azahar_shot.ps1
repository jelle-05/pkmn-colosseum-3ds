# azahar_shot.ps1 - run the port in Azahar and save screenshots of its window.
#
#   powershell -File tools/azahar_shot.ps1 [-Rom build/port/colosseum.3dsx]
#       [-At 10,20,30] [-Out build/shots]
#
# Takes a screenshot of the Azahar window at each of the given seconds
# after start, then closes Azahar. For checking the renderer without
# watching the screen.
param(
    [string]$Rom = "build/port/colosseum.3dsx",
    [int[]]$At = @(20),
    [string]$Out = "build/shots",
    [string]$Azahar = "C:\Program Files\Azahar\azahar.exe"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[Win]::SetProcessDPIAware() | Out-Null

$Out = [IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Out))   # .NET needs full paths
New-Item -ItemType Directory -Force $Out | Out-Null
$romPath = (Resolve-Path $Rom).Path
$p = Start-Process $Azahar -ArgumentList "`"$romPath`"" -PassThru
try {
    $start = Get-Date
    foreach ($t in ($At | Sort-Object)) {
        $wait = $t - ((Get-Date) - $start).TotalSeconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
        $p.Refresh()
        $h = $p.MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { Write-Output "no window at ${t}s"; continue }
        [Win]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 200
        $r = New-Object Win+RECT
        [Win]::GetWindowRect($h, [ref]$r) | Out-Null
        $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
        $bmp = New-Object System.Drawing.Bitmap $w, $hh
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
        $file = Join-Path $Out ("shot_{0:D3}s.png" -f $t)
        $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Output "saved $file (${w}x${hh})"
    }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
