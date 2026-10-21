#!/bin/sh
# Installs niri-island's login screen (a greetd greeter).
#
#   sudo ./greeter/install.sh            install, keep greetd's current greeter
#   sudo ./greeter/install.sh --enable   also make it greetd's greeter
#   sudo ./greeter/install.sh --disable  go back to the greeter that was there before
#
# Run it from the repository after building (cmake --build build). It copies
# the wallpaper and colours of the user who ran sudo, so the login screen
# looks like their desktop; run it again after changing the wallpaper.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
conf=/etc/greetd/config.toml
backup=/etc/greetd/config.toml.before-niri-island

if [ "$(id -u)" -ne 0 ]; then
    echo "Ejecútalo con sudo." >&2
    exit 1
fi

if [ "${1:-}" = "--disable" ]; then
    if [ -f "$backup" ]; then
        cp "$backup" "$conf"
        echo "greetd vuelve a usar el greeter anterior (se aplica en el próximo inicio)."
    else
        echo "No hay copia de la configuración anterior ($backup)." >&2
        exit 1
    fi
    exit 0
fi

install -Dm755 "$repo/build/niri-island-greeter" /usr/local/bin/niri-island-greeter
install -Dm755 "$here/niri-island-greeter-session" /usr/local/bin/niri-island-greeter-session
install -Dm644 "$here/greeter.kdl" /etc/niri-island/greeter.kdl

# The user's look: wallpaper (a still frame for videos) and palette.
user="${SUDO_USER:-}"
if [ -n "$user" ]; then
    home="$(getent passwd "$user" | cut -d: -f6)"
    wp="$(sed -n 's/.*"path"[^"]*"\([^"]*\)".*/\1/p' "$home/.local/state/niri-island/wallpaper.json" 2>/dev/null | head -1)"
    case "$wp" in
        *.mp4|*.webm|*.mkv|*.mov) wp="$home/.cache/niri-island/wallpaper-frame.jpg" ;;
    esac
    rm -f /etc/niri-island/greeter-wallpaper.jpg /etc/niri-island/greeter-wallpaper.png
    case "$wp" in
        *.png) [ -f "$wp" ] && install -m644 "$wp" /etc/niri-island/greeter-wallpaper.png ;;
        ?*) [ -f "$wp" ] && install -m644 "$wp" /etc/niri-island/greeter-wallpaper.jpg ;;
    esac
    [ -f "$home/.cache/niri-island/colors.json" ] && install -m644 "$home/.cache/niri-island/colors.json" /etc/niri-island/greeter-colors.json
    # their avatar, where the greeter (another user) can read it
    if [ -f "$home/.face" ]; then
        install -Dm644 "$home/.face" "/var/lib/AccountsService/icons/$user"
    fi
fi
echo "Instalado: /usr/local/bin/niri-island-greeter y /etc/niri-island/"

if [ "${1:-}" = "--enable" ]; then
    [ -f "$backup" ] || cp "$conf" "$backup"
    tmp="$(mktemp)"
    awk '
        /^\[default_session\]/ { insec=1; print; next }
        /^\[/ { insec=0 }
        insec && /^command[ \t]*=/ { print "command = \"/usr/local/bin/niri-island-greeter-session\""; next }
        { print }
    ' "$conf" > "$tmp"
    install -m644 "$tmp" "$conf"
    rm -f "$tmp"
    echo "greetd usará niri-island en el próximo inicio. Para volver atrás: sudo $0 --disable"
fi
