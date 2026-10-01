# ATmega OS 全系統導讀：Native Kernel、RV32I Emulator 與 Guest Loader

這份文件不是只列 API。目標是讓你能從「電源打開」一路追到某個字元從
COM6 進來、喚醒 shell、執行命令、再透過 UART interrupt 傳回筆電。

目前這個 repository 同時保存兩條路線。第一條是原本的小型 native AVR RTOS：
獨立 task stack、preemptive scheduling、sleep、task lifecycle、semaphore、
mutex、priority inheritance、message queue、event flags、UART driver 與 shell。
第二條是在 AVR firmware 裡用 C 寫的 RV32I emulator，讓 SRAM 中的 RISC-V
machine code 由軟體逐條解譯。兩者沒有互相取代；只是目前這塊 LGT8F 相容板預設
使用不切換 native stack 的 safe mode，原本的完整 kernel 仍保留在
`LGT_SAFE_MODE=0` build。

## 先把六個名字講清楚

### 1. Host firmware / 我們的 ATmega OS

這是用 AVR GCC 編譯、真正由 ATmega/LGT 硬體執行的程式，位於 Arduino Flash。
它包含 UART driver、shell、timer、原本的 scheduler，以及 RV32I emulator。
`make upload` 燒錄的 `atmega_os.hex` 就是這一層。

### 2. Shell

Shell 是 OS 裡面一個「收到文字命令，再呼叫對應 C function」的迴圈。它不是另一個
OS，也不是編譯器。`os>` 是 prompt，意思只是「Arduino 已準備好收下一行命令」。

```text
你鍵盤輸入 guest run + Enter
        │ COM6 / UART RX
        ▼
shell_task() 收集一行字串
        │ 比對到 "guest run"
        ▼
呼叫 rv_demo_run()
```

Shell 採 device echo：你按下 `g`，Arduino 收到 `g` 後才把 `g` 傳回螢幕。因此完全
沒有 echo 時，不是命令打錯，而是 Arduino/COM6 沒有回應。這時退出後使用
`make monitor-reset`。

### 3. ISA 與 RV32I

ISA（Instruction Set Architecture）是 CPU 看得懂的機器指令規格。AVR 與 RISC-V
是不同 ISA，指令 binary 不相容。RV32I 的名字可拆成：

- `RV`：RISC-V；
- `32`：register 和 address 的基本寬度是 32 bits；
- `I`：Base Integer Instruction Set，包含整數運算、load/store、branch、jump 等。

你的 Arduino CPU 不會直接執行 RV32I；它真正只會 AVR-compatible 指令。

### 4. Emulator

Emulator 是 [rv32i.c](../src/vm/rv32i.c) 編譯出的 AVR 程式。它在 C struct 裡放
32 個假 RISC-V registers 和一個假 PC，每次從 `guest_memory` 取 4 bytes，判斷這是
ADD、LOAD、JUMP 或 ECALL，再用 AVR 運算改變假的 registers/memory。

```text
真 AVR CPU 執行 rv32_step() 的 AVR 指令
                         │
                         └── 模擬一條 RV32I 指令的效果
```

所以畫面寫「RV32I guest 正在跑」，實際上是 AVR 正在跑 emulator。

### 5. Guest 與 guest C

Guest 是「被 emulator 執行的程式」。Host 是外面的 ATmega OS；guest 是住在它提供
的 192-byte 虛擬環境裡的小程式。

`guest-riscv/src/main.c` 叫 guest C，因為它是 C source，但編譯目標是 RV32I：

```text
guest C source
    │ riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32
    ▼
RV32I machine code
    │ UART load 到 SRAM
    ▼
RV32I emulator 解譯
```

它不能直接呼叫 AVR `PORTB` 或一般作業系統 `printf`。若想輸出文字，guest 執行
`ECALL` 請 host 的 UART driver 幫忙；`guest_write()` 就是這層包裝。

### 6. `exit=62`

目前 guest 的 `main()` 最後計算 `40 + 22` 並 `return 62`。RISC-V ABI 規定 function
return value 放在 `a0`。`start.S` 在 `main()` return 後把 syscall number 3 放到
`a7`，執行 `ECALL`：

```text
main() return 62
        ▼
a0 = 62
a7 = 3（EXIT）
ECALL
        ▼
emulator 停止 guest
host 印出 rv32 C guest exit=62
```

62 不是錯誤碼，也不是 OS 固定要求；它只是這份示範 C 程式算出的回傳值。你把
`main()` 改成 `return 7;`，成功執行後就會看到 `exit=7`。

## 開機畫面逐行解讀

```text
ATmega OS UART ready
shell ready; type help
os>
```

- `ATmega OS UART ready`：真正的 AVR host firmware 已啟動，UART 可以傳資料。
- `shell ready; type help`：host 現在進入文字命令迴圈。
- `os>`：prompt。游標在後面時，輸入 `guest run` 再按 Enter。

現在開機不會自動執行 guest。輸入 `guest run` 後才會看到 `guest C start`、
`guest C awake` 與 `rv32 C guest exit=62`。

如果輸入 `guest run` 後上述整段連 `ATmega OS UART ready` 都再次出現，代表 MCU
reset，不是 shell 正常刷新。目前已確認這塊 LGT 相容板採 virtual boot partition，
並有非標準 watchdog/vector 行為；詳細實機結論見本文件最後的「目前硬體除錯紀錄」。

## 先別讀後面：五分鐘把自己的 C 跑在 Arduino 上

### 你現在操作的到底是哪一份程式？

平常要修改的是：

```text
/mnt/d/atmega_os/guest-riscv/src/main.c
```

這不是 AVR C 程式。它會被 RISC-V compiler 編譯成 RV32I machine code，再透過
UART 放進 Arduino 的 SRAM，最後由 Arduino Flash 裡的 emulator 解譯。

```text
guest-riscv/src/main.c       你寫的 C
          │ RISC-V GCC
          ▼
guest-riscv/build/demo.elf   有 symbol/layout 的 RISC-V executable
          │ objcopy
          ▼
guest-riscv/build/demo.bin   要傳送的 raw machine code
          │ UART COM6，一次傳完整檔案
          ▼
uploaded_image[192]          Arduino SRAM 中的乾淨原始 image
          │ guest run 時複製
          ▼
guest_memory[192]            真正被 emulator 執行及修改的記憶體
```

### 第一次：把 OS/emulator firmware 燒進 Arduino

只有修改 outer OS、driver、emulator 後才需要做：

```bash
cd /mnt/d/atmega_os
make clean all
make upload PORT=COM6
```

這會編譯 AVR code，產生 `build/atmega_os.hex`，再用 AVRDUDE 以 115200 baud 寫入
Flash。這不是上傳 guest。現在這塊板已經燒好，平常改 guest 不必重做。

### 平常：修改 C，編譯、載入 SRAM、立刻執行

先把 [guest main.c](../guest-riscv/src/main.c) 改成例如：

```c
#include "guest_syscall.h"

static const char message[] = "hello from my guest";

int main(void)
{
    guest_write(message, sizeof(message) - 1u);
    guest_sleep(100u);
    return 7;
}
```

存檔後只執行：

```bash
cd /mnt/d/atmega_os/guest-riscv
make load-run
```

這一條命令會依序完成：

1. 用 RISC-V GCC 編譯 `src/start.S + src/main.c`。
2. 產生 `build/demo.elf` 和 `build/demo.bin`。
3. 確認程式沒有超過 192-byte guest memory。
4. 開啟 Windows `COM6`，application UART 使用 4800 baud。
5. 把 BIN 切成最多 16-byte chunks，以 hex 傳給 shell。
6. 每個 chunk 等 Arduino 回覆 `ACK`。
7. `guest commit` 比對完整長度與 CRC16。
8. 自動送出 `guest run`，emulator 從虛擬 PC=0 開始執行。

成功時會看到類似：

```text
LOAD OK bytes=... crc=....
os> guest run
hello from my guest
rv32 C guest exit=7
os>
loaded ... bytes, CRC16-CCITT=....
```

最後一行是筆電 uploader 印的；`hello from my guest` 與 exit code 是 Arduino 上的
emulator 執行 guest 後產生的結果。

### 只載入、不立刻執行

```bash
cd /mnt/d/atmega_os/guest-riscv
make load
```

它只做到 CRC commit。工具結束後 COM6 會關閉。接著開互動 shell：

```bash
cd /mnt/d/atmega_os
make monitor
```

如果 monitor 已開啟，但鍵盤輸入完全沒有 echo、Enter 也沒有出現 `os>`，先按
`Ctrl+]` 離開，再執行：

```bash
make monitor-reset
```

它會透過 COM6 的 DTR 重置 Arduino，等待約 1.8 秒讓 bootloader 結束，再接受
shell 輸入。Reset 會清除 SRAM uploaded guest，所以只有板子不回應時才使用；一般
`make monitor` 不 reset，才能保留 `make load` 放進 SRAM 的 image。

所有會開啟序列埠的 Make targets（`upload`、`upload-wsl`、`monitor`、
`monitor-reset`、`guest-load`/`load-run`）現在都會先執行 `serial-close`，關閉本專案
上一次殘留的 `serial-monitor.ps1` relay。它不會主動關閉 Arduino IDE；如果 IDE
自己的 Serial Monitor 占著 COM6，仍須手動關閉。

在 `os>` 後輸入：

```text
guest info
guest run
guest regs
```

- `guest info`：看目前 image 是 `uploaded` 還是 Flash 內建的 `builtin`。
- `guest run`：把乾淨 image 複製到 execution memory，從頭執行一次。
- `guest regs`：看停止時的虛擬 PC、sp、a0、a7。
- `guest builtin`：放棄 SRAM upload，改回 firmware 內建 image。

離開 monitor 按 `Ctrl+]` 或 `Ctrl+C`。Arduino IDE Serial Monitor、`make monitor`、
`make load-run` 三者不能同時開，因為同一時間只能有一個程式占用 COM6。

Shell 目前沒有方向鍵歷史功能。方向鍵送來的是 ANSI escape sequence（例如 ↑ 是
`ESC [ A`），shell 會整串忽略，不會再把 `[A` 顯示在 prompt。中文輸入法送出的
UTF-8 bytes 也不屬於目前只接受的 ASCII command，請切到英文輸入法輸入命令。
Windows relay 開啟 COM6 後也會先清除 input buffer，避免 AVRDUDE 的 115200 baud
切換到 application 4800 baud 時留下無法復原的舊亂碼。

### SRAM 版本的資料能留多久？

`make load-run` 不會改 Flash。拔電、按 reset 或重新燒 firmware 後，SRAM upload
就消失，系統會改回 Flash 內建 guest。要再次執行剛修改的 C，再跑一次：

```bash
make load-run
```

如果希望 guest 斷電後仍存在，目前可用 `make upload` 把它包進整份 firmware；未來
SD loader 完成後，則會從 SD 檔案重新載入。現在尚未實作 SD driver，不能使用
`sd load` 或 `guest load /file`。

### 目前 guest C 可以與不可以做什麼？

可以：

- 普通整數運算、條件、迴圈和函式呼叫；
- 小型區域變數、global/static data 和字串；
- `guest_write(text, length)` 輸出 UART；
- `guest_sleep(milliseconds)` 請 host 等待；
- `guest_yield()` 回到 host；
- `return n`：`start.S` 會把 return value 當成 EXIT code。

目前不可以：

- `printf`、檔案 API、Arduino library、`malloc`；目前沒有 libc 和 heap；
- 直接使用 `PORTB`、AVR register 或 AVR header；guest 是 RV32I，不是 AVR code；
- 使用 floating point、compressed RISC-V 或 emulator 尚未實作的指令；
- 讓 image/`.bss` 超過位址 159；linker 強制留下至少 32 bytes stack；
- 同時執行多個 RISC-V guest。現在只有一份 VM CPU state 和 execution memory。

### 現在到底有沒有 preemption？

對目前這塊 LGT 相容板的預設 `LGT_SAFE_MODE=1`，答案是：**沒有可靠的多 task
preemption**。`rv_demo_run()` 每次最多解譯四條 guest 指令便回到 host，但 safe
mode 裡只有一條 supervisor execution flow，所以這不等於兩個 guest 正在輪流跑。

若下一步要做多 guest，正確的 SRAM 版本是：每個 guest 各自保存 `rv32_cpu_t`、PC
與一小塊 memory，VM scheduler 每次各跑固定 instruction budget。這不需要逐條連線
筆電；但 Uno 只有 2 KiB SRAM，必須先決定每個 guest 的 memory 大小。SD 卡未來可
保存許多 image，卻不會自動增加正在執行中的 SRAM。

## 閱讀地圖：這份文件分成三部

- **第一部（第 1～17 節）Native AVR OS**：AVR 記憶體、task stack、context
  switch、scheduler、同步原語、UART、shell、software timer 與 panic。
- **第二部（第 18～21 節）從 native task 走向 VM**：為什麼 AVR 不能直接執行
  SRAM code、SD/VM 架構、第一個 emulator milestone，以及 LGT safe mode。
- **第三部（第 22～33 節）RV32I Emulator 實作與使用教材**：目錄、兩個 Git、
  fetch/decode/execute、ECALL、ELF、自己寫 guest、目前的上傳流程、未來 UART
  loader 與 SD 卡 loader。

第一次讀建議先看第 1～4 節建立 AVR 基礎，再直接跳第 22 節理解 emulator；需要
追 native scheduler 細節時再回頭讀第 5～17 節。

## 第一部：Native AVR OS

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

上圖描述 `LGT_SAFE_MODE=0` 的完整 native OS。Timer2 每 0.5 ms 進一次
interrupt，保存當前 task 的 CPU context，執行 scheduler，再恢復被選中的
task。這就是 preemption（搶占）。目前實機預設的 safe mode 不走這條 ISR 與
stack-switch 路徑；差異在第 21 節完整說明。

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

### 3.1 目前兩種 build 的 SRAM 用量

現在必須分清楚 safe mode 與完整 native OS，不能再只看一組數字。2026-09-10
實際 build 結果是：

| Build | Flash | 靜態 SRAM | 尚未由 linker 配置的 SRAM |
|---|---:|---:|---:|
| `LGT_SAFE_MODE=1`（目前板子預設） | 13756 | 763 | 1285 |
| `LGT_SAFE_MODE=0`（完整 preemptive OS） | 15944 | 1564 | 484 |

`avr-size` 的 Data 是 `.data + .bss + .noinit`，不是 runtime 真正的最高 RAM
用量。尚未配置的區域還要容納從 `RAMEND` 往低位址生長的 C startup/main stack；
因此絕不能把 `2048 - Data` 全部再分給 global arrays。

兩種模式都包含下列主要物件：

| 物件 | 大約 bytes | 用途 |
|---|---:|---|
| RV32 register/CPU state | 142 | x0～x31、PC、memory pointer、status、ECALL |
| RV32 guest memory | 192 | 同時放 guest code、data、bss、stack |
| Uploaded image slot | 192 | 保留 UART 載入的原始 image，讓 `guest run` 可重跑 |
| RV syscall context | 16 | syscall number、a0、a1、exit code |
| UART RX/TX arrays | 64 | 兩個 32-byte ring buffers |
| panic `.noinit` | 5 | reset 後保留 panic code/detail |

為什麼 runtime loader 需要兩份 192-byte array？`uploaded_image` 保存未執行的原始
machine code；每次 `guest run` 先把它複製到 `guest_memory`，後者才交給 CPU 執行。
guest 的 store、data 與 stack 都會修改 `guest_memory`，如果只留一份，跑過一次後
就失去乾淨 image，不能可靠重跑。執行 `guest builtin` 只把 image source 切回 Flash，
不需要清除整個 upload slot。

完整模式額外配置五份 native task stack：

| Native task | stack bytes |
|---|---:|
| LED | 160 |
| shell | 224 |
| software timer service | 160 |
| idle | 128 |
| RV demo | 128 |
| **合計** | **800** |

safe mode 只有一個 16-byte TCB 與 64-byte placeholder stack。placeholder 只是讓
既有 task 查詢介面仍有合法物件；實際 supervisor 沿用 C startup 在 `RAMEND`
建立的 main stack，所以 `stack` 指令會明確顯示 `stack=main(RAMEND)`。

完整模式的 `unused == 0` 絕不是最佳化成功，而是 stack 已經碰到配置邊界的
紅色警報。實務上要跑最壞 workload，再保留至少 16～32 bytes。watermark 的
used 也包含偽造的 37-byte 初始 frame、Timer ISR 保存的 context 與 scheduler
call frame，不只有 C 區域變數。

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
3. `uart_init()`：配置 4800 8-N-1、ring buffers 的 semaphore/mutex。
4. `led_task_init()`：清 event bits，LED 進入 blink mode。
5. 傳 boot Huffman frame。
6. `task_init()` 建立五個 TCB 和初始 stack frame。
7. `scheduler_init()` 選最高 priority 的初始 task（shell）。
8. `scheduler_start()` 用 assembly 載入該 stack，`ret` 直接進入 task entry。

目前 task ID 是 build 時的 array index：

| ID | entry | base priority | 功能 |
|---:|---|---:|---|
| 0 | `led_task` | LOW=1 | LED blink/event control |
| 1 | `shell_task` | HIGH=3 | UART 命令處理 |
| 2 | `software_timer_task` | NORMAL=2 | 執行到期 timer callback |
| 3 | `idle_task` | IDLE=0 | 沒別人能跑時提供 context |
| 4 | `rv_demo_task` | NORMAL=2 | 執行 RV32I demo，完成後 return |

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
重疊。4800 8-N-1 每 byte 約需 10 bits，也就是約 2.08 ms。

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

看到 `restart failed` 不代表 restart 壞掉；LED、shell、timer service 和 idle
都是常駐無限迴圈，不會進入 TERMINATED。`rv_demo_task` 完成後會 return，因此
可以從 shell 對 task 4 執行 `restart 4`，再次執行 emulator demo。

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

## 第二部：從 Native Task 走向可載入 Guest

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

## 20. 第一個 RV32I Emulator Milestone

目前已加入 portable C 實作的 RV32I subset。`src/vm/rv32i.c` 不依賴 AVR
register，因此同一份 decoder 可以先由筆電上的單元測試驗證，再由 `avr-gcc`
編進 firmware。這是刻意建立的除錯邊界：CPU decode 錯誤不必每次都燒錄板子，
而 Arduino 上的問題也不會和 SD driver 混在一起。

`rv32_cpu_t` 保存 32 個 32-bit guest integer registers、32-bit PC、guest memory
指標與大小、ECALL callback 和目前狀態。`x[0]` 在每條指令後強制寫回零，因為
RISC-V 的 `x0` 是硬接零的 register。Guest memory 採 little-endian，所有
instruction fetch、`LW`、`SW` 都做 4-byte alignment 與 bounds check；錯誤會
進入 fault 狀態，不允許 guest 越界讀寫 AVR SRAM。

