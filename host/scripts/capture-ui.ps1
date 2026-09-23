# Launches the WPF host, drives it through UI Automation, and captures window screenshots
# so UI acceptance can be reviewed without a human at the keyboard.
#
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -Seconds 8 -ThenButtonName stop
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -SimRate 500 -Tag rate500 -Seconds 5
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -Mode ble -Tag ble -Seconds 6
#   powershell -ExecutionPolicy Bypass -File scripts\capture-ui.ps1 -ExePath ..\publish\NimBleImuHost.exe -Tag publish
#
# -Mode sim drives the built-in generator; -Mode ble selects the real device, scans, picks it out
# of the list, connects and waits for the first notifications.
#
# Keep this file ASCII-only. Windows PowerShell 5.1 decodes a BOM-less UTF-8 .ps1 as ANSI,
# and the resulting mojibake can swallow a newline and eat the following line.
param(
    [ValidateSet('sim', 'ble')][string]$Mode = 'sim',
    [string]$ButtonName = '',
    [string]$ThenButtonName = '',
    [int]$Seconds = 3,
    [int]$Shots = 2,
    [string]$Configuration = 'Release',
    [string]$SimRate = '',
    [int]$ScanSeconds = 8,
    [int]$ConnectTimeout = 30,
    [string]$Tag = 'sim',
    [string]$ExePath = '',
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# Captions are Chinese, so build them from code points.
function Resolve-Label([string]$name) {
    switch ($name) {
        'start'    { [string]([char]0x5F00) + [char]0x59CB }                       # kai shi
        'stop'     { [string]([char]0x505C) + [char]0x6B62 }                       # ting zhi
        'ble'      { [string]([char]0x771F) + [char]0x5B9E + ' BLE' }              # zhen shi
        'scan'     { [string]([char]0x626B) + [char]0x63CF + [char]0x8BBE + [char]0x5907 }
        'connect'  { [string]([char]0x8FDE) + [char]0x63A5 + [char]0x6240 + [char]0x9009 + [char]0x8BBE + [char]0x5907 }
        'connected' { [string]([char]0x5DF2) + [char]0x8FDE + [char]0x63A5 }       # yi lian jie
        'streaming' { [string]([char]0x6570) + [char]0x636E + [char]0x6D41 + [char]0x8FD0 + [char]0x884C + [char]0x4E2D }
        'found'    { [string]([char]0x53D1) + [char]0x73B0 }                       # fa xian
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

# Point -ExePath at publish\NimBleImuHost.exe to acceptance-test the single-file artifact
# instead of the build output; the harness and the checks are otherwise identical.
# A relative -ExePath is resolved against this script's folder, not the caller's directory.
$exe = if ($ExePath) { Join-Path $PSScriptRoot $ExePath } else {
    Join-Path $PSScriptRoot "..\src\NimBleImuHost\bin\$Configuration\net8.0-windows10.0.19041.0\NimBleImuHost.exe"
}
if (-not (Test-Path $exe)) { throw "exe not found: $exe (build first)" }

$outDir = Join-Path $PSScriptRoot '..\artifacts\ui'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if ($Mode -eq 'sim' -and -not $ButtonName) { $ButtonName = Resolve-Label 'start' }

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

# The readout strip: every short Text element in the window, in UIA order.
function Get-Texts([System.Windows.Automation.AutomationElement]$win) {
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                        [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) {
        $ct = $e.Current.ControlType.ProgrammaticName -replace 'ControlType\.', ''
        $n = $e.Current.Name
        if ($ct -eq 'Text' -and $n -and $n.Length -le 40) { $n }
    }
}

# Returns the readout line and echoes it; Write-Host output is never swallowed by an assignment.
function Show-Text([System.Windows.Automation.AutomationElement]$win, [string]$tag) {
    $line = ((Get-Texts $win) -join ' | ')
    Write-Host "--- $tag ---"
    Write-Host $line
    return $line
}

function Wait-ForText([System.Windows.Automation.AutomationElement]$win, [string[]]$patterns, [int]$timeoutSec, [string]$what) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $line = ((Get-Texts $win) -join ' | ')
        # Several patterns because the connect message is replaced by the first-sample message
        # within milliseconds; polling at 500 ms cannot reliably catch the intermediate one.
        foreach ($pattern in $patterns) {
            if ($line -like "*$pattern*") { "$what : matched '$pattern'"; return $line }
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Host "--- $what timed out (no match for $($patterns -join ', ') in ${timeoutSec}s) ---"
    Write-Host ((Get-Texts $win) -join ' | ')
    throw "$what timed out waiting for $($patterns -join ', ')"
}

function Find-First([System.Windows.Automation.AutomationElement]$root, $property, $value) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($property, $value)
    return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

# WPF hands different controls different patterns: Button is Invoke, RadioButton is SelectionItem
# (its peer also offers Invoke/Toggle, but only after the group is realised), ListBoxItem is
# SelectionItem. Take whichever one the element actually supports.
function Invoke-Element([System.Windows.Automation.AutomationElement]$el) {
    $pat = $null
    try { $pat = $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern) } catch { $pat = $null }
    if ($pat) { $pat.Invoke(); return 'InvokePattern' }

    try { $pat = $el.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern) } catch { $pat = $null }
    if ($pat) { $pat.Select(); return 'SelectionItemPattern' }

    try { $pat = $el.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern) } catch { $pat = $null }
    if ($pat) { $pat.Toggle(); return 'TogglePattern' }

    return $null
}

