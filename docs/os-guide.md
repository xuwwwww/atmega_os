# ATmega OS: complete system guide

This document describes the current project as one system. It starts with the user-facing workflow, then explains the native AVR kernel, RV32I virtual machine, guest toolchain, UART loader, and two-board I2C execution model.

## 1. What this project is

ATmega OS is an educational operating-system laboratory for the ATmega328P. It contains three layers:

1. A native AVR kernel with tasks, context switching, timers, and synchronization.
2. An RV32I interpreter that treats RISC-V machine code as data and executes it inside a small virtual address space.
3. A distributed profile in which an Uno stores programs and sends selected programs to a Nano worker over I2C.

It is not Linux and it does not provide memory protection. Its purpose is to make low-level mechanisms visible on a machine with 32 KiB of Flash and 2 KiB of SRAM.

## 2. Essential terminology

These words refer to different things:

- **Host PC**: the WSL/Windows computer that compiles firmware and guest code.
- **AVR firmware**: the ATmega328P machine code flashed with AVRDUDE.
- **Native task**: an AVR function executed directly by the hardware CPU.
- **Kernel**: AVR code that schedules native tasks and owns hardware drivers.
- **RV32I interpreter**: AVR code that fetches and emulates RISC-V instructions.
- **Guest source**: a freestanding C or assembly program compiled for RV32I.
- **Guest image**: the raw `.bin` bytes produced from that source.
- **Guest instance**: an image plus its virtual registers, PC, memory, and run state.
- **Program slot**: SRAM storage holding one named guest image and metadata.
- **Job**: one request for the Nano to execute a named guest image.
- **Coordinator**: the Uno that accepts shell commands and controls jobs.
- **Worker**: the Nano that receives images, executes jobs, and reports results.

A guest is not an AVR task. The real ATmega CPU runs the interpreter; the interpreter changes a virtual PC and virtual RISC-V registers.

## 3. Current hardware layout

The reference distributed setup is:

- Uno R3 on `COM5`: coordinator;
- Nano ATmega328P on `COM9`: worker;
- A4/SDA connected to A4/SDA;
- A5/SCL connected to A5/SCL;
- GND connected to GND;
- each board powered by its own USB cable.

Never connect two independently powered 5 V pins together. A common ground is mandatory.

The AVR internal pull-ups can be sufficient for a short, slow experimental bus. External I2C pull-ups are preferable for longer wiring or higher speed. The project deliberately uses a conservative bus rate.

## 4. Fastest working procedure

From the repository root:

```sh
cd /mnt/d/atmega_os
make guest-library GUEST_CS='guest-riscv/workloads/*.c'
make upload PORT=COM5
make monitor PORT=COM5
```

After the monitor prints the prompt:

```text
os$ ls
os$ run checksum
os$ mem
```

The first command builds an unlimited PC-side library in `guest-riscv/build/`.
The second flashes the one-slot local Uno firmware. When `run checksum` is
entered, the monitor reads `checksum.bin`, loads it in CRC-protected chunks,
shows byte and percentage progress, and starts the RV32I VM on the Uno. The
next run replaces the SRAM slot. The separate two-board profile remains
available through `make nodes-upload`.

## 5. Shell operation

The prompt identifies the firmware role:

- `uno$` is the coordinator shell.
- `nano$` is the worker diagnostic shell.
- `os$` is a local non-distributed build.

Type a command and press Enter. The PC monitor handles local echo and command history. Use the Up and Down arrow keys to browse history. `history` asks the monitor to print it.

### Daily commands

```text
help                 concise command summary
ls                   images in the PC build library
run NAME             demand-load and execute one image on Uno
run NAME high        load it with an explicit VM priority
mem                  SRAM/static/current-stack-gap report
history              PC-side command history
clear                clear the terminal
```

The commands above describe the primary local profile. `uno$` adds distributed
commands such as `status`, `jobs`, `kill`, `results`, and `run --all`.

### Batch and diagnostic commands

```text
run --all             run every program stored on Uno, one at a time
run -n 10 checksum    run checksum ten times
run --verify checksum run it twice and compare exit values
cluster               friendly cluster status
nodes                 raw heartbeat/failure counters
programs remote       Nano's cached-program directory
programs local        detailed local slot directory
last                  last distributed job record
advanced              advanced kernel and protocol commands
ps                    native task table
free                  native stack watermarks
faults                reset cause, panic record, and UART errors
uptime                local millisecond clock
```

