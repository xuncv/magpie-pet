# 连续截图：把鼠标移到指定位置，等一会，再抓窗口
# usage: shot-seq.ps1 -Steps "1505,790|ui-a|900","1266,773|ui-b|500"
param([string]$OutDir = ".", [string]$Steps = "", [string]$ProcName = "pet", [double]$Zoom = 1.5)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class Seq {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$target = Get-Process $ProcName -ErrorAction SilentlyContinue
if (-not $target) { throw "$ProcName not running" }
$pid2 = $target.Id
$script:rr = $null

function Grab([string]$name) {
  $script:rr = $null
  $cb = [Seq+EnumProc]{ param($h, $l)
    $p = 0; [void][Seq]::GetWindowThreadProcessId($h, [ref]$p)
    if ($p -eq $pid2 -and [Seq]::IsWindowVisible($h)) {
      $r = New-Object Seq+RECT; [void][Seq]::GetWindowRect($h, [ref]$r)
      if (($r.Right - $r.Left) -gt 50 -and ($r.Bottom - $r.Top) -gt 50) { $script:rr = $r }
    }
    return $true }
  [void][Seq]::EnumWindows($cb, [IntPtr]::Zero)
  if (-not $script:rr) { Write-Host "no window"; return }
  $r = $script:rr
  $w = [int]($r.Right - $r.Left); $h = [int]($r.Bottom - $r.Top)
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $big = New-Object System.Drawing.Bitmap -ArgumentList $bmp, ([int]($w * $Zoom)), ([int]($h * $Zoom))
  $big.Save((Join-Path $OutDir "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose(); $big.Dispose()
  Write-Host ("{0} -> rect=({1},{2}) {3}x{4}" -f $name, $r.Left, $r.Top, $w, $h)
}

foreach ($s in $Steps.Split(";")) {
  $parts = $s.Split("|")
  if ($parts.Count -lt 3) { continue }
  $xy = $parts[0].Split(",")
  [void][Seq]::SetCursorPos([int]$xy[0], [int]$xy[1])
  Start-Sleep -Milliseconds ([int]$parts[2])
  Grab $parts[1]
}
