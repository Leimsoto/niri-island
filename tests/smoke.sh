#!/bin/sh
# Smoke test against the running island (needs a niri session): opens and
# closes everything that can be opened without side effects, and checks the
# island is still alive after each step. Nothing is saved, applied or sent.
#
#   tests/smoke.sh            (uses `niri-island` from PATH)
#   ISLAND=build/niri-island tests/smoke.sh
set -u
ISLAND="${ISLAND:-niri-island}"
pid="$(pgrep -x niri-island | head -1)"
[ -n "$pid" ] || { echo "niri-island no está en marcha"; exit 2; }
fails=0
step() {
    "$ISLAND" "$@" >/dev/null 2>&1
    sleep "${DELAY:-0.6}"
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "  FALLA  $*  (la isla se cerró)"
        fails=$((fails + 1))
        exit 1
    fi
    echo "  ok     $*"
}
for v in cc wifi bt sound weather calendar tasks notes stats session clipboard notifications tray timer stopwatch glance; do
    step "$v"; step close
done
step settings bar; step debug-settings-search anillos; step debug-settings-search ""; step close
step settings wallpapers; step close
step wallpapers; step wallpapers
step capture; step capture
step capture record; step capture
step screensaver; step screensaver stop
step lock --preview 2; sleep 2.5
step status
echo "$( [ $fails -eq 0 ] && echo "todo bien" || echo "$fails fallos" )"
exit $fails
