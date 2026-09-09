# ATmega OS 全系統導讀：從 16 MHz AVR 到 Event Flags 與 Shell

這份文件不是只列 API。目標是讓你能從「電源打開」一路追到某個字元從
COM6 進來、喚醒 shell、執行命令、再透過 UART interrupt 傳回筆電。

目前這個專案已經算是一個小型 RTOS kernel，而不是桌面 OS。它有獨立 task
stack、preemptive scheduling、sleep、task lifecycle、semaphore、mutex、
priority inheritance、message queue、event flags、UART driver 與 shell；但它
沒有 process address space、MMU、filesystem、user mode 或 executable loader。

## 1. 先建立一張全貌圖

```text
Windows terminal / make monitor
        │ 鍵盤字元、解碼後文字
        ▼
Windows COM6 ── USB/UART bridge ── ATmega328P-compatible UART0
                                      │ RX interrupt
                                      ▼
                                RX ring buffer
                                      │ give semaphore
                                      ▼
                                  shell task
                          ┌───────────┼────────────┐
                          │           │            │
                      scheduler   LED control      UART TX
                          │           │            │
                    task/stack查詢  event flags   TX mutex/ring
                          │           │            │
                          └───────────┴────────────┘
                                      │ TX interrupt
                                      ▼
                                    COM6
```

Timer2 每 0.5 ms 進一次 interrupt。它保存當前 task 的 CPU context，執行
scheduler，再恢復被選中的 task。這就是 preemption（搶占）。

## 2. 常見專有名詞

- **Bare metal**：程式直接跑在 MCU 上，底下沒有 Linux、Windows 或另一個
  OS 幫你配置 thread、memory 和 driver。
- **Kernel**：管理 CPU 時間、task 狀態與同步資源的核心程式。
- **Task / thread**：這個專案中兩者可近似理解為同一件事：共享同一份 Flash
  與 global data，但各自擁有 SRAM stack 和暫停後的 CPU context。
- **Process**：通常有獨立虛擬位址空間和權限。在沒有 MMU 的 ATmega328P
  上，目前的 task 不是 process，彼此可以讀寫所有 SRAM。
- **Context**：一段程式暫停後，未來要從原處繼續所需的 CPU 狀態。本專案
  保存 r0-r31、SREG、SP 和 return address。
- **Stack**：函式呼叫、return address、區域變數與暫存器備份使用的後進先出
  記憶體。每個 task 都有自己的固定陣列作為 stack。
- **TCB (Task Control Block)**：kernel 用來記錄一個 task 的 SP、狀態、
  priority、wake time 等 metadata。
- **Scheduler**：從 READY tasks 中決定下一個 CPU 使用者的程式。
- **Preemption**：task 沒有主動讓出 CPU，也會被 timer interrupt 暫停。
- **Critical section**：不能被另一個執行流程看到「做一半」的操作區段。
- **Atomic**：其他 task/ISR 不可能觀察到中間狀態，像是一個不可分割操作。
- **ISR (Interrupt Service Routine)**：硬體事件發生時，CPU 暫停一般程式而
  跳去執行的函式。ISR 必須短，不能 blocking。
- **Blocking**：task 暫時不具備執行資格；不是卡在 while loop 浪費 CPU。
- **Polling / busy wait**：反覆檢查條件。若沒有必要，通常比 blocking 浪費。
- **Race condition**：執行結果取決於兩個流程難以預測的先後順序。
- **Deadlock**：tasks 互相等待對方持有的資源，永遠不能前進。
- **Starvation**：task 並非死鎖，卻長時間一直搶不到 CPU 或資源。
- **Priority inversion**：高優先 task 等待低優先 task 的鎖，而中優先 task
  又一直搶占低優先 task，間接讓高優先 task 被中優先 task 阻擋。
- **Watermark**：預先用特定 byte 填 stack，執行後檢查哪些 byte 沒被改過，
  用來估算 stack 曾經最深使用量。
- **Ring buffer**：固定陣列首尾相接使用；head/tail 到底後回到 0。
- **Serialization / framing**：把記憶體資料轉成 byte stream，並標出訊息邊界。

## 3. ATmega328P 的記憶體不是一大塊 RAM

AVR 是 Harvard architecture，指令與資料使用不同的位址空間：

```text
Flash 32 KiB
├── interrupt vector
├── .text：機器指令
├── PROGMEM：Huffman payload、shell command 常數
└── bootloader（板子既有）

SRAM 2 KiB
├── .data：有非零初值的 global/static data
├── .bss：零初始化的 global/static data
├── 本專案手動配置的 task stack arrays
└── main/startup stack 可使用的剩餘區域

EEPROM 1 KiB
└── 斷電仍保留；目前未使用
```

