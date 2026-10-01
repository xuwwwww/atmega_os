MCU      ?= atmega328p
F_CPU    ?= 16000000UL
TARGET   ?= build/atmega_os
SRC      := src/main.c \
	 src/kernel/timer.c \
	 src/kernel/nodes.c \
	 src/kernel/job_protocol.c \
	 src/kernel/software_timer.c \
	 src/kernel/scheduler.c \
	 src/kernel/panic.c \
	 src/kernel/semaphore.c \
	 src/kernel/mutex.c \
	 src/kernel/event_flags.c \
	 src/kernel/message_queue.c \
	 src/vm/rv32i.c \
	 src/vm/rv32_compat.c \
	 src/drivers/uart.c \
	 src/generated/rv_guest_image.c \
	 src/generated/log_messages.c \
	 src/tasks/idle_task.c \
	 src/tasks/led_task.c \
	 src/tasks/queue_test_tasks.c \
	 src/tasks/rv_demo_task.c \
	 src/tasks/shell_task.c
ASM_SRC  := src/kernel/context.S
# The FIREPHX/LGT clone's USB-UART path corrupts occasional bytes at 9600.
# 4800 has passed a complete three-image SRAM upload on the actual COM6 board;
# the loader still checks every prefix CRC and retransmits damaged chunks.
UART_BAUD ?= 4800UL
KERNEL_TICK_HZ ?= 2000UL
# This project's Uno Type-C clone uses an LGT8F-compatible core.  Repeated
# switching between private AVR stacks is not reliable on that chip, so the
# default hardware profile runs the RV guest and shell in one native task.
# Set LGT_SAFE_MODE=0 on a genuine ATmega328P to restore the full preemptive
# multi-task build.
LGT_SAFE_MODE ?= 1
LGT_SAFE_STACK_SIZE ?= 64
LED_STACK_SIZE ?= 160
SHELL_STACK_SIZE ?= 224
TIMER_SERVICE_STACK_SIZE ?= 160
IDLE_STACK_SIZE ?= 128
RV_DEMO_STACK_SIZE ?= 128
RV_GUEST_SLOT_COUNT ?= $(if $(filter 1,$(LGT_SAFE_MODE)),3,1)
MONITOR_BAUD ?= 4800
BUILD_DIR := build
LOG_MANIFEST := tools/log_messages.json
LOG_GENERATOR := tools/generate_log_codec.py
LOG_C := src/generated/log_messages.c
LOG_H := src/generated/log_messages.h
LOG_PY := tools/generated_log_codec.py
RV_GUEST_BIN := guest_images/demo.bin
RV_GUEST_GENERATOR := tools/generate_guest_image.py
RV_GUEST_C := src/generated/rv_guest_image.c
RV_GUEST_H := src/generated/rv_guest_image.h
RV_GUEST_CAPACITY ?= 192

# Use Microchip's official ATmega Device Family Pack installed on this machine.
DFP_DIR      ?= /mnt/d/CodexHome/runtime/toolchains/Microchip.ATmega_DFP.3.6.299
DEVICE_DIR   := $(DFP_DIR)/gcc/dev/$(MCU)
DEVICE_FLAGS := -B$(DEVICE_DIR)/ -I$(DFP_DIR)/include

CC       := avr-gcc
OBJCOPY  := avr-objcopy
SIZE     := avr-size
AVRDUDE  := avrdude

WINDOWS_AVRDUDE ?= /mnt/c/Users/User/AppData/Local/Microsoft/WinGet/Packages/AVRDudes.AVRDUDE_Microsoft.Winget.Source_8wekyb3d8bbwe/avrdude.exe
WINDOWS_POWERSHELL ?= powershell.exe

CFLAGS   := $(DEVICE_FLAGS) -std=gnu11 -Os -Wall -Wextra -ffunction-sections -fdata-sections -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DUART_BAUD=$(UART_BAUD) -DKERNEL_TICK_HZ=$(KERNEL_TICK_HZ) \
	-DLGT_SAFE_MODE=$(LGT_SAFE_MODE) \
	-DLGT_SAFE_STACK_SIZE=$(LGT_SAFE_STACK_SIZE) \
	-DLED_STACK_SIZE=$(LED_STACK_SIZE) \
	-DSHELL_STACK_SIZE=$(SHELL_STACK_SIZE) \
	-DTIMER_SERVICE_STACK_SIZE=$(TIMER_SERVICE_STACK_SIZE) \
	-DIDLE_STACK_SIZE=$(IDLE_STACK_SIZE)