目前支援 RV32I 整數核心中，足以讓 freestanding C 程式工作的部分：

```text
LUI, AUIPC
ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI
ADD, SUB, SLT, SLTU, XOR, OR, AND, SLL, SRL, SRA
LB, LBU, LH, LHU, LW
SB, SH, SW
BEQ, BNE, BLT, BGE, BLTU, BGEU
JAL, JALR, FENCE, ECALL
```

目前沒有 `M` extension（乘除法）、compressed `C` extension、CSR、privilege mode、
exception vector 或 virtual memory。這不是 Linux 能看見的完整 RISC-V machine，
而是一個有 bounds check 的 RV32I user-mode sandbox。

`rv32_run(cpu, budget)` 最多執行 `budget` 條 guest instructions。Budget 用完時
仍回傳 `RV32_RUNNING`，native VM task 便可呼叫 `task_yield()`，防止 guest 的
無限迴圈長期霸佔 AVR。沒有註冊 handler 的 `ECALL` 目前代表正常 halt；下一階段
會把 guest syscall number 映射到 UART、sleep、yield 和 exit。

`guest-riscv/` 是獨立 Git repository。現在 guest 不再只是手寫七條組語，而是：

```text
src/start.S + src/main.c
        ↓ riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32
154-byte raw payload demo.bin
        ↓ make export
../guest_images/demo.bin
        ↓ parent image generator
AVR Flash 裡的 PROGMEM array
        ↓ rv_demo_task 啟動
192-byte AVR SRAM guest memory
```

outer OS build 再把這份 binary 轉成 AVR Flash 裡的 `PROGMEM` array。
`src/start.S` 只做最小 C runtime：把 guest `sp` 設成 address 192，呼叫 C
`main()`，再把 `main()` 的 return value 當成 exit code。Linker 限制 image 不得
超過 160 bytes，保留最高 32 bytes 給 guest C stack。

guest C 程式透過 `a7` 放 syscall number、`a0/a1` 放參數，執行 `ECALL`：

```text
0 = yield()
1 = sleep(milliseconds)
2 = write(guest_address, byte_count)
3 = exit(code)
```

`ECALL` callback 只複製 register 並回傳 `RV32_PAUSED`，不直接呼叫 UART 或
`task_sleep()`。原因是 emulator decoder 當時仍占用 native task C stack；先讓
`rv32_run()` unwind 回 `rv_demo_task`，再執行可能 block/context-switch 的 host
service，會讓 stack lifetime 很清楚，也比較容易檢查剩餘 stack。

demo 的實際行為是印出 `guest C start`、主動 yield、sleep 25 ms、印出
`guest C awake`，最後用一個 `noinline` C function 算出 `20 + 22` 並 return 42。
host 收到 exit syscall 後印出 `rv32 C guest exit=42`。這已驗證 C calling
convention、guest stack、load/store、function call/return、四種 syscall，以及
guest 暫停後由 native OS task 恢復執行。

Host 測試涵蓋算術、負 immediate、`x0`、load/store、branch、jump、ECALL、
instruction budget、misaligned access、out-of-bounds load 與 illegal instruction：

```bash
make test-rv32i
```

修改 guest assembly 後的完整 build pipeline：

```bash
cd guest-riscv
make inspect       # 看 ELF disassembly 與 raw bytes
make export        # 更新 ../guest_images/demo.bin
cd ..
make clean all     # 重新嵌入 guest image 並編譯 AVR firmware
make upload
```

Arduino 收到的是已編譯的 RISC-V machine code，不會在板上執行 assembler。
image 可以隨 AVR firmware 一起燒到 Flash，也可以透過現在完成的 UART line loader
動態替換 SRAM image；SD reader 則仍是後續階段。

也可以在 inner repository 直接執行：

```bash
make upload
```

這個 target 會 build guest、export binary，再委託 parent Makefile build/upload；
它不是把 RISC-V `.bin` 直接交給 AVR bootloader。

目前還沒有從 SD 取指令。下一個 emulator milestone 是把單一 demo context 改成
多個 guest process context，再由 UART 或 SD loader 取代編譯期 `PROGMEM` image。

## 21. LGT8F 相容核心的 context-switch 驗收結果

這塊 Uno Type-C 板不是原廠 ATmega328P，而是 LGT8F328P 類相容核心。它可以用
`atmega328p` ABI 編譯，也能正常執行一般 C 的 `CALL/RET`；硬體 probe 亦確認
`CALL` 會讓 SP 下降 2 bytes。但在兩個獨立 SRAM stack 間反覆切換時，返回 PC 的
高位會被改成與標準 ATmega328P 不同的值，最後跳進未使用 vector 或 reset。

已分別排除：

- UART UDRE ISR；
- Timer ISR（完全關閉 tick，只讓兩個 task 主動 yield 仍可重現）；
- 特定 task 程式（兩個只呼叫 `task_yield()` 的空 task 仍可重現）；
- stack 跨過 `0x0200`；
- 未關閉的 INT0/Timer1 等 peripheral interrupt source；
- `CALL` 只壓一個 byte（實測是兩個）。