Flash 可以直接取指令，SRAM 不可以直接被 CPU 當成 AVR 指令執行。因此之前
想的「把 process code load 到 SRAM 再跑」在這顆晶片上不是一般做法。可行的
模型是：所有 task entry 在 build 時一起 link 到 Flash；task 的變數、stack
和訊息放 SRAM。若要動態行為，使用 function pointer、bytecode interpreter
或自己做 VM，而不是把 AVR machine code 複製到 SRAM。

### 3.1 目前 862 bytes 靜態 SRAM 精確去哪裡

正式 build 的 `avr-size`：

```text
Program: 7960 bytes
Data:     862 bytes
```

這裡的 Data 是 `.data + .bss + .noinit`；panic record 位於 `.noinit`，其餘主要
是 `.bss`：

| 使用者 | bytes | 內容 |
|---|---:|---|
| 4 個 `task_t` | 64 | 4 × 16-byte TCB |
| LED stack | 160 | task 0 |
| shell stack | 224 | task 1，內含 64-byte command line |
| timer-service stack | 160 | task 2，執行到期 callback |
| idle stack | 128 | task 3 |
| UART RX/TX buffers | 64 | 32 + 32 |
| UART indices/overflow | 5 | RX 三個、TX 兩個 byte |
| UART RX semaphore | 3 | available character count |
| UART TX mutex | 4 | owner、waiters、linked-list pointer |
| LED event flags | 12 | bits、waiter modes、8 個 masks |
| LED mode/state | 2 | blink/off/on 與目前輸出 |
| timer/software-timer globals | 7 | millis 4、sub-ms 1、timer list 2 |
| scheduler globals | 7 | current/task pointers、count/index/depth |
| mutex registry pointer | 2 | priority inheritance 掃描入口 |
| panic `.noinit` record | 5 | reset cause、magic、code、detail |
| LED software timer | 15 | callback、argument、deadline、period、flags |
| **合計** | **862** | |

所以 link-time 尚有 `2048 - 862 = 1186` bytes。reset 到
`scheduler_start()` 之前，startup code 與 `main()` 的確仍使用 SRAM 頂端的
硬體 stack；開始排程後 SP 會切到各 task stack array，ISR 和 scheduler 的
呼叫深度也會記在該 task 的 watermark 中。因此這 1183 bytes 大部分目前是
未配置空間，但仍應替啟動期 stack、未來 globals 與錯誤診斷保留餘裕，不能
因為 linker 顯示有空間就一次吃滿。

板上跑過 shell 指令後量到：

| task | 配置 | 最大曾使用 | unused watermark |
|---|---:|---:|---:|
| LED | 160 | 待斷電重啟後重測 | — |
| shell | 224 | 待斷電重啟後重測 | — |
| timer service | 160 | 待斷電重啟後重測 | — |
| idle | 128 | 待斷電重啟後重測 | — |

`unused == 0` 絕不是最佳狀態，而是紅色警報：你不知道它剛好用到邊界，還是
已經越界後又破壞別人。實務上要跑最壞 workload，再保留至少 16～32 bytes。
表中的 used 也包含一開始偽造的 37-byte初始 frame，以及 task 被 Timer ISR
搶占時壓入的 context 和 scheduler call frames；所以幾乎沒區域變數的 idle
task 仍會出現數十 bytes 的使用量，這是正常的。

### 3.2 為什麼不能真的做 `uint4_t`

AVR 最小可定址單位是 byte。C pointer 不能指到「半個 byte」，所以一般
變數沒有真正 4-bit storage type。C bit-field 可把同一個 byte 分給多個欄位，
TCB 已經如此使用：

```c
struct {
    uint8_t state : 3;
    uint8_t base_priority : 2;
    uint8_t effective_priority : 2;
    uint8_t timed_wait : 1;
} control;
```

四個邏輯值剛好共用 1 byte。但 bit-field 的 layout 是 implementation-defined，
不應讓 assembly 直接依賴其中 bit 順序。本專案 assembly 只依賴 `saved_sp` 在
TCB offset 0；並用 `_Static_assert(sizeof(task_t) == 16)` 防止 layout 靜默變動。

## 4. Build 到底做了什麼

```bash
make clean all
```

流程是：

1. `generate_log_codec.py` 讀 `tools/log_messages.json`，統計所有固定 log 的
   byte 頻率，建立 Huffman tree。
2. 產生 AVR 端 `src/generated/log_messages.c/.h` 和筆電端
   `tools/generated_log_codec.py`。兩端必須使用同一張 code table。
3. `avr-gcc -c` 分別把 `.c`/`.S` 編成 `.o`。`-Os` 是以尺寸最佳化，
   `-mmcu=atmega328p` 告訴 compiler 暫存器、指令集、vector table 和 ABI。
4. linker 把 object 合成 ELF。`--gc-sections` 刪除沒有被引用的 function/data；
   `.map` 顯示每個 symbol 放哪裡。
5. `avr-objcopy` 從 ELF 取出可燒錄的 Intel HEX。
6. `avr-size` 報告 Flash 與靜態 SRAM。

