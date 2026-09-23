[CmdletBinding()]
param()

$ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object

if ($ports.Count -eq 0) {
    Write-Host '[UART] No COM ports found.'
    exit 1
}

Write-Host '[UART] Available COM ports:'
foreach ($port in $ports) {
    Write-Host ("  {0}" -f $port)
}
