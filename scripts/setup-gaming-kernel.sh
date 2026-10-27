#!/usr/bin/env bash
# Instala el helper adaptativo de kernel/GPU/VM y las reglas de Polkit / GameMode
# para el Modo Gaming Pro de niri-island, filtrando por capacidades reales de hardware.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Ejecuta este script con sudo: sudo $0"
    exit 1
fi

TARGET_USER="${SUDO_USER:-$(logname 2>/dev/null || echo leimsoto)}"

echo "=== Detectando capacidades de hardware y kernel para Modo Gaming Pro ==="
CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | xargs || echo 'Desconocido')"
KERNEL_VER="$(uname -r)"
echo "  • CPU:    $CPU_MODEL"
echo "  • Kernel: $KERNEL_VER"

if [[ -f /sys/devices/system/cpu/intel_pstate/status ]] && [[ "$(cat /sys/devices/system/cpu/intel_pstate/status)" == "active" ]] && [[ -e /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost ]]; then
    echo "  • [ACTIVO]  Intel P-State HWP Dynamic Boost detectado"
else
    echo "  • [OMITIDO] Intel P-State HWP Dynamic Boost no presente en esta CPU"
fi

INTEL_GT_FOUND=0
for card in /sys/class/drm/card*; do
    if [[ -f "$card/device/vendor" ]] && [[ "$(cat "$card/device/vendor" 2>/dev/null)" == "0x8086" ]] && [[ -r "$card/gt_RP1_freq_mhz" && -w "$card/gt_min_freq_mhz" ]]; then
        INTEL_GT_FOUND=1
        echo "  • [ACTIVO]  GPU Intel GT detectada en $(basename "$card") (RP1=$(cat "$card/gt_RP1_freq_mhz") MHz, RPn=$(cat "$card/gt_RPn_freq_mhz" 2>/dev/null || echo 100) MHz)"
    fi
done
if [[ $INTEL_GT_FOUND -eq 0 ]]; then
    echo "  • [OMITIDO] GPU Intel GT con control RP1/RPn no detectada"
fi

if [[ -f /sys/kernel/mm/lru_gen/enabled ]] && [[ "$(cat /sys/kernel/mm/lru_gen/enabled)" != "0" ]] && [[ -e /sys/kernel/mm/lru_gen/min_ttl_ms ]]; then
    echo "  • [ACTIVO]  Kernel MGLRU (Multi-Gen LRU) detectado"
else
    echo "  • [OMITIDO] Kernel MGLRU no disponible"
fi

if [[ -e /sys/kernel/debug/sched/preempt ]]; then
    echo "  • [ACTIVO]  Preempción dinámica del planificador (PREEMPT_DYNAMIC) detectada"
else
    echo "  • [OMITIDO] Preempción dinámica no expuesta"
fi

install -d -m 0755 /usr/local/libexec
cat <<'EOF' > /usr/local/libexec/niri-island-gaming-helper
#!/usr/bin/env bash
# Helper adaptativo de bajo nivel para el Modo Gaming Pro de niri-island.
# Verifica cada subsistema de hardware/kernel antes de tocarlo y guarda el estado
# previo exacto en /run/niri-island-gaming.state para restaurarlo fielmente al salir.
set -uo pipefail

MODE="${1:-off}"
STATE_FILE="/run/niri-island-gaming.state"

write_if_writable() {
    local file="$1"
    local val="$2"
    if [[ -e "$file" ]]; then
        printf '%s\n' "$val" > "$file" 2>/dev/null || true
    fi
}

read_ bracketed_choice() {
    local file="$1"
    if [[ -r "$file" ]]; then
        sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$file" | head -n1
    fi
}

if [[ "$MODE" == "on" ]]; then
    # Guardar el estado original solo si aún no había una sesión activa guardada
    if [[ ! -f "$STATE_FILE" ]]; then
        {
            [[ -r /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost ]] && \
                echo "PREV_HWP=$(cat /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost 2>/dev/null || echo 0)"
            for card in /sys/class/drm/card*; do
                cname="$(basename "$card")"
                if [[ -f "$card/device/vendor" ]] && [[ "$(cat "$card/device/vendor" 2>/dev/null)" == "0x8086" ]] && [[ -r "$card/gt_min_freq_mhz" ]]; then
                    echo "PREV_GT_MIN_${cname}=$(cat "$card/gt_min_freq_mhz" 2>/dev/null || echo 100)"
                fi
            done
            [[ -r /proc/sys/vm/compaction_proactiveness ]] && \
                echo "PREV_COMPACT=$(cat /proc/sys/vm/compaction_proactiveness 2>/dev/null || echo 20)"
            [[ -r /proc/sys/vm/watermark_boost_factor ]] && \
                echo "PREV_WMARK=$(cat /proc/sys/vm/watermark_boost_factor 2>/dev/null || echo 15000)"
            if [[ -r /sys/kernel/mm/transparent_hugepage/enabled ]]; then
                thp="$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled | head -n1)"
                [[ -n "$thp" ]] && echo "PREV_THP=$thp"
            fi
            [[ -r /sys/kernel/mm/lru_gen/min_ttl_ms ]] && \
                echo "PREV_MGLRU=$(cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>/dev/null || echo 0)"
            [[ -r /proc/sys/kernel/sched_autogroup_enabled ]] && \
                echo "PREV_AUTOGROUP=$(cat /proc/sys/kernel/sched_autogroup_enabled 2>/dev/null || echo 1)"
            if [[ -r /sys/kernel/debug/sched/preempt ]]; then
                pr="$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' /sys/kernel/debug/sched/preempt | head -n1)"
                [[ -n "$pr" ]] && echo "PREV_PREEMPT=$pr"
            fi
        } > "$STATE_FILE" 2>/dev/null || true
    fi

    # 1. Filtro CPU Intel con intel_pstate activo -> HWP Dynamic Boost
    if [[ -f /sys/devices/system/cpu/intel_pstate/status ]] && [[ "$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null)" == "active" ]]; then
        write_if_writable /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost 1
    fi

    # 2. Filtro GPU Intel (vendor 0x8086) con control de frecuencia GT -> piso RP1
    for card in /sys/class/drm/card*; do
        if [[ -f "$card/device/vendor" ]] && [[ "$(cat "$card/device/vendor" 2>/dev/null)" == "0x8086" ]] && [[ -r "$card/gt_RP1_freq_mhz" && -e "$card/gt_min_freq_mhz" ]]; then
            rp1="$(cat "$card/gt_RP1_freq_mhz" 2>/dev/null || echo 300)"
            write_if_writable "$card/gt_min_freq_mhz" "$rp1"
        fi
    done

    # 3. Filtro Memoria Virtual y MGLRU (si el kernel lo soporta)
    write_if_writable /proc/sys/vm/compaction_proactiveness 0
    write_if_writable /proc/sys/vm/watermark_boost_factor 0
    write_if_writable /sys/kernel/mm/transparent_hugepage/enabled madvise
    if [[ -f /sys/kernel/mm/lru_gen/enabled ]] && [[ "$(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null)" != "0" ]]; then
        write_if_writable /sys/kernel/mm/lru_gen/min_ttl_ms 1000
    fi

    # 4. Filtro Planificador y Preempción Dinámica (XanMod / PREEMPT_DYNAMIC)
    write_if_writable /proc/sys/kernel/sched_autogroup_enabled 0
    write_if_writable /sys/kernel/debug/sched/preempt full