常用命令：

```bash
make test
make upload PORT=COM6
make monitor PORT=COM6
```

修改 stack 大小後要 clean build，避免舊 object 的 compile-time define 殘留：

```bash
make clean all SHELL_STACK_SIZE=240
make upload SHELL_STACK_SIZE=240
```

## 5. 從 reset 到第一個 task

`main.c` 依序：

1. `DDRB |= (1 << DDB5)`：把 PB5 設成 output。Uno 的板載 LED 通常接 PB5。
2. `timer_init()`：配置 Timer2 CTC 和 compare interrupt。
3. `uart_init()`：配置 115200 8-N-1、ring buffers 的 semaphore/mutex。
4. `led_task_init()`：清 event bits，LED 進入 blink mode。
5. 傳 boot Huffman frame。
6. `task_init()` 建立四個 TCB 和初始 stack frame。
7. `scheduler_init()` 選最高 priority 的初始 task（shell）。
8. `scheduler_start()` 用 assembly 載入該 stack，`ret` 直接進入 task entry。

目前 task ID 是 build 時的 array index：

| ID | entry | base priority | 功能 |
|---:|---|---:|---|
| 0 | `led_task` | LOW=1 | LED blink/event control |
| 1 | `shell_task` | HIGH=3 | UART 命令處理 |
| 2 | `software_timer_task` | NORMAL=2 | 執行到期 timer callback |
| 3 | `idle_task` | IDLE=0 | 沒別人能跑時提供 context |

## 6. 一個「空 task」到底怎麼開始執行

新 task 還沒真的跑過，所以 CPU 不可能曾替它保存 context。`task_init()` 的
關鍵是偽造一個看起來像「以前被暫停過」的 stack frame。

它先把整個 stack 填成 `0xA5`，再從高位址往低位址放入：

```text
task_exit 的 return address
task entry 的初始 PC
r0
SREG（I bit=1，代表恢復後可開 interrupt）
r1
r2 ... r31
```

然後 `saved_sp` 指向這個 frame 底端。第一次被 scheduler 選中時，
`CONTEXT_RESTORE` pop r31 到 r0、恢復 SREG，最後 `ret`。`ret` 以為自己正在
從函式返回，其實第一個 return address 是我們人工放入的 task entry。

task entry 自己若執行 `return`，第二個人工 return address 會讓 CPU 進入
`task_exit()`。因此真正流程是：

```text
scheduler_start -> fake RET -> shell_task -> return -> task_exit
```

`task_exit()` 把狀態改成 TERMINATED 並永不返回。`task_restart(id)` 只允許
TERMINATED task；它拿原本 entry、stack
和 priority 再跑一次 `task_init()`，等於把 stack 與起跑 context 重建。

目前限制：如果 task 在持有 mutex 時 return，mutex owner 會殘留。成熟 kernel
要在 task exit 時拒絕退出、釋放資源或進入 panic；這是後續 lifecycle 工作。

## 7. Cooperative 與 preemptive context switch

`context.S` 的 `CONTEXT_SAVE`：

1. push r0。
2. 讀 SREG，`cli` 關 interrupt，再 push 原 SREG。
3. push r1-r31；r1 依 AVR ABI 必須保持 zero，保存後先 `clr r1`。
4. 從 `g_current_task` 取得 TCB pointer。
5. 把硬體 SPL/SPH 存到 TCB 的前兩 bytes，也就是 `saved_sp`。

`CONTEXT_RESTORE` 做完全相反的事：先把下一個 TCB 的 `saved_sp` 寫進硬體
SP，再逆序 pop 暫存器和 SREG。

保存 task context 後，assembly 先把 SP 切到 `RAMEND`，重用已不再需要的
`main()` stack 來執行 C scheduler。這避免 scheduler 的 C call frame 壓在 task
context 上；scheduler return 後才載入下一個 task 的 private SP。

兩條切換路徑的結尾不同：

```text
task_yield(): save -> scheduler_select_next -> restore -> RET
Timer ISR:    save -> scheduler_tick        -> restore -> RETI
```

`RET` 回到一般函式呼叫後；`RETI` 除了回到被 interrupt 的 PC，也符合 AVR
interrupt return 規則。兩條路都使用同一種 frame layout，才可在不同 task
之間任意切換。

## 8. Timer2、CTC 與 0.5 ms tick

CTC 是 Clear Timer on Compare Match。Timer2 從 0 向上數，等於 OCR2A 時：

- 設定 compare match flag；
- 因 TIMSK2 的 OCIE2A 被打開，所以進 ISR；
- counter 自動清回 0，再開始下一輪。

目前 16 MHz、prescaler 64、tick 2000 Hz：

```text
timer input = 16,000,000 / 64 = 250,000 Hz
每 tick counts = 250,000 / 2,000 = 125
OCR2A = 125 - 1 = 124
週期 = 1 / 2,000 s = 0.5 ms
```

