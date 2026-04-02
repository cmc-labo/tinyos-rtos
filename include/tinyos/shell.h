/**
 * @file shell.h
 * @brief Interactive shell for TinyOS — Enhanced Edition
 *
 * Features:
 *  - VT100 line editor: cursor movement, Home/End, Ctrl-A/E/K/U/W/L/C
 *  - Command history  : up/down arrows, SHELL_HISTORY_DEPTH entries
 *  - Tab completion   : command name matching with longest-common-prefix fill
 *  - Quoted strings   : tokens wrapped in " " are kept as one argument
 *  - 19 built-in commands (see shell.c for the full list)
 *
 * Usage:
 *   // Optionally register custom commands before starting
 *   shell_register_cmd("led", cmd_led, "led <on|off>  Toggle LED");
 *
 *   // Provide UART I/O callbacks and start
 *   shell_io_t io = { .getc = uart_getc, .puts = uart_puts };
 *   shell_start(&io);
 *
 *   // Optionally customise the prompt at any time
 *   shell_set_prompt("mydevice> ");
 */

#ifndef TINYOS_SHELL_H
#define TINYOS_SHELL_H

#include "../tinyos.h"
#include <stdint.h>
#include <stddef.h>

/*===========================================================================
 * Configuration  (override via compiler flags if needed)
 *===========================================================================*/

#ifndef SHELL_MAX_COMMANDS
#define SHELL_MAX_COMMANDS   32     /* Maximum registered commands          */
#endif

#ifndef SHELL_LINE_MAX
#define SHELL_LINE_MAX       128    /* Maximum input line length (bytes)    */
#endif

#ifndef SHELL_ARGV_MAX
#define SHELL_ARGV_MAX       16     /* Maximum arguments per command        */
#endif

#ifndef SHELL_HISTORY_DEPTH
#define SHELL_HISTORY_DEPTH  8      /* Command history ring-buffer entries  */
#endif

#ifndef SHELL_PROMPT
#define SHELL_PROMPT         "tinyos> "
#endif

/*===========================================================================
 * I/O interface  (platform-provided UART callbacks)
 *===========================================================================*/

/**
 * @brief Read one character from the terminal.
 * @param timeout_ms  0 = block forever, >0 = milliseconds to wait.
 * @return Character code (0-255), or -1 on timeout / error.
 */
typedef int (*shell_getc_fn_t)(uint32_t timeout_ms);

/**
 * @brief Write a NUL-terminated string to the terminal.
 */
typedef void (*shell_puts_fn_t)(const char *str);

typedef struct {
    shell_getc_fn_t  getc;   /* Must not be NULL */
    shell_puts_fn_t  puts;   /* Must not be NULL */
} shell_io_t;

/*===========================================================================
 * Command handler
 *===========================================================================*/

/**
 * @param argc  Token count (argv[0] is the command name).
 * @param argv  Token array.
 * @return 0 on success; non-zero prints the command's usage hint.
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
    SHELL_OK                =  0,
    SHELL_ERR_INVALID_PARAM = -1,
    SHELL_ERR_TABLE_FULL    = -2,
    SHELL_ERR_DUPLICATE     = -3,
} shell_error_t;

/*===========================================================================
 * Public API
 *===========================================================================*/

/**
 * @brief Register a user command.
 *        Call before shell_start().
 */
shell_error_t shell_register_cmd(const char *name, shell_cmd_fn_t fn,
                                  const char *help);

/**
 * @brief Start the interactive shell in a new RTOS task.
 * @param io  Platform I/O callbacks (must not be NULL).
 * @return SHELL_OK on success.
 */
shell_error_t shell_start(const shell_io_t *io);

/**
 * @brief Execute a single line of input.
 *        Useful for embedding the shell in an existing task or for scripting.
 * @param line  NUL-terminated input (modified in-place during tokenisation).
 */
void shell_exec(char *line);

/**
 * @brief Change the shell prompt string at runtime.
 * @param prompt  NUL-terminated prompt (max 31 characters + NUL).
 */
void shell_set_prompt(const char *prompt);

#endif /* TINYOS_SHELL_H */
