#!/bin/sh
# La compilación de depuración bajo gdb: si cae, su backtrace queda en
# ~/.cache/niri-island/gdb.log.
#   cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug && cmake --build build-dbg
#   kill -TERM $(pgrep -x niri-island); setsid tools/run-dbg.sh &
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec gdb -q -batch -ex "handle SIGPIPE nostop noprint pass" -ex "handle SIGCHLD nostop noprint pass" \
    -ex "handle SIGUSR1 nostop noprint pass" -ex "handle SIGUSR2 nostop noprint pass" \
    -ex "handle SIGTERM nostop noprint pass" -ex run -ex "thread apply all bt 12" \
    --args "$ROOT/build-dbg/niri-island" > "$HOME/.cache/niri-island/gdb.log" 2>&1