排程每 0.5 ms 有機會發生一次，但 `g_millis` 每兩個 tick 才加一，因此
`task_sleep(100)` 仍是 100 ms，不會因 tick 加速變成 50 ms。

`timer_millis()` 要短暫關 interrupt，因為 8-bit AVR 讀 32-bit 數要多條指令；
若讀一半時 ISR 把數值加一，就可能拼出撕裂的值。ISR 中的 `g_millis++` 不用
再次 `cli`，因為 AVR 進 ISR 時硬體已清 I bit，除非你刻意做 nested interrupt。

## 9. Scheduler 的狀態與選擇規則

狀態：

- READY：有資格執行，正在等 CPU。
- RUNNING：目前 CPU 上的 task。
- SLEEPING：單純等時間到，由 `task_sleep()` 產生。
- BLOCKED：等 semaphore、mutex、queue、UART byte 或 event。
- TERMINATED：entry 已 return；除非 restart，否則不再執行。

每次排程先做 bookkeeping：到期的 SLEEPING 改 READY；timed BLOCKED 到期時
呼叫該同步物件的 cancel callback，把 waiter bitmap 清掉，再將結果設成
WAIT_TIMEOUT。

接著環狀掃描 task array，選 effective priority 最高的 READY task。掃描從
目前 task 的下一格開始，所以同 priority tasks 形成 round-robin。這是
fixed-priority preemptive scheduler：高 priority READY 時一定壓過低 priority。
代價是設計不當會 starvation，所以 task 應在等 I/O 時 blocking，不要空轉。

## 10. 四種同步工具應該怎麼分

### 10.1 Semaphore：計數「有幾份」

UART RX semaphore 的 count 就是 buffer 中可讀 byte 數。ISR 收一個 byte 就
give；shell 沒 byte 時 take 並 BLOCKED。Semaphore 沒有 owner，因此不適合
表達「這把鎖是誰的」。

### 10.2 Mutex：保護共享資源的所有權

TX frame 不能被兩個 task 交錯，所以整個 frame 傳送期間持有 `tx_mutex`。
Mutex 記 owner，只有 owner 可 unlock，而且目前 non-recursive：同 task 再 lock
同一把會失敗。

如果 HIGH shell 等 LOW task 的 mutex，LOW 的 effective priority 暫時提升到
HIGH，讓它盡快跑完並 unlock，這是 priority inheritance。registry 允許 kernel
反覆掃描 mutex dependency，將提升沿鏈傳遞。它降低 priority inversion，但
不能修復 lock-order deadlock。

### 10.3 Message queue：傳一筆真正的資料

Queue 使用兩個 semaphore：`spaces` 表示空格、`items` 表示資料筆數；mutex
只保護 copy 和 head/tail 更新。它適合 producer/consumer，不必共享一堆
global variables。容量和 item size 固定，所以不需 `malloc()`。

### 10.4 Event flags：等待「哪些事情發生了」

一個 `event_flags_t` 有 8 個事件 bits。例如：

```c
#define RX_READY   (1u << 0)
#define TX_DONE    (1u << 1)
#define SHUTDOWN   (1u << 2)
```

它不是計數器。同一 bit 被 set 十次，在 clear 前仍只是 1；如果不能遺失次數，
應使用 semaphore 或 queue。

等待模式：

```c
event_flags_wait(&events, RX_READY | SHUTDOWN,
                 EVENT_WAIT_ANY, &matched);

event_flags_wait(&events, RX_READY | TX_DONE,
                 EVENT_WAIT_ALL | EVENT_AUTO_CLEAR, &matched);
```

- ANY：要求的 bits 中任一個存在便成功。
- ALL：要求的 bits 必須全部存在才成功。
- AUTO_CLEAR：成功時把 matched bits 清除。
- timeout 版本：時間內未符合則回 WAIT_TIMEOUT。

`event_flags_t` 的 12 bytes：目前 bits、waiters bitmap、wait-all bitmap、
auto-clear bitmap，加上每個 task 一個 requested mask。因為最多 8 tasks，一個
waiter bitmap 就能表示所有等待者，比 8 個 pointer 小很多。

Set 流程在 critical section 中：

1. `bits |= mask`。
2. 掃描 waiter bitmap。
3. 用每個 task 的 requested mask 判斷 ANY/ALL。
4. 符合時移除 waiter；需要時 clear matched bits。
5. 呼叫 `scheduler_complete_wait_value()`，把 task 改 READY。

matched bits 沒有另加進 16-byte TCB。等待完成後 deadline 已沒有用途，所以
`wake_time` 的低 byte 暫存 WAIT_ACQUIRED，高一個 byte暫存 matched bits。這是
安全的 lifetime reuse，不是同一時間把 timestamp 和 result 混用。