[LGT8FX8P databook](https://github.com/dbuezas/lgt8fx/blob/master/docs/LGT8FX8P_databook_v1.0.5-English.pdf)
宣稱 interrupt/return PC 是兩 bytes，但公開資料沒有描述它的「program
encryption algorithm」是否另有不能由 AVR software 讀寫的 return state。
[FreeRTOS 論壇的 LGT8F 討論](https://forums.freertos.org/t/logic-green/8541)
中也同時出現「task doesn’t switch」與改 Timer2 後「基本可跑」的回報，資料互相
矛盾。因此目前不能把這顆 clone 的長時間 stackful preemption 當成已驗收；同一
份 context port 還需要用**原廠 ATmega328P Uno**交叉驗證，或取得 LGT 專用
debug/programming 文件。

這不影響 RV32I decoder 的 host 單元測試，也不會刪掉既有 OS；它影響的是這塊
實體相容板能否可靠地在多個 native AVR stack 間切換。若只針對此 LGT 板繼續，
可考慮 stackless event loop/protothread；若目標是學傳統 preemptive RTOS，換原廠
ATmega328P 是比較能驗證標準 AVR context frame 的路線。

同一個相容性問題也會受 Flash link address 影響。Outer Makefile 目前刻意把
`uart.c`、`rv_demo_task.c`、`shell_task.c` 排在 object list 前段，讓實機頻繁呼叫的
UART、loader 與 shell functions 落在較低 Flash address。這不是標準 ATmega328P 的
一般需求，而是目前 LGT clone 的實測 workaround；不要為了讓 Makefile 看起來按
目錄排序就任意移回尾端。

### 21.1 目前實機使用的 LGT safe mode

為了讓這塊 LGT8F 相容板上的 UART shell 與 RV32I emulator 可以穩定驗收，預設
`make` 會加入 `LGT_SAFE_MODE=1`。此模式只建立一個 native AVR task：先執行一次
RISC-V guest，guest 結束後在同一個 stack 進入 shell。實測顯示這顆 clone 在
Timer2 執行期間曾進入未要求的 vector 2 與 vector 8，所以 safe mode 完全不開
global interrupt：Timer2 counter 照常運轉，但由 `timer_poll()` 消耗 compare flag；
UART RX 直接輪詢 `RXC0/UDR0`；`task_yield()` 不改 SP，只 tail-call timer polling。
因此不會執行已知不可靠的 interrupt return 或跨 stack context switch。
`main()` 也不呼叫 `scheduler_start()` 載入人工初始 frame；supervisor 從頭到尾
沿用 C startup 在 RAMEND 建立的硬體 stack。safe mode 內的 64-byte task stack
陣列只是保留 TCB/診斷介面所需的 placeholder，並不是正在執行的 stack。
因此 shell 的 `stack` 指令會顯示 `task=0 stack=main(RAMEND)`，不會把 placeholder
的 fill pattern 誤報成實際 supervisor stack 用量。

這不是刪除 scheduler，也不是把 preemptive OS 改寫掉。使用真正的 ATmega328P
時可明確編譯原本的五個 native task：

```bash
make clean all LGT_SAFE_MODE=0
```

兩種 build 的差異是：

- `LGT_SAFE_MODE=1`：給目前 LGT8F 實機；一個 AVR supervisor task，guest 與
  shell 可用，但沒有 interrupt/native task preemption，`task_sleep()` 是輪詢
  Timer2 compare flag 的定時 busy wait。
- `LGT_SAFE_MODE=0`：原本完整版本；LED、shell、software timer、idle、RV guest
  各有 private SRAM stack，由 Timer2 preemptive scheduling。

safe mode 中 guest 的 `yield` 仍然是有效的 guest/host 邊界，只是目前沒有另一個
native AVR task 可接手；guest 的 `sleep(25)` 由 Timer2 毫秒計數等待，之後才輸出
`guest C awake`。正常開機每個 guest 訊息只應出現一次，接著顯示 shell prompt。

### 21.2 Shell 的 emulator 診斷指令

除了原本的 task、stack、fault 與 LED 指令，shell 現在也可以直接重跑並檢查
內建 RV32I guest：

```text
about          顯示相容 CPU、build mode、F_CPU 與 kernel tick 設定
led status     顯示 LED mode；safe mode 會註明 blink service 未執行
guest run      清空 guest SRAM、重新載入 Flash image，從 PC=0 執行
guest info     顯示 image/memory bytes、active、status 與 exit code
guest regs     顯示虛擬 RISC-V 的 PC、sp(x2)、a0(x10)、a7(x17)
clear          傳送 ANSI clear-screen sequence
reboot         不建立 panic record，直接進入 firmware reset vector
```

guest status 使用 `rv32_status_t` 的數值：0 是 RUNNING、1 是 PAUSED、2 是
HALTED，3 到 7 分別是 fetch/load/store/misaligned/illegal-instruction fault。
`guest run` 不是跳到 AVR machine code；它重新初始化 `rv32_cpu_t`，把 image 複製
到 192-byte guest memory，再由 `rv32_run()` 解譯執行。

## 第三部：RV32I Emulator 實作與使用教材

## 22. 先分清楚 Host、Emulator 與 Guest

這個專案現在有三個不同層次的「電腦」。名稱相似，但執行位置完全不同：

| 層次 | 真正在哪裡執行 | 指令集 | 工作 |
|---|---|---|---|
| 開發用 host | 筆電的 WSL/Windows | x86-64 | 編譯 AVR firmware 與 RISC-V guest |
| AVR host firmware | Arduino 相容板 | AVR | UART、shell、timer、kernel、RV32I emulator |
| RISC-V guest | AVR 的 192-byte SRAM 陣列內 | RV32I（被模擬） | 執行你寫的 guest C/assembly |

完整資料流是：

```text
筆電上的 RISC-V source
        │ riscv64-unknown-elf-gcc
        ▼
RISC-V ELF（仍在筆電）
        │ objcopy -O binary
        ▼
RISC-V raw binary（仍在筆電）
        │ generate_guest_image.py
        ▼
AVR PROGMEM C array
        │ avr-gcc + avr-objcopy
        ▼
ATmega firmware.hex
        │ avrdude / COM6
        ▼
Arduino Flash
        │ 開機時逐 byte 複製
        ▼
guest_memory[192]（Arduino SRAM）
        │ rv32_step() 每次解譯一條
        ▼
模擬出來的 RISC-V 執行結果
```

最重要的觀念是：ATmega 的硬體 PC 從來沒有指向 `guest_memory`。AVR 硬體 PC
始終在執行 `rv32i.c` 編譯出的 AVR machine code；`rv32_cpu_t.pc` 只是一個
32-bit C 變數。emulator 用這個變數當索引，從 SRAM 取四個 byte，自己算出那條
RISC-V 指令應有的效果。

### 22.1 為什麼目錄會長這樣

目前有意義的目錄結構如下：

```text
atmega_os/
├── .git/                         外層 AVR OS repository
├── Makefile                      AVR firmware 的總 build/upload 入口
├── docs/
│   └── os-guide.md               這份全系統教材
├── src/
│   ├── main.c                    AVR reset 後的 firmware 入口
│   ├── kernel/                   native AVR kernel
│   │   ├── context.S             AVR context save/restore
│   │   ├── scheduler.c           task lifecycle 與排程
│   │   ├── timer.c               Timer2 時基
│   │   ├── semaphore.c           計數同步
│   │   ├── mutex.c               互斥與 priority inheritance
│   │   ├── event_flags.c         位元事件等待
│   │   ├── message_queue.c       固定大小訊息佇列
│   │   ├── software_timer.c      callback timer service
│   │   └── panic.c               panic record 與 reboot
│   ├── drivers/
│   │   └── uart.c                AVR UART0、RX ring、framing
│   ├── tasks/
│   │   ├── shell_task.c          命令解析與 emulator 診斷命令
│   │   ├── rv_demo_task.c        guest loader、ECALL bridge、run loop
│   │   ├── led_task.c            LED native task
│   │   └── idle_task.c           沒工作時執行的 native task
│   ├── vm/
│   │   ├── rv32i.h               虛擬 CPU state 與公開 API
│   │   └── rv32i.c               fetch/decode/execute 核心
│   └── generated/
│       ├── rv_guest_image.c       自動產生的 AVR PROGMEM guest bytes
│       └── log_messages.c         自動產生的 Huffman log bytes
├── guest_images/
│   └── demo.bin                  outer build 接收的 raw guest image
├── tools/
│   ├── generate_guest_image.py   .bin → PROGMEM C array
│   ├── generate_log_codec.py     log manifest → AVR/host codec
│   ├── wsl_serial_monitor.py     frame/Huffman decoder 與鍵盤 relay
│   ├── wsl_guest_upload.py       BIN → CRC/hex chunks → shell loader
│   └── test_rv32i.c              在筆電測 emulator 邏輯
└── guest-riscv/
    ├── .git/                     獨立的 guest repository
    ├── Makefile                  RISC-V guest build/export/upload pipeline
    ├── linker.ld                 guest memory layout
    └── src/
        ├── start.S               設 sp、call main、ECALL exit
        ├── main.c                目前的 guest application
        └── guest_syscall.h       guest 端 ECALL wrapper
```

`src/kernel` 與 `src/tasks` 是原本純 OS，沒有因為加入 emulator 而刪除。
`src/vm` 是純 CPU 解譯器；`rv_demo_task.c` 則是 OS 與 VM 之間的 glue layer。
把這兩層分開後，host unit test 可以只測 `rv32i.c`，不必模擬 UART 或 scheduler。

### 22.2 為什麼 `guest-riscv` 是「Git 裡面的 Git」

`guest-riscv/.git` 是獨立 repository，而外層 `.gitignore` 有：

```gitignore
/guest-riscv/
```

因此它目前**不是 Git submodule**。關係如下：

```text
outer atmega_os Git
├── 追蹤 src/vm、src/tasks、guest_images/demo.bin
└── 完全忽略 guest-riscv/ 內部 source/history

inner guest-riscv Git
└── 自己追蹤 linker.ld、Makefile、src/*
```

好處是 guest 能獨立開發、獨立 commit，未來可以有不同 guest 專案。風險是外層
commit 不會記錄「當時用了 inner repo 的哪個 commit」；它只記得 export 出來的
`guest_images/demo.bin`。換電腦 clone outer repo 時，也不會自動取得 inner repo。

分別檢查兩邊：

```bash
cd /mnt/d/atmega_os
git status
git log --oneline -5

cd /mnt/d/atmega_os/guest-riscv
git status
git log --oneline -5
```

若以後想讓 outer commit 精確鎖住 inner commit，可以再把它改成正式 submodule；
目前先維持兩個獨立教學專案也沒有問題。

### 22.3 開機後到底走哪一條路

目前板子的預設 `LGT_SAFE_MODE=1`：

```text
AVR reset
  → C startup 建立 main stack、清 .bss
  → main() 關閉殘留 peripheral interrupts
  → timer/uart/LED state 初始化
  → 建立一個邏輯 TCB（不載入它的人工 stack）
  → lgt_supervisor_task() 直接使用 main stack
      → rv_demo_task() 跑一次 guest，然後 return
      → shell_task() 永久等待命令
```

所以開機 guest 只跑一次；之後 `guest run` 才會再次呼叫 `rv_demo_run()`。safe mode
不會執行 `led_task()` 或 `software_timer_task()`，這正是 `led blink` 目前失效的原因；
`led on/off` 直接改 GPIO，所以仍然有效。

在真正 ATmega328P 使用 `LGT_SAFE_MODE=0` 時：

```text
AVR reset
  → main() 建立五個 TCB 與五份 private stack
  → scheduler_start() 載入第一個 task context
  → Timer2 ISR 每 0.5 ms 進 scheduler
  → LED / shell / timer service / idle / RV demo 並存
```

因此原本純 OS 並沒有被 emulator 取代；完整模式只是把 RV emulator 當成第五個
native task。safe mode 是因目前 LGT 相容核心不能可靠使用人工 return frame 與
跨 stack context switch，才暫時把可操作的 guest/shell 串成單一路徑。

## 23. 虛擬 RISC-V CPU 在 SRAM 裡長什麼樣

`rv32_cpu_t` 是一個普通 C struct：

```c
typedef struct rv32_cpu {
    uint32_t x[32];
    uint32_t pc;
    uint8_t *memory;
    uint16_t memory_size;
    rv32_ecall_handler_t ecall_handler;
    void *ecall_context;
    rv32_status_t status;
} rv32_cpu_t;
```

它不是實體 RISC-V CPU，只是「如果有一顆 RISC-V CPU，現在狀態應該是什麼」的
軟體紀錄：

- `x[0..31]`：32 個 32-bit RISC-V general-purpose registers。
- `pc`：下一條 guest instruction 的 byte address。
- `memory`：指向 AVR SRAM 裡的 `guest_memory`。
- `memory_size`：合法 guest address 範圍，目前是 192 bytes。
- `ecall_handler`：guest 要求 OS service 時呼叫的 AVR C function。
- `ecall_context`：傳給 handler 的 host-side 狀態。
- `status`：RUNNING、PAUSED、HALTED 或 fault。

常用 register 同時有 ABI 名稱：

| Register | ABI 名稱 | 目前用途 |
|---|---|---|
| x0 | zero | 永遠必須是 0 |
| x1 | ra | function return address |
| x2 | sp | guest stack pointer，初始為 192 |
| x10 | a0 | 第一參數、return value、exit code |
| x11 | a1 | 第二參數，例如 write length |
| x17 | a7 | syscall number |

AVR 自己也有 r0～r31，但那是另一組完全不同的實體 register。`cpu.x[10]` 是 SRAM
中的四個 byte，不是 AVR r10。

### 23.1 Guest 的 192-byte memory map

`rv_demo_task.c` 目前配置：

```c
static uint8_t guest_memory[192];
```

linker script 則要求 image 結尾不能超過 160，至少留下頂端 32 bytes 給 stack：

```text
guest address
0x0000  ┌─────────────────────────────┐
        │ .text.start：_start         │
        │ .text：main/functions       │
        │ .rodata：字串               │
        │ .data：有初值 global        │
        │ .bss：零初始化 global       │
≤0x009f ├─────────────────────────────┤  image end 必須 ≤ 160
        │ 未使用空間 / stack 餘裕     │
0x00bf  └─────────────────────────────┘  192 bytes 結尾
                          stack 往低位址長
```

目前 image 是 154 bytes。`load_demo_program()` 會先把全部 192 bytes 清成 0，再把
154-byte Flash image 複製到 address 0。先清零非常重要，因為 ELF 的 `.bss` 是
NOBITS：它需要零初始化，但通常不會占 raw `.bin` 的檔案內容。

每次 load/store/fetch 都經過 `address_valid()`；越界不會真的去讀任意 AVR RAM，
而是把 guest status 設成 FETCH、LOAD 或 STORE fault。這是一個軟體 sandbox，雖然
不像 MMU 那樣由硬體強制，但所有 guest memory access 都只能經過 interpreter，
所以只要 decoder 本身沒有 bug，guest 看不到 `guest_memory` 外的 AVR SRAM。

## 24. Fetch、Decode、Execute 到底如何運作

`rv32_step()` 一次只模擬一條 32-bit 指令。

### 24.1 Fetch：依虛擬 PC 取四個 byte

先檢查：

1. `pc` 必須 4-byte aligned。
2. `pc..pc+3` 必須都在 guest memory 內。
3. RISC-V 是 little-endian，所以低位 byte 放低位址。

假設 memory 是：

```text
address 0: 0x93
address 1: 0x00
address 2: 0x50
address 3: 0x00
```

`load_u32()` 合成：

```text
instruction = 0x00500093
```

這是 `addi x1, x0, 5`。

### 24.2 Decode：用 bit mask 拆欄位

RISC-V 指令把不同資訊固定放在 bit fields：

```c
opcode = instruction & 0x7f;
rd     = (instruction >> 7)  & 0x1f;
funct3 = (instruction >> 12) & 0x07;
rs1    = (instruction >> 15) & 0x1f;
rs2    = (instruction >> 20) & 0x1f;
funct7 = instruction >> 25;
```

這裡不是 C struct bit-field，因為 on-disk instruction encoding 必須精確遵守
RISC-V specification；shift/mask 的位元位置跨 compiler 都相同。

### 24.3 Execute：修改虛擬 register/memory/PC

decoder 先令 `next_pc = pc + 4`。一般算術指令修改 `cpu.x[rd]` 後採用這個
`next_pc`；branch/jump 若成立則改寫 `next_pc`。最後：

```c
cpu->x[0] = 0;
cpu->pc = next_pc;
```

每次都把 x0 清回 0，是因為真實 RISC-V 硬體會忽略對 x0 的寫入。C array 本身
沒有這個能力，所以 emulator 必須主動維持規則。

### 24.4 現在支援哪些指令

目前實作的是足夠編譯小型 freestanding C 的 RV32I 子系統：

- `LUI`、`AUIPC`；
- `ADDI/SLTI/SLTIU/XORI/ORI/ANDI/SLLI/SRLI/SRAI`；
- `ADD/SUB/SLL/SLT/SLTU/XOR/SRL/SRA/OR/AND`；
- `LB/LH/LW/LBU/LHU`；
- `SB/SH/SW`；
- `BEQ/BNE/BLT/BGE/BLTU/BGEU`；
- `JAL/JALR`；
- 基本 `FENCE`；
- `ECALL`。

目前沒有：

- `M` extension 的硬體乘除法指令；
- `C` extension 的 16-bit compressed instructions；
- CSR、privileged mode、page table、MMU；
- interrupt/trap controller；
- floating point；
- Linux 所需的完整 platform devices。

因此 guest build 必須使用 `-march=rv32i`，不能寫 `rv32im` 或 `rv32ic`。若 C
程式做一般乘除，compiler 可能想呼叫 libgcc helper；目前又使用 `-nostdlib`，所以
link 時可能報 undefined reference。第一階段請先使用加減、位移與小型 loop。

### 24.5 一次為什麼只跑四條

`rv_demo_task.c` 設定：

```c
#define RV_DEMO_BUDGET_PER_SLICE 4u
```

`rv32_run(&cpu, 4)` 最多呼叫四次 `rv32_step()`。若仍是 RUNNING，host 呼叫
`task_yield()`；若遇到 ECALL，status 變成 PAUSED，回到 host 處理 syscall。

完整 native OS 中，這讓其他 AVR task 有合作式切換機會；LGT safe mode 的
`task_yield()` 不切 SP，只會呼叫 `timer_poll()`，讓 guest 長時間運行時軟體時基
仍能前進。這個 budget 是 host scheduling granularity，不是 RISC-V clock speed。

## 25. ECALL：Guest 如何請 Host 做事

guest 不知道 AVR 的 `UDR0`、Timer2 或 shell，也不能直接呼叫 AVR function。
它把 request 放進 RISC-V ABI registers，再執行 `ecall`：

```text
a7(x17) = syscall number
a0(x10) = argument 0 / return value
a1(x11) = argument 1
```

目前 syscall table：

| a7 | 名稱 | a0 | a1 | Host 行為 |
|---:|---|---|---|---|
| 0 | YIELD | 0 | 0 | 暫停 guest slice，呼叫 `task_yield()` |
| 1 | SLEEP | milliseconds | 0 | 呼叫 `task_sleep(ms)` |
| 2 | WRITE | guest address | byte count | 驗證範圍後送 UART frame |
| 3 | EXIT | exit code | 0 | 將 guest 設成 HALTED |

guest C 端的 [guest_syscall.h](../guest-riscv/src/guest_syscall.h) 用指定 register 的
inline assembly 包裝這套 ABI。例如 `guest_write(text, length)` 並不是 C library
的 `printf`；它只是把 pointer 放 a0、長度放 a1、數字 2 放 a7，再執行 ECALL。

host 的 `handle_ecall()` 讀出 x17/x10/x11。WRITE 還會驗證：

- address 不得超出 192；
- length 不得超過 255（UART frame length 目前是一個 byte）；
- `address + length` 不得越過 guest memory。

檢查成功後，host 才能把 guest bytes送到 UART。這個邊界就是最小版 syscall/
kernel boundary。

## 26. 從 C/Assembly 到 ELF，再到 Arduino

### 26.1 `start.S` 做了什麼

guest linker entry 是 `_start`，不是 `main`。`start.S` 先執行：

```asm
addi sp, zero, 192
jal  ra, main
```

第一行建立 guest stack；第二行把 return address 放入 `ra`，跳到 C `main()`。
當 `main()` return 時，RISC-V ABI 把 return value 放在 a0。`_start` 接著：

```asm
addi a7, zero, 3
ecall
```

也就是 `exit(a0)`。後面的無限 loop 正常不會執行，只是防止錯誤 handler 回來後
掉進未知 memory。

### 26.2 `linker.ld` 決定虛擬 address

linker script 宣告：

```ld
MEMORY {
    GUEST (rwx) : ORIGIN = 0, LENGTH = 192
}
```

所以 `_start` 位於 guest address 0，而不是 AVR Flash address。`.text.start`
被 `KEEP`，避免 `--gc-sections` 認為它未被一般 C function 引用而刪除；後面依序
放 `.text`、`.rodata`、`.data`、`.bss`。最後的 ASSERT 保證至少留下 32 bytes
stack。

### 26.3 ELF 與 BIN 不同

`demo.elf` 是帶 metadata 的 executable container，裡面可有：

- ELF header：32-bit/little-endian/RISC-V、entry point；
- section headers：`.text/.rodata/.data/.bss`；
- program headers：loader 應載入哪些 byte；
- symbol table：`_start/main/message` 等名稱；
- debug 或 toolchain metadata。

`demo.bin` 則只是從最低 load address 開始連續排列的 raw bytes，沒有 magic、長度、
entry point 或 symbol。現在 `pc=0` 與 memory size 都由 firmware 事先約定，因此
raw binary 已足夠，而且比在 2 KiB SRAM MCU 上解析完整 ELF 簡單。

如果把 ELF 整份複製到 `guest_memory[0]`，CPU 會把 ELF magic
`7f 45 4c 46` 當成第一條 RISC-V instruction，結果必然 illegal instruction。

## 27. 如何自己寫並上傳 Guest

### 27.1 最簡單：修改 C guest

修改 [guest-riscv/src/main.c](../guest-riscv/src/main.c)：

```c
#include "guest_syscall.h"

int main(void)
{
    static const char message[] = "hello from my guest";
    guest_write(message, sizeof(message) - 1u);
    guest_sleep(100u);
    return 7;
}
```

編譯與檢查：

```bash
cd /mnt/d/atmega_os/guest-riscv
make clean
make inspect
```

`make inspect` 會產生並顯示：

```text
build/demo.elf    有結構與 symbol 的 RISC-V ELF
build/demo.bin    準備給 emulator 的 raw bytes
build/demo.lst    ELF 的 RISC-V disassembly
build/demo.map    linker 放置每個 section/symbol 的結果
```

確認 size 不超過 linker 限制後，一條指令完成 export、outer rebuild、COM6 upload：

```bash
make upload
```

最後：

```bash
make monitor
```

注意：這個 `make upload` 仍然會重燒整份 AVR firmware。它不是只把 154 bytes guest
送入已經運行的 shell。

loader firmware 已經燒過一次後，平常改 guest 可以改用：

```bash
make load       # 編譯 guest，載入 SRAM，但不執行
make load-run   # 編譯、載入 SRAM，接著立刻 guest run
```

這兩個 target 不會呼叫 avrdude，也不會重燒 AVR Flash。`make load` 結束後可以再開
`make monitor`，手動執行 `guest info` 與 `guest run`。注意同一時間只能有一個程式
開啟 COM6，所以執行 `make load` 前要先用 Ctrl+] 關閉 monitor。

### 27.2 自己寫純 RISC-V assembly

目前真正存在的是 `src/start.S` 與 `src/main.c`；IDE 若還開著舊的 `demo.S` tab，
那不是現在 Makefile 的輸入。可以建立 `src/my_guest.S`：

```asm
.option norvc

.section .text
.globl main
.type main, @function

main:
    la   a0, message
    li   a1, message_end - message
    li   a7, 2
    ecall

    li   a0, 42
    ret

.size main, .-main

.section .rodata
message:
    .ascii "hello from assembly guest"
message_end:
```

保留 `start.S`，只用 assembly 取代 C main：

```bash
cd /mnt/d/atmega_os/guest-riscv
make clean
make SRC="src/start.S src/my_guest.S" inspect
make SRC="src/start.S src/my_guest.S" upload
make monitor
```

`.option norvc` 禁止 assembler 產生 RVC 16-bit instruction；`-march=rv32i` 也會
再次限制 ISA。`la/li/ret` 是 assembler pseudo-instructions，assembler 會把它們
展開成目前 emulator 支援的 `AUIPC/ADDI/JALR` 等真正指令。

### 27.3 混合 C 與 assembly

也可以讓 C `main()` 呼叫 assembly function。例如 assembly export：

```asm
.globl add_three
add_three:
    addi a0, a0, 3
    ret
```

C 宣告：

```c
extern unsigned long add_three(unsigned long value);
```

build 時：

```bash
make clean
make SRC="src/start.S src/main.c src/math.S" inspect
```

這會實際練到 RISC-V calling convention：第一參數與 return value 都使用 a0。

### 27.4 如何看懂你產生的 ELF

工具不一定在 PATH，可以先設定專案專用變數：

```bash
RV_PREFIX=/home/louis/.local/toolchains/riscv64-unknown-elf/usr/bin/riscv64-unknown-elf-
```

查看 ELF header：

```bash
${RV_PREFIX}readelf -h build/demo.elf
```

你應確認 `Class: ELF32`、`Data: little endian`、`Machine: RISC-V`。

查看 sections 與 symbols：

```bash
${RV_PREFIX}readelf -S build/demo.elf
${RV_PREFIX}readelf -s build/demo.elf
```

查看 program headers 與 load mapping：

```bash
${RV_PREFIX}readelf -l build/demo.elf
```

反組譯：

```bash
${RV_PREFIX}objdump -d -M no-aliases,numeric build/demo.elf
```

`no-aliases` 會把 `ret/li/mv` 顯示成真正 ISA 指令，適合拿來對照 decoder。

## 28. Outer Makefile 如何把 Guest 包進 Firmware

inner `make upload` 的實際步驟是：

1. RISC-V GCC link 出 `guest-riscv/build/demo.elf`。
2. RISC-V objcopy 產生 `guest-riscv/build/demo.bin`。
3. `test` 檢查 raw binary 不超過 192 bytes。
4. `make export` 複製成 outer 的 `guest_images/demo.bin`。
5. outer `generate_guest_image.py` 讀取 bytes。
6. 產生 `src/generated/rv_guest_image.c/.h`。
7. AVR GCC 把該 array 放到 `PROGMEM`，和 kernel/emulator 一起 link。
8. AVR objcopy 產生 `build/atmega_os.hex`。
9. avrdude 經 COM6 寫入 Arduino Flash。

生成檔大致是：

```c
#define RV_GUEST_IMAGE_SIZE 154u

const uint8_t rv_guest_image[RV_GUEST_IMAGE_SIZE] PROGMEM = {
    0x13u, 0x01u, ...
};
```

不要手改這個 C array；下一次 generator 執行會覆蓋它。應該修改 inner source，
讓 build chain 重建 generated file。

切換 safe/full mode 時一定使用 `make clean`，因為 Make 不會把 command-line macro
值自動當成 object dependency：

```bash
# 目前 LGT 板
make clean all

# 真 ATmega328P 上驗證原本完整 OS
make clean all LGT_SAFE_MODE=0
```

## 29. 現在 Shell 能如何操作 Emulator

```text
guest run
```

會清空 guest memory、重新從 Flash 複製 image、清空 registers、令 PC=0，然後跑到
EXIT 或 fault。它不是從上一次 PC 繼續。

```text
guest info
```

範例：

```text
guest image=154 memory=192 active=0
status=2 exit=42
```

- `active=1`：某個 native execution flow 正在使用這份 CPU state。
- status 0：RUNNING。
- status 1：PAUSED，通常正在等 host 處理 ECALL。
- status 2：HALTED，正常 EXIT。
- status 3～7：fetch/load/store/misaligned/illegal-instruction fault。

```text
guest regs
```

範例：

```text
pc=12 sp=192 a0=42 a7=3
```

這是 guest 停下時的虛擬 register snapshot。`pc=12` 表示最後的 ECALL 已 decode；
`a7=3` 是 EXIT，`a0=42` 是 exit code。

## 30. 「上傳 Guest」現在能做什麼、還不能做什麼

先用這張表釐清專案目前的真實狀態：

| 能力 | 目前狀態 | 資料最後放哪裡 | 是否重燒 AVR |
|---|---|---|---|
| C/assembly 編成 RV32I ELF/BIN | 已完成 | 筆電的 `guest-riscv/build/` | 否 |
| BIN export 到 outer repo | 已完成 | `guest_images/demo.bin` | 否 |
| BIN 包成 `PROGMEM` image | 已完成 | AVR firmware 的 Flash image | 尚未；只是 build |
| `make upload` | 已完成 | guest 隨整份 firmware 進 AVR Flash | **是** |
| 開機把 image 載入 guest SRAM | 已完成 | `guest_memory[192]` | 否 |
| shell `guest run/info/regs` | 已完成 | 操作同一份 SRAM guest state | 否 |
| UART runtime `make load/load-run` | **已完成並實機驗收** | SRAM `uploaded_image[192]` | 否 |
| SD/SPI/FAT loader | 尚未實作 | 未來由 SD 讀入 `guest_memory` | 否 |

專案不採用「每條指令都向筆電請求」的 UART remote-fetch 模式。那種模式把 UART
round trip 放進 CPU fetch path；只要序列埠 reset、relay 延遲或漏掉一行，整個 VM
就會卡在等待指令。UART 在這裡只負責一次性傳送完整 BIN；commit 成功後，fetch、
decode、execute 都在 Arduino 本地完成。未來 SD 卡也採相同原則：先把可執行 image
載入 SRAM，或以本地 block cache 讀 SD，不依賴筆電逐指令回覆。

目前 application UART 使用 4800 baud；AVRDUDE 與 bootloader 仍使用 115200 baud。
這塊板在 9600 與 38400 都實測到 frame/command byte corruption，所以 SRAM loader
優先選擇可靠性。`make load-run`、`make monitor` 會自動使用
`MONITOR_BAUD=4800`，不用手動傳參數。

所以現在有兩種真正不同的「上傳」：

```text
make upload   → guest 嵌入 firmware → avrdude → AVR Flash
make load     → 已運行的 shell loader → SRAM uploaded_image
```

第一種斷電後仍存在；第二種已經具備「kernel 運行時載入 executable image」的核心
概念，但 SRAM 斷電即失，而且一次只保存一個 guest。重新開機後 source 會回到
`builtin`。

編譯期嵌入流程是：

```text
改 guest source → 產生 ELF/BIN → 重新 build AVR → avrdude 重燒
```

### 30.1 現在的 UART runtime loader 如何運作

PC 工具不把 raw binary 直接混進互動文字，而是使用可恢復、可觀察的 line protocol。
以目前 154-byte guest 為例，實際交換如下：

```text
os> guest load 154 3f5b
READY chunk=16 encoding=hex
os> guest data 0 1301000cef0080019308300073000000
ACK bytes=16
os> guest data 16 6f0000003305b50067800000130101ff
ACK bytes=32
...其餘 chunks...
os> guest data 144 74204320737461727400
ACK bytes=154
os> guest commit
LOAD OK bytes=154 crc=3f5b
```

三種命令分別對應 transaction 的三個階段：

- `guest load <size> <crc>`：檢查 1～192 bytes，取消舊 session，建立新 session；
- `guest data <offset> <hex>`：offset 必須等於目前 received count，一行最多 16 bytes；
- `guest commit`：只有 byte count 與 CRC 都吻合才把 upload slot 標成有效 image。

`wsl_guest_upload.py` 在 PC 計算 CRC16-CCITT-FALSE（initial value `0xffff`、polynomial
`0x1021`），把每個 binary byte 編成兩個 hex characters，再逐行等待 ACK。hex 讓
154-byte image 變成 308 個傳輸字元，效率只有 raw binary 的一半，但有幾個很適合
目前硬體的優點：

- 不會讓 NUL、CR、LF 或 UART frame escape 被誤認成 shell 控制字元；
- 每行短於 64-byte shell line buffer；
- 16-byte chunk 小於 32-byte RX ring 的有效容量；
- offset 能抓到遺失、重複或順序錯誤的 chunk；
- 失敗時 shell 不會卡在「還要等幾個 raw bytes」的特殊狀態；
- 全部成功前 `uploaded_image_valid` 都不會設為 1。

這叫 transactional load：begin 只是準備，data 只是暫存，commit 才讓新 image
生效。某個 data line 錯誤會取消整個 session；要從 `guest load` 重新開始。

`guest info` 現在會顯示 source：

```text
guest image=154 memory=192 active=0 source=uploaded
```

執行 `guest builtin` 會切回編譯在 Flash `PROGMEM` 的 image。這不會重開機：

```text
os> guest builtin
ok
os> guest info
guest image=154 memory=192 active=0 source=builtin
```

`make load-run` 已用包含 NUL bytes 的完整 154-byte RV32I image 在 COM6 實機驗收：
十個 chunks 全部 ACK、device/PC CRC 同為 `3f5b`，隨後 guest 正常 write、sleep 並
exit 42。這證明載入的不是文字內容，而是還原後的原始 RISC-V machine code。

### 30.2 Runtime loader 應傳 ELF 還是 BIN

第一版傳 BIN 比較合理：

這裡的「傳 BIN」指 logical payload 是 `demo.bin`。UART transport 把每個 byte 暫時
表示成兩個 hex characters，AVR 收到後立刻還原；它沒有把 C source、assembly 或
ELF metadata 傳進板子。

- ELF header + section/program header 會浪費傳輸與暫存空間；
- 現在 memory base 固定 0、entry 固定 0；
- PC 端工具比較有 RAM，應先驗證 ELF，再抽出 flat image；
- AVR 只需要 length、bytes、CRC。

等 SD loader 或多 segment executable 出現後，再讓 AVR 支援 ELF32 program
headers。不要一開始就做 section name/symbol/debug parser；runtime loader 真正需要
的是 `PT_LOAD` segments，不是 linker/debug sections。

## 31. 未來接 SD 卡時，整條路會怎麼變

### 31.1 硬體不是插上就能讀檔

一般 microSD 使用 3.3 V signaling。若是裸卡座，5 V AVR 輸出不能直接接卡；應
使用可靠的 3.3 V regulator 與 level shifting。某些 Arduino SD module 已經包含，
但必須看模組電路，不能只看商品名稱。

ATmega328P 的硬體 SPI pins 通常是：

| Arduino pin | AVR pin | SD SPI signal |
|---|---|---|
| D10 | PB2 | CS（可自行選 GPIO，但必須維持 SPI master 條件） |
| D11 | PB3 | MOSI |
| D12 | PB4 | MISO |
| D13 | PB5 | SCK |

目前 built-in LED 也使用 PB5/D13，所以 SD clock 會讓 LED 跟著閃，而且 LED 電路
可能影響高速訊號。加入 SD 時應停用/搬移 LED 功能，不要同時把 PB5 當普通 LED
輸出與 SPI SCK。

### 31.2 軟體要分五層

```text
shell command：guest load /DEMO.BIN
        ▼
guest loader：驗證長度/entry，寫 guest_memory
        ▼
file layer：open/read/seek
        ▼
FAT16/FAT32 layer：directory、cluster chain
        ▼
SD block driver：CMD、sector read
        ▼
AVR SPI driver：SPCR/SPSR/SPDR
```

每層先單獨驗收，否則「讀不到 guest」時會不知道是 wiring、SPI、SD command、FAT、
ELF 還是 emulator 的錯。

### 31.3 建議的 SD 實作順序

1. 寫 SPI byte transfer，低速初始化、高速資料模式。
2. 送 SD init commands，能讀出一個指定的 512-byte sector。
3. 暫時不做 FAT：把 `demo.bin` 放到已知 raw sector，載入並執行。
4. 加唯讀 FAT boot sector/BPB parser。
5. 支援根目錄找固定 `DEMO.BIN`。
6. 支援 cluster chain，讓檔案不必連續配置。
7. shell 加 `sd info`、`sd ls`、`guest load <file>`。
8. 最後才考慮 ELF32 `PT_LOAD` streaming loader。

第 3 步故意先跳過 filesystem，因為它能把問題切成兩半：先證明 SD sector → SRAM
→ emulator 正確，再處理 FAT directory。

### 31.4 SD 也不會讓 AVR 直接執行檔案

正確流程仍是：

```text
SD file bytes
      │ SPI read
      ▼
guest_memory in SRAM
      │ rv32_cpu_t.pc 當索引
      ▼
rv32_step() 解譯
```

SD 只是大容量非揮發儲存裝置，不是 executable memory。AVR hardware PC 仍然只執行
Flash 裡的 emulator。

### 31.5 若未來真的從 SD load ELF

只需要處理 ELF32 loader 必要部分：

1. 讀 52-byte ELF32 header。
2. 驗證 magic、32-bit、little-endian、`EM_RISCV=243`。
3. 驗證 entry 4-byte aligned 且落在 guest memory。
4. 依 `e_phoff/e_phnum/e_phentsize` 逐一讀 program header。
5. 只處理 `PT_LOAD`。
6. 把 file offset 的 `p_filesz` bytes 複製到 guest `p_vaddr`。
7. 把 `p_memsz - p_filesz` 清零，這就是 `.bss`。
8. 驗證每個 segment 都完全位於 0..191。
9. 所有 segment 成功後才設定 PC=`e_entry` 和 READY。

這可以 streaming，不需要把整份 ELF 放 SRAM。SD sector 是 512 bytes，而 safe
build 現在使用 763/2048 bytes 靜態 SRAM，尚餘 1285 bytes，但還要保留 main
stack；一份 512-byte cache 理論上
放得下，仍應以 map 與 stack worst-case 驗證。完整 preemptive build 只剩 484 bytes，
連完整 sector buffer 都放不下，因此必須共用 buffer、縮 stack、分段讀取或先取消
192-byte uploaded-image slot，不能直接再宣告 `uint8_t sector[512]`。

## 32. 這條路距離「跑 Linux」還差多少

目前 emulator 是很小的 RV32I user-style machine，不是 Linux platform。Linux
通常還需要遠超過 2 KiB 的 RAM、privileged ISA、CSR、trap/interrupt、timer、虛擬
memory 或明確的 no-MMU port、console/block device、boot protocol 等。即使能從 SD
讀 Linux image，記憶體容量也不代表能跑。

這個專案真正適合的目標是：

- 自己寫 freestanding RISC-V guest；
- 理解 ELF、ABI、loader、syscall；
- 做一到數個極小 VM process；
- 從 UART/SD 動態替換 guest；
- 建立 fault isolation 與 debugger shell。

若未來增加多 guest process，每個完整 `rv32_cpu_t` 約 142 bytes，再加各自 memory
很快就吃完 2 KiB SRAM。因此比較實際的是一次只 resident 一個 guest、切換時存到
SD，或縮小 register/state representation；不是直接複製桌面 OS 的 process model。

## 33. 建議你照著做的練習順序

1. 修改 C guest 的字串與 exit code，跑 `make inspect`，在 `.lst` 找到 a0/a7。
2. 寫純 assembly guest，只做 `write` 然後 `return 42`。
3. 故意 load address 193，觀察 `guest info` 的 LOAD fault。
4. 故意放非法 opcode，觀察 ILLEGAL fault 與 PC。
5. 在 host test 新增一條 instruction testcase，再改 `rv32_step()`。
6. 實作 shell `guest reset` 與 `guest step`，把 run 與 step debugger 分開。
7. ~~實作 UART chunk loader，先傳 BIN。~~ 已完成：hex transport、offset、ACK、CRC。
8. 實作 SD raw-sector loader。
9. 加唯讀 FAT 與 `guest load <filename>`。
10. 最後實作 streaming ELF32 `PT_LOAD` loader。

每一步都只增加一個未知數。UART runtime loader 已完成，現在改 guest 不必重燒
AVR firmware。下一個適合學 emulator 內部的 milestone 是 `guest reset/step` 小型
debugger；若要直接前進到外部儲存，則是先做 SPI transfer 與 SD raw-sector read，
暫時不要同時加入 FAT 和 ELF parser。
# 目前硬體除錯紀錄：隨機 reboot 與 UART guest upload

> 本節記錄 2026-09-11～2026-09-13 對目前這塊 Uno Type-C clone 的實機結果。它不是
> 一顆可以完全當成原廠 ATmega328P 看待的板子。

## 先講結論

這次看到的現象其實有兩層：

1. **UART 傳輸確實會損壞 byte**：在 9600 baud 可重複看到 command、ACK length
   或 CRC 的單一字元改變；AVRDUDE 也曾在 Flash 寫到 89% 時失去 programmer。
   所以這不只是 Huffman decoder 或 RV32I emulator 的錯。
2. **控制流程也曾跑到 vector 1**：panic record 實測為
   `panic_code=3 detail=1`。LGT 與 ATmega328P 的 vector 1 都是 INT0，但 firmware
   已經 `cli()` 且清掉 EIMSK，因此這更像 return PC／控制流遭破壞後落到 vector
   address，而不能直接斷言真的發生 INT0 interrupt。
3. **`invalid Huffman frame` 是次生症狀**：舊 decoder 收到被 reset 截斷的 frame
   後仍沿用舊長度，才會把下一次 boot frame 當成前一包 payload。現在每包都有
   `00 7e` sync marker，重啟後能丟掉殘包重新同步。

這塊 LGT bootloader 沒有 ATmega328P 那種真正獨立的 boot section。它會
把 application 的 reset vector 搬到 watchdog vector，再把 reset vector
改成跳往 bootloader。因此 WDT vector 在這塊板上不是普通的 application
ISR；直接套用標準 Uno 的 vector 假設會得到錯誤結論。

`make upload` 保留 `-V` 是因為這個 USB bridge 的 read-back 不可靠。但實測也曾在
write 89% 時直接 `programmer is not responding`；第二次完全相同的命令才寫完。
這是 USB bridge、線材、供電或板本身不穩的獨立證據。要證明 Flash 與 `.hex`
byte-for-byte 相同，應使用可靠的 ISP programmer；若換原廠 ATmega328P Uno、USB
線或 USB port 後問題消失，就能把 remaining fault 限縮到目前這塊 clone。

## 這次加入的保護

- LGT safe mode 的 UART 不再呼叫尚未啟動的 scheduler mutex。
- loader 會逐塊等待 ACK，上一塊尚未處理完以前不會送下一塊。
- Arduino 每收一塊 guest，不只回覆 byte count，還會重新從
  `uploaded_image[]` 讀回並計算 CRC：

  ```text
  ACK bytes=48 crc=e5dd
  ```

- commit 時同時檢查 parser running CRC 與 SRAM read-back CRC。
- PC uploader 遇到 `LOAD ERROR` 或 reboot banner 會重開整個 transaction，
  不會在已經失去同步的 offset 上繼續寫。
- application UART 降為 4800 baud；chunk 增至 16 bytes 以縮短整批暴露在不穩
  連線上的時間，仍逐塊驗證 SRAM prefix CRC 並支援覆寫重送。
- UART frame 加入 `00 7e` sync marker；已知 FE/DOR/UPE 錯誤的 RX byte 會丟棄，
  `faults` 可顯示 `uart_err=<count>/<flags>`。
- guest exit code 不再硬寫成 42。現在 `40 + 22` 的正確輸出是：

  ```text
  rv32 C guest exit=62
  ```

## 現在如何測試

先燒 host OS：

```bash
cd /mnt/d/atmega_os
make clean upload
```

再編譯、上傳並執行 guest：

```bash
cd /mnt/d/atmega_os/guest-riscv
make clean load-run
```

成功的完整條件不是只看到 `LOAD OK`，而是以下四件事都成立：

1. 每一行 `ACK bytes=N crc=xxxx` 都符合 PC 計算值。
2. 最後出現 `LOAD OK bytes=154 crc=d633`（數值會隨 guest 改變）。
3. guest 印出預期文字。
4. 出現正確 exit code，最後回到 `os>`。

手動 shell 仍使用：

```bash
cd /mnt/d/atmega_os
make monitor-reset
```

在 `os>` 後輸入 `help`、`guest info` 或 `guest run`；所有普通命令都應
逐字 echo。

## 接下來不要混在一起除錯

在目前 clone 上繼續做 emulator 功能以前，應先做 20 次最小硬體 soak
test，只測 `help`、UART 收發與 SRAM CRC，不執行 RV32I。若仍有控制流跑
飛，就應換原廠 ATmega328P 或改用 LGT 專用 core／ISP programmer；否則
每次改 linker 順序都可能看似「修好」，其實只是把壞點移到別處。

# 兩個 C Guest 與統一 Make 指令

> 2026-09-13 更新：下面先記錄兩個固定 slot 的演進。現行版本已改為具名 guest
> registry；操作方式以文件最後的「具名 Guest Registry」為準。

## 從專案根目錄完成所有操作

現在不必再 `cd guest-riscv`。以下指令都在 `/mnt/d/atmega_os` 執行：

```bash
# 編譯並燒錄 AVR host OS；不會執行 guest
make upload

# 只編譯第一個 C guest
make guest-build

# 編譯第一個 guest、UART 載入 slot 0、執行它
make guest-run

# 顯示第一個 guest 的 binary 與 RISC-V disassembly
make guest-inspect

# 只編譯第二個 C guest
make guest2-build

# 把第二個 guest 載入 slot 1，但先不執行
make guest2-load

# 編譯兩個 guest、分別載入 slot 0/1，最後同時執行
make guest-dual-run

# 開啟互動 shell
make monitor
```

所有會碰 COM6 的 Make target 都先執行 `serial-close`，關掉之前殘留的本專案
PowerShell serial relay。它不能關閉 Arduino IDE、PuTTY 等其他程式持有的 COM6；
若仍出現 `UnauthorizedAccessException`，必須自行關閉那個外部程式。

`make guest-dual-run` 的預期結果是：

```text
guest B start
guest C start
guest B awake
rv32 guest1 exit=7
guest C awake
rv32 guest0 exit=62
os>
```

實際先看到 B 或 C 不是 API 保證；重要的是兩個 guest 的輸出交錯、兩者都有自己的
exit code，且最後 shell prompt 回來。

## `guest-riscv/src/main.c` 逐段說明

這個 `main()` 不是 Arduino/AVR firmware 的 `main()`。它是要被 RISC-V compiler
編譯成 RV32I machine code，然後由 AVR 上的 `rv32i.c` 解譯的 guest 程式：

```c
static const char start_message[] = "guest C start";
static const char awake_message[] = "guest C awake";
```

字串會成為 guest binary 的資料，位址落在 guest 自己的 0..191 address space。
它不是 AVR C pointer；`guest_write()` 會把這個數字當作 guest address 交給 host。

```c
static __attribute__((noinline)) uint32_t add(uint32_t left, uint32_t right)
{
    return left + right;
}
```

`noinline` 要 compiler 保留一次真正的 function call，方便在 `.lst` 觀察 RISC-V
calling convention：參數通常在 `a0/a1`，return value 在 `a0`，`ra` 保存返回位址。
這不是功能上的必要條件。

```c
volatile uint32_t left = 40u;
volatile uint32_t right = 22u;
```

`volatile` 在這個教學程式中是為了阻止 compiler 直接在 build 時把 `40 + 22`
常數折疊成 62，讓 emulator 確實執行 load/add。它不表示這些變數由 interrupt
共享，也不提供 atomicity。

```c
guest_write(start_message, sizeof(start_message) - 1u);
guest_yield();
guest_sleep(25u);
guest_write(awake_message, sizeof(awake_message) - 1u);
return (int)add(left, right);
```

這四種事件的意思是：

1. `guest_write`：把 syscall number 2 放進 RISC-V `a7`，字串 address/length 放進
   `a0/a1`，執行 `ecall`。Emulator 暫停，AVR 驗證範圍後透過 UART 印字。
2. `guest_yield`：syscall 0，主動表示現在可以換另一個 guest。
3. `guest_sleep(25)`：syscall 1，只讓這個 guest 等 25 ms；另一個 guest仍可跑。
4. `return 62`：C `main` 把 62 放在 RISC-V `a0` 並 return 到 `start.S`。
   `_start` 再發 syscall 3（EXIT），host 才把該 slot 標成完成並印 exit code。

`start.S` 先把 guest 的 `sp` 設成 192。因為 RISC-V stack 向低 address 成長，而
code/data 從 address 0 向上擺，所以兩者會從 guest memory 的兩端朝中間靠近。
目前沒有 MMU 或硬體 protection；image 越大，留下的 stack 就越少。第一個 image
是 154 bytes，表面只剩 38 bytes 給 runtime stack，因此這仍是刻意極小的 demo，
不是一般 C runtime 環境。

## 兩個 Guest 在 SRAM 裡是什麼

Safe mode 配置兩份完全獨立的 slot。每份主要包含：

```text
slot 0: RV32 registers/PC/status + uploaded image[192] + working memory[192]
slot 1: RV32 registers/PC/status + uploaded image[192] + working memory[192]
```

`uploaded_image` 是 UART loader 已驗證的原始副本；每次 run 前複製到 `memory`，所以
guest 修改自己的 RAM 後仍可重新執行原始 image。這用 RAM 換取簡單可靠的 restart。
目前 safe build 靜態 SRAM 是 1331/2048 bytes，尚餘 717 bytes 給 AVR main stack
及執行期餘裕。完整 native scheduler build只配置一個 VM slot，靜態 SRAM 是
1567/2048 bytes，因為它還包含多個 AVR task stack。

兩個 slot 不是兩個真正 RISC-V CPU，也不是兩個 AVR task。同一顆 AVR 依序做：

```text
slot 0 最多解譯 4 條 RV32I 指令
  -> slot 1 最多解譯 4 條 RV32I 指令
  -> 經過 1 ms，處理各自 sleep countdown
  -> 重複，直到兩者 EXIT 或 FAULT
```

「最多四條就換人」代表 guest 沒有呼叫 `yield()` 也會被強制切走，從 guest 的觀點
屬於 preemptive time slicing。實作上切換的是 `rv32_cpu_t`（假的 registers、PC 與
status），不是用 `context.S` 切 AVR SP。這正好避開目前 LGT clone 不可靠的 native
AVR context switch；在真正 ATmega328P 上，原本的 AVR preemptive scheduler 仍由
`make LGT_SAFE_MODE=0` 保留。

## Shell 手動操作兩個 slot

平常請使用 Make target 傳 binary，因為手打 hex 很容易錯。對應的 shell protocol
是：

```text
guest load <size> <crc>     開始上傳 slot 0
guest load1 <size> <crc>    開始上傳 slot 1
guest data <offset> <hex>   傳目前 session 的下一塊資料
guest commit                驗證長度與 CRC，提交 image
guest run                   只執行 slot 0
guest runall                輪流執行 slot 0 與 slot 1
```

`guest data` 沒寫 slot number，因為 `guest load`/`load1` 已建立一個帶 slot 的 upload
session。一次只能有一個 upload transaction，但提交後兩個 image 可同時留在 SRAM。

## 偶發 reset：這次能確定與不能確定的事

這輪在實機完成燒錄、兩份逐 chunk SRAM read-back CRC、雙 guest 執行與正確 exit。
另外 safe-mode 的所有毫秒 delay 與每個 VM instruction slice 都會執行 `WDR`，避免
bootloader 若意外留下 watchdog 時，長時間 sleep/執行 VM 期間因沒有餵狗而 reset。
啟動的 `.init3` 與 `main()` 也都會清 reset cause 並停用 WDT。

這是防護，不代表目前已證明所有數分鐘後 reset 都消失。若再次看到 boot banner，
立刻輸入：

```text
faults
```

常見 `MCUSR` bit 值可相加：1=power-on、2=external reset、4=brown-out、8=watchdog。
例如 8 強烈指向 watchdog；4 則先查 USB 線、5 V 電源與周邊負載。若顯示 0，這塊
LGT 相容晶片的 bootloader/vector 映射可能使標準 ATmega 的 `MCUSR` 證據不可靠，
也可能根本不是硬體 reset，而是控制流程跳回 address 0。要真正定案，應保存每次
banner 前後的完整 serial log，並做至少數分鐘 soak test；若問題隨 link layout
漂移，最有效的對照組仍是真正 ATmega328P 或 ISP programmer。

# 具名 Guest Registry：指定 C、萬用字元與 Priority

## 指定一個 `.c` 編譯及上傳

檔名去掉 `.c` 就是 shell 裡的 guest name：

```bash
cd /mnt/d/atmega_os
make guest-upload GUEST_C=guest-riscv/src/second.c
```

它產生 `guest-riscv/build/second.elf`、`second.bin`，再將 BIN 上傳成名為
`second` 的 resident guest；只上傳，不執行。舊的立即執行模式仍可用：

```bash
make guest-run GUEST_C=guest-riscv/src/second.c
```

## 用 `*` 選擇多個獨立 Guest

```bash
make guest-selected-upload GUEST_CS='guest-riscv/src/*.c'
make guest-selected-upload GUEST_CS='guest-riscv/src/test_*.c'
```

引號要保留。每個 `.c` 都必須各自有 `main()`，會被當成獨立 executable；這不是
把多個 C source link 成同一個程式。Guest name 只能是 1..11 個小寫英文字母、
數字、`_`、`-`。

Batch uploader 遇到普通 CRC/字元錯誤會重傳目前 image；若看到 boot banner，代表
SRAM 中較早上傳的 guest 已消失，工具會從第一個 selected guest 重新建立整批。

## 在 `os>` 選擇要執行的 Guest

```text
guest list
guest second -run
guest second -priority 3
guest -runall
```

- `guest list`：列出 name、priority、image bytes。
- `guest NAME -run`：只執行指定名稱。
- `guest NAME -priority N`：設定 0..3。
- `guest -runall`：排程所有 resident guests，直到全部 exit/fault。

Priority 沿用 native scheduler：0=IDLE、1=LOW、2=NORMAL、3=HIGH。每 1 ms 選
runnable 中最高 priority，每次最多解譯 4 條 RV32I 指令；同 priority 使用
round-robin。高 priority 若永遠不 sleep/block，低 priority 可能 starvation；
`guest_yield()` 只保證重新排程，不保證讓低 priority 執行。

## 為什麼預設是三個，不是假裝無限個

Safe build 現在預設 `RV_GUEST_SLOT_COUNT=3`，也可在 build 時調整：

```bash
make clean all RV_GUEST_SLOT_COUNT=2
```

每個 guest 都需要 32 個 32-bit registers、PC/status/syscall state、192-byte address
space、name 與 scheduling metadata。為了從兩個提升到三個，現行版本取消每個 slot
額外的 `uploaded_image[192]`，直接在 execution memory 執行。代價是 writable
globals/stack 會保留；要完全恢復乾淨 image 就重新 upload。

四個 slot 雖曾通過 linker，卻只剩約 309 bytes 給 AVR call stack，實機很快出現
狀態破壞，因此不能當安全設定。三個 slot 再移除 safe mode 中未使用的 native
TCB/stack 後，靜態 SRAM 約 1288/2048 bytes。要更多 resident processes，必須把
inactive images 放到 SD，或換 SRAM 更大的 MCU。

## 目前 LGT/USB-UART 限制

Uploader 已把 chunk 從 16 降為 4 bytes，並保留 CRC/read-back/retry。4 bytes 不是
任意選的：三位數 offset 的完整 `guest data` command 約 24 bytes，明確小於 UART
RX ring 可用的 31 bytes；原本 8-byte payload 會形成 32-byte command，存在 overflow
邊界。相同 offset 與內容現在也可安全重送；ACK 字元若損壞，host 不必從頭開始。

批次 uploader 只開啟 COM6 一次，避免每個 guest 之間重開 SerialPort 所造成的 DTR
reset。如果途中真的看到 boot banner，代表 SRAM resident images 已被清除，工具會
自動從整批第一個 guest 重建。實機曾成功
完成具名 `main`、`second` 上傳、priority 修改與交錯執行；但這塊 LGT 相容板仍會
偶發 output byte 損壞或無 reset cause 的 boot banner。若 batch 失敗，先分辨 log
是 C compile error、`LOAD ERROR`、CRC mismatch，還是 boot banner 重新出現。

# 多步驟 Guest Workload 與具名輸出

## 三個範例問題

`guest-riscv/workloads/` 現在有三支彼此獨立的 C guest：

- `fibonacci.c`：從 `(0, 1)` 開始做 24 次 Fibonacci 狀態更新。
- `checksum.c`：用 shift、XOR 與 round number 做 48 次 checksum 更新。
- `bitwalk.c`：做 36 次 left shift/XOR，觀察 bit pattern 演進。

它們都不是一次 `rv32_step()` 就能算完。每支程式先輸出 `start`，迴圈一半輸出
`half`，完成後輸出 `done`，最後以計算結果低 7 bits 作為 exit code。以 Fibonacci
為例，核心結構是：

```c
uint32_t left = 0u;
uint32_t right = 1u;

guest_write(&phases[0], 5u);       /* start */
for (uint8_t step = 0u; step < 24u; step++) {
    uint32_t next = left + right;
    left = right;
    right = next;
    if (step == 12u) {
        guest_write(&phases[5], 4u); /* half */
    }
}
guest_write(&phases[9], 4u);       /* done */
return (int)(left & 0x7fu);
```

`left/right` 不必是 `volatile`：return value 依賴迴圈結果，compiler 仍必須產生真正
的 RV32I loop。可在 `guest-riscv/build/fibonacci.lst` 看到 `add`、`bne` 與 backward
branch，而不是 host 在 build 時直接塞一個答案。

## 一次編譯並放入三個 resident slots

從專案根目錄執行：

```bash
make guest-selected-upload GUEST_CS='guest-riscv/workloads/*.c'
make monitor
```

引號很重要，讓 Python batch tool 而不是目前 shell 管理 wildcard。這個命令會：

1. 將每個 `.c` 分別和 `start.S` link 成獨立 ELF。
2. objcopy 成沒有 ELF metadata 的 raw RV32I `.bin`。
3. 開啟 COM6 一次。
4. 將 `bitwalk`、`checksum`、`fibonacci` 逐一送進三個 SRAM slots。
5. 每 4 bytes 比對累積 CRC；ACK 看不清時重送同一 chunk。

上傳只建立 resident guest，不會自動執行。進入 `os>` 後先看清單：

```text
os> guest list
guest=bitwalk priority=2 bytes=142
guest=checksum priority=2 bytes=150
guest=fibonacci priority=2 bytes=150
```

## 控制 output prefix

```text
os> guest prefix status
guest prefix=off
os> guest prefix on
ok
```

prefix off 時只會看到 `start`，三個 guest 同時跑時無法判斷來源。prefix on 時，一次
guest `WRITE` syscall 會被 host 包成同一個 UART frame：

```text
fibonacci: start
checksum: half
```

這個名稱是 slot metadata，不在 guest output buffer 裡；guest 自己只送 `start` 或
`half`。關閉方式是 `guest prefix off`。設定只存在 SRAM，reset 後回到 off。

## 實際排程實驗

同優先度 round-robin：

```text
os> guest prefix on
os> guest -runall
bitwalk: start
checksum: start
fibonacci: start
...
```

每個 slot 最多執行 4 條 RV32I instruction 就重新選擇。三者 priority 都是 2 時，
即使 guest 不呼叫 `guest_yield()`，PC/register state 仍會被保留並輪流推進；這就是
VM 層的 preemptive time slicing。

再試 strict priority：

```text
os> guest checksum -priority 3
ok
os> guest bitwalk -priority 1
ok
os> guest fibonacci -priority 1
ok
os> guest -runall
```

這次 checksum 是唯一 priority 3，所以它會先完成；之後兩個 priority 1 才互相
round-robin。這也展示 strict-priority scheduler 的 starvation 風險。注意 safe mode
的 shell 和 VM supervisor 共用同一個 AVR call stack，因此 `guest -runall` 執行中
shell 不接受新 command；「preemptive」是在多個 RISC-V guest 之間，不是 shell 與
VM 之間。所有 guest EXIT/FAULT 後才重新出現 `os>`。

早期 80/160/240-loop soak workload 在三 guest 同跑時，實機可觀察到三個 `start`
與交錯的 `half`，但這塊 LGT clone 會在約上千次 emulator step 後無 reset cause 地
回到 boot banner。範例因此改為 24/36/48 loops：仍明確跨越數十個 4-instruction
quantum，且三 guest 實機已完整跑到 `done`，exit code 依序為 fibonacci=32、
bitwalk=19、checksum=59。Priority 實驗也確認 priority 3 的 checksum 會先完整結束，
才輪到 priority 1 的兩支程式。

不過在緊接著、不重新上傳 image 的第二輪執行中，LGT 板仍曾出現 UART/記憶體內容
污染，最後兩個 guest 雖完成，但該輪不能算乾淨驗收。因此目前的結論是「首次載入後
三 guest 排程成功」，不是「長時間反覆執行已穩定」。Loader 能在上傳期重建 SRAM；
執行期自動 checkpoint/restart 尚未實作。
<!-- UART loader reliability note (2026-09-13) -->

### `invalid Huffman frame` 與 guest 批次上傳

目前這塊 FIREPHX/LGT 相容板的應用程式 UART 預設為 **4800 baud**。實機測試顯示，
COM6 的 USB-UART 路徑在 9600 baud 偶爾會翻轉或漏掉 byte；這會先破壞 shell 回覆，
舊 decoder 又會把重啟後的下一個 frame 接在殘缺 frame 後面，因而顯示
`invalid Huffman frame`。它不是 Huffman code table 本身算錯。

現在的保護分成三層：

1. 每個 UART 結構化 frame 都由不可混淆的 `00 7e` sync marker 開始，decoder
   遇到新 marker 就放棄殘缺 frame 並重新同步。
2. guest loader 對每個 16-byte chunk 檢查「目前總長度 + SRAM 前綴 CRC16」；ACK
   壞掉或 SRAM 內容不符時，電腦會覆寫並重送同一塊。
3. AVR 會讀取 UART 的 framing、overrun、parity error flags，已知錯誤的 byte
   不會交給 shell。shell 的 `faults` 指令會顯示 `uart_err=<count>/<flags>`。

從 repository 根目錄執行：

```sh
make upload
make guest-selected-upload GUEST_CS='guest-riscv/workloads/*.c'
```

第二個命令開啟 COM6 時會刻意 reset 一次，因為 reset 會清空 SRAM；接著三個
guest 必須在同一次 serial connection 內全部重新載入。看到每個 guest 的
`LOAD OK` 與最後的 `uploaded guests: ...` 才代表完成。傳輸途中若看到
`resending the idempotent chunk`，表示 CRC 防護抓到壞 byte 並成功重送，並不代表
guest 已損壞。

### 觀察 Guest time-slice 切換

guest 自己呼叫 `guest_write()` 印出的 `start`、`debug`、`done` 只是應用程式 debug
文字，不是 scheduler 訊息。若要觀察 host VM scheduler 實際選中了哪個 guest：

```text
guest trace on
guest prefix on
guest -runall
```

會另外看到：

```text
[switch -> bitwalk]
[switch -> checksum]
[switch -> fibonacci]
```

`guest trace off` 關閉；`guest trace status` 查狀態。實際上 VM 每個 slice 最多只跑
4 條 RV32I instruction，切換頻率遠高於 4800-baud UART 能輸出的速度，因此 trace
每 250 個 scheduler loop 取樣一次。這些提示仍代表當下真正選中的 slot，但不是
每一次 context switch 的完整逐筆紀錄；若每次都輸出，UART 本身會主宰執行時間。

### 長時間三 Guest 驗收與 8-bit register file

前文的 24/36/48-loop 結果是較早期的驗收；目前 workload 已提高為數萬輪，以下結果
取代那段舊結論。2026-09-13 在實際 COM6 板上以 trace 與 prefix 同時開啟，三個
resident guest 已完整執行：

```text
checksum: done
rv32 checksum exit=8
bitwalk: done
rv32 bitwalk exit=39
fibonacci: done
rv32 fibonacci exit=64
```

中途同樣能看到三個 `start`、三個 `debug`，以及大量交錯的 `[switch -> name]`；整輪
沒有重新出現 boot banner。驗收使用同一個 COM connection 完成 reset、三個 image
上傳、設定 prefix/trace 與 runall：

```sh
make guest-load-batch \
  GUEST_BATCH='--guest bitwalk=/mnt/d/atmega_os/guest-riscv/build/bitwalk.bin --guest checksum=/mnt/d/atmega_os/guest-riscv/build/checksum.bin --guest fibonacci=/mnt/d/atmega_os/guest-riscv/build/fibonacci.bin' \
  GUEST_UPLOAD_FLAGS='--prefix --trace --run-all --run-timeout 360 --fail-on-reboot'
```

Uploader 不能看到第一個 `os>` 就離開，因為 `guest runall` 的 shell prompt 可能早於
非同步 guest 結果。現在工具會等待每個已上傳名稱各自出現
`rv32 <name> exit=...`；任一 guest fault、重啟或逾時都回傳失敗。這使上面的命令同時
是可重複的 end-to-end 驗收，不只是上傳命令。

RV32 有 32 個 32-bit register，但 AVR 是 8-bit MCU。SRAM 中仍保留相同的 128 bytes，
實作則不再讓舊 avr-gcc 直接最佳化 `uint32_t x[32]` 的所有存取：

```text
x0  = byte 0..3
x1  = byte 4..7
...
x31 = byte 124..127
```

`rv32_compat_reg_read()` 以 little-endian 合成 32-bit 值，
`rv32_compat_reg_write()` 再拆成四個 byte。這兩個 primitive 位於
`src/vm/rv32_compat.c`，單獨禁止 LTO；大型 decoder 仍使用 LTO。這個邊界有三個目的：

1. register 存取不依賴 4-byte SRAM alignment。
2. ECALL callback 與 decoder 看到的 register 值完全一致。
3. 避免 avr-gcc 5.4 把「RV32 的 x0 永遠清成 0」錯誤合併到先前的 destination
   register store。

LTO 現在是主 Makefile 的預設，因為它把 host firmware 留在較可靠的低 Flash 區；
只有 `rv32_compat.o` 使用一般 `-Os`。目前預設三-slot safe-mode build 的量測是：

```text
Program: 14134 bytes
Data:     1310 bytes
```

### 這塊 LGT clone 的 SRLI 限制

RV32I decoder 仍實作 `SRLI`，host 的 `make test` 也會驗證 shift 指令。然而同一個
7001-loop right-shift stress guest 在此 LGT 相容板上曾於 reset 後產生不同 exit value，
即使 SRAM upload CRC 完全相同；16-loop 版本則正確。因此目前不能宣稱這塊板上的
「長時間 32-bit SRLI」已達 deterministic。這是實機限制紀錄，不代表從 ISA 移除
SRLI。

正式的長跑 checksum workload 改用 `SLLI + ADD + XOR`，仍會執行數萬條真正的 guest
instruction，但避開這個已知不穩定硬體路徑。若要驗證完整 RV32I correctness，應在
host test、真正 ATmega328P，或可可靠讀回 Flash 的 ISP 環境進行；不要只依賴目前這顆
LGT clone 的單次輸出。

# Timer1 Guest 時間片：為什麼不是「每毫秒四條指令」

2026-09-14 起，多 guest scheduler 不再用固定的 4-instruction budget。固定四條的
舊模型其實是：

```text
執行 4 條 RV32I instruction
→ 額外 busy-wait 1 ms
→ 換下一個 guest
```

它把「排程 quantum」錯誤地等同於「指令數」，所以大部分時間不是在 emulator
工作，而是在刻意等待。`fibonacci exit=64` 也不是只算到第 64 輪；workload 實際
執行 `WORK_ROUNDS=18000`，64 是最後 `left & 0x7f` 的低 7 bits exit code。

現行作法在 `run_slots()` 進入 guest 執行期時才啟動 16-bit Timer1：

```text
F_CPU = 16,000,000 Hz
Timer1 prescaler = 64
Timer1 count rate = 16,000,000 / 64 = 250,000 counts/s
1 ms = 250 counts
```

每個 guest slice 的流程是：

```text
start = TCNT1
do {
    rv32_step(cpu);       // 完整解譯並提交一條 guest instruction
} while (cpu 仍可執行 && (TCNT1 - start) < 250);
換到下一個 runnable guest
```

所以沒有「最多四條／三十二條」之類的人造 instruction ceiling。某條 RV32I 指令
若解譯得快，同一毫秒會跑較多條；load/store/branch 路徑較慢時則較少。16-bit
unsigned subtraction 即使 TCNT1 從 `65535` wrap 到 `0` 仍能正確算一個 1 ms interval。

這仍不是 Timer ISR 在任意 AVR machine instruction 中途硬切。`TIMSK1=0`，Timer1
只當免費運轉的時鐘；deadline 在每條 RV32I 指令完成後檢查。這是刻意的：guest 的
PC、`x0..x31` 與 status 都已經存在 `rv32_cpu_t`（SRAM），instruction boundary 是
一致且可恢復的狀態。RV32I base ISA 也沒有 x86 那種獨立 FLAGS register；比較結果
直接由 branch instruction 讀 general registers 決定，因此沒有另一份 flags 要保存。

Timer1 只在 `guest run`／`guest runall` 期間運轉，shell 與 UART upload 階段保持停止。
但這只能說明目前的時間片設計；不能單靠一次正確的 exit code 宣稱這塊 LGT clone
已達長時間 deterministic。2026-09-14 的重複驗收出現相同 upload CRC、不同 exit
code，後面的「最新實機限制」有完整結論。

早先「從開機便啟動 Timer1」的版本在 upload 階段落入 panic vector，但那次同時
改寫了全域 `timer_delay_ms()`、增加約 200 bytes code 並移動 Flash addresses，不能
視為 Timer1 單一變因。這塊 LGT virtual-boot 板本來就觀察過 control-flow 對 link
address 敏感；panic detail 只表示最後落到某個 vector handler，並不等於該 peripheral
真的提出 interrupt。現行最小版本只有 guest 執行期 Timer1 polling，已通過實機。

真正的 native AVR preemption 仍是另一層：`LGT_SAFE_MODE=0` 使用 Timer2 ISR 保存
AVR registers/SREG/SP 並切換 private stacks。這塊 LGT clone 的 interrupt return 與
virtual boot vector 尚未可靠驗收；要完成它，應移植 LGT 專用 startup/vector 支援，
或用原廠 ATmega328P 作對照，而不是把 Timer1 polling 誤稱為 native ISR context switch。

# Shell history、Guest ID 與 Priority/Deadlock

## History 為什麼放在電腦端

互動 monitor 現在支援：

```text
↑                 上一條 command
↓                 下一條 command
history           列出本次 monitor session 的 command
```

請從 repository 根目錄用 `make monitor`。history 最多保留 32 條，連續重複的 command
只保留一次；關閉 monitor 後即清空。它不寫 EEPROM，也不占 Arduino SRAM。

這不是偷懶，而是 Uno 只有 2048 bytes SRAM。目前三個 guest slot 加上 UART buffer、
kernel state 與 globals 已使用約 1322 bytes；若 MCU 自己保存 4 條 32-byte command，
就再永久吃掉 128 bytes。實機也已證明增加 device-side history 後，長 guest workload
更容易暴露記憶體／執行不穩定。因此鍵盤方向鍵與字串陣列由
`tools/wsl_serial_monitor.py` 管理；MCU 收到 `history` 時只送一個特殊 request frame，
monitor 再把 PC 上的 history 印出來。

直接開別的 serial terminal 時，普通 command 仍可用，但那個 terminal 若沒有實作
這份 host protocol，`history` 與方向鍵不會有完整功能。

## Native task ID 和 guest slot 是兩套 ID

這個 OS 現在有兩層 execution context：

```text
native AVR task       scheduler.c 管理；用 task ID
RV32 guest VM         rv_demo_task.c 管理；用 slot 與 name
```

查 native task：

```text
os> tasks
task=0 state=... priority=<base>/<effective>
```

`restart 0` 的數字就是這裡的 `task=0`，而且只允許 restart 已經 TERMINATED 的 native
task。可是目前這塊 LGT 板預設是 `LGT_SAFE_MODE=1`，不啟動多 native-task scheduler，
所以 `tasks` 沒有可供 restart 的 native ID；這條 command 是留給真正 ATmega328P 上的
`LGT_SAFE_MODE=0` build。

查 guest：

```text
os> guest list
slot=0 guest=bitwalk priority=2 bytes=151
slot=1 guest=checksum priority=2 bytes=155
slot=2 guest=fibonacci priority=2 bytes=155
```

重新從 PC=0、register=0 開始跑某個 guest，使用 name：

```text
os> guest checksum -restart
```

`-run` 和 `-restart` 目前語意相同，都會呼叫 `prepare_slot()` 重設 CPU context；已上傳
的 image bytes 仍留在該 slot SRAM。`guest info` 是舊的 slot-0 詳細檢視，所以現在也會
明示 `guest slot=0 name=...`。要看所有 ID/name，應使用 `guest list`。

## Priority inheritance 不能解除 deadlock

Native mutex 已經實作 priority inheritance。假設 low-priority task 持有 mutex，而
high-priority task 正在等它，mutex code 會暫時把 owner 的 effective priority 拉高；
`tasks` 顯示的 `priority=base/effective` 就能看到差別。若有 A 等 B、B 再等 C 的 mutex
chain，`mutex_recompute_all_priorities()` 會跑最多 task-count 個 propagation pass，把
priority 沿鏈傳下去。unlock 或 timeout 取消 wait 後會重新計算並降回合理值。

這解決的是 **priority inversion**，不是 **deadlock**：

```text
A 持有 M1，等待 M2
B 持有 M2，等待 M1
```

把 A 或 B priority 拉到最高也不會憑空釋放 M1/M2。避免 deadlock 要用固定 lock ordering
（例如永遠先鎖 M1 再鎖 M2）、`mutex_try_lock()` 後失敗就回退，或
`mutex_lock_timeout()` 讓等待能超時。現在沒有 wait-for graph deadlock detector。

Guest 的 `guest <name> -priority N` 是另一套 VM scheduler priority。Guest syscall 尚未
提供 guest mutex/semaphore，因此它目前也沒有 native mutex priority inheritance；而且
safe mode 執行 `guest -runall` 時 shell 會等所有 guest 結束，不能在中途手動改 priority。

## 為什麼 trace 以前只顯示 fibonacci

先前每 250 個 scheduler loop 才印一次 trace。只剩兩個 equal-priority guest 時，
`250 % 2 == 0`，每次取樣剛好落到同一個 slot，形成 sampling alias；這不表示 checksum
沒有執行，因為它最後確實會完成。現在 trace 除了 countdown，還記住上次印出的 slot；
直到實際選中不同 guest 才印下一個提示，並在只剩一個 guest 時抑制重複洗版。

## 2026-09-14 最新實機限制：長算術仍不穩定

這次實機驗收把「timer/scheduler」和「VM 長算術」分開測試：

- 三個 image 的 SRAM upload CRC 全部正確。
- Timer1 1 ms slicing 下，三個 guest 能輪流被選中且沒有 reboot，但 exit 曾得到
  `45/30/28`，不是預期 `39/8/64`。
- 同一 image 單獨 restart 時，bitwalk 得到正確 39，但 checksum/fibonacci 曾得到
  80/57。
- 把 Timer1 完全排除、暫時改回每 slice 固定 4 instructions，仍得到錯誤結果。
- 關閉 trace 再單跑 checksum，結果又漂到 48。

因此目前證據排除了「Timer1 deadline 造成算錯」、「多 guest context switch 漏存
register」以及「trace 輸出造成算錯」。真正尚未定位的是這顆 LGT clone 上長時間執行
RV32 decoder 的 nondeterminism；先前一次得到 39/8/64 只能視為單次通過，不能視為
穩定保證。Host `make test` 的 RV32I tests 仍通過。下一個正確的硬體除錯步驟是用真正
ATmega328P 作 A/B 對照，或使用 LGT 專用 toolchain/startup，而不是繼續任意更換 timer。

### Integrity 診斷韌體的後續結果

隨後在每個 guest EXIT/FAULT 後加入一行執行期驗證：

```text
verify checksum ptr=1 size=192 crc=d8ca/d8ca
```

`ptr=1` 表示 `cpu.memory` 仍指向該 slot 的 memory，`size=192` 表示 fetch boundary
metadata 沒有被改寫；兩個 CRC 分別是結束時與開始時的 guest image CRC。這三項正好
能檢查先前 `fault=3 pc=100` 的矛盾情況：PC 100 明明合法，只有 memory pointer/size
壞掉才應該在那裡 fetch fault。

加入診斷後的 firmware 在同一批 SRAM image、不 reset、不重新上傳的情況下連跑六輪，
每輪都固定得到 checksum=8、bitwalk=39、fibonacci=64，且三個 CRC 全部不變。這是目前
板上已燒錄版本的實測狀態。不過 integrity code 本身不會修復 register；它主要改變了
Flash code layout，因此這個結果再次支持「LGT clone 對 code placement/standard AVR
toolchain 不完全可靠」，尚不能當成已找到根因。診斷行暫時保留，若日後再次漂移，便能
立即知道 image 或 CPU memory metadata 是否先被破壞。

# 只更新 Guest，不重燒 AVR Flash

修改 `guest-riscv` 裡的 C 程式時，不需要執行 `make upload`。`make upload` 是把整份
ATmega OS HEX 經 AVRDUDE 寫進 Flash；runtime guest loader 則只透過 UART 改寫 SRAM
slot，兩條路徑完全分開。

從 repository 根目錄查看提示：

```sh
make guest-help
```

只編譯並上傳一支 C guest：

```sh
make guest-only GUEST_C=guest-riscv/workloads/fibonacci.c
```

它會產生 `guest-riscv/build/fibonacci.elf` 與 `fibonacci.bin`，再把 BIN 放進名為
`fibonacci` 的 SRAM slot。若同名 slot 已存在就覆蓋該 slot；不會產生或上傳 AVR HEX，
也不會執行 AVRDUDE。上傳完成後仍由使用者在 shell 決定何時跑：

```text
os> guest list
os> guest fibonacci -run
```

一次選多支（glob 必須加引號，讓 Python 而不是 shell 管理選取）：

```sh
make guests-only GUEST_CS='guest-riscv/workloads/*.c'
```

多支上傳會刻意使用同一個 COM connection，並在開始時 reset 一次後重建所有 SRAM
slots；它仍然沒有重燒 Flash。只改一支程式時優先使用 `guest-only`，因為它不必重新
傳其他 guest，是目前最快又保留 CRC/ACK 安全檢查的路徑。

其他相關命令：

```sh
# 只編譯，不連 COM6
make guest-build GUEST_C=guest-riscv/workloads/fibonacci.c

# 編譯、上傳 SRAM，接著立即執行
make guest-run GUEST_C=guest-riscv/workloads/fibonacci.c

# 已有 .bin，完全不重新編譯
make guest-load \
  GUEST_BIN=/mnt/d/atmega_os/guest-riscv/build/fibonacci.bin \
  GUEST_NAME=fibonacci
```

目前 application UART 保持 4800 baud；loader transport 已升級成以下 binary protocol。

# Binary UART loader 與 Guest image directory

<!-- The standalone hardware experiment is documented at the end of this guide. -->

## 日常使用

一般不需要手打 loader protocol。改完 C guest 後，在 repository 根目錄執行：

```sh
# 一支 guest：編譯後寫入 SRAM，不重燒 AVR Flash
make guest-only GUEST_C=guest-riscv/workloads/checksum.c

# 多支 guest：共用一次 COM6 connection
make guests-only GUEST_CS='guest-riscv/workloads/*.c'

# 非零 entry point；這是 image 內的 byte offset，不是 AVR 位址
make guest-only \
  GUEST_C=guest-riscv/workloads/checksum.c \
  GUEST_ENTRY=4
```

上傳完成後可在 shell 查看 image directory：

```text
os> guest dir
id=0 name=bitwalk size=151 crc=2891 pri=2 entry=0
id=1 name=checksum size=155 crc=d8ca pri=2 entry=0
id=2 name=fibonacci size=155 crc=ea5e pri=2 entry=0
```

各欄位的意思：

- `id`：目前 SRAM slot 編號，只在本次開機有效。
- `name`：上傳時由 `.c/.bin` 檔名取得的 guest 名稱。
- `size`：有效 image bytes；不是整個 192-byte VM address space。
- `crc`：完整 image 的 CRC16-CCITT，用來證明 PC 與 Arduino 看到同一批 bytes。
- `pri`：VM scheduler priority，目前 0..3，數字越大越優先。
- `entry`：假的 RV32 PC 初值，是 image 內 4-byte aligned byte offset。通常 `_start`
  位於 offset 0，所以預設為 0。

這份 directory 在 SRAM，不在 EEPROM/Flash；reset 或斷電後會消失。Builtin image 可以從
AVR Flash 重新建立，但 UART 上傳的 images 必須重新傳入。CRC 與 entry point 放在獨立的
平行 metadata arrays，沒有擴大 VM slot 本身，因此每個 slot 維持實機驗證過的 376-byte
stride。

## Wire protocol

建立 session 的第一步仍是可讀的 shell command：

```text
guest bload <name> <size> <crc16-hex> <entry-decimal>
```

MCU 回覆：

```text
READY name=checksum size=155 chunk=48 encoding=binary
```

從這一刻到 COMMIT 為止，UART input 不再當文字 shell 解析，而是接收 packet：

```text
A5 5A | op | offset_lo offset_hi | length | payload | crc_lo crc_hi
```

- `A5 5A` 是 sync marker；若中途出現雜訊，receiver 可重新尋找封包起點。
- `op=1` 是 DATA，`op=2` 是 COMMIT，`op=3` 是 CANCEL。
- `offset` 是 image 內的絕對 offset，不是「下一塊」的隱含狀態。
- `length` 最大 48，DATA 後面跟 0..48 bytes payload。
- packet CRC 是 CRC16-CCITT，涵蓋 `op + offset + length + payload`，不涵蓋 sync。

舊版每次傳 16 bytes，還要把每個 byte 展成兩個 ASCII hex characters。新版每 packet
傳 48 raw bytes，因此不只 chunk 是三倍，payload 本身也不再膨脹成兩倍文字。UART baud
仍是 4800，但 protocol overhead 大幅降低。

每個 DATA 成功後，MCU 回覆累積長度和「目前已存 image prefix」的 CRC：

```text
BINARY ACK bytes=96 crc=09c8
```

如果 ACK 遺失或 CRC/NACK/timeout，PC 會用相同 absolute offset 重送。重送是
idempotent：同一區 bytes 被覆寫成同一內容，不會因 ACK 遺失而把資料 append 兩次。
COMMIT 時 MCU 再同時檢查：收到的總長度、running CRC、直接重讀 SRAM 算出的 CRC，以及
entry point 範圍／alignment。全部相符才把 slot 標成可執行；失敗時整批 uploader 會
重新同步 `os>` 並從第一個 guest 重建，因為真正 reboot 會讓先前 SRAM slots 全消失。

## Monitor ownership 與 `os>`

`make monitor`、`make upload` 和 guest upload targets 都先執行 `serial-close`。它只尋找
command line 真的以 `-File ...serial-monitor.ps1` 啟動的舊 relay，強制結束後最多等待
2 秒，確定 COM port owner 消失才啟動下一個命令。

Monitor 自己離開時只關閉它建立的 PowerShell child，不再做全域掃描。這避免以下 race：
舊 monitor 被新命令關閉後，其 `finally` 又誤殺剛連上的新 monitor。若故意留下舊
`make monitor` 再開新的，舊 terminal 出現 `Error 255` 是「舊 relay 被接替」的結果；
新的 terminal 應正常顯示 `os>`。

Windows relay 開啟 COM6 後先送一次 Enter；WSL monitor 再等 Uno bootloader/reset
窗口約 2 秒後送一次。因此即使開 port 觸發 reset、或開啟時 boot banner 已被清掉，
成功連線最後仍會落在：

```text
os>
```

# 兩節點 I²C：先驗證線路，再做分散式排程

## 最新階段：heartbeat 已接進 OS

### 2026-09-15：第一版跨板 Job 派送

#### 2026-09-17：guest 以 Uno 為程式庫，經 I²C 自動部署到 Nano

新的正常資料流是：

```text
PC 編譯 C -> RV32I binary
        |
        | USB/UART，make guest-uno / guests-uno
        v
Uno 的 3-slot SRAM guest library
        |
        | I2C begin + 11-byte chunks + commit + CRC16
        v
Nano 的 SRAM guest cache -> RV32I emulator -> I2C result -> Uno
```

一個程式上傳到 Uno（不是 Nano）：

```sh
make guest-uno GUEST_C=guest-riscv/workloads/fibonacci.c
```

一次把最多三個 `.c` 各自編譯成獨立 guest 並放入 Uno：

```sh
make guests-uno GUEST_CS='guest-riscv/workloads/*.c'
```

接著只連 COM5：

```text
uno> list
Programs stored on Uno:
bitwalk  [copies on first run]
checksum  [copies on first run]
fibonacci  [copies on first run]
uno> run fibonacci
[COPYING] fibonacci Uno -> Nano. It will start automatically.
[DONE] fibonacci returned 64
```

`run` 先比較 Nano directory 的 name、size、CRC。完全相同就直接發 RUN；沒有或
版本不同則自動部署，使用者不用連 COM7。部署協定沿用 21-byte command frame：
version 4 是 BEGIN（job ID、name、size、完整 image CRC），version 5 是絕對 offset
加最多 11 bytes payload，version 6 是 COMMIT。每個 frame 自己另有 packet CRC16。
Nano 只按預期 offset 寫入；重送已寫過的絕對 offset 不會重複追加。COMMIT 時 Nano
重新計算整個 image CRC，吻合才標記可執行，接著 Uno 自動建立 RUN request。

Nano 仍保留 builtin，另兩格作部署快取；第三個自訂程式執行時會輪替較早的 cache。
Uno 保有三個自訂程式，故 `list` 是「Uno 程式庫」，並以 `[ready on Nano]` 或
`[copies on first run]` 表示 cache 狀態。兩邊都只是 SRAM，任一板重開後該板的動態
內容會消失；Uno 重開就需重新執行 `make guests-uno`，Nano 重開則下次 run 自動再傳。

部署期間 `last` 顯示 `[COPYING]`，`stop` 會取消 Nano 的 partial upload；失聯則變
UNKNOWN。`retry yes` 刻意不支援半完成的 deploy，應先 `forget`，再重新 `run`，
以新的 ID 從 BEGIN 開始。這避免把 DATA chunk 誤當一般計算工作重試。

`advanced` 現在是分類後的診斷首頁：System、VM、I2C、Recovery、Hardware、Danger；
`advanced raw` 才顯示完整舊命令。`job debug` 顯示 id/state/result/detail，日常的
`last` 維持人類可讀訊息。

實機驗證：fibonacci 只載入 COM5 Uno，第一次 run 經 I²C 複製後回傳 64，list
轉為 ready；接著 Uno 同時載入 bitwalk/checksum/fibonacci，依序自動部署 checksum
與 bitwalk，分別回傳 8、39，Nano cache 滿時成功輪替，fibonacci 回到 copies 狀態。
另在 bitwalk 的 COPY 中立刻 stop，Nano 回報 STOPPED；不重開板子直接部署 checksum
仍完成並回傳 8。這個測試抓出並修正兩個競態：DATA frame 不能拿來取得 cancel name，
以及不能用 enum 數值大小把 COPYING 誤判成 terminal。現在兩處皆採明確狀態判斷。

若要把 Uno 程式庫裡目前存在的程式全部驗收，不必再逐一輸入名稱：

```text
uno> run all
[BATCH] Running 3 programs. Use stop to cancel.
[COPYING] bitwalk 33/151 bytes Uno -> Nano.
[DONE] bitwalk returned 39
[NEXT] Starting checksum.
...
[BATCH DONE] passed=3 failed=0
```

這不是三個 guest 同時佔用 Nano 的 VM。Uno 建立一個很小的 slot bit mask，依序對每個
已載入程式執行「必要時複製 → 執行 → 收到終態 → 下一個」。因此只需 Nano 原有的一份
active VM memory，而且每個程式都有自己的 DONE／FAULT／STOPPED 結果。`last` 在傳輸中
會顯示已確認的 bytes／總 bytes。輸入 `stop` 會取消目前工作，同時清除尚未開始的批次；
若通訊進入 UNKNOWN，批次會暫停而不冒險自動執行下一項。

COM5 Uno／COM7 Nano 實機驗收 `run all`：bitwalk 回傳 39、checksum 回傳 8、
fibonacci 回傳 64，最後顯示 `passed=3 failed=0`；三個程式在測試前只上傳到 Uno，
Nano 由空的自訂 cache 開始，因此同時涵蓋三次 I²C 部署與兩格 cache 的輪替。

#### 2026-09-22：重複測試、雙跑驗證與最近結果

Uno shell 現在能自己串接多個 job，不需要人在每次完成後再輸入一次：

```text
uno> verify checksum
[VERIFY] Running the same image twice and comparing exit results.
[DONE] checksum returned 8
[VERIFY] First run finished. Starting comparison run.
[DONE] checksum returned 8
[VERIFY PASS] Both runs returned 8
```

`verify <name>` 建立兩個不同 job ID，執行相同的 Uno image，並比較兩次正常完成的
32-bit exit result。任何一次 FAULT、REJECTED、STOPPED 或 UNKNOWN 都不算通過。
這能找出「相同輸入偶爾算出不同值」的非確定性錯誤，但不能證明兩次相同的錯誤答案
是正確答案，也不比較 UART 文字輸出；真正的多節點驗證仍要把同一 image 派給兩塊 worker。

```text
uno> repeat bitwalk 3
[REPEAT] Running bitwalk 3 times. Use stop to cancel.
...
[REPEAT DONE] passed=3 failed=0
```

`repeat <name> <1..99>` 適合 soak／重現偶發錯誤。每次都有新的 job ID；上一筆到達
確定終態後才開始下一筆，所以不會重疊使用 Nano VM。`stop` 取消目前執行並把尚未開始
的次數清零。實機在五次 fibonacci 測試開始後輸入 stop，得到 `STOPPED`，摘要為
`passed=0 failed=1`，沒有繼續啟動剩餘四次。

Uno SRAM 另保留最近三筆「確定結束」的紀錄：

```text
uno> results
Newest completed results:
#7 bitwalk DONE=39
#6 bitwalk DONE=39
#5 bitwalk DONE=39
uno> results clear
Completed-result history cleared.
```

`#7` 是 job ID，不是 task ID。UNKNOWN 不寫入 history，因為它可能稍後恢復成 DONE；
DONE、FAULT、REJECTED、STOPPED 才寫入。History 只在 SRAM，Uno reset 後清空。

兩塊板現在也能分開編譯／燒錄，不必因一塊暫時未連線讓另一塊無法更新：

```sh
make nodes-master-upload                         # 預設 COM5
make nodes-worker-upload                         # 目前預設 COM9
make nodes-worker-upload NODE_WORKER_PORT=COM7  # Windows 重新編號時覆寫
make nodes-upload                                # 兩塊都在線時一起更新
```

2026-09-22 實機上 Nano 從 COM7 重新枚舉成 COM9；以覆寫變數成功燒錄並 read-back。
COM5→COM9 的 `verify checksum` 兩次皆為 8；`repeat bitwalk 3` 三次皆為 39；最近結果
ring 顯示三個不同 ID。這也證明 automation 使用新 job，而不是重印 cached result。

#### 2026-09-22：改成 Unix 風格的操作模型

最新 nodes firmware 的 prompt 由 `uno>`／`nano>` 改為 `uno$`／`nano$`。`$` 不是
user mode 或權限隔離的證明；這顆 AVR 沒有 Unix user mode。它只是告訴使用者「這是一個
可互動的命令 shell」，並讓命令名稱遵守同一套 Unix 習慣：

| 新命令 | 意思 | 舊 alias |
|---|---|---|
| `ls` | 列出 Uno 程式庫及 Nano cache 位置 | `list` |
| `run PROGRAM` | 在 Nano 執行程式 | — |
| `run --all` | 依序執行全部程式 | `run all` |
| `run -n N PROGRAM` | 執行 N 次 | `repeat PROGRAM N` |
| `run --verify PROGRAM` | 執行兩次並比較 exit result | `verify PROGRAM` |
| `jobs` | active job 加最近三筆結果 | `last`／`results` 是較窄的舊 view |
| `kill` | 取消目前工作及尚未開始的自動次數 | `stop` |
| `cluster` | 顯示 Uno→Nano I²C link | `status` |
| `load` | 顯示 PC 上的 guest 編譯／上傳命令 | `upload` |
| `ps`／`free` | native task／stack 診斷 | `tasks`／`stack` |
| `man COMMAND` | 顯示單一命令說明 | — |

第一個操作流程現在是：

```text
uno$ help
uno$ ls
PROGRAM       LOCATION
bitwalk  uno
checksum  uno+nano
fibonacci  uno+nano
uno$ run bitwalk
[6] copying bitwalk Uno -> Nano. It will start automatically.
uno$ jobs
[COPYING #6] bitwalk 99/151 bytes Uno -> Nano.
uno$ kill
```

`LOCATION=uno` 代表 image 在 Uno SRAM、第一次 run 需要經 I²C 複製；`uno+nano`
表示 Nano cache 已有同 name/size/CRC 的版本。`jobs` 的 `#6` 是 distributed job ID。
Shell 是非阻塞的：run 後立刻回 `$`，所以工作執行期間仍可輸入 `jobs` 或 `kill`。

`help` 只顯示日常命令分類，`man run` 顯示 run options，`advanced` 才顯示 kernel、VM、
raw I²C 與 recovery 工具。說明文字已納入 build-time Huffman 壓縮；PC uploader 同時接受
新的 `$` prompt 與舊 `>` prompt，避免尚未更新的板子無法載入。

這次實機也修正一個資料所有權錯誤：deploy DATA 階段的 `command[]` 是 wire buffer，
其中 name 欄位已被 offset/payload 覆寫，不能拿來當 UI 的 job name。Coordinator 現在把
name 保存在獨立的 `deploy_name`，`nodes_job_name()` 與 cancel 都讀這份穩定資料。
實測 COPY 中的 `jobs` 已正確顯示 `bitwalk 99/151`，不再出現亂碼名稱或 `/0 bytes`。

為避免 nodes profile 同時保留兩套 shell，舊的 `guest runall`、prefix、trace 與
`guest <name> -run` parser 只留在 `NODE_ROLE=0` 原生單板 build。Uno/Nano 仍保留 binary
uploader 必需的 `guest bload`，以及 `guest info/regs` 診斷；日常執行統一走 `ls/run/jobs`。
這讓 Uno Flash 從一度接近滿載降至 23318 bytes，並避免兩套 UI 繼續分歧。

目前 I²C deploy 的 entry point 固定為 0；一般由 `start.S` 產生的 C guest 都符合。
若 Uno directory 裡是非零 entry 的進階 image，`run` 會拒絕，而不會默默從錯誤位址執行。
目前 nodes build：Uno Flash 23318 bytes、靜態 SRAM 1569 bytes；Nano Flash 21770 bytes、
靜態 SRAM 1465 bytes。剩餘 SRAM仍必須保留給 AVR call stack／ISR，不能視為全可用空間。
Unix UI 說明採 Huffman 壓縮，重複的舊 nodes guest parser 則由 LTO 移除；因此 Uno Flash
由重構中途的 32406 bytes 降到 71.2%，重新留下約 9 KB 給真正的分散式功能。

#### 2026-09-16：以使用者操作為主的新 shell

一般操作不再暴露 `job ID / detail / heartbeat / CRC`。連 COM5 會看到 `uno>`，
連 COM7 會看到 `nano>`，所以不用猜目前控制哪塊板。最短完整流程是：

```text
uno> status
[ONLINE] Uno can reach Nano over I2C.
uno> list
Programs ready on Nano:
builtin  [normal]
fibonacci  [normal]
Start one with: run <name>
uno> run fibonacci
[STARTED] fibonacci is running on Nano.
You can keep typing. Use stop to interrupt it.
uno> stop
[WAIT] Asking Nano to stop. Waiting for confirmation.
[STOPPED] fibonacci was stopped.
uno> last
[STOPPED] fibonacci was stopped.
```

六個日常指令：`status` 看連線、`list` 看 Nano 程式、`run <name>` 執行、
`stop` 中斷、`last` 重看結果、`upload` 看 PC 上傳命令。`help` 只列這些。
`advanced` 才顯示舊的 kernel/VM/protocol 診斷指令。舊 `nodes`、`programs`、
`job run`、`job cancel`、`jobs` 仍可用，避免既有腳本失效，但不再作為入門 UI。

畫面上的 program 是 Nano SRAM 裡的 RISC-V guest；`run` 讓 Uno 經 I²C 傳送
開始命令，Nano 的 emulator 執行，結果再經 I²C 回 Uno。一般 UI 不必先理解
內部稱作 job 的「一次執行請求」。Nano 本機 shell 的 `run <name>` 則不經 I²C，
執行時按 `q` 中斷。`upload` 明確指出命令要在 PC terminal 執行，不是在 `uno>`。

異常時 `last` 顯示 `[UNKNOWN]` 並指示先用 `status`，再用 `retry` 或 `forget`。
`retry` 不會立刻動作，必須再輸入 `retry yes`，避免使用者無意重複副作用。
內部 ID、狀態碼和錯誤計數仍可由 `advanced` 與舊診斷命令檢查。

#### 最新操作：在 Uno 看 Nano 目錄、派送、收結果

現在不用切到 Nano 查程式名稱。PC 開 `make monitor PORT=COM5`，在 Uno 輸入：

```text
help
nodes
programs
job run builtin
```

`programs` 顯示 **Nano** 上已載入的 guest：名稱、大小（bytes）、文字優先度、
entry point（從哪個 guest 位址開始執行）。`programs local` 則列 **Uno** 本機程式。
Nano 上的 `programs` 列本機程式。兩份 directory 不是同一份記憶體。
若顯示 syncing，等待約 2 秒再查；Nano offline 時不把舊目錄當作可用程式顯示。
這是分頁更新的快取，不是跨板一致性的即時快照；上傳或重啟後可能短暫看到舊資料。

自己的 C 程式仍先在 PC 執行（先 Ctrl+] 退出 monitor）：

