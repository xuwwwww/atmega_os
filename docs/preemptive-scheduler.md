# Preemptive scheduler

## 兩種 context switch 入口

目前同一組 `CONTEXT_SAVE`/`CONTEXT_RESTORE` 有兩個入口。

Task 主動切換：

```text
CALL task_yield       CPU 先 push task return PC
CONTEXT_SAVE          push r0-r31、SREG，保存 SP
scheduler_select_next
載入下一個 SP
CONTEXT_RESTORE
SEI
RET                   pop 下一個 task PC
```

Timer 強制搶占：

```text
Timer interrupt       hardware push interrupted PC
CONTEXT_SAVE
scheduler_tick        更新 tick/millis 並選下一個 task
載入下一個 SP
CONTEXT_RESTORE
RETI                  pop PC 並啟用 interrupt
```

兩條路在 context save 前都已經有 2-byte PC 位於 stack，因此保存 frame 的
形狀相同。Cooperative 路徑用 `SEI; RET`，interrupt 路徑用 `RETI`。

## Task return

每個新 stack 的初始 PC 都指向 `task_bootstrap()`：

```c
static void task_bootstrap(void)
{
    g_current_task->entry();
    task_exit();
}
```

如果 entry 是無限迴圈，永遠不會抵達 `task_exit()`。如果 entry return，
`task_exit()` 在中斷關閉下設為 `TASK_TERMINATED`，然後切換出去。
Scheduler 只挑 READY task，所以不會再回到已終止 task。

目前 stack 是靜態陣列，因此終止只回收 CPU eligibility，不會回收 SRAM。
`task_restart(index)` 只接受 `TASK_TERMINATED`，它保留原 entry、stack address
與 stack size，重新填入 watermark、建立初始 frame，再設為 READY。不能用它
強制重啟 RUNNING、READY、SLEEPING 或 BLOCKED task，因為那些 task 的 context
可能仍在使用中。

## Sleep 不是週期排程

`task_sleep(n)` 的意思是「從呼叫當下起，至少 n ms 內不要排我」，不是
「每 n ms 自動執行一次」。例如 task 執行工作花 3 ms，再 sleep 100 ms，
兩次工作的起點約相隔 103 ms。若將來需要不累積誤差的固定週期，應另做
`task_delay_until(&next_wake, period)`；現在少量 task 的線性 sleep 掃描比較
省 SRAM，也更容易驗證。

## 為什麼現在開始需要同步

Cooperative 時，只要 task 不 yield，另一個 task 就不會碰共享資料。現在
Timer 可以在任意 instruction 邊界搶占，所以以下 read-modify-write 不再
天然安全：

```c
shared_counter++;
queue.head = next;
resource_owner = current;
```

需要分清四種工具：

```text
critical section  保護非常短的 kernel/ISR 共享操作，暫時關 interrupt
mutex             task 對 task 的單一資源所有權，可睡眠等待
binary semaphore  事件/訊號，不強調 owner
counting semaphore N 個同類資源或累積事件數量
```

ISR 不可以等待 mutex，因為 ISR 不能睡眠。ISR 應更新短小狀態、give
semaphore 或喚醒 task，然後返回。

核心仍保留可巢狀的 `scheduler_preempt_disable/enable()`。它只禁止 task
switch，不關閉 Timer/UART interrupt；tick 仍會累加，ISR 仍可工作。這只
適合極短的 kernel 區段：持有期間其他 task 完全不能執行，而且沒有 owner
或等待佇列。UART frame 已改用下面的 mutex，不再靠禁止 preemption 保護。

## Counting semaphore 的實作

目前 `semaphore_t` 只占 3 bytes：

```text
waiters   1 byte，bit n 代表 task n 正在等
count     1 byte，現在可取得的 token 數
max_count 1 byte，count 上限
```

`semaphore_take()` 在關 interrupt 的短區段內檢查 count。有 token 就減一；
沒有就把目前 task 的 waiter bit 設起來、將狀態改成 BLOCKED，然後 yield。
「檢查 count」和「登記 waiter」不可被 ISR 插入，否則可能在兩者之間收到
give 而永遠睡下去，這就是 lost wake-up。

`semaphore_give()` 若有人等待，直接清掉一個 waiter bit 並將該 task 改成
READY；這是 token 的直接移交，所以不另外增加 count。沒人等待才增加
count。`semaphore_give_from_isr()` 使用相同規則，但不在 ISR 中直接切換，
被喚醒的 task 最晚在下一個 timer tick 被選中。

目前選最低 task index，不保證 FIFO 公平；bitmap 的優點是固定 1 byte，
代價是最多 8 個 task。等真正需要更多 task 或 priority 時，再換成 intrusive
wait queue。

