# AVR 記憶體與 context switch

## 實際靜態記憶體配置

使用 `make` 連結後，`build/atmega_os.map` 會列出各 section 與 symbol。

加入 event flags 與 shell 後的正式 build 結果：

```text
Flash: .text 7126 + .data initial image 0 = 7126 bytes
SRAM:  .data 0 + .bss/.noinit 557 = 557 bytes
```

ATmega328PB 的內部 SRAM 是 `0x0100..0x08ff`。目前 `.bss` 後面接著
`.noinit`，最後一個靜態 byte 位於 `0x032c`，因此未被靜態物件配置的範圍是
`0x032d..0x08ff`，共 1491 bytes。main 啟動期間的 stack 也使用這個區域，
但 `scheduler_start()` 後
CPU 會改用各 task 的 stack。

這 557 bytes 可以從 linker symbols 完整加回來：

```text
三個 task stack                         400
三個 16-byte TCB                         48
timer：g_millis 4 + sub-ms counter 1       5
scheduler globals：兩個 pointer 與 3 bytes  7
mutex registry head pointer                    2
UART TX：buffer 32 + head/tail 2 + mutex 4 38
UART RX：buffer 32 + head/tail/overflow 3
         + semaphore 3                     38
LED event flags 12 + mode/state 2          14
panic/reset `.noinit`                        5
                                         ----
                                          557 bytes
```

所以不是有一大塊 RAM 不明消失：400/557 bytes 是你主動保留的三塊 task
stack。剩餘 1491 bytes 還要供啟動期 stack、新增 task stack、queue 和
其他 globals 使用，不能全部視為安全可分配空間。AVR 沒有
MMU，配置時也應保留 overflow 安全空間。

各 task 依實際呼叫深度使用不同的固定 stack：

```text
shell_stack        192
led_stack          112
idle_stack          96
                  ----
                   400 bytes
```

陣列大小現在由 Makefile 的 `LED_STACK_SIZE`、`SHELL_STACK_SIZE`、
`IDLE_STACK_SIZE` 控制。C 需要在 build/link 時就
決定靜態陣列大小；task 跑完後看到 watermark，不能把已經編譯好的陣列在
原地自動縮短。正確流程是壓力測試、讀 unused、保留 margin，再重新 build。

`unused == 0` 技術上可以被 report 出來，但這是紅色警報，不是理想大小。
Watermark 只能看到 stack 內的 `0xa5` 是否被覆蓋；一旦寫出陣列底部，它
無法告訴你破壞了哪個相鄰物件。

## TCB 與 saved SP

`task_t.saved_sp` 故意放在 structure 的第一個欄位。`context.S` 只需取得
`g_current_task`，就能把 SPL、SPH 寫到 TCB offset 0 和 1。

```text
task_t
+0  saved_sp low byte
+1  saved_sp high byte
+2  stack pointer low byte
+3  stack pointer high byte
+4  wake_time（4 bytes）
+8  entry function pointer（2 bytes）
+10 wait_object pointer（2 bytes）
+12 timeout cleanup function pointer（2 bytes）
+14 stack_size（1 byte）
+15 control byte：state 3 + base priority 2 + effective priority 2 + timed wait 1 bit
```

為了允許 task entry return，TCB 保留 entry pointer，由共同的
`task_bootstrap()` 呼叫。`stack_size` 最大不超過 255，因此仍使用 8-bit。
`wake_time` 由 SLEEPING deadline、timed BLOCKED deadline 和喚醒結果重用。
每個 TCB 是 16 bytes，三個 TCB 共 48 bytes。

`state` 使用 3 bits，base/effective priority 各使用 2 bits，整個 control
仍然只有一個 byte；最後 1 bit 表示目前是否為 timed wait。build 以 `_Static_assert`
確認 `sizeof(task_t) == 16`，避免更換 compiler 後 bit-field layout 靜默改變。

## 初始 stack frame

新 task 還沒有真正執行過，所以沒有可恢復的 context。`task_init()` 會在
它的 stack 頂端手工建立一份假的 context：

```text
高位址
task bootstrap PC low
task bootstrap PC high
r0
SREG（I bit = 1）
r1（必須為 0）
r2
...
r31
saved_sp 指向這裡
低位址
```

`scheduler_start()` 載入第一個 task 的 SP，反向 POP r31 到 r0，最後
執行 `RET`。`RET` 會把假的 return address 載入 Program Counter，因此
CPU 第一次進入 `task_bootstrap()`，再透過 TCB 的 entry pointer 呼叫 task。
若 entry return，bootstrap 呼叫 `task_exit()`，將狀態改成 TERMINATED 並
永久切走。TERMINATED task 的靜態 stack 尚未回收，但 scheduler 不會再選它。

