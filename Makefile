MCU      ?= atmega328p
F_CPU    ?= 16000000UL
TARGET   ?= build/atmega_os
SRC      := src/main.c \
	 src/kernel/timer.c \
	 src/kernel/software_timer.c \
	 src/kernel/scheduler.c \
	 src/kernel/panic.c \
	 src/kernel/semaphore.c \
	 src/kernel/mutex.c \
	 src/kernel/event_flags.c \
	 src/kernel/message_queue.c \
	 src/drivers/uart.c \
	 src/generated/log_messages.c \
	 src/tasks/idle_task.c \
	 src/tasks/led_task.c \
	 src/tasks/queue_test_tasks.c \
	 src/tasks/shell_task.c
ASM_SRC  := src/kernel/context.S
UART_BAUD ?= 115200UL
KERNEL_TICK_HZ ?= 2000UL
LED_STACK_SIZE ?= 160
SHELL_STACK_SIZE ?= 224
TIMER_SERVICE_STACK_SIZE ?= 160
IDLE_STACK_SIZE ?= 128
MONITOR_BAUD ?= 115200
BUILD_DIR := build
LOG_MANIFEST := tools/log_messages.json
LOG_GENERATOR := tools/generate_log_codec.py
LOG_C := src/generated/log_messages.c
LOG_H := src/generated/log_messages.h
LOG_PY := tools/generated_log_codec.py

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
	-DLED_STACK_SIZE=$(LED_STACK_SIZE) \
	-DSHELL_STACK_SIZE=$(SHELL_STACK_SIZE) \
	-DTIMER_SERVICE_STACK_SIZE=$(TIMER_SERVICE_STACK_SIZE) \
	-DIDLE_STACK_SIZE=$(IDLE_STACK_SIZE)
CFLAGS   += $(EXTRA_CFLAGS)
ASFLAGS  := $(DEVICE_FLAGS) -mmcu=$(MCU) -x assembler-with-cpp
ASFLAGS  += $(EXTRA_ASFLAGS)
LDFLAGS  := $(DEVICE_FLAGS) -mmcu=$(MCU) -Wl,--gc-sections,-Map=$(TARGET).map

OBJ := $(SRC:src/%.c=$(BUILD_DIR)/%.o) \
	$(ASM_SRC:src/%.S=$(BUILD_DIR)/%.o)
ELF      := $(TARGET).elf
HEX      := $(TARGET).hex
WINDOWS_HEX := $(shell wslpath -w "$(abspath $(HEX))")
WINDOWS_MONITOR := $(shell wslpath -w "$(abspath tools/serial-monitor.ps1)")

# This clone exposes the ATmega328P-compatible signature and vector table.
AVR_PART ?= m328p
BAUD     ?= 115200
PORT     ?= COM6
WSL_PORT ?= /dev/ttyACM0
# This FIREPHX USB bridge leaves its bootloader during avrdude's slow flash
# readback. The running firmware provides the end-to-end acceptance check.
# Override with AVRDUDE_WRITE_FLAGS= when using a programmer with reliable
# readback support.
AVRDUDE_WRITE_FLAGS ?= -V

.PHONY: all clean size test test-codec test-event test-queue test-timer upload upload-wsl monitor

all: $(HEX) size

$(LOG_C) $(LOG_H) $(LOG_PY) &: $(LOG_MANIFEST) $(LOG_GENERATOR)
	python3 $(LOG_GENERATOR) \
		--manifest $(LOG_MANIFEST) \
		--c-source $(LOG_C) \
		--c-header $(LOG_H) \
		--python $(LOG_PY)

$(BUILD_DIR):
	mkdir -p $@


$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: src/%.S | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/main.o $(BUILD_DIR)/tasks/shell_task.o: $(LOG_H)

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

test: test-codec test-event test-queue test-timer

upload: $(HEX)
	"$(WINDOWS_AVRDUDE)" $(AVRDUDE_WRITE_FLAGS) -c arduino -p $(AVR_PART) -P $(PORT) -b $(BAUD) -D -U "flash:w:$(WINDOWS_HEX):i"

# Direct WSL USB access is kept as an option for other USB-serial adapters.
upload-wsl: $(HEX)
	$(AVRDUDE) $(AVRDUDE_WRITE_FLAGS) -c arduino -p $(AVR_PART) -P $(WSL_PORT) -b $(BAUD) -D -U flash:w:$(HEX):i

monitor: $(LOG_PY)
	python3 tools/wsl_serial_monitor.py --script "$(WINDOWS_MONITOR)" --port "$(PORT)" --baud $(MONITOR_BAUD)

clean:
	rm -rf build

-include $(OBJ:.o=.d)