```sh
make guest-only PORT=COM7 GUEST_C=guest-riscv/workloads/checksum.c
make monitor PORT=COM5
```

等約 2 秒，在 Uno `programs` 就能看到 checksum，然後 `job run checksum`。
結果會先顯示容易讀的摘要 `Nano: checksum job=... completed, return=8`，
再保留 job ID/state/result/detail 供除錯。`jobs` 可重看最近工作；不代表工作佇列。
`help` 已以這個完整流程為主，`help debug` 保留底層操作。

I²C report 現在為 version 2、36 bytes：前 12 bytes 是 heartbeat/job 狀態，
12..33 是一筆 directory page（slot/count/loaded/name/size/CRC/priority/entry），
34..35 是涵蓋前 34 bytes 的 CRC16。每約 500 ms 換一筆，3 slots 約 1.5 秒更新一輪。
Nano 在 main loop 取得 metadata、原子發布 snapshot；TWI ISR 只傳快照，
不從 loader 正在更新的 metadata 中讀取半筆資料，也不執行 guest。
Uno 只接受 CRC 正確且 page index 合法的回覆。舊 14-byte report 不相容，需更新兩板。

這一版沒有新增多 job 佇列、自動故障重派、遠端 debug print 轉送或多 worker 選擇。
Nano 快照在換頁或距離上次發布至少 128 ms 時更新，不會每條解譯指令都重建。
靜態 SRAM：Uno 1530 bytes（剩 518），Nano 1476 bytes（剩 572）；剩餘空間
還需容納 AVR call stack 和 ISR frame，不能全部拿來擴充 guest/job。
下面保留各階段歷史說明；其中「尚未實作遠端目錄」描述的是前一版。