The shell stays interactive during a remote job. Use `kill` to request cancellation. A local, non-distributed `run NAME` prints that `q` can interrupt it.

### Understanding job states

- `COPYING`: Uno is transferring an image to Nano.
- `WAIT`: the request is queued.
- `RUNNING`: Nano is executing the guest.
- `DONE`: the guest called exit normally.
- `STOPPED`: cancellation was confirmed.
- `FAILED`: the VM reported a fault.
- `NOT-RUN`: Nano rejected the request.
- `UNKNOWN`: communication was lost, so Uno cannot prove whether it ran.

An `UNKNOWN` job is an important distributed-systems state. Retrying the same side-effecting operation may perform it twice. `retry` therefore warns first; `retry yes` allocates a new job ID. `forget` discards the uncertain record without claiming it did not execute.

## 6. Repository layout

```text
atmega_os/
├── Makefile
├── README.md
├── docs/
├── guest-riscv/          independent Git submodule
├── src/
│   ├── drivers/          UART hardware driver
│   ├── generated/        generated messages and built-in guest bytes
│   ├── kernel/           scheduling, IPC, timers, panic, I2C jobs
│   ├── tasks/            shell and service tasks
│   ├── vm/               RV32I interpreter
│   └── main.c            boot and profile selection
└── tools/                uploaders, monitor, generators, host tests
```

`guest-riscv` is intentionally a separate repository because it targets a different CPU. Clone the complete project with:

```sh
git clone --recurse-submodules https://github.com/xuwwwww/atmega_os.git
```

If an existing clone has an empty guest directory:

```sh
git submodule update --init --recursive
```

## 7. Build profiles

`NODE_ROLE` selects the distributed role:

- `0`: local firmware; I2C node support disabled.
- `1`: Uno coordinator.
- `2`: Nano worker at I2C address `0x12`.

`LGT_SAFE_MODE` selects the native scheduling mode:

- `0`: full native multi-stack preemption using Timer2.
- `1`: one stable native supervisor stack; intended for the problematic LGT-compatible clone and currently used by node builds.

Build the local firmware:

```sh
make clean all
```

Build both distributed images without flashing:

```sh
make nodes-build
```

Flash individual roles:

```sh
make nodes-master-upload NODE_MASTER_PORT=COM5
make nodes-worker-upload NODE_WORKER_PORT=COM9
```

Or flash both:

```sh
make nodes-upload NODE_MASTER_PORT=COM5 NODE_WORKER_PORT=COM9
```

The upload targets stop known serial relays before opening a COM port. This avoids the common case where an abandoned monitor owns the port.

## 8. Monitor behavior

Start the coordinator monitor with:

```sh
make monitor PORT=COM5
```

The bootloader uses 115200 baud. The reference Uno application uses 1,000,000
baud. At 16 MHz with double-speed UART, `UBRR=1` produces that rate with zero
divider error. Safe mode now uses a small RX ISR to fill the 32-byte ring;
millisecond polling was unable to receive high-speed command bursts. Binary
loads use 16-byte payloads so a complete packet fits in that ring while CRC is
being calculated. The faulty clone can still be built at 4800 baud with its RX
interrupt disabled.

Exit with `Ctrl+]` or `Ctrl+C`. Opening a serial port can toggle DTR and reset an Arduino. Batch upload keeps one connection open so multiple images do not cause repeated resets.

If a port is inaccessible:

1. close Arduino IDE Serial Monitor and other terminal programs;
2. run `make serial-close`;
3. confirm the Windows COM number;
4. retry `make monitor PORT=COMx`.

## 9. Native AVR kernel

### Boot sequence

`main.c` initializes the watchdog/reset record, UART, timer, scheduler objects, tasks, and role-specific node service. A native preemptive build constructs initial stack frames and transfers control to `scheduler_start()`. A safe-mode build keeps one supervisor call stack and services the shell, I2C, and VM in a polling loop.

### Native task context

The full AVR context consists of `r0` through `r31`, `SREG`, the return PC, and the saved SP. Every stackful task owns a fixed SRAM stack. The switch assembly saves the task, changes to a common kernel stack, runs the C selector, loads the next SP, and restores the selected context.

