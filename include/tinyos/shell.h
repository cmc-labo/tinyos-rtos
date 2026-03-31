/**
 * @file shell.h
 * @brief Interactive shell for TinyOS
 *
 * Usage:
 *   // Register custom commands
 *   shell_register_cmd("led", cmd_led, "led <on|off>  Toggle LED");
 *
 *   // Provide UART read/write callbacks and start
 *   shell_io_t io = { .read = uart_read_char, .write = uart_write };
 *   shell_start(&io);
 */

#ifndef TINYOS_SHELL_H
#define TINYOS_SHELL_H

#include "../tinyos.h"
#include <stdint.h>
#include <stddef.h>

/*===========================================================================
 * Configuration
 *===========================================================================*/

#define SHELL_MAX_COMMANDS   24
#define SHELL_LINE_MAX       80
#define SHELL_ARGV_MAX       8
#define SHELL_PROMPT         "tinyos> "

/*===========================================================================
 * I/O interface (platform-provided UART callbacks)
 *===========================================================================*/

/**
 * @brief Read one character from the terminal.
 * @return The character read, or -1 on timeout / error.
 */
typedef int (*shell_getc_fn_t)(uint32_t timeout_ms);

/**
 * @brief Write a buffer to the terminal.
 */
typedef void (*shell_puts_fn_t)(const char *str);

typedef struct {
    shell_getc_fn_t  getc;   /* Must be non-NULL */
    shell_puts_fn_t  puts;   /* Must be non-NULL */
} shell_io_t;

/*===========================================================================
 * Command handler
 *===========================================================================*/

/**
 * @param argc  Number of tokens (argv[0] is the command name)
 * @param argv  Token array
 * @return 0 on success, non-zero to print usage hint
 */
typedef int (*shell_cmd_fn_t)(int argc, char *argv[]);

typedef struct {
    const char     *name;
    shell_cmd_fn_t  fn;
    const char     *help;   /* One-line description shown by "help" */
} shell_cmd_t;

/*===========================================================================
 * Error codes
 *===========================================================================*/

typedef enum {
    SHELL_OK               =  0,
    SHELL_ERR_INVALID_PARAM = -1,
    SHELL_ERR_TABLE_FULL    = -2,
    SHELL_ERR_DUPLICATE     = -3,
} shell_error_t;

/*===========================================================================
 * API
 *===========================================================================*/

/**
 * @brief Register a command.
 *        Must be called before shell_start().
 */
shell_error_t shell_register_cmd(const char *name, shell_cmd_fn_t fn,
                                  const char *help);

/**
 * @brief Start the interactive shell in a new RTOS task.
 * @param io  Platform I/O callbacks (UART read/write).
 * @return SHELL_OK on success.
 */
shell_error_t shell_start(const shell_io_t *io);

/**
 * @brief Run one line of input (useful when embedding in an existing task).
 * @param line  NUL-terminated input line (will be modified in-place).
 */
void shell_exec(char *line);

#endif /* TINYOS_SHELL_H */
