/* Compile-time settings. Rebuild after changing. */
#ifndef DUO_CONFIG_H
#define DUO_CONFIG_H

/* Command prefix. 0x01 is Ctrl-a (like screen); 0x02 would be Ctrl-b. */
#define PREFIX_KEY      0x01
#define PREFIX_NAME     "C-a"
#define PREFIX_LETTER   'a'     /* PREFIX_KEY then this sends a literal prefix */

/* TERM for the shells inside. Its terminfo must exist on this machine. */
#define INNER_TERM      "screen-256color"

/* Windows to open when a session starts. 1 is screen's behaviour; set it to
 * 2 (with C-a v or C-a S once inside) for duo's original two-shell start. */
#define INITIAL_WINDOWS 1

/* How long transient status-bar messages stay up. */
#define MESSAGE_MS      3000

#endif
