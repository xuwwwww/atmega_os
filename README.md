# ATmega_OS

這是一個用來學習 AVR bare-metal 與簡單作業系統核心概念的練習專案。

目前的最小核心會：

- 使用 Timer0 每 1 ms 增加一次系統 tick
- 在主迴圈中每 500 ms 切換一次 PB5
- 產生可燒錄到 ATmega328P 的 ELF 與 Intel HEX 檔案

預設假設：`ATmega328P`、`16 MHz`。如果你的晶片或時脈不同，請在 `make` 時覆寫 `MCU` 與 `F_CPU`。

## 1. 開啟 WSL 與進入專案

在 Windows PowerShell 執行：

```powershell
wsl.exe -d Ubuntu-22.04
```

進入 Ubuntu 後，Windows 的 `D:\ATmega_OS` 會掛載為：

```bash
cd /mnt/d/ATmega_OS
```

## 2. 確認工具鏈

```bash
avr-gcc --version
avr-objcopy --version
make --version
avrdude --version
```

## 3. 編譯

```bash
cd /mnt/d/ATmega_OS
make
```

成功後，檔案會在 `build/`：

- `build/atmega_os.elf`：給 GDB/除錯器使用
- `build/atmega_os.hex`：給燒錄器使用

清除編譯輸出：

```bash
make clean
```

指定不同晶片，例如 ATmega2560：

```bash
make clean
make MCU=atmega2560 F_CPU=16000000UL
```

## 4. 這個程式現在還不是完整 OS

AVR 通常沒有記憶體保護或行程隔離；目前程式是直接從 reset vector 進入 `main()` 的 bare-metal 核心骨架。接下來可以依序加入：

1. `sleep_ms()` 與 tick API
2. cooperative task scheduler
3. ring buffer 與 UART driver
4. command shell
5. task stack/context switch

## 5. 常見錯誤

- `device type is not defined`：沒有設定正確的 `MCU`。
- `F_CPU` 不正確：Timer tick 與 UART baud rate 會不準。
- `avrdude` 找不到 USB 裝置：WSL 編譯可以正常進行，但實體燒錄通常還需要 Windows 的 USB/WSL 裝置轉接設定。

