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

# HAL architecture selection (derived from ARCH)
# Maps ARCH prefix → HAL subdirectory name and compile-time -D flag.
ifneq ($(filter cortex-m%,$(ARCH)),)
  HAL_ARCH     := cortex_m
  HAL_ARCH_DEF := HAL_ARCH_CORTEX_M
else ifneq ($(filter riscv%,$(ARCH)),)
  HAL_ARCH     := riscv
  HAL_ARCH_DEF := HAL_ARCH_RISCV
  CROSS_COMPILE := riscv32-unknown-elf-
else ifneq ($(filter avr%,$(ARCH)),)
  HAL_ARCH     := avr
  HAL_ARCH_DEF := HAL_ARCH_AVR
  CROSS_COMPILE := avr-
else
  $(error Unsupported ARCH=$(ARCH). Supported prefixes: cortex-m*, riscv*, avr*)
endif

# Directories
SRC_DIR := src
INC_DIR := include
EXAMPLES_DIR := examples
DRIVERS_DIR := drivers
HAL_DIR := hal
BUILD_DIR := build

# Source files
KERNEL_SRCS := $(wildcard $(SRC_DIR)/*.c)
NET_SRCS    := $(filter-out $(SRC_DIR)/net/tls.c, $(wildcard $(SRC_DIR)/net/*.c))
DRIVER_SRCS := $(wildcard $(DRIVERS_DIR)/*.c)
HAL_SRCS    := $(wildcard $(HAL_DIR)/$(HAL_ARCH)/*.c)
EXAMPLE ?= blink_led
EXAMPLE_SRC := $(EXAMPLES_DIR)/$(EXAMPLE).c

# Assembly sources (Thumb-2, ARM Cortex-M specific)
ASM_SRCS := $(wildcard $(SRC_DIR)/*.s)

ALL_SRCS := $(KERNEL_SRCS) $(NET_SRCS) $(DRIVER_SRCS) $(HAL_SRCS) $(EXAMPLE_SRC) \
            $(if $(MBEDTLS_DIR),$(MBEDTLS_SRCS),)
C_OBJS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(notdir $(ALL_SRCS)))
ASM_OBJS := $(patsubst %.s,$(BUILD_DIR)/%.o,$(notdir $(ASM_SRCS)))
OBJS     := $(C_OBJS) $(ASM_OBJS)

# mbedTLS configuration (set MBEDTLS_DIR to your mbedTLS source tree)
MBEDTLS_DIR ?= $(HOME)/mbedtls
MBEDTLS_INC := $(MBEDTLS_DIR)/include
MBEDTLS_LIB := $(MBEDTLS_DIR)/library
MBEDTLS_SRCS := $(wildcard $(MBEDTLS_LIB)/aes.c \
                             $(MBEDTLS_LIB)/bignum.c \
                             $(MBEDTLS_LIB)/ctr_drbg.c \
                             $(MBEDTLS_LIB)/entropy.c \
                             $(MBEDTLS_LIB)/entropy_poll.c \
                             $(MBEDTLS_LIB)/error.c \
                             $(MBEDTLS_LIB)/gcm.c \
                             $(MBEDTLS_LIB)/hmac_drbg.c \
                             $(MBEDTLS_LIB)/md.c \
                             $(MBEDTLS_LIB)/oid.c \
                             $(MBEDTLS_LIB)/pem.c \
                             $(MBEDTLS_LIB)/pk.c \
                             $(MBEDTLS_LIB)/pk_wrap.c \
                             $(MBEDTLS_LIB)/pkparse.c \
                             $(MBEDTLS_LIB)/rsa.c \
                             $(MBEDTLS_LIB)/rsa_alt_helpers.c \
                             $(MBEDTLS_LIB)/sha256.c \
                             $(MBEDTLS_LIB)/ssl_ciphersuites.c \
                             $(MBEDTLS_LIB)/ssl_client.c \
                             $(MBEDTLS_LIB)/ssl_cookie.c \
                             $(MBEDTLS_LIB)/ssl_msg.c \
                             $(MBEDTLS_LIB)/ssl_tls.c \
                             $(MBEDTLS_LIB)/ssl_tls12_client.c \
                             $(MBEDTLS_LIB)/ssl_tls12_server.c \
                             $(MBEDTLS_LIB)/timing.c \
                             $(MBEDTLS_LIB)/x509.c \
                             $(MBEDTLS_LIB)/x509_crt.c)

# Assembler flags (same CPU target as C compiler)
ASFLAGS := -mcpu=$(ARCH) -mthumb -g

# Compiler flags
CFLAGS := -Wall -Wextra -Werror -Wno-stringop-truncation
CFLAGS += -std=c11
CFLAGS += -mcpu=$(ARCH) -mthumb
CFLAGS += -O2 -g
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -I$(INC_DIR) -I$(HAL_DIR) -I.
CFLAGS += -D$(HAL_ARCH_DEF)
CFLAGS += -DTINYOS_VERSION=\"1.0.0\"
# TLS support (add -DTINYOS_TLS_ENABLE to enable TLS; requires MBEDTLS_DIR)
MBEDTLS_AVAILABLE := $(shell test -f $(MBEDTLS_INC)/mbedtls/ssl.h && echo yes)
CFLAGS += $(if $(filter yes,$(MBEDTLS_AVAILABLE)),-DTINYOS_TLS_ENABLE -I$(MBEDTLS_INC),)

# Linker flags
LDFLAGS := -mcpu=$(ARCH) -mthumb
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += -nostartfiles

# Linker script
LDSCRIPT := linker.ld
LDFLAGS += -T $(LDSCRIPT)

# Targets
.PHONY: all clean size flash

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).bin
	@echo "Build complete!"
	@$(SIZE) $(BUILD_DIR)/$(TARGET).elf

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Assemble ARM Cortex-M sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.s | $(BUILD_DIR)
	@echo "Assembling $<"
	$(CC) $(ASFLAGS) -c $< -o $@

# Compile kernel sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Compile network sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/net/%.c | $(BUILD_DIR)
	@echo "Compiling network: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Compile drivers
$(BUILD_DIR)/%.o: $(DRIVERS_DIR)/%.c | $(BUILD_DIR)
	@echo "Compiling driver: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Compile HAL
$(BUILD_DIR)/%.o: $(HAL_DIR)/$(HAL_ARCH)/%.c | $(BUILD_DIR)
	@echo "Compiling HAL [$(HAL_ARCH)]: $<"
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
.PHONY: example-blink example-iot example-priority example-events example-timers example-power example-fs example-network example-ota example-mqtt example-coap example-condvar example-stats example-watchdog

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

example-coap:
	$(MAKE) EXAMPLE=coap_demo

example-condvar:
	$(MAKE) EXAMPLE=condition_variable

example-stats:
	$(MAKE) EXAMPLE=task_statistics

example-watchdog:
	$(MAKE) EXAMPLE=watchdog_demo

example-tls:
	$(MAKE) EXAMPLE=tls_demo

example-shell:
	$(MAKE) EXAMPLE=shell_demo

example-stdio:
	$(MAKE) EXAMPLE=stdio_uart_demo

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
	@echo "  example-coap     - Build CoAP client/server example"
	@echo "  example-condvar  - Build condition variable example (producer-consumer)"
	@echo "  example-stats    - Build task statistics monitoring example"
	@echo "  example-watchdog - Build watchdog timer example"
	@echo "  clean            - Remove build artifacts"
	@echo "  size             - Display memory usage"
	@echo ""
	@echo "Variables:"
	@echo "  ARCH=cortex-m4   - Target architecture (cortex-m*, riscv*, avr*)"
	@echo "  CROSS_COMPILE    - Toolchain prefix (auto-set from ARCH)"
	@echo "  EXAMPLE          - Example to build"
	@echo ""
	@echo "HAL_ARCH=$(HAL_ARCH)  HAL_ARCH_DEF=$(HAL_ARCH_DEF)"
