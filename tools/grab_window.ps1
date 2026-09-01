# Captures the game window to a PNG.
#
# The whole point of the bring-up is to see a frame, and "it did not crash"
# is not the same as "it drew the menu". This grabs the window's client area
# so a run can be checked without sitting in front of it.
param(
  [string]$Process = "warband_nx",
  [string]$Out     = "shot.png"
)

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindowW(string cls, string title);
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")]
  public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
  [DllImport("user32.dll")]
  public static extern bool ClientToScreen(IntPtr hWnd, ref System.Drawing.Point p);
  [DllImport("user32.dll")]
  public static extern bool SetForegroundWindow(IntPtr hWnd);
}
"@ -ReferencedAssemblies System.Drawing

# By process rather than by title: the window stops pumping messages while the
# guest is busy, and a title lookup on a non-responding window is unreliable.
$p = Get-Process -Name $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output "no such process: $Process"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "process has no window yet"; exit 1 }

$r = New-Object Win+RECT
[void][Win]::GetClientRect($h, [ref]$r)
$w = $r.Right - $r.Left
$hgt = $r.Bottom - $r.Top
if ($w -le 0 -or $hgt -le 0) { Write-Output "window has no client area"; exit 1 }

# The client rect is window-relative; the copy needs screen coordinates.
$origin = New-Object System.Drawing.Point 0, 0
[void][Win]::ClientToScreen($h, [ref]$origin)
[void][Win]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 400

$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $Out ($w x $hgt)"
