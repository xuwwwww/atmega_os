# ATmega_OS

這是一個用來學習 AVR bare-metal 與作業系統核心概念的專案。目前目標板是：

- Arduino Uno Type-C 相容板
- ATmega328P 相容核心（本板為 LGT8F328P 類相容晶片），16 MHz
- UART0，115200 baud，8-N-1

目前已有：

- Timer2：預設每 0.5 ms 產生 scheduler tick，可在 build 時調整
- stackful preemptive scheduler：每個 task 有獨立 SRAM stack
- 4 級 fixed-priority scheduling，同優先權 task 使用 round-robin
- AVR assembly context switch：保存 r0-r31、SREG 與 stack pointer
- task state：READY、RUNNING、SLEEPING、BLOCKED、TERMINATED
- `task_sleep(ms)` 與 wrap-safe wake time
- idle task：保證 scheduler 永遠有可執行的 context
- task entry 可以 return，經預置 return address 自動進入 `task_exit()`
- `task_restart(index)` 可重建已 TERMINATED task 的初始 stack frame
- compact 16-byte TCB：priority、stack metadata 與 timed-wait cleanup
- build-time Huffman fixed logs：AVR 只傳壓縮 frame，筆電 monitor 解碼
- LED task
- UART RX/TX interrupt 與各 32-byte ring buffer
- 3-byte counting semaphore 與 8-task waiter bitmap
- 4-byte non-recursive mutex：owner validation、priority inheritance 與 handoff
- semaphore、mutex 與 UART receive blocking timeout
- 固定容量 message queue：FIFO、try、blocking 與 timeout API
- 12-byte event flags：wait-any、wait-all、auto-clear、timeout 與 ISR set
- UART shell task：查 uptime、task 狀態、stack 水位、控制 LED 與 restart task
- 4-byte stack guard、UART panic frame、`.noinit` panic record 與 software reboot
- one-shot/periodic software timer 與 deferred callback timer-service task

## 專案結構

```text
src/
├── drivers/        硬體驅動，例如 UART
├── kernel/         timer 與 scheduler
├── tasks/          使用核心服務的工作
└── main.c          初始化與 task 註冊
tools/
└── serial-monitor.ps1
```

每個 task 依需求配置固定 SRAM stack，並以無限迴圈執行。task 呼叫
`task_yield()` 後，assembly 會保存 CPU context、切換 SP，再恢復下一個
task。現在是 0.5 ms time slice 的 fixed-priority preemptive scheduling；
同優先權之間使用 round-robin。Timer2 ISR 可以搶占沒有主動呼叫
`task_yield()` 的 task。`task_yield()` 與
`task_sleep()` 仍可用於提早交出 CPU。

UART ISR 只在硬體與 ring buffer 之間搬移 byte，並 give RX semaphore；echo
task 沒資料時會進入 BLOCKED，不占 time slice。收到 byte 後 ISR 將它改回
READY，真正的 echo 仍由 task context 完成。

記憶體配置與 context switch 細節請見
[`docs/memory-and-context-switch.md`](docs/memory-and-context-switch.md)。
Preemptive tick 與同步原語路線請見
[`docs/preemptive-scheduler.md`](docs/preemptive-scheduler.md)。
Huffman build pipeline 與 UART framing 請見
[`docs/huffman-uart.md`](docs/huffman-uart.md)。
Timer 選擇、頻率與桌面 OS 的差異請見
[`docs/timers.md`](docs/timers.md)。
Message queue 結構與同步流程請見
[`docs/message-queue.md`](docs/message-queue.md)。
目前整個 OS、event flags、software timer、shell、RV32I emulator 與 SRAM 的逐段教學請見
[`docs/os-guide.md`](docs/os-guide.md)。

## 在 WSL 編譯

```bash
wsl.exe -d Ubuntu-22.04
cd /mnt/d/ATmega_OS
make clean all
```

預設 `KERNEL_TICK_HZ=2000`，也就是 0.5 ms time slice。若要切回 1 ms：

```bash
make clean all KERNEL_TICK_HZ=1000UL
make upload KERNEL_TICK_HZ=1000UL
```

提高 tick rate 會降低最壞排程延遲，但 context save、scheduler 掃描與 ISR
也會更頻繁。目前 2 kHz 已通過實機測試；`timer_millis()` 仍每一個真正的
1 ms 才加一，所以既有 sleep 時間不受影響。

五個 task stack 也能在 build 時各自設定：

```bash
make clean all \
  LED_STACK_SIZE=160 \
  SHELL_STACK_SIZE=224 \
  TIMER_SERVICE_STACK_SIZE=160 \
  IDLE_STACK_SIZE=128 \
  RV_DEMO_STACK_SIZE=128
```

每個值必須大於初始 context frame 的 37 bytes，且不超過 255。不要把
watermark 的 `unused == 0` 當成最佳化目標；0 代表沒有任何安全餘裕，也
無法分辨 stack 是否已經越界。先用最壞 workload 測量，再保留至少 16～32
bytes margin。

輸出檔案：

- `build/atmega_os.elf`：包含符號資訊，適合除錯
- `build/atmega_os.hex`：燒錄到晶片的映像

Makefile 使用已安裝在 `D:\CodexHome\runtime\toolchains\Microchip.ATmega_DFP.3.6.299` 的官方 Microchip Device Family Pack。

## 從 WSL 燒錄

這塊 FIREPHX USB-Serial 在 WSL USB passthrough 下無法穩定傳輸，因此 USB 保持在 Windows，由 WSL 直接呼叫 Windows AVRDUDE：

```bash
cd /mnt/d/ATmega_OS
make upload
```

預設使用 `COM6`。若 Windows 之後分配其他 COM port：

```bash
make upload PORT=COM7
```

板子的 bootloader 與 interrupt vector table 都使用 ATmega328P 相容介面，所以編譯與燒錄目標統一使用 `atmega328p` / `m328p`。

這塊相容板的自動 reset 不穩定。若看到 `programmer is not responding`，在 `make upload` 開始後立刻短按一次 Reset，讓 AVRDUDE 正好遇到 bootloader 的同步時間窗；不需要連按或長按。

## 從 WSL 看 UART 並輸入

```bash
cd /mnt/d/ATmega_OS
make monitor
```

啟動時會顯示一次：

```text
ATmega OS UART ready
```

啟動後會顯示 `os> ` prompt。輸入由板子逐字 echo；輸入 `help` 再按 Enter
可列出 shell 指令。週期性的 `kernel alive` 和 stack report 已移除，需要
資訊時使用 `uptime`、`tasks`、`stack`。按 `Ctrl+]` 或 `Ctrl+C` 離開。

`screen /dev/ttyACM0 115200` 看似卡住，是因為 screen 本來就會佔據終端，而且這塊 USB bridge 經 usbipd 傳到 WSL 後目前收不到 UART byte。若使用其他相容的 USB-Serial adapter，仍可測試：

```bash
make upload-wsl WSL_PORT=/dev/ttyACM0
screen /dev/ttyACM0 115200
```

screen 的離開方式是依序按 `Ctrl+A`、`K`、`Y`，不是 `Ctrl+C`。

## 下一步

目前 event flags、UART shell、stack guard、kernel panic、software reboot
與 software timer service 已可運作。接下來可選擇處理 task lifecycle，或沿
SD card + bytecode VM 路線實作可載入 task。詳細路線放在 `docs/os-guide.md`。