The change to a common kernel stack is essential. An older implementation called scheduler C code while still using the outgoing task stack. Those calls overwrote saved task frames and produced apparently random reboot loops.

See `memory-and-context-switch.md` for the exact model.

### Scheduling and synchronization

The native scheduler has four priorities and round-robin selection among equal-priority ready tasks. Tasks can yield, sleep, block, terminate, and restart.

The kernel includes:

- counting semaphores;
- nonrecursive mutexes with priority inheritance;
- fixed-storage message queues;
- event flags;
- one-shot and periodic software timers with deferred callbacks.

Priority inheritance limits priority inversion but does not solve deadlock. Code still needs a consistent lock order.

### Panic and reset records

A kernel panic emits a short UART record and reboots. A `.noinit` SRAM record can survive a watchdog reset long enough for the next boot to report the panic code. It is not guaranteed across power loss and does not consume EEPROM endurance.

## 10. AVR memory model

The ATmega328P uses Harvard architecture. AVR code executes from Flash. Mutable data and stacks live in 2 KiB SRAM. EEPROM is nonvolatile but write-limited.

Native AVR processes cannot be loaded into SRAM and executed because the CPU does not fetch instructions there. This is the reason the project added an interpreter. To the AVR, RISC-V instructions are only bytes; `rv32_step()` reads those bytes and performs the equivalent operation in C.

There is no MMU. Native tasks share one address space. RV32 guests receive a software-enforced 192-byte virtual memory array, but the interpreter and kernel themselves are not isolated from one another.

## 11. How the RV32I interpreter works

Each guest CPU state contains:

```c
uint32_t x[32];       /* virtual RISC-V registers */
uint32_t pc;          /* virtual program counter */
uint8_t *memory;      /* guest address zero */
uint16_t memory_size; /* currently 192 bytes */
status and ECALL callback fields
```

One `rv32_step()` operation performs this cycle:

1. Check that PC is four-byte aligned and inside guest memory.
2. Read four little-endian bytes as one instruction.
3. Decode opcode, register fields, function fields, and immediate.
4. Execute the corresponding integer, branch, load, store, jump, or system behavior.
5. Keep `x0` equal to zero.
6. Update PC or set a precise VM fault.

Loads and stores are bounds-checked and alignment-checked. Illegal instructions, invalid fetches, and invalid memory operations halt the guest with a fault rather than intentionally accessing arbitrary AVR memory.

The interpreter implements the RV32I base operations required by the freestanding guest toolchain. It does not implement RISC-V compressed instructions, multiplication/division, floating point, privilege levels, page tables, or Linux devices.

### Time slicing guests

`rv32_run()` executes an instruction budget and returns to the supervisor. The node profile also observes Timer1 so a guest that never calls yield cannot monopolize the shell indefinitely. Its register file, PC, and memory already reside in a C object, so pausing a guest does not require copying a real CPU stack.

Multiple guest names can be stored, but the small Nano executes one distributed VM job at a time. This is time-sliced supervision, not simultaneous multi-core execution.

## 12. Guest system calls

Guest code cannot call `uart_write()` or manipulate AVR registers. It requests a host service with RISC-V `ECALL`. `guest_syscall.h` provides wrappers for:

- writing text;
- yielding to the supervisor;
- sleeping for a requested interval;
- exiting with a numeric result.

The convention uses ordinary RISC-V argument registers and a service number. The host callback reads those virtual registers, validates guest addresses, performs the service, and resumes or halts the VM.

The line:

```text
rv32 checksum exit=118
```

means the emulated program named `checksum` called the guest exit service with value 118. It is not an AVR process exit code and it is not an error unless that workload expected another value.

## 13. Building a guest from C

The guest repository pipeline is:

```text
start.S + selected.c -> ELF -> raw BIN -> UART -> Uno SRAM
```

`start.S` sets the guest stack pointer and calls C `main()`. The linker script places the entry instruction at virtual address zero and reserves the top of the 192-byte region for the guest stack.

Build one file from the parent repository:

```sh
make guest-build GUEST_C=guest-riscv/workloads/checksum.c
```

Inspect its assembly and ELF metadata:

```sh
make guest-inspect GUEST_C=guest-riscv/workloads/checksum.c
```

Build any number of guests into the PC backing store:

```sh
make guest-library GUEST_CS='guest-riscv/workloads/*.c'
```

