# 10 · Depuración y rendimiento

## Registros

| Proceso | Dónde |
|---|---|
| niri-island | stdout/stderr del proceso (si se lanza desde niri, el diario de la sesión: `journalctl --user -f`) |
| motor: servidor | `~/.cache/niri-island/engine.log` |
| motor: renderizadores | `~/.cache/niri-island/engine-vk.log` |
| puente de Steam | `~/.cache/niri-island/steam.log` |
| fondos web (CEF) | `~/.cache/niri-island/web.log` |
| mpvpaper | `~/.cache/niri-island/mpvpaper.log` |
| compilación de depuración bajo gdb | `~/.cache/niri-island/gdb.log` |

Prefijos útiles en el registro de la isla: `[config]`, `[niri]`, `[notify]`, `[loop]`. En `engine-vk.log`, cada
escena deja: lienzo y resolución interna (`logical scene canvas … rasterized at …`), presupuesto
(`scene budget chains_admitted=… effect_target_bytes=…`), plan de targets, ruta de presentación
(`scene path = Vulkan DMA-BUF`), tope (`scene animated (N effect chain(s)), 30fps cap`) y prioridad de GPU.

## Variables de entorno de la isla

| Variable | Efecto |
|---|---|
| `NIRI_ISLAND_DEBUG=1` | imprime fps y ms por fotograma de la isla |
| `NIRI_ISLAND_OVLOG=1` | lo mismo para los overlays |
| `NIRI_SOCKET` | lo pone niri; sin él no hay espacios de trabajo ni pausa por ventanas |

