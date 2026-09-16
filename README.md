# duo

A small terminal multiplexer, in the spirit of `screen`: windows you create,
name and close, panes to see several at once, a copy mode that can search the
scrollback, and a status bar along the bottom showing what's running. Sessions
keep running when your SSH connection drops, so you reconnect and carry on.

No dependencies beyond libc.

## Build, install, uninstall

```sh
make                      # build ./build/duo; you can run it from there without installing
make install              # copy it to ~/.local/bin/duo
make uninstall            # remove it, plus the autostart lines if you added them
make autostart            # optional: open duo automatically on SSH logins (see below)
make uninstall-autostart  # remove just the autostart lines
make check                # terminal-emulator tests (make asan: same, with sanitisers)
make help                 # this list
```

For a system-wide install, build as yourself and install with sudo:
`make && sudo make install PREFIX=/usr/local`. Give `uninstall` and
`autostart` the same `PREFIX`, and run `autostart` without sudo.

Uninstalling doesn't end running sessions; `make uninstall` tells you if any
are left. After you reinstall a new version, sessions that were already
running keep using the old one until you end them with `duo -k`.

## Use

```sh
duo             # attach to the session, starting it if it isn't running
duo -l          # list running sessions
duo -k          # end the session and all its windows
duo -s work     # a second, independent session named "work"
```

A new session opens **two** windows, which is what the name is about: two
shells side by side is the layout it was built for, and `Ctrl-a c` expands
from there (up to 16). `Ctrl-a Ctrl-a` flips between the two, and `Ctrl-a v`
puts them on screen together. Change the starting count with
`INITIAL_WINDOWS` in `src/config.h`.

If the connection drops, run `duo` again after reconnecting; if an old client
is somehow still attached, the new one takes over.

A session file can replace that default start: see below.

## Session files

To have a session always open the same windows, write a file like this:

```
layout columns          # tabs, columns, rows or grid
panes 2                 # how many panes are open at the start
focus 1                 # which window has focus (1-based)

window shell
window climate -- watch -n 5 python3 /home/matt/aht20/aht20.py
window logs    -- journalctl -f
```

duo looks for `~/.config/duo/<session>.conf`, then `~/.config/duo/default.conf`,
and `duo -f FILE` overrides both. `examples/session.conf` in this repo is a
commented starting point.

- **`window [NAME] [-- COMMAND]`** opens a window. The name is optional; with
  no name the status bar shows whatever is running. Everything after `--` is
  typed into that window's shell, so Ctrl-C stops it and leaves you at a
  prompt in that window rather than closing it.
- **Repeating commands** are just `watch`: `watch -n 5 CMD` reruns every five
  seconds and clears in between. For the opposite — keeping every reading so
  you can scroll back through them with `C-a [` — use a loop instead:
  `while :; do CMD; sleep 5; done`. duo redraws only the cells that changed,
  so neither flickers.
- The file is read **when a session starts**. Attaching to a running session
  ignores it; `duo -k` first if you want to apply a change.
- Mistakes are reported before anything starts, with the line number:
  `duo: line 3: layout must be tabs, columns, rows or grid`.

## Keys

Press **Ctrl-a**, then:

**Windows**

| Key        | Action                                        |
|------------|-----------------------------------------------|
| `c`        | new window                                    |
| `1`…`9`    | go to that window                             |
| `n` `p`    | next / previous window                        |
| `Ctrl-a`   | back to the last window                       |
| `"`        | pick from a list (arrows or a number, Enter)  |
| `w`        | list windows in the status bar                |
| `A`        | rename the window (empty name restores the automatic one) |
| `k`        | close the window (asks first)                 |

**Panes and layouts**

| Key            | Action                                      |
|----------------|---------------------------------------------|
| `S`            | split: add a pane, showing another window   |
| `X`            | close the focused pane (the window stays open) |
| `Tab` or `o`   | focus the next pane                         |
| arrow keys     | focus the pane in that direction            |
| `t`            | one window, full screen                     |
| `v` or `\|`    | columns                                     |
| `s` or `-`     | rows                                        |
| `g`            | grid                                        |
| `Space`        | cycle through the layouts                   |

**Copy mode** (`[` to enter, `q` or Esc to leave)

| Key                | Action                                    |
|--------------------|-------------------------------------------|
| arrows, `hjkl`     | move                                      |
| PgUp/PgDn, `b`/`f` | move a page                               |
| `g` `G`            | oldest line / newest line                 |
| `0` `$`            | start / end of line                       |
| `/` `?`            | search back / forward, then `n` `N` to repeat |
| `Space`            | start selecting                           |
| `Enter` or `y`     | copy (the whole line if nothing selected) |
| `]` (outside copy mode) | paste what you copied                |

**Session**