實機驗證：Nano SRAM 上傳 checksum 後，Uno 的 `programs` 自動列出 builtin
和 checksum；`programs local` 不誤列 Nano 資料。`job run checksum high`
自動回報 return=8，再查目錄 priority 更新為 high。兩板韌體均讀回驗證，
CRC corruption／非法 directory page 的 host tests 通過。尚未做斷電／丟封包故障注入。

#### 中斷執行與 UNKNOWN 復原

Uno 派出的 guest 正在 Nano 執行時，Uno shell 仍可輸入：

```text
job cancel
```

Uno 以 I²C 傳送帶有相同 job ID 的 cancel control packet。Nano 不在 TWI ISR
裡破壞 VM register；VM 每個排程 slice 開頭呼叫 `nodes_poll()`，看到取消旗標後，
在完整 RV32I 指令的邊界釋放 slot，回報 CANCELLED。一般延遲約一個 I²C poll
週期（目前 500 ms）加一個 VM slice，不是硬體 instruction-level interrupt。
取消封包最多重送三次，同一 job ID 的取消具冪等性，不會產生另一個 job。
若 cancel 比原 RUN 封包先抵達，Nano 先保存該 ID 的 CANCELLED tombstone；稍後抵達的
同 ID RUN 也會被去重而不執行，避免「畫面說取消、程式卻晚一步啟動」的競態。

