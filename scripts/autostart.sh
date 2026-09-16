#!/bin/sh
# Adds or removes the lines that start duo on interactive SSH logins.
#
#   autostart.sh add BIN [bash] [zsh]   default: rc files that exist, plus the login shell's
#   autostart.sh remove
#
# The lines sit between marker comments, so running add again replaces them
# and remove takes out exactly what add put in.
set -eu

BEGIN='# >>> duo autostart >>>'
END='# <<< duo autostart <<<'

rc_file() {
    case $1 in
    bash) printf '%s\n' "$HOME/.bashrc" ;;
    zsh)  printf '%s\n' "${ZDOTDIR:-$HOME}/.zshrc" ;;
    *)    echo "autostart: unknown shell '$1' (use bash and/or zsh)" >&2; exit 2 ;;
    esac
}

block() {
    cat <<EOF
$BEGIN
# Start duo on interactive SSH logins. Added by duo's 'make autostart';
# 'make uninstall-autostart' or 'make uninstall' removes it.
if [[ \$- == *i* && -n \$SSH_TTY && -z \$DUO_SESSION && -x "$1" ]]; then
    "$1"
fi
$END
EOF
}

# Prints the file without the duo block or the blank line added before it.
strip_block() {
    awk -v b="$BEGIN" -v e="$END" '
        $0 == b { skip = 1; held = 0; next }
        skip    { if ($0 == e) skip = 0; next }
        {
            if (held) print ""
            held = 0
            if ($0 == "") { held = 1; next }
            print
        }
        END { if (held) print "" }
    ' "$1"
}

# Writes through cat, so a symlinked rc file (dotfile managers) stays a symlink.
replace_contents() {
    cat "$2" > "$1"
    rm -f "$2"
}

case ${1:-} in
add)
    if [ $# -lt 2 ]; then
        echo "usage: autostart.sh add BIN [bash] [zsh]" >&2
        exit 2
    fi
    bin=$2
    shift 2
    if [ "$(id -u)" = 0 ]; then
        echo "autostart: run this without sudo; it edits your own shell config" >&2
        exit 1
    fi

    shells=$*
    if [ -z "$shells" ]; then
        login=$(getent passwd "$(id -un)" | cut -d: -f7)
        login=${login##*/}
        for sh in bash zsh; do
            if [ -f "$(rc_file $sh)" ] || [ "$login" = "$sh" ]; then
                shells="$shells $sh"
            fi
        done
    fi
    if [ -z "$shells" ]; then
        echo "autostart: no bash or zsh config found; use SHELLS=bash or SHELLS=zsh" >&2
        exit 1
    fi
    for sh in $shells; do
        rc_file "$sh" > /dev/null || exit 2 # check every name before editing any file
    done

    for sh in $shells; do
        rc=$(rc_file "$sh")
        tmp=$(mktemp)
        if [ -f "$rc" ]; then
            cp -p "$rc" "$rc.duo-backup"
            strip_block "$rc" > "$tmp"
            if [ -s "$tmp" ]; then
                echo >> "$tmp"
            fi
        fi
        block "$bin" >> "$tmp"
        replace_contents "$rc" "$tmp"
        echo "autostart: added to $rc"
    done
    if [ ! -x "$bin" ]; then
        echo "autostart: note: $bin isn't installed yet; the lines do nothing until 'make install'"
    fi
    ;;

remove)
    if [ "$(id -u)" = 0 ]; then
        echo "autostart: skipped shell config (running as root); run 'make uninstall-autostart' as yourself"
        exit 0
    fi
    found=0
    for sh in bash zsh; do
        rc=$(rc_file "$sh")
        if [ -f "$rc" ] && grep -qxF "$BEGIN" "$rc"; then
            tmp=$(mktemp)
            strip_block "$rc" > "$tmp"
            replace_contents "$rc" "$tmp"
            echo "autostart: removed from $rc"
            found=1
        fi
    done
    if [ "$found" = 0 ]; then
        echo "autostart: no autostart lines to remove"
    fi
    ;;

*)
    echo "usage: autostart.sh add BIN [bash] [zsh] | autostart.sh remove" >&2
    exit 2
    ;;
esac
