# TinyOS Makefile
# Ultra-lightweight RTOS for IoT devices

# Toolchain configuration
CROSS_COMPILE ?= arm-none-eabi-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE := $(CROSS_COMPILE)size

# Target configuration
TARGET := tinyos
ARCH ?= cortex-m4

# Directories
SRC_DIR := src
INC_DIR := include
EXAMPLES_DIR := examples
BUILD_DIR := build

# Source files
KERNEL_SRCS := $(wildcard $(SRC_DIR)/*.c)
EXAMPLE ?= blink_led
EXAMPLE_SRC := $(EXAMPLES_DIR)/$(EXAMPLE).c

ALL_SRCS := $(KERNEL_SRCS) $(EXAMPLE_SRC)
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(ALL_SRCS)))

# Compiler flags
CFLAGS := -Wall -Wextra -Werror
CFLAGS += -std=c11
CFLAGS += -mcpu=$(ARCH) -mthumb
CFLAGS += -O2 -g
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I$(INC_DIR)
CFLAGS += -DTINYOS_VERSION=\"1.0.0\"

# Linker flags
LDFLAGS := -mcpu=$(ARCH) -mthumb
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += -nostartfiles

# Linker script (to be created for specific hardware)
LDSCRIPT := linker.ld

# Targets
.PHONY: all clean size flash

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin
	@echo "Build complete!"
	@$(SIZE) $(BUILD_DIR)/$(TARGET).elf

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile kernel sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Compile example
$(BUILD_DIR)/$(EXAMPLE).o: $(EXAMPLES_DIR)/$(EXAMPLE).c | $(BUILD_DIR)
	@echo "Compiling example: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Link
$(BUILD_DIR)/$(TARGET).elf: $(OBJS)
	@echo "Linking..."
	$(CC) $(LDFLAGS) $^ -o $@

# Create binary
$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	@echo "Creating binary..."
	$(OBJCOPY) -O binary $< $@

# Display size
size: $(BUILD_DIR)/$(TARGET).elf
	$(SIZE) $<

# Clean
clean:
	rm -rf $(BUILD_DIR)

# Build examples
.PHONY: example-blink example-iot

example-blink:
	$(MAKE) EXAMPLE=blink_led

example-iot:
	$(MAKE) EXAMPLE=iot_sensor

# Help
help:
	@echo "TinyOS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build default configuration"
	@echo "  example-blink    - Build LED blink example"
	@echo "  example-iot      - Build IoT sensor example"
	@echo "  clean            - Remove build artifacts"
	@echo "  size             - Display memory usage"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=cortex-m4   - Target architecture"
	@echo "  CROSS_COMPILE    - Toolchain prefix"
	@echo "  EXAMPLE          - Example to build"