This target neither calls AVRDUDE nor consumes Uno SRAM. `make monitor` is the
backing-store server. It loads only the image named by a later shell `run`.

### Writing another guest

Create a freestanding source file, for example `guest-riscv/workloads/counter.c`:

```c
#include "../src/guest_syscall.h"

int main(void)
{
    guest_write("counter: start\n");
    for (unsigned i = 0; i < 1000; ++i) {
        if ((i & 63u) == 0u)
            guest_yield();
    }
    return 7;
}
```

Then run on the PC:

```sh
make guest-library GUEST_CS=guest-riscv/workloads/counter.c
make monitor PORT=COM5
```

And at `os$`:

```text
ls
run counter
mem
```

Guest C is freestanding. There is no `printf`, heap, file system, operating-system process API, or normal C library. Keep code and data within the 192-byte image/memory constraint.

## 14. ELF versus BIN

An ELF file contains machine code plus sections, symbols, addresses, and debugging metadata. The tiny interpreter does not parse ELF. `objcopy -O binary` extracts the loadable bytes into a raw `.bin` image.

The linker script is therefore part of the runtime contract. It ensures the first byte in the raw file belongs at guest address zero and rejects an image that exceeds capacity.

You normally write C or assembly and let the toolchain create ELF. Hand-writing an ELF container is possible but provides no advantage here. Hand-writing RV32I instruction words can be useful as an interpreter test, but it is much easier to make encoding and relocation errors.

## 15. UART guest loader

The PC uploader opens the shell connection, starts a named load session, then sends binary chunks. Every chunk carries position and integrity information. Uno acknowledges accepted progress; the PC retries lost or damaged transfers.

The current high-speed chunk size is 16 bytes. Each packet has an absolute
offset and CRC; the host waits for an ACK before sending the next packet. The
monitor displays confirmed progress rather than merely bytes attempted. The
resident slot records:

- name;
- byte size;
- whole-image CRC;
- scheduling priority;
- entry point.

The loader does not write EEPROM or AVR Flash. The PC library survives an Uno
reset, while the single resident cache does not. A subsequent `run` simply
loads the requested image again.

The one-slot local build currently links 613 bytes of static SRAM, leaving
1,435 bytes between linked data and the top of the 2 KiB SRAM before runtime
stack use. The slot itself is 376 bytes. Earlier three-slot coordinator builds
linked 1,569 bytes of static SRAM. The `mem` command reports the linked static
region and the live SP-to-data gap, and the build fails if the configured
minimum runtime reserve is violated.

Huffman text frames and binary loader frames are different protocols. Both are decoded incrementally because Windows serial reads may split a frame anywhere.

## 16. Distributed I2C execution

### Normal run path

When the user types `run checksum`, Uno:

1. Finds the named image in its local SRAM directory.
2. Reads Nano's program directory.
3. Compares name, size, and whole-image CRC.
4. Transfers the image if Nano does not have an exact match.
5. Commits the image only after Nano validates the size and CRC.
6. Sends a run request with a new job ID and priority.
7. Polls Nano's report while leaving the shell usable.
8. Prints the final exit result or fault.

The transfer is split into small I2C commands because the wire protocol has fixed frames and the Nano has little buffering. Current command frames are 21 bytes; report frames are 36 bytes.

### Job IDs and deduplication

Each run receives a monotonically advancing job ID. Nano remembers the active/recent ID and does not execute a duplicate command twice merely because Uno retried a transmission. This gives at-most-once behavior while both nodes retain their volatile state.

A reboot erases volatile history, so the protocol reports uncertainty instead of pretending to provide permanent exactly-once execution.

### Heartbeats

Uno polls Nano continuously and classifies the link:

- `ONLINE`: valid reports are arriving.
- `SUSPECT`: a recent exchange failed.
- `OFFLINE`: Nano has missed the allowed window.

The `status` command gives a friendly explanation. `nodes` exposes raw counters. On Nano, `nodes pause` and `nodes resume` deliberately stop and restore heartbeat behavior for failure testing.

### Cancellation

`kill` cancels an automatic batch and sends a cancel command for the current job. If the worker is executing a guest, the VM checks the cancellation request at its supervision boundary. If the image is still copying, the transfer is abandoned. The coordinator waits for confirmation rather than immediately claiming success.

### Priority