else
    # Cargar valores previos exactos si existen
    if [[ -f "$STATE_FILE" ]]; then
        # shellcheck disable=SC1090
        source "$STATE_FILE" 2>/dev/null || true
        rm -f "$STATE_FILE" 2>/dev/null || true
    fi

    if [[ -f /sys/devices/system/cpu/intel_pstate/status ]] && [[ "$(cat /sys/devices/system/cpu/intel_pstate/status 2>/dev/null)" == "active" ]]; then
        write_if_writable /sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost "${PREV_HWP:-0}"
    fi

    for card in /sys/class/drm/card*; do
        cname="$(basename "$card")"
        var_name="PREV_GT_MIN_${cname}"
        if [[ -f "$card/device/vendor" ]] && [[ "$(cat "$card/device/vendor" 2>/dev/null)" == "0x8086" ]] && [[ -e "$card/gt_min_freq_mhz" ]]; then
            rpn="$(cat "$card/gt_RPn_freq_mhz" 2>/dev/null || echo 100)"
            write_if_writable "$card/gt_min_freq_mhz" "${!var_name:-$rpn}"
        fi
    done

    write_if_writable /proc/sys/vm/compaction_proactiveness "${PREV_COMPACT:-20}"
    write_if_writable /proc/sys/vm/watermark_boost_factor "${PREV_WMARK:-15000}"
    write_if_writable /sys/kernel/mm/transparent_hugepage/enabled "${PREV_THP:-always}"
    if [[ -f /sys/kernel/mm/lru_gen/enabled ]] && [[ "$(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null)" != "0" ]]; then
        write_if_writable /sys/kernel/mm/lru_gen/min_ttl_ms "${PREV_MGLRU:-0}"
    fi
    write_if_writable /proc/sys/kernel/sched_autogroup_enabled "${PREV_AUTOGROUP:-1}"
    write_if_writable /sys/kernel/debug/sched/preempt "${PREV_PREEMPT:-lazy}"
fi
EOF
chmod 0755 /usr/local/libexec/niri-island-gaming-helper

# Acción Polkit para pkexec sin contraseña
install -d -m 0755 /usr/share/polkit-1/actions
cat <<'EOF' > /usr/share/polkit-1/actions/org.niri_island.gaming.policy
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE policyconfig PUBLIC
 "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/PolicyKit/1.0/policyconfig.dtd">
<policyconfig>
  <vendor>niri-island</vendor>
  <action id="org.niri_island.gaming.helper">
    <description>Aplicar optimizaciones adaptativas de kernel y GPU para Modo Gaming Pro</description>
    <message>Se requiere autorización para optimizar el kernel para gaming</message>
    <defaults>
      <allow_any>yes</allow_any>
      <allow_inactive>yes</allow_inactive>
      <allow_active>yes</allow_active>
    </defaults>
    <annotate key="org.freedesktop.policykit.exec.path">/usr/local/libexec/niri-island-gaming-helper</annotate>
  </action>
</policyconfig>
EOF

# Regla Polkit para GameMode y el helper de niri-island
install -d -m 0755 /etc/polkit-1/rules.d
cat <<'EOF' > /etc/polkit-1/rules.d/90-niri-island-gaming.rules
polkit.addRule(function(action, subject) {
    if ((action.id == "org.niri_island.gaming.helper" ||
         action.id.indexOf("com.feralinteractive.GameMode.") === 0) &&
        (subject.isInGroup("sudo") || subject.isInGroup("gamemode") || subject.isInGroup("wheel"))) {
        return polkit.Result.YES;
    }
});
EOF

# Grupo gamemode y límites de prioridad nice
getent group gamemode >/dev/null || groupadd -r gamemode
usermod -aG gamemode "$TARGET_USER" || true

install -d -m 0755 /etc/security/limits.d
cat <<EOF > /etc/security/limits.d/90-niri-island-gaming.conf
@gamemode - nice -15
$TARGET_USER - nice -15
EOF

echo "✓ Optimización adaptativa de kernel/GPU/Polkit instalada para $TARGET_USER."
