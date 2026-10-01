param(
    [string]$PortName = "COM6",
    [int]$BaudRate = 115200,
    [int]$DurationSeconds = 0,
    [switch]$ResetOnOpen
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
    $inputBuffer = [byte[]]::new(256)
    $inputRead = $standardInput.ReadAsync($inputBuffer, 0, $inputBuffer.Length)
}
else {
    try {
        $null = [Console]::KeyAvailable
        $useConsoleKeys = $true
    }
    catch {
        $standardInput = [Console]::OpenStandardInput()
        $inputBuffer = [byte[]]::new(256)
        $inputRead = $standardInput.ReadAsync($inputBuffer, 0, $inputBuffer.Length)
    }
}

try {
    $serial.Open()
    if ($ResetOnOpen) {
        # The Uno reset capacitor turns a DTR assertion into a reset pulse.
        # Wait for the bootloader before accepting shell input.
        $serial.DtrEnable = $true
        Start-Sleep -Milliseconds 100
        $serial.DtrEnable = $false
        Start-Sleep -Milliseconds 1800
    }
    # AVRDUDE uses 115200 while the application uses its configured baud. Bytes emitted by
    # the freshly started firmware before AVRDUDE closes COM6 may already have
    # been decoded at the bootloader baud and left in the USB-serial buffer.
    # They can never be recovered, so discard them before relaying new input.
    $serial.DiscardInBuffer()
    # Every successful connection asks the shell for a fresh prompt.  This
    # makes `make monitor` visibly land at `os>` even when the boot banner was
    # emitted before the port opened and was discarded above.
    $serial.Write("`r")

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
            elseif ($key.Key -eq [ConsoleKey]::UpArrow) {
                $serial.Write(([char]27).ToString() + "[A")
            }
            elseif ($key.Key -eq [ConsoleKey]::DownArrow) {
                $serial.Write(([char]27).ToString() + "[B")
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

            $receivedInput = $inputRead.Result
            $exitIndex = [Array]::IndexOf($inputBuffer, [byte]29, 0, $receivedInput)
            if ($exitIndex -ge 0) {
                if ($exitIndex -gt 0) {
                    $serial.Write($inputBuffer, 0, $exitIndex)
                }
                break
            }

            # Preserve all 8 bits.  SerialPort.Write(char) passes through its
            # text Encoding and corrupts bytes >= 0x80; binary guest packets
            # must use the byte[] overload.
            $serial.Write($inputBuffer, 0, $receivedInput)

            $inputBuffer = [byte[]]::new(256)
            $inputRead = $standardInput.ReadAsync($inputBuffer, 0, $inputBuffer.Length)
        }

        Start-Sleep -Milliseconds 10
    }
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}