`run name low`, `normal`, or `high` includes a priority in the job record. Priority matters when selecting queued work, but it cannot interrupt an AVR instruction and it does not repair deadlocks. The current tiny worker executes one remote job at a time.

## 17. Timer ownership

The project does not link the Arduino core, so it configures hardware timers itself.

- Native preemptive profile: Timer2 provides the configurable scheduler tick.
- Distributed node profile: Timer0 provides the wall clock.
- RV32 supervisor: Timer1 supplies a polled slice deadline.

Increasing the native tick frequency gives more scheduling opportunities but consumes more CPU in context switching. It does not accelerate the 16 MHz CPU or make each emulated instruction cheaper.

See `timers.md` for formulas and atomicity rules.

## 18. Why output may look strange

### Repeated boot banners

If `ATmega OS UART ready` repeats without a deliberate reset, the MCU is rebooting. Past causes included corrupted native task contexts and an unstable clone board. Use `faults` to inspect reset cause and panic state.

### No local echo or Enter response

The repository monitor supplies terminal behavior and understands framed shell output. A generic serial terminal may use a different newline mode or may not decode compressed messages. Use `make monitor` first when diagnosing the shell.

### `invalid huffman frame`

The firmware and PC decoder were generated from different message manifests, or binary traffic was interpreted as text. Rebuild and flash from one checkout, then restart the monitor.

### A monitor cannot reconnect

Another serial process still owns the COM port. The Make targets call `serial-close`, but Arduino IDE or a manually launched terminal may need to be closed explicitly.

### Random clone-board resets

The original Type-C clone behaved like an LGT-compatible device and was unreliable during private-stack AVR context switching. That board remains useful as a fault-injection target. The genuine Uno is the reference coordinator, and node builds use safe mode for predictable supervision.

## 19. Testing

Run all host tests:

```sh
make test
```

Compile both board profiles:

```sh
make nodes-build
```

Run the multi-board regression without reflashing AVR firmware:

```sh
make hw-test
```

Flash first, then run the hardware regression:

```sh
make hw-test-flash
```

The default hardware list includes the stable Uno, Nano, and intentionally unstable clone. Override `HW_TEST_PORTS` and `HW_TEST_UNSTABLE_PORTS` for another setup.

## 20. Adding SD storage later

An SD card would solve persistence and capacity, not execution by itself. A practical design is:

1. Store named `.bin` images and a directory on FAT or a tiny custom format.
2. Read one selected image into the 192-byte guest RAM or a larger external-memory window.
3. Validate its size and CRC.
4. Initialize virtual registers and PC.
5. Run it through the existing RV32I interpreter.

The current UART and I2C loaders already establish the image contract needed for that path. The storage backend can change while the interpreter remains mostly the same.

Running Linux is a very different scale. A Linux-capable RISC-V emulator needs far more RAM, a much broader ISA and privilege model, traps, timers, interrupt controllers, and emulated devices. An ATmega328P can demonstrate instruction emulation and loading, but its 2 KiB SRAM cannot host a useful Linux machine state.

## 21. Current limits

- Native tasks have no memory isolation.
- Safe-mode node builds do not provide native multi-stack preemption.
- Guest memory is only 192 bytes per active image.
- Uno and Nano guest directories are volatile SRAM.
- The Nano executes one distributed guest job at a time.
- No filesystem, dynamic linker, virtual memory, user/kernel privilege boundary, or general process ABI exists.
- I2C reliability is suitable for a short educational bench, not a hostile network.
- Job history does not survive power loss.

These are useful boundaries rather than hidden failures. Each one suggests a later OS exercise: persistent storage, a richer executable format, resource accounting, fault containment, multiple workers, replicated results, or a larger MCU.

## 22. Recommended roadmap

1. Keep the two-board load/run/cancel workflow stable under long repetitions.
2. Add structured fault injection: disconnect SDA, reset Nano mid-copy, and corrupt one frame.
3. Persist the program directory on SD while keeping CRC validation.
4. Add a second worker and coordinator-side placement policy.
5. Define guest resource limits and observable accounting.
6. Add a versioned executable header for entry point, memory needs, and capabilities.
7. Only then consider a larger RV32 machine model or a larger hardware platform.

The existing project already reaches the central learning goal: code is compiled for a foreign ISA, loaded as bytes at runtime, interpreted with a virtual PC and registers, scheduled as a named job, moved between physical nodes, interrupted, checked, and reported through a shell.
