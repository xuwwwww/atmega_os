# 固定容量 Message Queue

## 目的

Semaphore 只能傳遞 token；message queue 能在 task 之間傳遞固定大小的資料。
本實作完全不使用 `malloc()`，queue object 與 payload storage 都由呼叫者配置。

```c
typedef struct
{
    uint8_t command;
    uint16_t value;
} command_t;

static message_queue_t command_queue;
static command_t command_storage[4];

message_queue_init(&command_queue, command_storage, 4u,
                   sizeof(command_t));
```

`message_queue_t` 在 AVR 上固定是 16 bytes：

```text
storage pointer                 2
item_size/capacity/head/tail    4
mutex                           4
items semaphore                 3
spaces semaphore                3
                              ----
                                16 bytes
```

Storage 另占 `capacity * item_size` bytes。Queue 和 storage 必須具有永久
lifetime，例如 static/global；不能建立在即將 return 的函式 stack 上。

## Send 與 receive

初始化容量為 N 時：

```text
items.count  = 0
spaces.count = N
```

Send：

```text
take spaces       queue 滿時 BLOCKED
lock mutex
copy item 到 head
head 循環前進
unlock mutex
give items        喚醒 receiver
```

Receive：

```text
take items        queue 空時 BLOCKED
lock mutex
從 tail copy item
tail 循環前進
unlock mutex
give spaces       喚醒 sender
```

Mutex 只在 copy 和 index 更新期間持有，並具有 priority inheritance。

## API

永久等待：

```c
message_queue_send(&queue, &message);
message_queue_receive(&queue, &message);
```

限時等待：

```c
wait_result_t result =
    message_queue_send_timeout(&queue, &message, 100u);

result = message_queue_receive_timeout(&queue, &message, 100u);
```

完全不等待：

```c
if (!message_queue_try_send(&queue, &message))
{
    /* queue full 或 mutex 當下忙碌 */
}

if (!message_queue_try_receive(&queue, &message))
{
    /* queue empty 或 mutex 當下忙碌 */
}
```

Timeout 目前限制的是等待 item/space 的時間。取得 semaphore token 後，內部
queue mutex 採永久等待；這個 mutex 只保護很短的固定大小 copy，不應在正常
使用下造成長時間延遲。

## 為什麼現在沒有 ISR API

ISR 不可呼叫上述 blocking API，因為 ISR 不能等待 mutex 或執行
`task_yield()`。未來的 `message_queue_send_from_isr()` 必須立即成功或失敗，
並需要限制 item size 或使用專門的 single-producer/single-consumer 路徑，
避免在關 interrupt 時複製過大的 message。

## 測試

Host FIFO、full/empty、wraparound、try 和 timeout 測試：

```bash
make test
```

板上 producer/consumer self-test：

```bash
make clean all EXTRA_CFLAGS=-DMESSAGE_QUEUE_SELF_TEST
make upload EXTRA_CFLAGS=-DMESSAGE_QUEUE_SELF_TEST
make monitor
```

輸出定義：

```text
Q  一組 8 個 sequence 全部依 FIFO 順序收到
T  consumer 在空 queue 上正確 timeout
E  順序或 wait result 錯誤
```

測試 queue 容量只有 3，producer 一次送 8 筆，因此會實際走過 full queue、
sender BLOCKED、receiver 喚醒 sender、empty queue 與 receive timeout。

測完恢復正式 build：

```bash
make clean all
make upload
```