如果多個 auto-clear waiter 想消費同一 bit，目前 task ID 小者先取得，後者
可能繼續等。這是明確但簡單的政策；未來可改成 priority-aware 或 round-robin。

## 11. LED task 如何用 event flags

舊版本是 `task_sleep(2000)` 後才看下一步。若 shell 在它剛睡下時執行
`led on`，只改 mode 不一定能立刻改變 task 的等待狀態。

現在 blink mode 使用：

```c
event_flags_wait_timeout(&led_events,
    LED_EVENT_MODE_CHANGED,
    EVENT_WAIT_ANY | EVENT_AUTO_CLEAR,
    led_on ? 100u : 2000u,
    &events);
```

- WAIT_TIMEOUT：表示期間沒有命令，正常 toggle LED。
- WAIT_ACQUIRED：shell 改了 mode，立刻醒來，重新判斷 on/off/blink。
- 強制 ON/OFF mode：LED task 永久等待 mode-changed event，完全不耗 time slice。

`led_task_set_mode()` 先在 critical section 內更新 mode 和 PORTB，使燈立即反應，
再 set event 喚醒 LED task。即使 preemption 剛好發生，event bit 仍會保留，不會
有「先 set、後 wait 所以錯過通知」的 lost wakeup。

## 12. UART driver：interrupt、ring buffer 與 framing

RX ISR 只做短工作：讀 UDR0、放入 RX ring、give semaphore。它不解析 shell，
因為解析可能很久，也可能需要 mutex/blocking，這些都不該放 ISR。

TX 端把 byte 放入 TX ring，開啟 Data Register Empty interrupt；硬體可以再收
下一個 byte 時，ISR 從 ring 搬一個到 UDR0。這讓 transmission 與 task 執行
重疊。115200 8-N-1 每 byte 約需 10 bits，也就是約 86.8 μs。

UART stream 本身沒有 message boundary，所以協定使用：

```text
raw byte:        普通 byte；0x00 要重複 escape
compressed:      00 01 bit_count_low bit_count_high payload...
stack report:    00 02 byte_count payload...
shell text:      00 03 byte_count payload...
panic:           00 04 02 panic_code detail
```

固定訊息使用 Huffman，是 Flash 內的 compressed payload；板子不 decode，只
傳 frame。筆電的 Python monitor 用 build 時產生的 table decode。動態 shell
數字不能預先壓縮，因此走 length-delimited shell text frame。

## 13. Shell task 逐段拆解

Shell 有一個 64-byte local array：

```c
char line[SHELL_LINE_CAPACITY];
```

因為它是 local，而且 shell 永不 return，這 64 bytes 長期位於 shell 的 192
byte task stack。這正是 shell stack 比舊 echo task 大的主要原因。

輸入 loop 用 `uart_read_char_wait()`。沒輸入時 semaphore 讓 shell BLOCKED；
按鍵進來後 RX ISR give semaphore，HIGH-priority shell 變 READY，下一次 tick
或 ISR 返回後的排程點便能執行。

板子會逐字 echo，並在每次命令後顯示 `os> `。CR/LF 執行；CRLF 只執行一次；
Backspace/Delete 會輸出 `backspace-space-backspace` 擦掉畫面上的字；printable
ASCII 加到 line；超過 63 字會響 terminal bell，Enter 時回報錯誤，絕不寫出
array 邊界。輸入會正規化成小寫，並去掉命令前後的空白，所以 `  HELP  ` 也行。

命令常數宣告成 `PROGMEM`：

```c
static const char command_help[] PROGMEM = "help";
```

因此用 `strcmp_P` 而非普通 `strcmp`。`_P` 版本知道第二個 pointer 指向 Flash。
若直接拿一般 SRAM load 指令讀 PROGMEM pointer，會讀到錯誤資料。

沒有使用 `printf`。`buffer_append_u32()` 以 `% 10` 反向產生 digits，再倒過來
append。它只需要 10-byte暫存陣列，可處理完整 `uint32_t` uptime。輸出 task
狀態時，輸入 line 已解析完，所以重用同一個 64-byte buffer 組 response。

指令：

```text
help                 顯示命令、state 與 priority 編號
uptime               顯示 timer_millis()
tasks                顯示 state、base/effective priority
stack                顯示配置、used、unused watermark
faults               顯示 reset cause 與上次 panic record
panic                產生 code=127 的測試 panic，然後 software reboot
led on               強制點亮 PB5
led off              強制熄滅 PB5
led blink            回到短亮、長滅的 blink 模式
restart <task-id>    只重啟 TERMINATED task
```

看到 `restart failed` 不代表 restart 壞掉；目前四個 task 都設計成常駐無限迴圈，
不是 TERMINATED。要練習可暫時新增一個 entry 只做一次工作後 return，再從 shell
對其 ID 執行 restart。

## 14. 實際操作與讀輸出

```bash
make clean all
make test
make upload PORT=COM6
make monitor PORT=COM6
```

