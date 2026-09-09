param(
    [string]$PortName = "COM6",
    [int]$BaudRate = 115200,
    [int]$DurationSeconds = 0
)

$serial = [System.IO.Ports.SerialPort]::new(
    $PortName,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)

$serial.DtrEnable = $false
$serial.RtsEnable = $false
$startedAt = [DateTime]::UtcNow
$standardOutput = [Console]::OpenStandardOutput()
$serialBuffer = [byte[]]::new(256)
$useConsoleKeys = $false
$standardInput = $null
$inputBuffer = $null
$inputRead = $null

if ([Console]::IsInputRedirected) {
    $standardInput = [Console]::OpenStandardInput()
    $inputBuffer = [byte[]]::new(1)
    $inputRead = $standardInput.ReadAsync($inputBuffer, 0, 1)
}
else {
    try {
        $null = [Console]::KeyAvailable
        $useConsoleKeys = $true
    }
    catch {
        $standardInput = [Console]::OpenStandardInput()
        $inputBuffer = [byte[]]::new(1)
        $inputRead = $standardInput.ReadAsync($inputBuffer, 0, 1)
    }
}

try {
    $serial.Open()

    while ($DurationSeconds -eq 0 -or ([DateTime]::UtcNow - $startedAt).TotalSeconds -lt $DurationSeconds) {
        $available = $serial.BytesToRead
        if ($available -gt 0) {
            $toRead = [Math]::Min($available, $serialBuffer.Length)
            $received = $serial.Read($serialBuffer, 0, $toRead)
            if ($received -gt 0) {
                $standardOutput.Write($serialBuffer, 0, $received)
                $standardOutput.Flush()
            }
        }

        if ($useConsoleKeys -and [Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)

            if ([int]$key.KeyChar -eq 29) {
                break
            }

            if ($key.Key -eq [ConsoleKey]::Enter) {
                $serial.Write("`r")
            }
            elseif ($key.KeyChar -ne [char]0) {
                $serial.Write($key.KeyChar.ToString())
            }
        }
        elseif (-not $useConsoleKeys -and $null -ne $inputRead -and $inputRead.IsCompleted) {
            if ($inputRead.Result -eq 0) {
                $inputRead = $null
                continue
            }

            $inputByte = $inputBuffer[0]

            if ($inputByte -eq 29) {
                break
            }

            if ($inputByte -eq 10 -or $inputByte -eq 13) {
                $serial.Write("`r")
            }
            else {
                $serial.Write([char]$inputByte)
            }

            $inputBuffer = [byte[]]::new(1)
            $inputRead = $standardInput.ReadAsync($inputBuffer, 0, 1)
        }

        Start-Sleep -Milliseconds 10
    }
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
