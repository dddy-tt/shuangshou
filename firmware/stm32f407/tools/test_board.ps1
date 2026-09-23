[CmdletBinding()]
param(
    [string]$Firmware,

    [ValidatePattern('^COM\d+$')]
    [string]$Port = 'COM3',

    [ValidateRange(1200, 2000000)]
    [int]$Baud = 9600,

    [ValidateRange(1, 600)]
    [int]$Seconds = 15
)

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Firmware)) {
    $Firmware = Join-Path $projectRoot 'MDK-ARM\shuangshou\shuangshou.hex'
}

$flashScript = Join-Path $PSScriptRoot 'flash_stlink.ps1'
$captureScript = Join-Path $PSScriptRoot 'capture_uart.ps1'
$output = Join-Path $projectRoot 'artifacts\uart_latest.log'

& $flashScript -Firmware $Firmware
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

# Let the reset complete before opening the Bluetooth virtual COM port.
Start-Sleep -Milliseconds 800

& $captureScript -Port $Port -Baud $Baud -Seconds $Seconds -Output $output
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host ("[TEST][OK] Latest log: {0}" -f ([System.IO.Path]::GetFullPath($output)))
