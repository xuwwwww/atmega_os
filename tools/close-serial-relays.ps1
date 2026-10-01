$currentProcessId = $PID

# Close only monitor relay processes created by this project.  Do not kill
# Arduino IDE or unrelated PowerShell sessions.
$monitorPattern = '(?i)-File\s+"?[^\r\n]*serial-monitor\.ps1"?(?:\s|$)'
$relays = @(Get-CimInstance Win32_Process |
    Where-Object {
        $_.ProcessId -ne $currentProcessId -and
        $null -ne $_.CommandLine -and
        $_.CommandLine -match $monitorPattern
    })

$relays | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
}

# Do not return while a killed relay can still own COM6.  `make monitor`
# starts the replacement only after this script exits.
foreach ($attempt in 1..20) {
    $remaining = @(Get-CimInstance Win32_Process |
        Where-Object {
            $_.ProcessId -ne $currentProcessId -and
            $null -ne $_.CommandLine -and
            $_.CommandLine -match $monitorPattern
        })
    if ($remaining.Count -eq 0) {
        break
    }
    Start-Sleep -Milliseconds 100
}
