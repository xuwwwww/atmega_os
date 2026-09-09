# Timer 選擇與 tick rate

## 目前為什麼用 2 kHz

CPU clock 固定是 16 MHz；提高 scheduler tick 不會加快 instruction，只會
縮短 READY task 等待下一次排程的最壞時間。

```text
1 kHz tick  約 1 ms scheduling latency，開銷較低
2 kHz tick  約 0.5 ms scheduling latency，開銷約增加一倍
5 kHz tick  約 0.2 ms，但目前 full-context save/線性掃描通常不值得
```

現在選 2 kHz 是 UART 互動延遲與 8-bit AVR 開銷之間的折衷。Timer0 使用
16 MHz /64，2 kHz 需要 125 counts，所以 `OCR0A=124`。`timer_millis()`
累積兩個 tick 才加一，時間 API 仍是毫秒。

提高頻率前應先量測 CPU load、interrupt latency 和 stack watermark。現代
桌面 OS 也不會單純把 periodic tick 無限提高：Linux 常在 idle CPU 停止
scheduler tick，並另有高解析度 timer；Windows 高解析度 timer 也只在需要
時提高 clock interrupt rate，因為會增加功耗與降低整體效能。

## 這顆晶片有幾個 timer

一般 Arduino Uno 的 ATmega328P 有三組：Timer0、Timer1、Timer2。這個專案
實際編譯目標是 ATmega328PB；它有五組 Timer/Counter：兩個 8-bit TC0/TC2，
以及三個 16-bit TC1/TC3/TC4。

```text
TC0  8-bit   CTC、overflow、兩路 compare、PWM；目前作為 OS tick
TC1  16-bit  較長計時、高解析 PWM、input capture
TC2  8-bit   獨立 prescaler，可接外部 32.768 kHz crystal 做低功耗時間基準
TC3  16-bit  328PB 額外 timer，用途類似 TC1
TC4  16-bit  328PB 額外 timer，用途類似 TC1
```

Arduino framework 通常用 Timer0 維護 `millis()`/`delay()`，其他 library
可能用 Timer1 做 Servo、Timer2 做 tone/PWM。但本專案是 bare metal，沒有
連結 Arduino core，因此 Timer0 沒有 framework 衝突。保留 16-bit Timer1
給 input capture 或精準量測、保留 Timer2 給 asynchronous RTC，是目前很
合理的分工。TC3/TC4 的輸出腳位是否能使用，還要看相容板是否將 328PB 的
額外 pin 引出。

官方參考：

- Microchip ATmega328PB datasheet: https://ww1.microchip.com/downloads/en/DeviceDoc/40001906C.pdf
- Microchip timer example: https://developerhelp.microchip.com/xwiki/bin/view/products/mcu-mpu/8-bit-avr/peripherals/timers-counters/example-project/
- Linux NO_HZ: https://www.kernel.org/doc/html/latest/timers/no_hz.html
- Microsoft high-resolution timers: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/high-resolution-timers