Variables del motor: ver [07](07-motor.md#rendimiento-perillas). Recuerda que llegan al renderizador solo desde el
entorno del **servidor**.

## Probar sin tocar el teclado

La isla tiene comandos de depuración por IPC que inyectan clics, teclas y desplazamientos en sus propias
superficies (lista en [04](04-isla.md#depuración-pruebas-sin-pulsaciones-simuladas)). Ejemplos:

```bash
niri-island wallpapers; sleep 2.5
niri-island debug-picker-click 960 410 right      # clic derecho en el centro del selector (1920×1080)
niri-island debug-picker-key Escape
niri-island debug-click 10 10                     # clic en la isla, coordenadas relativas a ella
niri-island settings wallpapers
niri-island debug-settings-search motor
grim -g "1520,120 400x640" /tmp/panel.png         # mirar el resultado
```

No usar `wtype`/`ydotool`: las teclas irían a la ventana enfocada (el terminal desde el que se prueba, por ejemplo).

`niri-island debug-demo` oculta datos personales para capturas; `debug-idle 5` arranca el protector a los 5 s de
inactividad.

## Errores de memoria y cuelgues

```bash
# backtrace al caer
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DNIRI_ISLAND_ENGINE=OFF && cmake --build build-dbg
kill -TERM $(pgrep -x niri-island); setsid tools/run-dbg.sh &
# … reproducir …; luego:  less ~/.cache/niri-island/gdb.log

# ASan + UBSan sobre los tests
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DNIRI_ISLAND_ENGINE=OFF \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && build-asan/niri-island-tests
```

Errores típicos ya vistos:

- **Índices viejos tras cambiar una lista** (p. ej. `visible_` del selector después de cambiar de fuente): toda
  lista que un worker reemplaza debe invalidar los índices que apuntan a ella.
- **Resultados de workers que llegan tarde**: comprobar la generación antes de aplicar.
- **Superficie sin entrada**: si algo de la capa background no recibe clics, seguramente está en la regla
  `place-within-backdrop` de niri.

## Medir CPU

```bash
top -bn2 -d3 -p $(pgrep -x niri-island) | tail -1               # % de CPU de la isla
P=$(pgrep -f engine-vk | head -1); top -H -bn2 -d3 -p $P          # por hilo en el renderizador
```

## Medir la GPU

Hay dos herramientas en `tools/`:

```bash
tools/gpu-busy.sh 4      # Intel: % ocupada (residencia RC6) y frecuencia media durante 4 s
tools/gpu-procs.py 4     # cualquier driver DRM: % de tiempo de GPU por proceso (fdinfo drm-engine-*)
```

`gpu-procs.py` es la que dice **quién** ocupa la GPU. Ejemplo real con «Sleeping District» antes de las mejoras:

```
 84.6%  niri-island-eng
  6.0%  lniri
  2.7%  ghostty
```

El renderizador se comía la GPU y al compositor le quedaba un 6 %: toda la pantalla iba a tirones aunque la CPU
estuviera tranquila. Si el monitor del sistema marca la GPU al 100 % y la CPU baja, es esto.

`intel_gpu_top` (paquete `intel-gpu-tools`) da el desglose por motor (Render/3D, Video…), pero necesita permisos de
perf (`perf_event_paranoid` ≤ 0 o root).

## Medir una escena

1. Arrancar el servidor del motor con estadísticas y aplicar la escena:

   ```bash
   pkill -x niri-island-eng; sleep 1
   SKWD_VK_SCENE_STATS=1 setsid ~/.local/bin/niri-island-engine serve >/dev/null 2>&1 &
   sleep 1
   W=~/.local/share/Steam/steamapps/workshop/content/431960
   ~/.local/bin/niri-island-engine apply '*' $W/<id> --kind we --mute true --replace-all --scene-fps 60
   sleep 8; grep "scene stats" ~/.cache/niri-island/engine-vk.log | tail -3
   ```

   ```
   scene stats fps=16.0 compose_avg=60.2ms compose_max=63.4ms
   ```

2. Leerlo:
   - `fps` por debajo del tope y `compose_avg` ≈ 1000/fps → el fotograma no cabe: la escena es demasiado cara.
   - Mirar a la vez `tools/gpu-procs.py` y la CPU del renderizador: GPU alta y CPU baja → limitada por la GPU
     (probar `--scene-max 1280`); CPU de un núcleo al 100 % → scripts o partículas; las dos a medias → CPU y GPU en
     serie (como Koi Pond).
3. Devolver el control a la isla: `pkill -x niri-island-eng; niri-island wallpaper set "$(niri-island wallpaper current)"`.

Para saber **qué parte** de la escena cuesta: `SKWD_VK_PROFILE=1` en lugar de (o junto a) `SKWD_VK_SCENE_STATS`
da el tiempo de GPU de cada cadena de efectos, snapshot y modelo, y el de CPU de cada fase (ver
[07](07-motor.md#perfilador-skwd_vk_profile1)).

Más instrumentos: `SKWD_VK_SCENE_GPU_TIMESTAMPS` (tiempo de GPU por pase), `SKWD_DUMP_SHARED_TARGETS=dir` con
`capture-scenes` (ver los targets intermedios como PNG), `SKWD_MESH_DEBUG` (modelos de los scripts),
`SKWD_PAPER_SHADER_BACKEND=glsl` (descartar dxc).

## Mediciones de referencia (Intel UHD, 1080p, esta máquina)

| Qué | Resultado |
|---|---|
| GPU con fondo estático (solo niri + terminal) | ~44 % ocupada a ~470 MHz |
| Escena ligera (gato) | ~60 % ocupada a ~470-575 MHz; motor 2-5 % del tiempo de GPU |
| Sleeping District, antes | 68 ms/fotograma, 14 fps, GPU al 100 % a 1200 MHz |
| Sleeping District, ahora | 34 ms completa · 18 ms Ligera |
| Koi Pond | ~47-49 ms (CPU ~35 % de un núcleo + GPU ~60 %), ~20 fps |

La ocupación por RC6 cuenta también el tiempo a baja frecuencia: un 44 % a 470 MHz no es lo mismo que a 1200 MHz.
Para comparar, mirar la frecuencia o usar `gpu-procs.py`.
