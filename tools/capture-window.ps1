# Captures one window by process name into a PNG, even when other windows
# cover it (PrintWindow with PW_RENDERFULLCONTENT). Used for the manual's
# screenshots:  powershell -File tools/capture-window.ps1 usdtweak out.png
param([string]$ProcessName = "usdtweak", [string]$OutFile = "window.png")

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class WindowShot {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Save(IntPtr hWnd, string path) {
        SetProcessDPIAware();
        RECT r; GetWindowRect(hWnd, out r);
        using (Bitmap bmp = new Bitmap(r.Right - r.Left, r.Bottom - r.Top)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                PrintWindow(hWnd, hdc, 2);
                g.ReleaseHdc(hdc);
            }
            bmp.Save(path);
        }
    }
}
'@
$p = Get-Process $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
[WindowShot]::Save($p.MainWindowHandle, $OutFile)
Write-Output "saved $OutFile"