UART RX ring buffer 將 semaphore 當「已收到、尚未取出的 byte 數」。RX ISR
成功放入一個 byte 就 give；shell task 用 `uart_read_char_wait()` take。因此
沒有輸入時 shell 是 BLOCKED，而不是一直 polling。

## Timer tick 與原子性

預設 `KERNEL_TICK_HZ=2000`，16 MHz、Timer0 /64 時需要 125 counts，所以
`OCR0A=124`，每 0.5 ms 進一次 ISR；內部累積兩次才讓 millisecond 加一。
若以 `KERNEL_TICK_HZ=1000UL` 編譯，則需要 250 counts、`OCR0A=249`，每
1 ms 進一次 ISR。

AVR 是 8-bit CPU，所以 `g_millis++` 這個 32-bit 操作本身不是單一 atomic
instruction。但它只在 Timer ISR 中執行；AVR 進 ISR 時硬體已清除 SREG 的
I bit，普通 ISR 不會互相巢狀，因此 increment 周圍不必再 `cli()`。task
讀取 32-bit `g_millis` 則可能讀到一半被 ISR 更新，所以 `timer_millis()`
必須短暫關 interrupt，複製完成後恢復原 SREG。

## Mutex

目前 mutex 是 non-recursive，固定占 4 bytes：2-byte registry next pointer、
一個 owner task index 和一個 8-bit waiter bitmap。`owner == 0xff` 表示未上鎖。

```text
mutex_lock
  沒 owner       → current 成為 owner
  current 已擁有 → 回傳失敗，避免自己把自己 BLOCKED
  別人擁有       → 登記 waiter、current 變 BLOCKED、yield

mutex_unlock
  current 非 owner → 回傳失敗，不改 mutex
  有 waiter        → 直接把 ownership 交給一個 waiter 並喚醒它
  沒 waiter        → owner 改回 0xff
```

檢查 owner、寫 waiter bitmap 和改 task state 都放在同一個短 critical
section，避免 lost wake-up。Mutex 不提供 ISR API：ISR 不能等待一個可能由
task 持有的鎖。

UART TX 現在使用此 mutex 保護完整 string、compressed frame 或 binary
frame。當 TX ring buffer 滿了，持有者可被 timer 搶占，其他 UART writer
會 BLOCKED，而 UDRE ISR 仍能排空 buffer。這比原本整段禁止 preemption 更
接近一般 OS 的同步方式。

### Priority inheritance

高優先權 task 加入 mutex waiters 後，kernel 會由所有 task 的 base priority
重新計算 effective priority。每一輪把 waiter 的 effective priority 傳給
mutex owner，最多執行 `task_count` 輪，因此也能處理：

```text
High 等待 A 持有的 mutex
A 又等待 B 持有的 mutex
→ High priority 傳給 A，再傳給 B
```

Unlock 後不是直接把 owner 恢復成 base，而是重新檢查它仍持有的其他 mutex
和 waiters；因此同時持有多把 mutex 時不會過早降 priority。Handoff 會選
effective priority 最高的 waiter。

目前重算在關 interrupt 的 critical section 內完成，成本上限由 8 tasks
和少量 mutex 限制。若將來 mutex 數量明顯增加，應改成增量式 inheritance
與明確的 held/waiting lists，縮短 interrupt latency。

## 下一個核心里程碑

目前 scheduler 已有四級 priority，永遠選 effective priority 最高的 READY
task；同 priority 依目前 index 循環選擇，因此仍是 round-robin。TCB 同時
保存 base/effective priority，兩者各 2 bits。加入 timed-wait cleanup metadata
後，TCB 現在是 16 bytes。

Blocking timeout 已完成，使用 `WAIT_ACQUIRED`、`WAIT_TIMEOUT`、`WAIT_ERROR`
區分結果。Scheduler deadline 到期時會透過 TCB 的 cleanup callback，先從
實際 semaphore/mutex waiter bitmap 移除 task；mutex callback 也會重算
priority inheritance，最後才將 task 改回 READY。

固定容量 message queue 也已完成，它以 items/spaces semaphore 和一把具有
priority inheritance 的 mutex 組成。

1. 加入 event flags。
2. 建立 UART shell 與 tasks/stack/uptime 指令。
3. 視實際需求決定是否加入 recursive mutex 與 recursion count。
4. 把 idle task 的 NOP loop 改成 AVR sleep mode，量測空閒功耗。

不要一開始就只做 spinlock。在單核心 AVR 上，等待中的 task 若持續 spin，
只會浪費它的 time slice；task-level mutex/semaphore 應把等待者設成 BLOCKED。
