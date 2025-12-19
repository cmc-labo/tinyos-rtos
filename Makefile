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
DRIVERS_DIR := drivers
BUILD_DIR := build

# Source files
KERNEL_SRCS := $(wildcard $(SRC_DIR)/*.c)
DRIVER_SRCS := $(wildcard $(DRIVERS_DIR)/*.c)
EXAMPLE ?= blink_led
EXAMPLE_SRC := $(EXAMPLES_DIR)/$(EXAMPLE).c

ALL_SRCS := $(KERNEL_SRCS) $(DRIVER_SRCS) $(EXAMPLE_SRC)
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

# Compile drivers
$(BUILD_DIR)/%.o: $(DRIVERS_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling driver: $<"
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
.PHONY: example-blink example-iot example-priority example-events example-timers example-power example-fs example-network example-ota example-mqtt example-condvar example-stats

example-blink:
	$(MAKE) EXAMPLE=blink_led

example-iot:
	$(MAKE) EXAMPLE=iot_sensor

example-priority:
	$(MAKE) EXAMPLE=priority_adjustment

example-events:
	$(MAKE) EXAMPLE=event_groups

example-timers:
	$(MAKE) EXAMPLE=software_timers

example-power:
	$(MAKE) EXAMPLE=low_power

example-fs:
	$(MAKE) EXAMPLE=filesystem_demo

example-network:
	$(MAKE) EXAMPLE=network_demo

example-ota:
	$(MAKE) EXAMPLE=ota_demo

example-mqtt:
	$(MAKE) EXAMPLE=mqtt_demo

example-condvar:
	$(MAKE) EXAMPLE=condition_variable

example-stats:
	$(MAKE) EXAMPLE=task_statistics

# Help
help:
	@echo "TinyOS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build default configuration"
	@echo "  example-blink    - Build LED blink example"
	@echo "  example-iot      - Build IoT sensor example"
	@echo "  example-priority - Build dynamic priority adjustment example"
	@echo "  example-events   - Build event groups example"
	@echo "  example-timers   - Build software timers example"
	@echo "  example-power    - Build low-power modes example"
	@echo "  example-fs       - Build file system example"
	@echo "  example-network  - Build network stack example (TCP/UDP/HTTP/Ping)"
	@echo "  example-ota      - Build OTA firmware update example"
	@echo "  example-mqtt     - Build MQTT client example"
	@echo "  example-condvar  - Build condition variable example (producer-consumer)"
	@echo "  example-stats    - Build task statistics monitoring example"
	@echo "  clean            - Remove build artifacts"
	@echo "  size             - Display memory usage"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=cortex-m4   - Target architecture"
	@echo "  CROSS_COMPILE    - Toolchain prefix"
	@echo "  EXAMPLE          - Example to build"
