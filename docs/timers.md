# Timer 選擇與 tick rate

## 目前為什麼用 2 kHz

CPU clock 固定是 16 MHz；提高 scheduler tick 不會加快 instruction，只會
縮短 READY task 等待下一次排程的最壞時間。

```text
1 kHz tick  約 1 ms scheduling latency，開銷較低
2 kHz tick  約 0.5 ms scheduling latency，開銷約增加一倍
5 kHz tick  約 0.2 ms，但目前 full-context save/線性掃描通常不值得
```

現在選 2 kHz 是 UART 互動延遲與 8-bit AVR 開銷之間的折衷。完整 native OS
使用 Timer2：16 MHz /64，2 kHz 需要 125 counts，所以 `OCR2A=124`。`timer_millis()`
累積兩個 tick 才加一，時間 API 仍是毫秒。

提高頻率前應先量測 CPU load、interrupt latency 和 stack watermark。現代
桌面 OS 也不會單純把 periodic tick 無限提高：Linux 常在 idle CPU 停止
scheduler tick，並另有高解析度 timer；Windows 高解析度 timer 也只在需要
時提高 clock interrupt rate，因為會增加功耗與降低整體效能。

## 這顆晶片有幾個 timer

一般 Arduino Uno 的 ATmega328P 有三組：Timer0、Timer1、Timer2。目前 Makefile
以 `atmega328p` 為編譯目標；實體板則是 LGT8F328P 類相容晶片，不能因為曾經使用
過 ATmega328PB DFP 就假設 TC3/TC4 的標準 328PB 暫存器一定適用。

```text
TC0  8-bit   CTC、overflow、兩路 compare、PWM；目前未使用
TC1  16-bit  guest 執行期間以 polling 提供 1 ms VM time slice
TC2  8-bit   `LGT_SAFE_MODE=0` 時產生 2 kHz native OS tick
```

Arduino framework 通常用 Timer0 維護 `millis()`/`delay()`，其他 library
可能用 Timer1 做 Servo、Timer2 做 tone/PWM。但本專案是 bare metal，沒有
連結 Arduino core，因此沒有 framework 的 timer ownership 衝突。LGT safe mode
不開任何 timer interrupt；Timer1 只自由計數並由 emulator polling。

官方參考：

- Microchip ATmega328PB datasheet: https://ww1.microchip.com/downloads/en/DeviceDoc/40001906C.pdf
- Microchip timer example: https://developerhelp.microchip.com/xwiki/bin/view/products/mcu-mpu/8-bit-avr/peripherals/timers-counters/example-project/
- Linux NO_HZ: https://www.kernel.org/doc/html/latest/timers/no_hz.html
- Microsoft high-resolution timers: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/high-resolution-timers