CFLAGS   += -DRV_DEMO_STACK_SIZE=$(RV_DEMO_STACK_SIZE)
CFLAGS   += -DRV_GUEST_SLOT_COUNT=$(RV_GUEST_SLOT_COUNT)
NODE_ROLE ?= 0
CFLAGS   += -flto -DNODE_ROLE=$(NODE_ROLE)
CFLAGS   += $(EXTRA_CFLAGS)
ASFLAGS  := $(DEVICE_FLAGS) -mmcu=$(MCU) -x assembler-with-cpp \
	-DLGT_SAFE_MODE=$(LGT_SAFE_MODE)
ASFLAGS  += $(EXTRA_ASFLAGS)
LDFLAGS  := $(DEVICE_FLAGS) -mmcu=$(MCU) -Wl,--gc-sections,-Map=$(TARGET).map

OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o) \
	$(ASM_SRC:src/%.S=$(BUILD_DIR)/%.o)

# Keep bytewise register access opaque to whole-program optimization.
$(BUILD_DIR)/vm/rv32_compat.o: CFLAGS := $(filter-out -flto,$(CFLAGS))

ELF      := $(TARGET).elf
HEX      := $(TARGET).hex
WINDOWS_HEX := $(shell wslpath -w "$(abspath $(HEX))")
WINDOWS_MONITOR := $(shell wslpath -w "$(abspath tools/serial-monitor.ps1)")
WINDOWS_SERIAL_CLOSE := $(shell wslpath -w "$(abspath tools/close-serial-relays.ps1)")
GUEST_BIN ?= guest_images/demo.bin
GUEST_NAME ?= $(basename $(notdir $(GUEST_BIN)))
GUEST_C ?= guest-riscv/src/main.c
GUEST_BUILD_NAME = $(basename $(notdir $(GUEST_C)))
GUEST_CS ?= guest-riscv/src/*.c
GUEST_RUN_FLAG ?=
GUEST_UPLOAD_FLAGS ?=
GUEST_SLOT ?= 0
GUEST_ENTRY ?= 0

# This clone exposes the ATmega328P-compatible signature and vector table.
AVR_PART ?= m328p
BAUD     ?= 115200
# COM5 is the genuine Uno R3 and is now the primary development/reference
# board. COM6 remains the intentionally unstable LGT-compatible fault target.
PORT     ?= COM5
WSL_PORT ?= /dev/ttyACM0
NODE_MASTER_PORT ?= COM5
NODE_WORKER_PORT ?= COM9
HW_TEST_PORTS ?= COM5 COM9 COM6
HW_TEST_UNSTABLE_PORTS ?= COM6
HW_TEST_RUN_TIMEOUT ?= 120
# This FIREPHX USB bridge leaves its bootloader during avrdude's slow flash
# readback. The running firmware provides the end-to-end acceptance check.
# Override with AVRDUDE_WRITE_FLAGS= when using a programmer with reliable
# readback support.
AVRDUDE_WRITE_FLAGS ?= -V

.PHONY: all clean size test test-codec test-event test-queue test-timer test-rv32i test-loader test-hardware-tool serial-close upload upload-wsl monitor monitor-reset guest-build guest-upload guest-run guest-inspect guest-selected-upload guest-only guests-only guest-uno guests-uno guest-help guest-load guest-load-batch guest-load-run guest2-build guest2-load guest-dual-run hw-test hw-test-flash

all: $(HEX) size

$(LOG_C) $(LOG_H) $(LOG_PY) &: $(LOG_MANIFEST) $(LOG_GENERATOR)
	python3 $(LOG_GENERATOR) \
		--manifest $(LOG_MANIFEST) \
		--c-source $(LOG_C) \
		--c-header $(LOG_H) \
		--python $(LOG_PY)

$(RV_GUEST_C) $(RV_GUEST_H) &: $(RV_GUEST_BIN) $(RV_GUEST_GENERATOR)
	python3 $(RV_GUEST_GENERATOR) \
		--input $(RV_GUEST_BIN) \
		--c-source $(RV_GUEST_C) \
		--c-header $(RV_GUEST_H) \
		--capacity $(RV_GUEST_CAPACITY)

$(BUILD_DIR):
	mkdir -p $@


$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: src/%.S | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/main.o $(BUILD_DIR)/tasks/shell_task.o: $(LOG_H)
$(BUILD_DIR)/tasks/rv_demo_task.o: $(RV_GUEST_H)

$(ELF): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(ELF)
	$(SIZE) --format=avr --mcu=$(MCU) $(ELF)

test-codec: $(LOG_PY)
	python3 tools/test_log_codec.py $(LOG_MANIFEST)

test-queue: | $(BUILD_DIR)
	cc -std=c11 -Wall -Wextra -Werror -Isrc/kernel \
		tools/test_message_queue.c src/kernel/message_queue.c \
		-o build/test_message_queue
	./build/test_message_queue

test-event: | $(BUILD_DIR)
	cc -std=c11 -Wall -Wextra -Werror -Itools/host_avr -Isrc/kernel \
		tools/test_event_flags.c src/kernel/event_flags.c \
		-o build/test_event_flags
	./build/test_event_flags

test-timer: | $(BUILD_DIR)
	cc -std=c11 -Wall -Wextra -Werror -Itools/host_avr -Isrc/kernel \
		tools/test_software_timer.c src/kernel/software_timer.c \
		-o build/test_software_timer
	./build/test_software_timer

test-rv32i: | $(BUILD_DIR)
	cc -std=c11 -Wall -Wextra -Werror -Isrc/vm \
		tools/test_rv32i.c src/vm/rv32i.c src/vm/rv32_compat.c \
		-o build/test_rv32i
	./build/test_rv32i

test-loader:
	python3 tools/test_guest_upload.py

test-hardware-tool:
	python3 tools/test_hardware_regression.py

.PHONY: test-job-protocol
test-job-protocol: | $(BUILD_DIR)
	cc -std=c11 -Wall -Wextra -Werror -Isrc/kernel tools/test_job_protocol.c src/kernel/job_protocol.c -o build/test_job_protocol
	./build/test_job_protocol

test: test-codec test-event test-queue test-timer test-rv32i test-loader test-hardware-tool test-job-protocol

serial-close:
	@$(WINDOWS_POWERSHELL) -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$(WINDOWS_SERIAL_CLOSE)" >/dev/null

upload: serial-close $(HEX)
	"$(WINDOWS_AVRDUDE)" $(AVRDUDE_WRITE_FLAGS) -c arduino -p $(AVR_PART) -P $(PORT) -b $(BAUD) -D -U "flash:w:$(WINDOWS_HEX):i"

# Direct WSL USB access is kept as an option for other USB-serial adapters.
upload-wsl: serial-close $(HEX)
	$(AVRDUDE) $(AVRDUDE_WRITE_FLAGS) -c arduino -p $(AVR_PART) -P $(WSL_PORT) -b $(BAUD) -D -U flash:w:$(HEX):i

monitor: serial-close $(LOG_PY)
	python3 tools/wsl_serial_monitor.py --script "$(WINDOWS_MONITOR)" --port "$(PORT)" --baud $(MONITOR_BAUD)

monitor-reset: serial-close $(LOG_PY)
	python3 tools/wsl_serial_monitor.py --script "$(WINDOWS_MONITOR)" --port "$(PORT)" --baud $(MONITOR_BAUD) --reset

guest-load: serial-close $(GUEST_BIN) $(LOG_PY)
	python3 tools/wsl_guest_upload.py --script "$(WINDOWS_MONITOR)" --port "$(PORT)" --baud $(MONITOR_BAUD) --input "$(GUEST_BIN)" --name "$(GUEST_NAME)" --entry $(GUEST_ENTRY) $(GUEST_RUN_FLAG) $(GUEST_UPLOAD_FLAGS)

# GUEST_BATCH is a sequence of `--guest name=/absolute/path.bin` arguments.
# Keeping them in one uploader process avoids reopening COM6 (and pulsing DTR)
# between SRAM images.
guest-load-batch: serial-close $(LOG_PY)
	python3 tools/wsl_guest_upload.py --script "$(WINDOWS_MONITOR)" --port "$(PORT)" --baud $(MONITOR_BAUD) --reset-on-open $(GUEST_BATCH) $(GUEST_UPLOAD_FLAGS)

guest-load-run: GUEST_RUN_FLAG=--run
guest-load-run: guest-load

# All normal workflows can be launched from this repository root.
guest-build:
	$(MAKE) -C guest-riscv NAME=$(GUEST_BUILD_NAME) MAIN=$(abspath $(GUEST_C)) all

guest-upload: guest-build
	$(MAKE) guest-load GUEST_BIN=$(abspath guest-riscv/build/$(GUEST_BUILD_NAME).bin) GUEST_NAME=$(GUEST_BUILD_NAME)

guest-run: guest-build
	$(MAKE) guest-load-run GUEST_BIN=$(abspath guest-riscv/build/$(GUEST_BUILD_NAME).bin) GUEST_NAME=$(GUEST_BUILD_NAME)

guest-inspect:
	$(MAKE) -C guest-riscv NAME=$(GUEST_BUILD_NAME) MAIN=$(abspath $(GUEST_C)) inspect

guest-selected-upload:
	python3 tools/guest_batch.py --repo "$(CURDIR)" --sources $(GUEST_CS)

# Explicit aliases for the common runtime-only workflow.  Neither target has
# an AVR .hex prerequisite or invokes AVRDUDE: they compile RV32 code on the
# PC and replace only the selected SRAM guest image(s) through the shell.
guest-only: guest-upload

guests-only: guest-selected-upload

# Friendly aliases for the distributed workflow: the PC library lives on Uno
# (COM5); `run <name>` later deploys the selected image to Nano over I2C.
guest-uno: PORT=COM5
guest-uno: guest-only

guests-uno: PORT=COM5
guests-uno: guests-only

guest-help:
	@echo "one guest, SRAM only:  make guest-only GUEST_C=guest-riscv/workloads/fibonacci.c"
	@echo "many guests, SRAM only: make guests-only GUEST_CS='guest-riscv/workloads/*.c'"
	@echo "compile only:           make guest-build GUEST_C=guest-riscv/workloads/fibonacci.c"
	@echo "run after upload:       make guest-run GUEST_C=guest-riscv/workloads/fibonacci.c"
	@echo "AVR firmware flash:     make upload  (only when the OS itself changed)"
	@echo "three-board SRAM test: make hw-test"
	@echo "flash + three-board:   make hw-test-flash"

# `hw-test` preserves AVR Flash and replaces only the volatile SRAM guests.
# `hw-test-flash` first puts the exact same HEX on every selected board.
hw-test:
	python3 tools/hardware_regression.py --repo "$(CURDIR)" \
		--ports $(HW_TEST_PORTS) \
		--expected-unstable $(HW_TEST_UNSTABLE_PORTS) \
		--run-timeout $(HW_TEST_RUN_TIMEOUT)

hw-test-flash: $(HEX)
	python3 tools/hardware_regression.py --repo "$(CURDIR)" --flash \
		--ports $(HW_TEST_PORTS) \
		--expected-unstable $(HW_TEST_UNSTABLE_PORTS) \
		--run-timeout $(HW_TEST_RUN_TIMEOUT)

guest2-build:
	$(MAKE) -C guest-riscv NAME=second MAIN=src/second.c all

guest2-load: guest2-build
	$(MAKE) guest-load GUEST_BIN=$(abspath guest-riscv/build/second.bin) GUEST_NAME=second

guest-dual-run: guest-build guest2-build
	$(MAKE) guest-load GUEST_BIN=$(abspath guest-riscv/build/demo.bin) GUEST_NAME=demo
	$(MAKE) guest-load GUEST_BIN=$(abspath guest-riscv/build/second.bin) GUEST_NAME=second GUEST_RUN_FLAG=--run-all

clean:
	rm -rf build

# Separate wire-test firmware. Does not replace build/atmega_os.hex.
.PHONY: i2c-test-build i2c-test-upload
i2c-test-build: build/i2c-master.hex build/i2c-slave.hex

build/i2c-master.elf: tools/i2c_heartbeat.c | $(BUILD_DIR)
	$(CC) $(DEVICE_FLAGS) -mmcu=atmega328p -DF_CPU=16000000UL -DI2C_MASTER=1 -Os -Wall -Wextra $< -o $@

build/i2c-slave.elf: tools/i2c_heartbeat.c | $(BUILD_DIR)
	$(CC) $(DEVICE_FLAGS) -mmcu=atmega328p -DF_CPU=16000000UL -DI2C_MASTER=0 -Os -Wall -Wextra $< -o $@

build/i2c-%.hex: build/i2c-%.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

i2c-test-upload: serial-close i2c-test-build
	"$(WINDOWS_AVRDUDE)" -c arduino -p m328p -P $(NODE_WORKER_PORT) -b 115200 -D -U "flash:w:$(shell wslpath -w '$(abspath build/i2c-slave.hex)'):i"
	"$(WINDOWS_AVRDUDE)" -c arduino -p m328p -P $(NODE_MASTER_PORT) -b 115200 -D -U "flash:w:$(shell wslpath -w '$(abspath build/i2c-master.hex)'):i"

.PHONY: nodes-build nodes-master-build nodes-worker-build \
	nodes-upload nodes-master-upload nodes-worker-upload
nodes-master-build:
	$(MAKE) BUILD_DIR=build/nodes-master TARGET=build/nodes-master/os NODE_ROLE=1 LGT_SAFE_MODE=1 all

nodes-worker-build:
	$(MAKE) BUILD_DIR=build/nodes-worker TARGET=build/nodes-worker/os NODE_ROLE=2 LGT_SAFE_MODE=1 all

nodes-build:
	$(MAKE) nodes-worker-build
	$(MAKE) nodes-master-build

nodes-master-upload: nodes-master-build serial-close
	"$(WINDOWS_AVRDUDE)" -c arduino -p m328p -P $(NODE_MASTER_PORT) -b 115200 -D -U "flash:w:$(shell wslpath -w '$(abspath build/nodes-master/os.hex)'):i"

nodes-worker-upload: nodes-worker-build serial-close
	"$(WINDOWS_AVRDUDE)" -c arduino -p m328p -P $(NODE_WORKER_PORT) -b 115200 -D -U "flash:w:$(shell wslpath -w '$(abspath build/nodes-worker/os.hex)'):i"

nodes-upload: nodes-worker-upload nodes-master-upload

-include $(OBJ:.o=.d)
