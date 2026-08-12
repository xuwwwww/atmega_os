MCU      ?= atmega328p
F_CPU    ?= 16000000UL
TARGET   ?= build/atmega_os
SRC      := src/main.c
BUILD_DIR := build

CC       := avr-gcc
OBJCOPY  := avr-objcopy
SIZE     := avr-size
AVRDUDE  := avrdude

CFLAGS   := -std=gnu11 -Os -Wall -Wextra -mmcu=$(MCU) -DF_CPU=$(F_CPU)
LDFLAGS  := -mmcu=$(MCU)

OBJ      := $(BUILD_DIR)/main.o
ELF      := $(TARGET).elf
HEX      := $(TARGET).hex

# Arduino Uno uses m328p and usually 115200 baud.
# Arduino Nano with an old bootloader usually needs BAUD=57600.
AVR_PART ?= m328p
BAUD     ?= 115200

.PHONY: all clean size upload

all: $(HEX) size

$(BUILD_DIR):
	mkdir -p $@

$(OBJ): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(ELF)
	$(SIZE) --format=avr --mcu=$(MCU) $(ELF)

upload: $(HEX)
	@test -n "$(PORT)" || (echo "Set PORT, for example: make upload PORT=/dev/ttyS3" && exit 1)
	$(AVRDUDE) -c arduino -p $(AVR_PART) -P $(PORT) -b $(BAUD) -D -U flash:w:$(HEX):i

clean:
	rm -rf build

-include $(OBJ:.o=.d)
