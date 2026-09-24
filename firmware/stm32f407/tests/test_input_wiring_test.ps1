$ErrorActionPreference = 'Stop'

$firmwareRoot = Join-Path $PSScriptRoot '..'
$main = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Src\main.c')
$source = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Src\test_input.c')
$jy61p = Get-Content -Raw (Join-Path $firmwareRoot 'Core\Inc\jy61p.h')
$project = Get-Content -Raw (Join-Path $firmwareRoot 'MDK-ARM\shuangshou.uvprojx')
$ioc = Get-Content -Raw (Join-Path $firmwareRoot 'shuangshou.ioc')

if ($source -notmatch 'source = TEST_INPUT_SOURCE_REAL') {
    throw 'Test input must default to REAL mode.'
}
if ($source -notmatch 'TEST_INPUT_TIMEOUT_MS') {
    throw 'Virtual mode timeout protection is missing.'
}
if ($main -notmatch 'TestInput_Init\(\)') {
    throw 'main.c does not initialize the test input state.'
}
if ($main -notmatch 'TestInput_Service\(now\)') {
    throw 'main.c does not service automatic timeout exit.'
}
if ($main -notmatch 'TestInput_GetSource\(\) == TEST_INPUT_SOURCE_VIRTUAL[\s\S]*?TestInput_PublishMotion\(&JY61P_Right') {
    throw 'Virtual motion input is not wired before the real JY61P path.'
}
if ($main -notmatch 'TestInput_GetSource\(\) == TEST_INPUT_SOURCE_VIRTUAL[\s\S]*?TestInput_MainPublishFlex') {
    throw 'Virtual FLEX input is not wired before the real ADC update path.'
}
if ($main -notmatch 'TestInput_HandleLine\(bt_line') {
    throw 'USART3 complete lines are not routed to the TEST parser.'
}
if ($source -notmatch 'test_input_parse_decimal') {
    throw 'TEST floating-point fields must use the strict decimal parser.'
}
if ($source -match 'sscanf[\s\S]*%f') {
    throw 'TEST input must not depend on scanf floating-point conversion.'
}
if ($jy61p -notmatch 'JY61P_ACC_SCALE\s+0\.00478515625f') {
    throw 'Level 2 must preserve the existing REAL-mode JY61P ACC scale.'
}
if ($project -notmatch '<FileName>test_input\.c</FileName>') {
    throw 'Keil project does not compile test_input.c.'
}
if ($ioc -notmatch 'PC10\.Signal=USART3_TX' -or
    $ioc -notmatch 'PC11\.Signal=USART3_RX' -or
    $ioc -notmatch 'USART3\.BaudRate=9600') {
    throw 'USART3 pin/baud assumptions no longer match the CubeMX file.'
}

Write-Output 'test_input_wiring_test: all checks passed'