function Click-Button([System.Windows.Automation.AutomationElement]$win, [string]$name) {
    if (-not $name) { return }
    $btn = Find-First $win ([System.Windows.Automation.AutomationElement]::NameProperty) $name
    if (-not $btn) { Write-Warning "button '$name' not found"; return }
    $how = Invoke-Element $btn
    if (-not $how) { throw "no usable UIA pattern on '$name'" }
    "clicked: $name ($how)"
}

function Select-Device([System.Windows.Automation.AutomationElement]$win, [string]$needle) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::ListItem)
    $items = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
    # WPF reports a data-templated ListBoxItem's UIA Name as the item's ToString(), not as the
    # concatenated TextBlocks, so match on a substring rather than a prefix.
    foreach ($it in $items) {
        if ($it.Current.Name -like "*$needle*") {
            $how = Invoke-Element $it
            if (-not $how) { throw "ListItem '$($it.Current.Name)' has no SelectionItem pattern" }
            "picked device: $($it.Current.Name)"
            return
        }
    }
    $seen = foreach ($it in $items) { $it.Current.Name }
    throw "no ListItem containing '$needle'; list contains: [$($seen -join ', ')]"
}

# After a successful BLE connect the source is already Streaming, so the run button reads
# "stop" and clicking it would disconnect. Only press start when it is actually offered.
function Ensure-Running([System.Windows.Automation.AutomationElement]$win) {
    $start = Resolve-Label 'start'
    $btn = Find-First $win ([System.Windows.Automation.AutomationElement]::NameProperty) $start
    if ($btn) { Click-Button $win $start; return }
    "run button already reads stop - stream is running, not pressing it"
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
    Save-Shot $win '00-initial.png'

    if ($Mode -eq 'ble') {
        $ble = Resolve-Label 'ble'
        $radio = Find-First $win ([System.Windows.Automation.AutomationElement]::NameProperty) $ble
        if (-not $radio) { throw "radio button '$ble' not found" }
        $how = Invoke-Element $radio
        if (-not $how) { throw "no usable UIA pattern on the BLE radio button" }
        "switched to real-device mode ($how)"
        Start-Sleep -Milliseconds 500
        Save-Shot $win '00-ble-mode.png'

        Click-Button $win (Resolve-Label 'scan')
        "scanning ${ScanSeconds}s (firmware advertises at 500 ms intervals)"
        Start-Sleep -Seconds $ScanSeconds
        Show-Text $win 'after scan' | Out-Null
        Save-Shot $win '01-scanned.png'

        Select-Device $win 'NimBLE_GATT'
        Click-Button $win (Resolve-Label 'connect')
        $null = Wait-ForText $win @((Resolve-Label 'connected'), (Resolve-Label 'streaming')) $ConnectTimeout 'connect'
        Save-Shot $win '02-connected.png'

        Ensure-Running $win
    } else {
        Set-SimRate $win $SimRate
        Click-Button $win $ButtonName
    }

    # One dump + screenshot per slot; -Shots 5 with -Seconds 4 gives a 20 s film strip, which is
    # what a "tilt the board now" check needs to be readable after the fact.
    for ($i = 1; $i -le $Shots; $i++) {
        Start-Sleep -Seconds $Seconds
        Show-Text $win "t=+$($i * $Seconds) s" | Out-Null
        Save-Shot $win ("{0:d2}-running.png" -f $i)
    }

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

        if ($Mode -eq 'ble') {
            # Pressing start on the BLE source re-scans and re-connects by itself, which takes a
            # few seconds; waiting for the status text is the actual proof it came back.
            Ensure-Running $win
            $null = Wait-ForText $win @((Resolve-Label 'connected'), (Resolve-Label 'streaming')) $ConnectTimeout 'reconnect'
        } else {
            Click-Button $win $ButtonName
        }
        Start-Sleep -Seconds 2
        Show-Text $win 'restarted' | Out-Null
    }
}
finally {
    if ($KeepRunning) { "process left running (pid $($p.Id))" }
    elseif (-not $p.HasExited) { $p.Kill(); "process stopped" }
}
