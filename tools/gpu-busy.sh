#!/bin/sh
# Ocupación de la GPU Intel durante N segundos (por defecto 3), a partir del
# tiempo que sus motores pasan dormidos (RC6), y su frecuencia media.
#   tools/gpu-busy.sh 4
# Otra tarjeta: GT=/sys/class/drm/card0/gt/gt0 tools/gpu-busy.sh
G=${GT:-$(ls -d /sys/class/drm/card*/gt/gt0 2>/dev/null | head -1)}
[ -r "$G/rc6_residency_ms" ] || { echo "sin RC6 legible (¿no es i915?)"; exit 1; }
N=${1:-3}
a=$(cat "$G/rc6_residency_ms"); t0=$(date +%s%N); f=0; i=0
while [ $i -lt $((N * 10)) ]; do
    f=$((f + $(cat "$G/rps_act_freq_mhz"))); i=$((i + 1)); sleep 0.1
done
b=$(cat "$G/rc6_residency_ms"); t1=$(date +%s%N)
dt=$(((t1 - t0) / 1000000))
echo "gpu ocupada: $((100 - 100 * (b - a) / dt))%  frecuencia media: $((f / (N * 10))) MHz (máx $(cat "$G/rps_max_freq_mhz"))  throttle: $(cat "$G/throttle_reason_status")"
