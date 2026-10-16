# ATmega OS

ATmega OS is an educational bare-metal operating-system project for 16 MHz
ATmega328P-compatible boards. It combines three related experiments:

- a stackful preemptive AVR kernel;
- a small RV32I interpreter that runs freestanding RISC-V guests from SRAM;
- a two-board distributed profile in which an Uno coordinates a Nano over I2C.

The primary profile uses a genuine Uno on `COM5`. Guest binaries remain on the
PC and the monitor loads one requested image into the Uno's single SRAM cache
before the Uno executes it. The earlier Uno/Nano I2C distributed profile is
still available as a separate build.

## Quick start: PC-backed Uno

```bash
make guest-library GUEST_CS='guest-riscv/workloads/*.c'
make upload PORT=COM5
make monitor PORT=COM5
```

At the `os$` shell:

```text
os$ ls
os$ run checksum
os$ mem
```

`guest-riscv/` is a Git submodule containing the freestanding guest sources.
The PC cross compiler produces RV32I images no larger than 192 bytes. `ls`
lists the PC library. `run NAME` demand-loads that image in CRC-protected
16-byte chunks, displays progress, and starts the Uno VM automatically. A new
run replaces the previous SRAM image, so the PC library is not limited to the
AVR slot count.

Clone both repositories with:

```bash
git clone --recurse-submodules https://github.com/xuwwwww/atmega_os.git
```

## Implemented features

### Native AVR kernel

- Timer2 scheduler tick, configurable with `KERNEL_TICK_HZ`;
- stackful preemptive tasks with private SRAM stacks;
- four fixed-priority levels and round-robin selection within a level;
- AVR assembly context switching for `r0`-`r31`, SREG, and SP;
- READY, RUNNING, SLEEPING, BLOCKED, and TERMINATED states;
- wrap-safe sleep and timed waits;
- task return, exit, and restart support;
- compact TCBs and stack watermarks;
- counting semaphores and non-recursive mutexes with priority inheritance;
- fixed-capacity message queues and event flags;
- one-shot and periodic software timers with deferred callbacks;
- stack guards, UART panic frames, `.noinit` panic records, and reboot support.

### UART and shell

- RX/TX ring buffers;
- binary framing with raw-byte escaping;
- build-time Huffman compression for fixed messages;
- host-side decoding in the Python monitor;
- command history maintained on the PC;
- role-aware `uno$`, `nano$`, and `os$` prompts.
- a 1,000,000-baud application link on the reference Uno;
- PC-backed demand loading with visible byte/percentage progress.

### RV32I and distributed execution

- an RV32I register file, virtual PC, and 192-byte guest address space;
- fetch/decode/execute for the base integer instructions needed by the guests;
- ECALL services for output, yield, sleep, and exit;
- named SRAM images with size, CRC, priority, and entry-point metadata;
- binary UART loading with chunk CRC, acknowledgement, and retry;
- I2C image deployment, whole-image verification, execution, cancellation,
  timeout handling, and job-ID deduplication;
- `run --all`, repeated runs, two-run result verification, and recent results.

## Repository layout

```text
src/
├── drivers/        AVR hardware drivers
├── generated/      generated compressed message payloads
├── kernel/         scheduler, IPC, timers, panic, and node protocol
├── tasks/          shell, VM service, LED, and test tasks
├── vm/             RV32I interpreter
└── main.c          boot and profile initialization
guest-riscv/        freestanding RV32I guest submodule
tools/              uploaders, monitor, generators, and host tests
docs/               design and tutorial documents
```

## Build

From Ubuntu under WSL:

```bash
cd /mnt/d/atmega_os
make clean all
```

The default native tick is 2 kHz, or one scheduling opportunity every 0.5 ms:

```bash
make clean all KERNEL_TICK_HZ=1000UL
```

Task stack capacities are build-time settings:

```bash
make clean all \
  LED_STACK_SIZE=160 \
  SHELL_STACK_SIZE=224 \
  TIMER_SERVICE_STACK_SIZE=160 \
  IDLE_STACK_SIZE=128 \
  RV_DEMO_STACK_SIZE=128
```

Do not optimize a stack until its unused watermark reaches zero. Zero means
there is no measured safety margin and overflow may already have occurred.
Measure the worst workload and retain at least 16-32 bytes of margin.

Build outputs:

- `build/atmega_os.elf`: symbols and link information;
- `build/atmega_os.hex`: AVR flash image;
- `build/atmega_os.map`: linked memory map.

The Makefile uses the installed Microchip ATmega device family pack. Override
`DFP_DIR` if the pack is installed elsewhere.

## Flash and monitor

The project invokes the Windows AVRDUDE binary from WSL because the tested USB
serial bridges are more reliable when owned by Windows:

```bash
make upload PORT=COM5
make monitor PORT=COM5
```

Bootloader upload uses 115200 baud. The reference Uno application UART uses
1,000,000 baud with `U2X=1` and `UBRR=1`, which has zero divider error at
16 MHz. Its interrupt-driven RX path was validated with complete CRC-checked
guest transfers. Exit the monitor with `Ctrl+]` or `Ctrl+C`.

For the distributed profiles:

```bash
make nodes-master-upload NODE_MASTER_PORT=COM5
make nodes-worker-upload NODE_WORKER_PORT=COM9
make nodes-upload
```

Windows may reassign a COM number after reconnecting a board. Override the
corresponding Make variable rather than editing the Makefile.

## Build the PC guest library

Build one selected C file without occupying Uno SRAM:

```bash
make guest-build GUEST_C=guest-riscv/workloads/checksum.c
```

Build any number of selected images into `guest-riscv/build/`:

```bash
make guest-library GUEST_CS='guest-riscv/workloads/*.c'
```

Keep `make monitor` running, then use `run NAME`. The monitor performs the
binary transfer through the same open serial connection. The Uno retains only
one 376-byte VM slot; the next run evicts it. `mem` reports linked static SRAM,
the current stack/data gap, slot count, and VM storage size.

## Optional two-node profile

Connect Uno A4/SDA to Nano A4/SDA, A5/SCL to A5/SCL, and GND to GND. Each board
remains powered by its own USB cable. Build and flash with:

```bash
make nodes-upload NODE_MASTER_PORT=COM5 NODE_WORKER_PORT=COM9
```

## Tests

```bash
make test
make nodes-build
```

The host suite covers message framing and compression, event flags, queues,
software timers, RV32I execution, guest upload packets, hardware-regression
parsing, and the distributed job protocol.

## Documentation

- [Full system guide](docs/os-guide.md)
- [Memory and context switching](docs/memory-and-context-switch.md)
- [Preemptive scheduler](docs/preemptive-scheduler.md)
- [Message queues](docs/message-queue.md)
- [Huffman UART protocol](docs/huffman-uart.md)
- [Timer ownership and tick rates](docs/timers.md)

This is an educational RTOS and emulator, not a production or safety-critical
operating system.