```text
Nano: fibonacci job=7 cancelled
job=7 state=CANCELLED result=0 detail=0
```

直接在某塊板用 `guest <name> -run` 本機執行時，輸入 `q` 可在 VM 指令邊界中止。
不用 Ctrl+C，因為 PC monitor 會把 Ctrl+C 當成本機退出訊號攔截。執行期間其他
UART 字元不當成 shell command；它們會被丟棄。

若 Nano 失聯超過 2 秒或一筆工作超過 120 秒，Uno 把執行中工作標記 UNKNOWN。
UNKNOWN 不是失敗，而是 Uno 無法證明它是否已經完成。此時不能再送一般新工作：

```text
job discard
```

只清除 Uno 的不確定紀錄，不會撤銷 Nano 已產生的副作用。或者在 Nano ONLINE 後：

```text
job retry
job retry confirm
```

第一行只顯示重複執行警告；第二行才建立 **新的 job ID**。舊工作可能其實已執行，
因此 retry 只適合 checksum 這類沒有外部副作用的純計算，不適合「扣款」「寫 EEPROM」
或不可逆致動。Cancel control packet 變成 UNKNOWN 時不提供 retry，避免把控制命令誤當工作。

2026-09-16 實機驗證：COM5 Uno 派送 Nano SRAM 中的 fibonacci，兩秒後執行
`job cancel`，Uno 收到同 ID 的 CANCELLED/result=0/detail=0，Nano 未重開；再次
`jobs` 保持 CANCELLED。COM7 本機 `guest fibonacci -run` 執行中輸入 `q` 也回到
`os>`。取消封包 CRC/逐 byte corruption tests 與既有完整 host tests 均通過。
UNKNOWN 的真實拔線／恢復測試尚未執行，文件不把它標成已驗證。