## task_yield

task 呼叫 `task_yield()` 時，CALL 指令已經先把返回位址放到目前 task 的
stack。接著 `CONTEXT_SAVE`：

1. 保存 r0 與 SREG。
2. 關閉全域中斷。
3. 保存 r1-r31。
4. 把目前 SPL/SPH 寫進目前 TCB。
5. 呼叫 `scheduler_select_next()` 選擇下一個 TCB。
6. 從下一個 TCB 載入 SPL/SPH。
7. POP 下一個 task 的 r31-r0 與 SREG。
8. `RET` 回到下一個 task 上次呼叫 `task_yield()` 的位置。

這不是把 code 搬到 SRAM。每個 task 的 machine code 一直位於 Flash；
切換的是 SRAM 中的 stack 與 CPU register snapshot。

## stack watermark

`task_init()` 先以 `0xa5` 填滿 stack，再在高位址端建立初始 context。
`task_stack_unused()` 從低位址開始計算仍為 `0xa5` 的 bytes，可用來估算
stack 的最低剩餘量。

watermark 只能協助偵測，不是記憶體保護。如果 stack 已越過陣列邊界，
ATmega328PB 沒有 MMU 能阻止它破壞相鄰資料。

目前改由 shell 的 `stack` 指令按需顯示 watermark，實測：

```text
task=0 stack=112 used=80 unused=32
task=1 stack=192 used=141 unused=51
task=2 stack=96 used=61 unused=35
```

依 task 註冊順序換算最高用量：

```text
LED     112 - 32 = 80 bytes
Shell  192 - 51 = 141 bytes
Idle     96 - 35 = 61 bytes
```

這些數字是目前 preemptive workload 的觀察值，不是永久保證。加入新的
函式呼叫或較大的區域變數後必須重新測量。每次搶占也會把完整 register
frame 壓到被搶占 task 的 stack，因此 watermark 已包含實際 ISR 負擔。

## READY、RUNNING、SLEEPING、BLOCKED 與 TERMINATED

`task_sleep(delay_ms)` 先計算：

```text
wake_time = timer_millis() + delay_ms
state = TASK_SLEEPING
```

接著呼叫 `task_yield()`。scheduler 每次切換時掃描所有 task，使用
`(int32_t)(now - wake_time) >= 0` 判斷 wake time 是否已到。這種相減後再
判斷的方法可以跨越 32-bit timer wraparound，但單次 delay 必須小於
`2^31` ms。

這是相對 delay，不是自動週期觸發。只有主動呼叫 `task_sleep()` 的 task
會進入 SLEEPING；UART echo 沒有呼叫它，而是等 semaphore 時進入 BLOCKED。
四到八個 task 時，每次 tick 線性檢查 sleep state 的成本很小，而且不需
額外 timer queue 記憶體，所以目前保留簡單版本。

目前選擇規則是：

1. RUNNING task 若只是 yield，改回 READY。
2. 到期的 SLEEPING task 改成 READY。
3. 從目前 index 的下一格開始尋找 READY task。
4. 選中後標成 RUNNING，並讓 assembly 載入它的 SP。

## 為什麼需要一個看起來什麼都沒做的 idle task

如果 LED 與 shell 都因等待事件或 UART 資料而 BLOCKED，
scheduler 可能找不到任何 READY context。但 CPU 不能在「沒有 stack、沒有
Program Counter」的狀態下等待；它仍必須有一個合法 context 可以恢復。

idle task 就是這個永遠 READY 的保底 context。現在它只重複執行 `NOP`，
完全不主動 yield，用來驗證 Timer 確實能搶占；未來可以改成 AVR `SLEEP`，由 Timer/UART interrupt
喚醒，以降低耗電。idle task 不是未初始化 task，它同樣有 TCB、80-byte
stack、初始 register frame 和 entry address。

## Preemptive Timer0 tick

Timer0 compare match A 現在直接進入 naked assembly ISR：

```text
hardware push interrupted PC
CONTEXT_SAVE
timer_tick_from_isr()
scheduler_select_next_at(now)
CONTEXT_RESTORE
RETI
```

idle task 故意完全不呼叫 `task_yield()`；LED 與 shell 仍能運作，
因此實機上已證明 Timer ISR 可以強制搶占。

UART event wait/block、counting semaphore、mutex、priority scheduler、
priority inheritance、blocking timeout、message queue、event flags 與互動式
UART shell、stack guard、kernel panic 與 watchdog recovery 都已完成。下一步
建議加入 software timer service。
