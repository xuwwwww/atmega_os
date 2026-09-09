# Build-time Huffman UART log

## 目標

固定 log 在筆電 build 時壓縮，AVR 只保存並傳送壓縮 bitstream；解碼器位於
WSL/Python monitor，不占 MCU Flash decoder code 或 SRAM buffer。

```text
tools/log_messages.json
        ↓ generate_log_codec.py（筆電）
        ├─ src/generated/log_messages.c/h：AVR PROGMEM payload
        └─ tools/generated_log_codec.py：筆電 decode table
```

修改 `tools/log_messages.json` 後執行 `make`，Makefile 會在編譯 C source 前
重新產生三個檔案。generated files 不應手動修改。

## 目前壓縮結果

```text
訊息                         原始 bytes   Huffman bits   payload bytes
ATmega OS UART ready...          24           126             16
shell ready...                   24           118             15
os> prompt                        4            19              3
shell help                      191          1028            129
```

manifest 目前共有 9 個固定 shell 訊息。payload 使用 `PROGMEM`，位於
`.text`/Flash，因此不占 SRAM `.data`；週期性的 `kernel alive` 已移除。

## UART framing

UART 是 byte stream，本身沒有 message boundary。協定使用 `0x00` 作為
escape：

```text
raw nonzero byte       BYTE
raw zero byte          00 00
compressed log frame   00 01 BITS_LO BITS_HI PAYLOAD...
```

payload 長度由 `(bit_count + 7) / 8` 算出，因此不需要再傳 byte count。
最後一個 byte 沒用到的低位 bits 填 0，host decoder 只讀 bit_count 指定的
有效 bits。

PowerShell helper 現在只負責以 binary stream 搬運 COM bytes；Python
`SerialDecoder` 保存跨 read 的 frame state。即使一個 frame 被 Windows
拆成數次 read，decoder 仍會等待完整 payload 才解碼。

## 固定與動態資料

Huffman tree 只根據 manifest 裡的固定訊息建立。固定 log 可以直接傳預先
編碼的 payload；shell 的逐字 echo 是任意輸入，仍走 raw byte escaping。

如果未來要傳數字或 task stats，應增加新的 frame type，例如：

```text
00 02 LENGTH BINARY_PAYLOAD
```

由 host 格式化成人類文字，避免 MCU 保存格式字串或連結 `printf`。

`0x02` 保留給 binary stack report，`0x03` 是動態 shell text。正式 firmware
已移除週期性 output task，`tasks` 與 `stack` 改由 shell 按需傳送 `0x03`
frame，因此 monitor 不會在輸入中途插入週期報告。舊 `0x02` decoder 仍保留，
方便 self-test 或未來診斷使用。

可用 `make test-codec` 在不連接 Arduino 的情況下測試所有 manifest 訊息、
逐 byte 分段解碼，以及 raw NUL escaping。