#### 新版簡單操作（先讀這裡；下方 CRC 指令是進階用法）

兩塊板各自跑同一套 OS 的不同角色。COM5 Uno 是 coordinator（派送者）；
COM7 Nano 是 worker（執行者）。Guest 是編譯成 RISC-V 指令的程式，
由板上的 emulator 執行；job 是「請 Nano 執行某個 guest 的一次請求」。
不是輸入 C 原始碼給 Nano，也不是在 PC shell 輸入 `job run`。

先在 PC 的專案根目錄開 Uno shell：

```sh
make monitor PORT=COM5
```

在出現 `os>` 後輸入：

```text
help
nodes
job run builtin
```

等待自動結果即可。`jobs`、`job status` 或 `job` 都是查最近一筆請求，
不是查所有 guest，也不是重新執行。DONE 的 result 是程式 return 數值；
REJECTED 下方會直接說明找不到程式、版本不同或 VM 正忙。

要跑自己寫的程式，退出 monitor（Ctrl+]），在 PC shell 執行：

```sh
make guest-only PORT=COM7 GUEST_C=guest-riscv/workloads/checksum.c
make monitor PORT=COM5
```

然後在 Uno 的 `os>` 輸入 `job run checksum`。這會跑 **Nano 上**的 checksum，
不會重燒 AVR Flash。`job run checksum high` 可指定 HIGH，另有 low、normal；
省略時為 normal。現在只有一個遠端 job，優先度不是跨板負載平衡或插隊機制。

查看 Nano 上有哪些程式：退出 Uno monitor，`make monitor PORT=COM7`，
在 Nano 輸入 `guest list`。兩板的 SRAM guest directory 各自獨立，
Uno 的 `guest list` 不會列出 Nano 的程式；`guest <name> -run` 也只跑目前連線的板子。
目前尚未實作 I²C 遠端目錄查詢。重新啟動 Nano 後，上傳的 SRAM 程式需要重新載入。

`help` 是依目前角色顯示的入門步驟；`help debug` 才列底層指令與數字狀態。
舊指令 `job run checksum 2 d8ca` 仍可用：它要求 Nano 上 image CRC 恰好是 d8ca。
新版 `job run checksum` 是 protocol version 2，選 Nano 當下 directory 的同名 image，
不指定某個版本 fingerprint；binary loader 的 image CRC、I²C packet CRC、ACK/retry、
job ID 去重仍保留。使用新版前必須更新兩塊板韌體（舊 worker 不接受 version 2）。
名稱模式不會額外重新計算整個執行中 guest memory 的 CRC，也不是記憶體隔離。

新版已在 COM5/COM7 實測：`job run builtin` 與 `job run builtin high` 都自動
回報 DONE=42；不合法的優先度會顯示範例；舊版指定錯 CRC 仍被拒絕並說明原因。
Checksum 重新以 `make guest-only PORT=COM7` 載入後，`job run checksum`
不需手填 CRC 即可自動回報 DONE=8。兩板 Flash 均讀回驗證，host tests 通過。

#### `jobs` 是什麼？現在會自動顯示結果

Job 是「交給另一塊板執行的一次工作」，不是額外的 AVR native task。
例如 Uno 要 Nano 執行 builtin guest，這次請求會取得一個 job ID。
`jobs` 只查詢最近一筆請求的狀態與結果，不是開始執行，也不是完整歷史清單。

