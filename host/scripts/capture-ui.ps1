# Launches the WPF host, drives it through UI Automation, and captures window screenshots
# so UI acceptance can be reviewed without a human at the keyboard.
#
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -Seconds 8 -ThenButtonName stop
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -SimRate 500 -Tag rate500 -Seconds 5
#
# Keep this file ASCII-only. Windows PowerShell 5.1 decodes a BOM-less UTF-8 .ps1 as ANSI,
# and the resulting mojibake can swallow a newline and eat the following line.
param(
    [string]$ButtonName = 'start',
    [string]$ThenButtonName = '',
    [int]$Seconds = 3,
    [string]$Configuration = 'Release',
    [string]$SimRate = '',
    [string]$Tag = 'sim',
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Button captions are Chinese, so build them from code points.
function Resolve-Label([string]$name) {
    switch ($name) {
        'start' { [string]([char]0x5F00) + [char]0x59CB }
        'stop'  { [string]([char]0x505C) + [char]0x6B62 }
        default { $name }
    }
}
$ButtonName = Resolve-Label $ButtonName
$ThenButtonName = Resolve-Label $ThenButtonName

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

$exe = Join-Path $PSScriptRoot "..\src\NimBleImuHost\bin\$Configuration\net8.0-windows10.0.19041.0\NimBleImuHost.exe"
if (-not (Test-Path $exe)) { throw "exe not found: $exe (build first)" }

$outDir = Join-Path $PSScriptRoot '..\artifacts\ui'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Get-Window([System.Diagnostics.Process]$proc) {
    for ($i = 0; $i -lt 40; $i++) {
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) {
            $hwnd = $proc.MainWindowHandle
            $null = [Win32]::ShowWindow($hwnd, 9)   # SW_RESTORE
            $null = [Win32]::SetForegroundWindow($hwnd)
            Start-Sleep -Milliseconds 400
            return [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        }
        Start-Sleep -Milliseconds 500
    }
    throw 'main window did not appear'
}

function Save-Shot([System.Windows.Automation.AutomationElement]$win, [string]$name) {
    $path = Join-Path $outDir "$Tag-$name"
    $rect = New-Object Win32+RECT
    [void][Win32]::GetWindowRect([IntPtr]$win.Current.NativeWindowHandle, [ref]$rect)
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { Write-Warning "bad window rect -> skip $name"; return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    "shot -> $path ($w x $h)"
}

# Returns the readout line and echoes it; Write-Host output is never swallowed by an assignment.
function Show-Text([System.Windows.Automation.AutomationElement]$win, [string]$tag) {
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                        [System.Windows.Automation.Condition]::TrueCondition)
    $vals = foreach ($e in $all) {
        $ct = $e.Current.ControlType.ProgrammaticName -replace 'ControlType\.', ''
        $n = $e.Current.Name
        if ($ct -eq 'Text' -and $n -and $n.Length -le 40) { $n }
    }
    $line = ($vals -join ' | ')
    Write-Host "--- $tag ---"
    Write-Host $line
    return $line
}

function Find-First([System.Windows.Automation.AutomationElement]$root, $property, $value) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($property, $value)
    return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

function Click-Button([System.Windows.Automation.AutomationElement]$win, [string]$name) {
    if (-not $name) { return }
    $btn = Find-First $win ([System.Windows.Automation.AutomationElement]::NameProperty) $name
    if (-not $btn) { Write-Warning "button '$name' not found"; return }
    $inv = $btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    $inv.Invoke()
    "clicked: $name"
}

# The sim-rate ComboBox is the load-test knob: picking 500 there proves both the selector and the
# pipeline, which a command-line switch would not. A closed WPF ComboBox changes selection on Down/Up,
# which is far more reliable through UIA than reaching into the popup's list items.
function Set-SimRate([System.Windows.Automation.AutomationElement]$win, [string]$rate) {
    if (-not $rate) { return }

    $options = @('20', '100', '250', '500')
    $target = [Array]::IndexOf($options, $rate)
    if ($target -lt 0) { throw "rate '$rate' not one of $($options -join '/')" }

    $combo = Find-First $win ([System.Windows.Automation.AutomationElement]::ControlTypeProperty) `
        ([System.Windows.Automation.ControlType]::ComboBox)
    if (-not $combo) { throw 'sim rate ComboBox not found' }

    # The control starts at the first option, so the delta is just the target index.
    $null = $combo.SetFocus()
    Start-Sleep -Milliseconds 200
    for ($i = 0; $i -lt $target; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait('{DOWN}')
        Start-Sleep -Milliseconds 120
    }
    Start-Sleep -Milliseconds 200

    # WPF exposes no selected-item Name here; the running status line ("... 100 Hz") is the proof.
    "sent $target Down presses to the sim rate combo"
}

$p = Start-Process $exe -PassThru
try {
    $win = Get-Window $p
    Set-SimRate $win $SimRate
    Save-Shot $win '00-initial.png'

    Click-Button $win $ButtonName
    Start-Sleep -Seconds $Seconds
    Show-Text $win "t=+$Seconds s" | Out-Null
    Save-Shot $win '01-running.png'

    Start-Sleep -Seconds $Seconds
    Show-Text $win "t=+$((2 * $Seconds)) s" | Out-Null
    Save-Shot $win '02-running.png'

    # Second click (e.g. stop): the readouts must then stop changing.
    if ($ThenButtonName) {
        Click-Button $win $ThenButtonName
        Start-Sleep -Seconds 2
        $a = Show-Text $win "after '$ThenButtonName' +2 s"
        Start-Sleep -Seconds 2
        $b = Show-Text $win "after '$ThenButtonName' +4 s"
        if ($a -eq $b) { "FROZEN after '$ThenButtonName' (readouts identical across 2 s)" }
        else { "STILL CHANGING after '$ThenButtonName'" }
        Save-Shot $win '03-after-second-click.png'

        Click-Button $win $ButtonName
        Start-Sleep -Seconds 2
        Show-Text $win 'restarted' | Out-Null
    }
}
finally {
    if ($KeepRunning) { "process left running (pid $($p.Id))" }
    elseif (-not $p.HasExited) { $p.Kill(); "process stopped" }
}
