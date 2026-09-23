[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Firmware,

    [ValidatePattern('^0x[0-9A-Fa-f]+$')]
    [string]$BinAddress = '0x08000000'
)

function Find-CubeProgrammerCli {
    if (-not [string]::IsNullOrWhiteSpace($env:STM32_CUBE_PROGRAMMER_CLI)) {
        if (Test-Path -LiteralPath $env:STM32_CUBE_PROGRAMMER_CLI -PathType Leaf) {
            return $env:STM32_CUBE_PROGRAMMER_CLI
        }
        throw 'STM32_CUBE_PROGRAMMER_CLI is set but does not point to an existing file.'
    }

    $candidates = @(
        'E:\download\bin\STM32_Programmer_CLI.exe',
        (Join-Path $env:ProgramFiles 'STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe')
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    $command = Get-Command 'STM32_Programmer_CLI.exe' -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw 'STM32CubeProgrammer CLI was not found. Install STM32CubeProgrammer, add its bin directory to PATH, or set STM32_CUBE_PROGRAMMER_CLI.'
}

$firmwarePath = (Resolve-Path -LiteralPath $Firmware).Path
$extension = [System.IO.Path]::GetExtension($firmwarePath).ToLowerInvariant()

if ($extension -notin @('.hex', '.bin')) {
    throw "Unsupported firmware type '$extension'. Use a .hex or .bin file."
}

$cli = Find-CubeProgrammerCli
$arguments = @('-c', 'port=SWD', 'mode=UR', 'reset=HWrst', '-w', $firmwarePath)
if ($extension -eq '.bin') {
    $arguments += $BinAddress
}
$arguments += @('-v', '-rst')

Write-Host '[FLASH] Writing through ST-LINK SWD. No erase, option-byte, or protection command is used.'
Write-Host ("[FLASH] Firmware: {0}" -f $firmwarePath)

& $cli @arguments
if ($LASTEXITCODE -ne 0) {
    Write-Error ("[FLASH][ERROR] STM32CubeProgrammer exited with code {0}." -f $LASTEXITCODE)
    exit $LASTEXITCODE
}

Write-Host '[FLASH][OK] Programming, verification, and reset completed.'
