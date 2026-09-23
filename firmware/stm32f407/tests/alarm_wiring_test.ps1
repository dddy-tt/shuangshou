$ErrorActionPreference = 'Stop'

$mainPath = Join-Path $PSScriptRoot '..\Core\Src\main.c'
$main = Get-Content -LiteralPath $mainPath -Raw

if ($main -match 'Flex_CheckSpasm\s*\(') {
    throw 'Flex_CheckSpasm must not be wired into the main alarm/buzzer path.'
}

$flexCall = $main.IndexOf('Flex_Update();', [System.StringComparison]::Ordinal)
$gestureCall = $main.IndexOf('Gesture_Evaluate();', $flexCall, [System.StringComparison]::Ordinal)
if ($flexCall -lt 0 -or $gestureCall -lt 0 -or $gestureCall -le $flexCall) {
    throw 'Could not locate the Flex scheduler boundary in main.c.'
}
$flexTask = $main.Substring($flexCall, $gestureCall - $flexCall)
if ($flexTask -match 'sos_flag') {
    throw 'The Flex scheduler must not set sos_flag.'
}
if ($main -match '\bsos_flag\b' -or $main -match '\bbuzzer_active\b') {
    throw 'Legacy MAX30102/SOS buzzer path must not bypass the JY61P alarm state machine.'
}
if ($main -notmatch '__HAL_TIM_SET_COMPARE\(&htim4, TIM_CHANNEL_3, BUZZER_PWM_SILENT_COMPARE\);\s*if \(HAL_TIM_PWM_Start\(&htim4, TIM_CHANNEL_3\)') {
    throw 'The low-level-triggered buzzer must be preloaded to the silent level before PWM start.'
}

$jyPath = Join-Path $PSScriptRoot '..\Core\Src\jy61p.c'
$jyHeaderPath = Join-Path $PSScriptRoot '..\Core\Inc\jy61p.h'
$jy = Get-Content -LiteralPath $jyPath -Raw
$jyHeader = Get-Content -LiteralPath $jyHeaderPath -Raw
if ($jyHeader -notmatch '\blast_error\b' -or
    $jyHeader -notmatch '\bacc_sample_seen\b') {
    throw 'JY61P runtime diagnostic state is incomplete.'
}
if ($main -notmatch 'JY\|ONLINE=%u\|ERR=%u\|LAST=%u\|AGE=%lu') {
    throw 'The compact dynamic JY diagnostic frame is missing.'
}
if ($main -notmatch 'if\s*\(\s*JY61P_Right\.angle_sample_seen\s*!=\s*0U\s*\)\s*\{\s*snprintf\(imu_line') {
    throw 'IMU telemetry must continue publishing the most recent valid pose after an all-zero angle snapshot.'
}
if ($main -match 'JY61P_Right\.angle_valid\s*!=\s*0U\s*&&\s*JY61P_Right\.angle_sample_seen') {
    throw 'Current-sample angle_valid must not suppress the retained valid IMU pose from BLE telemetry.'
}
if ($jy -notmatch 'jy61p_record_result' -or
    $jy -notmatch 'JY61P_ERR_PROBE') {
    throw 'JY61P error status/recovery instrumentation is missing.'
}
if ($jy -match 'HAL_I2C_Master_Transmit') {
    throw 'Recovery must not send an undocumented I2C reset command.'
}

Write-Output 'alarm_wiring_test: all checks passed'
