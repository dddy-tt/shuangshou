[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [ValidateRange(1200, 2000000)]
    [int]$Baud = 9600,

    [ValidateRange(1, 600)]
    [int]$Seconds = 15,

    [string]$Output
)

if ([string]::IsNullOrWhiteSpace($Output)) {
    $projectRoot = Split-Path -Parent $PSScriptRoot
    $Output = Join-Path $projectRoot 'artifacts\uart_latest.log'
}

$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDir = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $Baud,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 250
$serial.NewLine = "`n"
$serial.DtrEnable = $false
$serial.RtsEnable = $false

$writer = $null

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $writer = [System.IO.StreamWriter]::new($outputPath, $false, [System.Text.UTF8Encoding]::new($false))

    Write-Host ("[UART] Capturing {0} at {1} baud for {2} seconds" -f $Port, $Baud, $Seconds)
    $deadline = [System.DateTime]::UtcNow.AddSeconds($Seconds)
    $lineCount = 0

    while ([System.DateTime]::UtcNow -lt $deadline) {
        try {
            $line = $serial.ReadLine().TrimEnd("`r")
            if ($line.Length -eq 0) {
                continue
            }

            $record = '[{0:HH:mm:ss.fff}] {1}' -f (Get-Date), $line
            Write-Host $record
            $writer.WriteLine($record)
            $writer.Flush()
            $lineCount++
        } catch [System.TimeoutException] {
            # A timeout only means no complete UART line arrived in 250 ms.
        }
    }

    Write-Host ("[UART][OK] Captured {0} lines: {1}" -f $lineCount, $outputPath)
} catch {
    Write-Error ("[UART][ERROR] {0}" -f $_.Exception.Message)
    exit 1
} finally {
    if ($null -ne $writer) {
        $writer.Dispose()
    }
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
