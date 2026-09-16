/* Compile-time settings. Rebuild after changing. */
#ifndef DUO_CONFIG_H
#define DUO_CONFIG_H

/* Command prefix. 0x01 is Ctrl-a (like screen); 0x02 would be Ctrl-b. */
#define PREFIX_KEY      0x01
#define PREFIX_NAME     "C-a"
#define PREFIX_LETTER   'a'     /* PREFIX_KEY then this sends a literal prefix */

/* TERM for the shells inside. Its terminfo must exist on this machine. */
#define INNER_TERM      "screen-256color"

/* Windows a new session opens. Two is the default duo is named for; C-a c
 * adds more, up to MAX_WINDOWS. Set it to 1 for screen's behaviour. */
#define INITIAL_WINDOWS 2

/* How long transient status-bar messages stay up. */
#define MESSAGE_MS      3000

#endif