範例：

```text
shell ready; type help
os> uptime
uptime_ms=11591
os> tasks
task=0 state=4 priority=1/1
task=1 state=1 priority=3/3
task=2 state=4 priority=2/2
task=3 state=0 priority=0/0
```

這個 snapshot 中：LED 正等 event，所以 BLOCKED；shell 正在執行 `tasks`，所以
RUNNING；timer service 正等到期通知，所以 BLOCKED；idle 可跑但尚未被選中，
所以 READY。`priority=base/effective` 相同
表示當下沒有 priority inheritance。週期性 output task 已移除；資料完全由
shell 按需查詢，因此不再有 `kernel alive` 干擾輸入，並省下 128-byte stack
和 16-byte TCB。

`make test` 現在包含：

- Huffman round-trip、所有 9 個固定訊息與 raw escape；
- event flags ANY、ALL、auto-clear、blocking wake、timeout、clear；
- message queue empty/full、FIFO、wraparound、try 與 timeout。
- software timer one-shot、periodic catch-up、stop 與 32-bit wrap-around。

Host test 不能證明 assembly/hardware 正確，所以仍做過 COM6 實機測試：燒錄
7694-byte 映像已完整寫入。先前 15 ms watchdog panic 使 bootloader 陷入 reset
loop，需斷電一次清除 watchdog；新版改用 reset-vector software reboot。

## 15. 現在不是什麼，以及容易踩的坑

- 沒有 MPU/MMU：任何 wild pointer 都能破壞 kernel 或其他 task。
- 沒有 MPU 式 stack protection；4-byte guard 只能在排程點偵測，不能阻止寫入。
- 沒有動態 task allocation：task/stack 都由 `main.c` 靜態配置。
- 沒有 kill running task：restart 只接受 TERMINATED。
- 沒有 cleanup ownership：持鎖 task 不應 return。
- `scheduler_preempt_disable()` 只禁止 scheduler 換 task，不等於關所有 interrupt；
  保護會被 ISR 同時存取的資料要用真正 critical section。
- 關 interrupt 的區段要非常短，否則 UART byte 可能丟失、tick latency 增加。
- Event bit 不計數；同事件連續發生 N 次需要 semaphore/queue。
- 高優先 task 忙迴圈會餓死低優先 task；高優先 task 更應 blocking。
- Stack 使用量依呼叫路徑與 interrupt 時機而變，現在數據不是永久保證。

## 16. Software Timer Service

### 16.1 Hardware timer 與 software timer 不同

Timer2 是實體硬體 peripheral，只有一組 counter、prescaler、OCR2A 和 compare
interrupt。Software timer 是 kernel 保存的資料結構；你可以建立多個 logical
timers，它們全部共用 Timer2 提供的 `timer_millis()` 時基：

```text
Timer2 2 kHz interrupt
  -> 每兩 tick 將 millis 加一
  -> 掃描 registered software timers
  -> 到期 timer 設 pending
  -> give timer-work semaphore
  -> scheduler 選到 timer-service task
  -> task context 執行 callback
```

因此「十個 software timers」不代表需要十個 hardware timers。Hardware timer
提供時間刻度；software timer objects 保存各自的 deadline 和行為。

### 16.2 15-byte timer object

```c
typedef struct software_timer {
    struct software_timer *next;       // 2 bytes
    software_timer_callback_t callback;// 2 bytes
    void *argument;                    // 2 bytes
    uint32_t deadline;                 // 4 bytes
    uint32_t period;                   // 4 bytes
    uint8_t control;                   // bit-field, 1 byte
} software_timer_t;                    // AVR 合計 15 bytes
```

`next` 把所有 static timer 串成 registry；沒有 `malloc()`。Timer object 必須有
永久 lifetime，通常宣告成 file-scope `static`，不能把函式即將 return 的 local
timer 註冊進 list。

Control byte 目前只使用三 bits：

- `active`：deadline 是否仍由 ISR 追蹤。
- `periodic`：到期後是否安排下一個 period。
- `pending`：到期工作是否已交給 service task、尚未 dispatch。

### 16.3 API

```c
static software_timer_t sensor_timer;

static void sensor_due(void *argument)
{
    /* 這裡是普通 task context，不是 ISR。 */
}

software_timer_init(&sensor_timer, sensor_due, 0);
software_timer_start_once(&sensor_timer, 500u);
software_timer_start_periodic(&sensor_timer, 1000u);
software_timer_stop(&sensor_timer);

if (software_timer_is_active(&sensor_timer))
{
    /* deadline 尚在追蹤 */
}
```

One-shot 到期時 `active=0, pending=1`，callback 只執行一次。Periodic timer 到期
時仍保持 active，deadline 以原本 deadline 加 period，而不是用 callback 執行
時間重新計算，因此不會每次都累積 callback latency。