I²C 本來就持續運作：Uno 的非阻塞 state machine 約每 500 ms 查詢 Nano，
Nano 的 TWI ISR 回覆 heartbeat 和 job 狀態；完成後並不停止輪詢。
這不是一直占用 bus，更不是再開一個 native task。Shell 等待輸入時仍會服務通訊。
本機執行其他長操作時，master 的 main-loop polling 仍可能延後；它不是全程背景 ISR。

現在 coordinator 會自動顯示 DONE、FAULT、REJECTED 或 UNKNOWN，每個 job 的
同一結果狀態只通知一次，不會每 500 ms 洗版；UNKNOWN 後取得完成結果也會再通知。
你正在打字時，通知延後到輸入行清空，不會覆蓋正在輸入的指令。
因此只要輸入下面的派送指令，接著等待，不必一直打 `jobs`：

```text
os> job run builtin 2 3f5b
submitted job=4
os>
job=4 state=DONE result=42 detail=2
os>
```

`4` 是範例 ID，實際會遞增。DONE 表示完成；42 是 guest 回傳的數值，不是錯誤碼；
這裡 detail=2 是 VM HALT 狀態。UNKNOWN 代表結果尚不能確定，不等於已失敗。
`jobs` 隨時可用來重新查詢。Guest 中間的 debug print 仍在 Nano 的 UART，
目前沒有透過 I²C 轉發到 Uno。

這版已不是只有 heartbeat。COM5 可以要求 COM7 執行它 directory 中的 guest，
並透過 I²C 讀回 job ID、狀態與結果。兩邊 `help` 已加入角色適用的命令。
先支援一個在途 job 和一筆最近結果；還沒有多 job queue、跨節點負載平衡、
自動重派或 guest context migration。

最快的操作（目前 builtin image CRC 為 `3f5b`；重新編譯 builtin 後請重新查）：

```sh
make nodes-upload
make monitor PORT=COM7
```

```text
os> guest dir
id=0 name=builtin size=154 crc=3f5b pri=2 entry=0
```

按 Ctrl+] 退出 Nano monitor，再切到 coordinator：

```sh
make monitor PORT=COM5
```

```text
os> nodes
os> job run builtin 2 3f5b
submitted job=1
os> jobs
job=1 state=DONE result=42 detail=2
```

`job run <name> <priority> <image-crc-hex>` 中 name/CRC 是 **Nano directory** 的 image，
不是 Uno directory，也不是 PC 上的檔案路徑。Coordinator 不傳 image code，只傳要求。
Priority 範圍 0..3；這階段遠端一次只執行一個 guest，所以 priority 還不會讓它和
其他遠端 jobs 競爭 CPU。它是派工描述的一部分，worker 會設定到本機 guest metadata。

自己的 C guest：

```sh
# 只編譯 RV32 C 並上傳 Nano SRAM，不重燒 AVR Flash
make guest-only PORT=COM7 GUEST_C=guest-riscv/workloads/checksum.c
make monitor PORT=COM7
```

查 `guest dir` 得到 checksum 的 CRC（這份 workload 為 `d8ca`），退出並切 COM5：

```text
os> job run checksum 2 d8ca
submitted job=2
os> jobs
job=2 state=RUNNING result=0 detail=0
os> nodes
os> jobs
job=2 state=DONE result=8 detail=2
```

Job 是非同步派送；`submitted` 只代表 Uno 保存要求，不代表 Nano 已接受。
`jobs` 需要再查，這版沒有自動插入結果通知，避免打斷正在輸入的 shell line。
Debug print/guest output 留在 Nano UART，不轉送到 Uno。Worker 跑 guest 時它的
shell parser 等 guest 結束，但 TWI ISR/heartbeat 繼續回應；Uno shell 不必等待。

| 狀態 | 意義 |
| --- | --- |
| NONE | 尚無 job |
| QUEUED | Uno 等接受，或 Nano 已接受、等 shell 安全點 |
| RUNNING | Nano 正在執行 |
| DONE | guest 正常 EXIT；result 是完整 32-bit exit code |
| FAULT | VM fault 或未能執行；detail 是 VM status |
| REJECTED | Nano 拒絕要求；detail 1=找不到 image、2=directory CRC 不符、3=本機 VM 忙 |
| UNKNOWN | 失聯／執行等待超過 120 秒／worker 丟失已接受記錄；不代表 guest 一定停止 |

DONE 的 detail=2 是 RV32_HALTED，與 REJECTED detail=2 的 CRC 錯誤不同；先看 state
再解釋 detail。RUNNING 時 result=0 是尚無完成結果，不是已正常 exit 0。

兩秒 heartbeat 失聯時，在途 job 變 UNKNOWN。相同 ID 的有效狀態回報可重新確認
RUNNING 或完成；超過 120 秒則不再把活躍狀態當成已確認完成，但仍可接受後來的
DONE/FAULT。UNKNOWN 暫不允許新 submit／手動 resend，防止工作其實仍在別處執行。
本版沒有 abandon/cancel。若 worker reboot 丟失記錄，只能人工確認，再重置兩板
（例如 `make nodes-upload`）並重新載入 SRAM images；不自動重跑有未知副作用的工作。

#### Retry 和去重

Uno 在沒看到相同 job ID 的狀態前，最多自動發送三次，間隔至少約一秒。
Hardware byte ACK 只表示 TWI 收到資料；application ACK 是 CRC-valid report 中
回傳相同 job ID，表示 worker 已驗證並保存這個要求（或保存拒絕結果）。

```text
os> job resend
resending same job ID
os> jobs
job=1 state=DONE result=42 detail=2
```

這個命令刻意保留同一 ID/payload，不產生新 job。在 Nano 查 `jobs` 可看到
`runs=1`，表示實際遠端 guest execution count，重送不應增加。
`runs` 不包含使用者直接 `guest ... -run`，也不包含 rejected commands。

Worker 保留最近 ID/command CRC/result：相同 ID 不再執行，較小 ID 也不接受，
不同 payload 的 conflicting duplicate 被忽略。Coordinator 開機後會從 worker
report 同步最大 ID，再分配更大的 16-bit ID；65535 後不自動 wrap，需人工重建 session。
這只是**本次 worker 開機內**的去重，不是持久化 exactly-once 保證。Worker reboot 後
SRAM 記錄消失，CRC 也不是密碼學身分驗證；正式版本需要 session/boot epoch 等設計。

#### Wire format 與 code

新協定兩板一起升級；原四 byte XOR heartbeat report 已改成 CRC16 report。
所有多 byte 欄位都 little-endian，CRC16-CCITT 初值 FFFF、多項式 1021。

```text
write 21 bytes:
magic 4A | job ID u16 | priority u8 | name[12] NUL-terminated
| image CRC u16 | version=1 | packet CRC u16

read 14 bytes:
node=12 | heartbeat u16 | job ID u16 | state u8 | detail u8
| result u32 | version=1 | packet CRC u16
```

`job_protocol.c/.h` 處理 serialization/CRC/validation，host unit tests 會逐 byte
翻轉封包驗證 corruption 被拒絕。`nodes.c` master 增加 write phases、retry、report
解析。Worker ISR 收到 STOP 且長度恰好 21 才把 inbox 標成 ready；`nodes_poll()`
原子取出封包後驗證 name、CRC、job ID，檢查 guest directory 的 image CRC。
`nodes_service()` 只從 shell 安全點呼叫，設定 RUNNING 後執行 `rv_demo_run_name()`，
結束再原子更新 job result/state。ISR 不呼叫 VM 或 UART。

Directory CRC 比對是 image 身分檢查，不是整個程序記憶體隔離，也不保證 guest 自己
沒有改寫其 memory。現在也不跨 I²C 讀取每個 image byte 或驗證任意 guest 計算結果。
COM6 那類「能 heartbeat 但算錯」仍需要預期結果或多節點比對才能發現。

靜態 SRAM 約 coordinator 1441 bytes、worker 1430 bytes；其餘必須留給 AVR call
stack/ISR frame，不適合立刻再堆大量 native tasks 和 jobs。原 3 個 VM image slots
仍保留，這版沒有為 remote job 額外配置 VM register file 或 guest memory。

實機已驗證 builtin DONE=42、missing guest REJECTED detail=1、錯誤 image CRC
REJECTED detail=2、同 ID resend 後 worker runs 不增加；checksum 只上傳到 Nano
SRAM 後，由 Uno 派送，觀察到 RUNNING 再 DONE=8。尚未驗證真實斷電後的
session recovery、丟 ACK 注入、120 秒超時及並發工作佇列。

COM5/COM7 現在可以跑 OS + I²C，不再只有獨立 wire test。編譯與燒錄：

```sh
make nodes-build    # 兩種角色，獨立 build directories
make nodes-upload   # COM7 worker、COM5 coordinator；兩板均讀回驗證
make monitor PORT=COM5
```

`build/nodes-master/os.hex` 是 coordinator (`NODE_ROLE=1`)，
`build/nodes-worker/os.hex` 是 worker (`NODE_ROLE=2`)。
普通 `build/atmega_os.hex` 預設 `NODE_ROLE=0` 不使用 I²C。
請不要用普通 `make upload` 更新 coordinator，否則會把 nodes 功能關掉。
兩個角色目前刻意沿用單一 native stack (`LGT_SAFE_MODE=1`)，在真正 16 MHz
ATmega328P 上額外啟用 Timer0 interrupt，worker 再啟用 TWI interrupt。
**這不是恢復多 native task 的 preemptive profile；跨板派工使用上面的 Job 介面。**

操作 coordinator：

```text
os> nodes
remote=0x12 state=ONLINE hb=17092 failures=0 err=0
```

`hb` 是 worker 的 16-bit millisecond clock 低位，每約 65.536 秒 wrap；
coordinator 只比較 heartbeat 是否變動，不直接拿兩台的 clock 相減。
`failures` 是開機以來累積 transaction failures，最大 65535，恢復後不歸零。
`err` 是最近 transaction 的 TWI status（十進位）；0 表示最近封包有效，
72 (`0x48`) 表示 slave address NACK，255 表示階段 timeout，254 表示封包驗證失敗。

State 的定義：

- OFFLINE：從未收到 heartbeat，或距離上次 heartbeat 變動已達 2000 ms。
- ONLINE：收到有效封包，且 heartbeat 與前次不同。
- SUSPECT：近期曾正常，但一次 transaction 失敗或 heartbeat 沒有進展；
  未達失聯期限，先懷疑、不立即認定死亡。

每 500 ms 開始一輪 probe；START/address/read/STOP 是非阻塞狀態機，
每次 `nodes_poll()` 只推進一步，不在裡面等硬體完成。各階段最多 50 ms，
超時後嘗試 STOP 並重新啟用 master TWI，不是完整 electrical bus recovery。
SDA 被故障板一直拉低時仍可能讓全部節點失聯，但 shell 不應因此忙等卡死。

## 不用拔線的故障注入

先在 Nano terminal 操作：

```sh
make monitor PORT=COM7
```

```text
os> nodes pause
worker heartbeat paused
```

它只停用 Nano TWI response，不停 UART/clock/VM。退出 monitor (`Ctrl+]`) 後：

```sh
make monitor PORT=COM5
```

```text
os> nodes
remote=0x12 state=OFFLINE hb=29090 failures=8 err=72
os> help
```

再回 Nano：

```text
os> nodes resume
worker heartbeat resumed
```

Uno 不需要 reboot，下一次有效 probe 就能回 ONLINE。`nodes` 在 Nano 顯示的是 local
狀態，paused 時 local OFFLINE；不是「Nano 對自己做 network probe」。
Monitor 目前仍一次只保留一個 relay，因此要依序切 terminal，不假設可以同時開兩個。

已實機驗證正常 heartbeat、pause→OFFLINE、offline 時 help/shell 可用、resume→ONLINE。
還沒做實體斷線、SDA stuck-low、長時間 soak 或觀測短暫 SUSPECT 的測試。

兩個 node profiles 的 SRAM 靜態用量：coordinator 1389 bytes、worker 1369 bytes，
在 ATmega328P 的 2048 bytes 中分別留下 659 / 679 bytes 給真正 AVR call stack 等動態
使用。剩餘數字不是「可任意再配置的 guest RAM」；三個 192-byte guest spaces 已包含
在靜態用量中。這階段沒有新增一個 native task stack，而是利用現有 shell/VM loop
呼叫 nonblocking poll。往後 job queue 也必須控制容量。

## Code 與 timer ownership

`src/kernel/nodes.c` 保存 node table、probe phase/deadline、worker response，
`nodes.h` 提供 init/poll/info/pause/resume。`main.c` 初始化，shell 等 UART 時每約
1 ms 呼叫 poll；VM run loop 也在 instruction slice 間呼叫，避免長 guest 完全餓死
network service。Binary upload 與較長 shell output 暫時不 pump master probe，
可能出現 transient timeout/SUSPECT；之後收到新 heartbeat 可恢復。

- Timer0：新 node wall clock，CTC /64 OCR0A=249，每毫秒 ISR 加一次 32-bit clock。
- Timer1：原本 guest slice deadline，仍是 polling、沒有 Timer1 ISR。
- Timer2：保留給原 native kernel；目前單 stack profile 停用。

Node clock 與舊 safe-mode `timer_millis()` 不是同一個 counter，這階段不改舊 timer API。
32-bit clock 讀取用 atomic block，deadline 使用 unsigned elapsed / signed difference
避免 wrap 破壞 timeout。Worker TWI ISR 取得 clock snapshot；ISR 本身不印 UART。
原 panic vector table 對本 profile 的 Timer0 COMPA/TWI vectors 留出合法 handlers，
沒有把所有未預期 interrupt 都靜默吞掉。

下一階段仍是 job queue + JOB_RUN/JOB_DONE + job ID/CRC/retry。Heartbeat 只能證明
節點能回應，不證明它算對，也不證明工作真的執行，因此 COM6 的錯誤運算要用
result validation 或 replicated computation 才能處理。

2026-09-14 實機：COM5 Uno R3 作 master、COM7 Nano 作 slave，地址 `0x12`
（十進位 18）。接 A4↔A4、A5↔A5、GND↔GND；各自 USB 供電，不互接 5V/VIN。
COM6 故障板尚未接入。無外接電阻，兩端啟用內部弱 pull-up，以 10 kHz 短線測試。
這只證明目前接線可通訊，不能保證弱 pull-up 適合長線或正式部署。

## 建立與使用測試韌體

```sh
make i2c-test-build       # 只編譯兩份獨立測試
make i2c-test-upload      # COM7 slave、COM5 master，覆寫兩板 Flash 並讀回驗證
make monitor PORT=COM5   # 4800 baud 查看 master 的每次讀取結果
```

`tools/i2c_heartbeat.c` 不依賴 OS scheduler 或 RV32 VM，方便先排除它們的影響。
生成 `build/i2c-master.hex` / `build/i2c-slave.hex`，不覆寫 `build/atmega_os.hex`。
**現在這兩塊板是測試韌體，沒有 shell、沒有 `os>`，也不能上傳 guest。**
主力 port 的 Makefile 預設已改為 COM5。

实測連續結果：

```text
sample=8 ONLINE heartbeat=5610
sample=9 ONLINE heartbeat=6172
sample=10 ONLINE heartbeat=6737
...
sample=18 ONLINE heartbeat=11257
```

`sample` 是 master 發起的讀取次數；`heartbeat` 是 slave 約每毫秒更新一次的
16-bit counter，不是精確 uptime。UART 與 ISR 都會讓軟體 delay 有誤差。
Counter 到 65535 會 wrap 回 0，不應把它當作 task ID。

## 程式與 protocol 的意思

ATmega328P 的 I²C peripheral 叫 TWI（Two-Wire Interface）。Master 用 `TWBR=198`、
prescaler=4，依 `F_CPU/(16 + 2*TWBR*prescaler)` 得到 10 kHz SCL。
SDA/SCL 保持 input，PORT bits 設為 1 只啟用弱 pull-up，不用 GPIO 主動驅動高電位。

Master 每次發 START、slave address + read bit，收 4 bytes，前三次 ACK、最後 NACK，
然後 STOP。每個等待最多約 50 ms，避免拔線／bus stuck 讓 master 永久忙等。
失敗後重新啟用 master controller，不等於已能解救一直拉低 SDA 的故障 slave。

Slave `TWI_vect` ISR 在被 master 讀取時取得 heartbeat snapshot，回覆：

```text
node_id=0x12 | heartbeat_low | heartbeat_high | XOR(previous 3 bytes)
```

Main loop 更新 16-bit counter 時暫時關 interrupt，避免 ISR 讀到一半新、一半舊的值。
ISR 不印 UART，避免阻塞 I²C。這份 wire test 用 XOR 做基本錯誤偵測，**不是 CRC**；
正式 job protocol 應使用 CRC、sequence number 和 ACK/retry。

Master 輸出 `ONLINE` 表示目前有效封包，`STALE` 表示兩次 counter 完全相同，
`BAD_FRAME` 表示 node ID/XOR 不符，`OFFLINE` 表示 transaction 失敗。
`twi_status` 是硬體狀態的十進位值；255 是我們自訂的 timeout。
`pins=48` 表示 A4/A5 當下都為高；它只是瞬間電位，不代表通訊已成功。

## 恢復原 OS

```sh
make upload PORT=COM5
make upload PORT=COM7
# 重新建立 SRAM guest（重燒／reset 不保留 UART 上傳的 image）
make guests-only PORT=COM5 GUEST_CS='guest-riscv/workloads/*.c'
```

先前新增的 `make hw-test` 用同一批三個 workloads 驗證多板的 exit 與 integrity records，
不重燒 Flash，要求板上已是 OS。`make hw-test-flash` 先逐板重燒 OS 再驗證；
預設測 COM5、COM7、COM6，COM6 失敗列為 XFAIL，正常板失敗回傳非零。
XFAIL 不代表錯誤被修復，只是這塊板被刻意列為已知不穩定測試對象。

## 接進 scheduler 的早期規劃（歷史紀錄）

以下是 heartbeat 階段的原始規劃；目前已完成上面的第一版 Job 派送。
當時通訊已測通，但沒有跨板工作派送。規劃是把 driver 做成 OS transport，新增
`nodes` 指令與 node table（node ID、最近 heartbeat、ONLINE/SUSPECT/OFFLINE）。
再加入 `JOB_RUN` / `JOB_DONE`、job ID、CRC、timeout 與 retry，讓 coordinator 選 worker。
各板自己的 scheduler 負責本機 task 切換，coordinator 負責「工作交給哪塊板」，
不是透過 I²C 直接切換別塊 AVR 的 stack pointer。

最後才加入 COM6：它既可能算錯，也可能拖住整條 bus。失聯與錯誤結果是兩種故障；
heartbeat 活著不代表計算正確，需要 expected result／雙節點比對等方法。
重派工作还需要去重（job ID）與處理副作用，否則「ACK 遺失」可能讓同一工作執行兩次。