| Key | Action                                          |
|-----|-------------------------------------------------|
| `d` | detach; windows keep running                    |
| `r` | redraw                                          |
| `a` | send a literal Ctrl-a (start of line in bash)   |
| `Q` | end the session and close every window (asks first) |
| `?` | help                                            |

The status bar lists the windows with what's running in each. The focused one
is highlighted, ones visible in another pane are shaded, and a `*` marks a
window that produced output while you weren't looking. In a split, the part of
the divider next to the focused pane is green.

A window closes when its shell exits. When the last one closes, the session
ends.

## Copy mode and the clipboard

`Ctrl-a [` freezes the pane and lets you move back through its history (2000
lines per window). `/` searches backwards, `?` forwards; matches are
highlighted, and `n` repeats. `Space` starts a selection, `Enter` copies it,
and `Ctrl-a ]` pastes it into the focused window.

Copied text is also offered to your own terminal's clipboard using OSC 52,
which many terminals accept over SSH; where it isn't supported, nothing
happens and the internal paste still works. Search matches ASCII text.

## Starting automatically over SSH

**Option 1: `make autostart` on this machine** (without sudo). This adds a few
marked lines to the end of `~/.bashrc` and/or `~/.zshrc`. By default it edits
the files that already exist, plus the one for your login shell. To choose,
use `make autostart SHELLS=zsh` or `SHELLS="bash zsh"`. Each file is copied to
`<file>.duo-backup` first. Running it again replaces the lines rather than
adding a second copy. The lines look like this:

```bash
# >>> duo autostart >>>
if [[ $- == *i* && -n $SSH_TTY && -z $DUO_SESSION && -x "/home/you/.local/bin/duo" ]]; then
    "/home/you/.local/bin/duo"
fi
# <<< duo autostart <<<
```

- They run duo by its full path, so your `PATH` setup doesn't matter, and they
  do nothing once duo is uninstalled.
- `SSH_TTY` is only set when SSH allocates a terminal, so `scp`, `rsync` and
  `ssh host some-command` aren't affected.
- `DUO_SESSION` is set inside duo's shells, which stops them from trying to
  start duo again.
- Detaching, or duo failing to start, drops you at a normal shell.
- To skip duo for one login: `ssh -t host 'DUO_SESSION=skip bash'`.
- `make uninstall-autostart` removes the lines, and so does `make uninstall`.
- duo's windows run your login shell (`$SHELL`). To use zsh in them without
  changing your login shell, start the session with `SHELL=/bin/zsh duo`.

**Option 2: on the machine you connect from**, in `~/.ssh/config` (OpenSSH
clients only):

```
Host pi-duo
    HostName your-pi-address
    RequestTTY yes
    RemoteCommand ~/.local/bin/duo
```

`ssh pi-duo` attaches to duo, while plain `ssh` to the host still gives you a
normal shell.

## Settings

Settings are in `src/config.h`; rebuild after changing them.

- `PREFIX_KEY`: the command key. Ctrl-b (`0x02`) works well if you use
  Ctrl-a in bash a lot.
- `INNER_TERM`: the `TERM` the shells see (default `screen-256color`).
- `INITIAL_WINDOWS`: how many windows a new session opens (default 2).
- `MESSAGE_MS`: how long status-bar messages stay up.

## How it works

`duo` is both the client and the server. The first run forks a background
server that owns a pty per window and listens on `/tmp/duo-$UID/<session>`.
The directory and socket are private to you, and the server checks the
connecting user's uid. The server runs a small VT100/xterm emulator per
window, composites the panes and status bar, and sends the attached client
only the cells that changed. The client just puts your terminal in raw mode
and forwards keystrokes; it holds no state, so losing it loses nothing.

A window appears in at most one pane, so it only ever has one size to be
resized to. Choosing a window that's already on screen swaps the two panes'
windows rather than showing it twice.

```
src/main.c    command line, socket path, start-or-attach
src/conf.c    session files
src/client.c  raw terminal <-> socket
src/server.c  event loop, connections, key bindings
src/session.h the session model: windows, panes, modes
src/window.c  windows and the window/pane rules
src/layout.c  pane rectangles per layout
src/copy.c    copy mode: movement, search, selection
src/render.c  compositing, status bar, overlays, screen diffing
src/vterm.c   terminal emulator
src/proto.c   message framing
```

## Limitations

- Up to 16 windows and 8 panes.
- No mouse support. Use copy mode, or your terminal's own selection (which in
  a split will span the panes).
- Copy-mode search matches ASCII.
- Scrollback is 2000 lines per window.
- Combining characters (accents written as separate code points) are dropped.
- Like `screen` and `tmux`, windows keep the environment of the session that
  started them. For example, `SSH_AUTH_SOCK` goes stale after you reconnect.