如果 CPU 忙到跨過多個 periods，ISR 用 catch-up loop 把 deadline 推進到未來；
`pending` 只有一 bit，所以漏掉的多次到期會合併成一次 callback。這種語意適合
「請更新狀態」，不適合「每一次 tick 都不能少」的計量；後者應使用 counter
或 queue。

Deadline 判斷仍採：

```c
(int32_t)(now - deadline) >= 0
```

所以 `uint32_t` 從 `0xffffffff` 回到 0 時仍能比較。前提是一個 delay 不超過
`INT32_MAX` milliseconds，約 24.8 天。

### 16.4 為何 callback 不放 ISR

Callback 可能 set event、send queue、格式化資料或碰 driver。若直接在 Timer2
ISR 執行：

- 所有 interrupt latency 都被 callback 拉長；
- blocking API 不能使用；
- 一個慢 callback 會讓 UART RX overflow；
- context switch frame 會在被中斷 task 的 stack 上繼續增長。

現在 ISR 只標記 pending 並 give 一個 3-byte binary semaphore。Priority NORMAL
的 `software_timer_task` 被喚醒後逐一取出 pending timer，再呼叫 callback。
Callback 雖在 task context，但仍應短而不長期 blocking，否則同一 service task
中的其他 timer 都會延遲。較大的工作應由 callback set event 或送 queue 給專用
worker task。

`software_timer_stop()` 能清 active/pending；若 service task 已經把 callback
從 pending list 取出，stop 不能追回「已開始 dispatch」的那一次。這是常見的
cancel race，未來若需要嚴格同步取消，可加入 generation number 或
`stop_and_wait()`。

### 16.5 LED 現在如何使用它

LED 需要不對稱週期：熄滅 2000 ms、點亮 100 ms，所以每次使用 one-shot：

```text
LED timer 到期
  -> timer service callback
  -> set LED_EVENT_TIMER_EXPIRED
  -> LED task READY
  -> toggle PORTB5
  -> 依目前亮/滅重新 start 100/2000 ms one-shot
```

`led on/off` 會 stop software timer；`led blink` set mode-change event，LED task
從目前 LED 狀態重新安排一個完整 interval。Mode-change 和 timer-expired 同時
存在時，以 mode command 優先，避免切回 blink 的瞬間被舊 timer event toggle。

## 17. Stack guard 與 Kernel Panic

每個 task stack 的最低位址現在保留 4-byte `0xD3` canary。Scheduler 每次選擇
task 前檢查所有 canary；損壞時送出 code=1、detail=task ID 的 panic frame。
`uart_panic_write()` 先排空 TX ring 並等待 TXC，接著停用 UART/Timer interrupts、
關閉 watchdog，再以 `jmp 0` 回到 reset vector。AVR startup 會重建 SP、清 `.bss`
並重新進入 `main()`；Panic record 位於 `.noinit`，所以重開後仍可用 `faults` 查。

最初版本使用 15 ms watchdog reset，但這塊板子的 bootloader 未能在 watchdog
再次到期前跳到 application 的 `.init3`，造成 `ATmega OS UART ready` 不斷刷新。
因此目前選擇 software reboot。它不像硬體 reset 一樣自動還原所有 peripherals，
所以 panic path 先停用目前使用的 UART/Timer，而各 driver init 也必須完整重設
自己的 register；`timer_init()` 現在會額外清 `TCNT0` 與 compare flag，`main()`
也會先清除 `PCICR` 與 `PCMSK0..3`。

實機曾出現啟動畫面不斷重印，但 `faults` 是 `reset_cause=0 panic=none`。原因不是
硬體 reset，而是未處理 interrupt 的 avr-libc 預設 handler 直接跳到 address 0。
加入 `BADISR_vect` 後得到 panic code=3，再以具名 vector handler 定位出
detail=3，也就是 `PCINT0`。Software reboot 保留了先前的 pin-change interrupt
enable，而 Port B 電位改變便再次觸發它。目前啟動會明確關閉未使用的 PCINT，
且保留 `PCINT0` handler；若它又被錯誤打開，會看到 `code=3 detail=3`，不會只
看到難以解釋的啟動畫面刷新。

## 18. 我們能不能從 SD Card Load Task？

### 18.1 目前是 native AVR execution

目前所有 task function 都由 `avr-gcc` 編譯成 AVR machine code，linker 在 build
時替它們決定 Flash address。TCB 的 `entry` 是一個 Flash code address：

```text
fake RET -> task entry -> Flash 中的 AVR 指令
```

ATmega328P 是 Harvard architecture，SRAM/SD card 裡的 bytes 不能直接成為 AVR
instruction fetch 的來源。因此不能像有 MMU 的電腦那樣，把 ELF 從 disk 載入
RAM、把 PC 指向 RAM 就執行。我們目前和 RISC-V emulator Linux 專案的 native
task 路線確實不同。

### 18.2 別人「用 Flash 跑 emulator」實際在做什麼

那類專案通常把 emulator/interpreter 本身編譯成 MCU 的 native code，放在
Flash。真正流程近似：

```text
Flash 中的 AVR/ARM emulator loop
  -> 從 SD 或 external RAM 讀 guest instruction
  -> 用 switch/decode 判斷這是哪條 RISC-V instruction
  -> 用 host MCU 指令模擬 guest register/memory 效果
  -> guest PC += 2/4
```

CPU 執行的始終是 Flash 中的 emulator；SD 上的 RISC-V code 被當成資料解讀，
不是硬體直接執行。Linux 還需要大量 guest RAM。Uno 只有 2 KiB SRAM，而一個
RV32I guest 光 32 個 32-bit integer registers 就要 128 bytes，SD 又是 512-byte
block storage、不是低延遲 random-access RAM。因此在 Uno 上模擬到能 boot Linux
理論上可研究，但通常還需要 external SPI SRAM/PSRAM、非常激進的 paging，速度
也會極慢；不適合作為目前 kernel 的下一個小步驟。

### 18.3 三種可載入架構

**A. 從 SD 載入 native AVR module 到 Flash**

需要自己處理 Flash page programming、relocation、symbol resolution、ABI、版本
相容、掉電時的原子更新和 Flash erase/write 壽命。一般 AVR application 也不能
把 SD bytes 當 RAM code 執行。這條路比較像寫 dynamic linker + bootloader，
難度最高，而且每次裝 module 都改 Flash。

**B. 在 Flash 寫 RISC-V emulator，從 SD 讀 guest program**

這就是你看到的方向。現有 scheduler 可以把 emulator 當一個 native task，但
每個 guest process 還需要 register file、guest memory、page cache 和 syscall
translation。若目標是 Linux，外接 RAM 幾乎是實際必要條件。

**C. 寫一個小型 bytecode VM，從 SD 載入 VM task**

這是最適合目前專案的方向。先定義少量 instructions：

```text
LOADI  ADD  SUB  LOAD  STORE  JMP  JZ
SYSCALL  YIELD  SLEEP  EXIT
```

Flash 中只放 `vm_runner()`；SD 放 bytecode。每個 VM task 在 SRAM 保存少量 virtual
registers、PC、stack 和一個 SD sector cache。VM 的 syscall 對接現有 kernel：

```text
VM SYSCALL_SLEEP -> task_sleep()
VM SYSCALL_WRITE -> UART/message queue
VM YIELD         -> 每執行 N 條 guest instructions 後 task_yield()
VM EXIT          -> task return / task_exit()
```

如此便真的能「換 SD 檔案，不重新編譯 firmware，載入新的 task 邏輯」，同時不
需要 AVR 從 SRAM 執行 native code。它不是硬體隔離：VM interpreter 必須檢查每
次 guest memory access，才能防止 bytecode 越界；這反而能用 software 做出類似
簡化 MPU/MMU 的 sandbox。

### 18.4 建議的 SD/VM 實作順序

```text
1. SPI driver
2. SD card 初始化與讀取單一 512-byte sector
3. 固定 sector layout，先不做 FAT filesystem
4. 8/16-bit 小型 bytecode interpreter
5. VM PC、register、stack bounds checking
6. YIELD/SLEEP/UART syscalls
7. 一個 native vm_runner task 載入並執行 SD bytecode
8. 再決定是否需要 FAT、external RAM 或 RV32I emulator
```

512-byte SD sector cache 已占 Uno SRAM 四分之一，所以設計時可能採 32/64-byte
window、共用 cache，或加外部 RAM。這會是很好的 OS/VM 專案，但應把「載入
bytecode」和「直接執行 native AVR code」清楚區分。

## 19. 我建議的下一條路線

Software timer、stack guard 與 panic 已完成。接下來有兩條合理路線：

1. **Task lifecycle cleanup**：記錄 task 持有哪些 mutex，禁止或處理持鎖退出；
   加入安全的 stop/restart policy。
2. **SD + VM loader**：沿上一章順序，先 raw sector，再跑自訂 bytecode task。
3. **更快的 waiter selection**：semaphore/event flags 目前按 task ID；可以改為
   priority-first 加同 priority round-robin。
4. **Task notification**：每 task 一個極小 event/value 通道，比每次建立完整
   semaphore/event object 更省 SRAM。
5. **Optional heap**：先做固定大小 block pool，不急著引入一般 `malloc/free`，
   避免 2 KiB SRAM fragmentation。
6. **應用層專案**：例如 sensor task -> queue -> logger task -> shell 查詢。這會
   真正驗證 IPC、timeout、priority 與錯誤處理，而不只是單元展示。

如果主要目標是學 RTOS kernel，下一步做 lifecycle cleanup；如果最想研究
「從儲存裝置載入程式」，下一步做 SPI + SD raw-sector reader，再接小型 VM。
