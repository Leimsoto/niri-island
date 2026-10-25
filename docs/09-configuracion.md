# 09 · Configuración y datos

## `~/.config/niri-island/config.json`

Se crea con los valores por defecto la primera vez y **se recarga solo** al guardarse (inotify). Casi todo se
edita desde Ajustes (`Mod+P` / `niri-island settings`). Todas las claves son opcionales; el JSON admite
comentarios (`//`). Si el archivo no es JSON válido se usan los valores por defecto (y se avisa por stderr).
Definición: `src/core/config.hpp`; lectura y escritura: `src/core/config.cpp`.

El archivo está **agrupado en cinco secciones**. Ojo: algunas claves no están donde su nombre sugiere (los ajustes
de fondos, protector y terminal viven en `commands`); se respeta así por compatibilidad con archivos ya escritos.

```jsonc
{
  "appearance":    { … },   // aspecto, barra, isla, sombras, cristal, módulos, monitor de rendimiento
  "notifications": { … },   // mode, timeout_ms, history
  "commands":      { … },   // comandos + fondos + Wallpaper Engine + protector + terminal
  "suite":         { … },   // carpetas de fondos, wallhaven, capturas
  "weather":       { … }    // cache, latitude, longitude, location_name
}
```

### `appearance`

| Clave | Defecto | Valores / notas |
|---|---|---|
| `theme` | `"adaptive"` | `adaptive` (paleta del fondo) o `lavender`, `ocean`, `rose`, `mint`, `amber`, `mono` |
| `font` / `mono_font` / `note_font` | `"Inter"` / `"JetBrainsMono Nerd Font"` / `"Caveat"` | familias Pango; los iconos son glifos de Nerd Font |
| `palette_file` | `"~/.cache/niri-island/colors.json"` | la escribe el fondo |
| `wallpaper_file` | `"~/.local/state/niri-island/wallpaper.json"` | |
| `margin_top` | `8` | px entre la isla flotante y el borde |
| `clock_24h` | `true` | |
| `modules` | ver abajo | lista ordenada `{id, side, on}`; el orden es hacia fuera desde la isla |
| `battery_percent` | `true` | número junto al icono |
| `perf_metrics` | `["cpu","ram","temp"]` | de `cpu`, `gpu`, `ram`, `temp`, `net`, `disk`, `swap` |
| `perf_style` | `"numbers"` | `numbers`, `rings` |
| `perf_split` | `false` | una cápsula por métrica |
| `bar_layout` | `"grouped"` | `grouped`, `spread`, `island` |
| `island_style` | `"floating"` | `floating`, `attached` |
| `dock_sides` | `true` | acoplada: las laterales también se acoplan |
| `bar_height` | `32` | 24–48 px |
| `roundness` | `1.0` | 0 casi cuadrada … 1 redonda |
| `reserve_space` | `true` | las ventanas dejan sitio a la isla (zona exclusiva) |
| `window_gap` | `4` | px entre la isla y las ventanas |
| `shadows` | `true` | |
| `shadow_opacity` / `shadow_softness` / `shadow_distance` / `shadow_angle` | `0.40` / `18` / `4` / `90` | 0..1, px, px, grados (90 = hacia abajo) |
| `glass` | `true` | cuerpo translúcido |
| `island_opacity` | `0.80` | opacidad del cuerpo con cristal |
| `compositor_blur` | `false` | pedir desenfoque a niri (ext-background-effect + regla KDL) |
| `blur_xray` | `false` | con desenfoque: `true` solo el fondo, `false` también las ventanas |
| `motion` | `1.0` | multiplicador de velocidad de los muelles; 0 sin animaciones |
| `glance_delay_ms` | `140` | espera al pasar el ratón antes del resumen |
| `show_workspaces`, `show_status` | `true` | heredadas: solo se usan si no hay `modules` |

Ids de módulo: `workspaces`, `window`, `search`, `tray`, `notifications`, `network`, `bluetooth`, `volume`,
`battery`, `perf`. Si una versión nueva añade módulos, `load()` añade los que falten (con sus valores por defecto)
al final de la lista guardada, para que aparezcan en el editor.

### `notifications`

| Clave | Defecto | Notas |
|---|---|---|
| `mode` | `"auto"` | `auto`, `server`, `monitor`, `off` (ver [05](05-servicios.md#notifications)) |
| `timeout_ms` | `5500` | |
| `history` | `60` | entradas guardadas |

### `commands`

| Clave | Defecto | Notas |
|---|---|---|
| `lock` | `"internal"` | bloqueo propio, o un comando |
| `logout` | `"niri msg action quit --skip-confirmation"` | |
| `suspend` / `reboot` / `poweroff` | `systemctl suspend/reboot/poweroff` | |
| `night_light` | `"pkill -x wlsunset \|\| setsid -f wlsunset -t 4000 -T 4001"` | |
| `wallpaper_next` / `wallpaper_picker` / `wallpaper_apply` | `"internal"` | el último admite `{path}` |
| `settings` | `""` | |
| `terminal` | `"ghostty"` | |
| `wallpaper_picker_style` | `"slices"` | `slices` (carrusel), `hex`/`grid` (panal), `cards` |
| `wallpaper_pause_covered` | `true` | pausar animados con una ventana a pantalla completa o maximizada |
| `wallpaper_engine` | `true` | elementos del Workshop en el selector |
| `wallpaper_engine_audio` | `false` | sonido de los fondos con audio |
| `wallpaper_engine_clean` | `true` | borrar lo que Steam deja al desuscribirse |
| `wallpaper_engine_fps` | `30` | tope de fps del motor (30/60 en Ajustes) |
| `wallpaper_engine_quality` | `0` | 0 completa, 1 equilibrada (1600 px), 2 ligera (1280 px) |
| `screensaver` | `true` | |
| `screensaver_minutes` | `5` | |
| `screensaver_lock_minutes` | `0` | 0 = no bloquear |
| `screensaver_bubbles` | `1` | 0 pocas, 1 normal, 2 muchas |
| `screensaver_speed` | `1.0` | 0,5 … 2 |

### `suite`

| Clave | Defecto | Notas |
|---|---|---|
| `wallpaper_dirs` | `["~/Imágenes/wallpapers", "~/Pictures/Wallpapers"]` | carpetas del selector |
| `wallhaven_dir` | `"~/Imágenes/wallpapers/wallhaven"` | descargas |
| `wallhaven_key` | `""` | clave de API (necesaria para NSFW) |
| `wallhaven_purity` | `"100"` | bits sfw/sketchy/nsfw al estilo wallhaven |
| `screenshot_dir` | `""` | vacío = `<Imágenes>/Capturas` (XDG `PICTURES`) |
| `recording_dir` | `""` | vacío = `<Vídeos>/Grabaciones` (XDG `VIDEOS`) |
| `screenshot_copy` | `true` | copiar también al portapapeles |

### `weather`

| Clave | Defecto | Notas |
|---|---|---|
| `cache` | `"~/.cache/noctalia/weather.json"` | caché de noctalia |
| `latitude` / `longitude` / `location_name` | `0` / `0` / `""` | si faltan, se toman de noctalia |

## Otros archivos

### Configuración (`~/.config/niri-island/`)

| Archivo | Qué |
|---|---|
| `config.json` | lo de arriba |
| `wallpaper-properties.json` | valores del usuario para las propiedades de cada elemento del Workshop: `{"<carpeta>": {"<propiedad>": valor}}` |

### Estado (`~/.local/state/niri-island/`)

| Archivo | Qué |
|---|---|
| `wallpaper.json` | fondo actual `{"path", "thumb", "type"}` |
| `tasks.db` | tareas y notas (SQLite, ver [05](05-servicios.md#store)) |
| `notifications.json` | historial de notificaciones |
| `launches.json` | frecuencia de uso del lanzador |
| `recorder.json` | opciones de grabación |
| `state.json` | estado suelto de la isla (No molestar…) |

### Caché (`~/.cache/niri-island/`) — se puede borrar

| Ruta | Qué |
|---|---|
| `colors.json`, `matugen.toml` | paleta generada y config mínima de matugen |
| `wallpapers.json`, `wallthumbs/` | índice y miniaturas del selector |
| `wallhaven/` | miniaturas y `full-*` de wallhaven |
| `workshop/` | vistas previas del Workshop |
| `art/` | carátulas descargadas |
| `clip/` | miniaturas del portapapeles |
| `weather.json` | clima propio (open-meteo) |
| `wallpaper-frame.jpg` | fotograma del vídeo de fondo (paleta, bloqueo) |
| `open-wallpaper-engine.zip` | descarga del renderizador web |
| `engine.log`, `engine-vk.log`, `steam.log`, `web.log`, `mpvpaper.log`, `gdb.log` | registros |
| `rec-*`, `rec-thumb.jpg` | temporales de la grabación |

Fuera de esa carpeta: `~/.cache/skwd-wall/scene-shaders/` (SPIR-V compilado de las escenas).

### Datos (`~/.local/share/niri-island/`)

| Ruta | Qué |
|---|---|
| `weweb/` | renderizador CEF de los fondos web (~340 MB) |

### En ejecución (`$XDG_RUNTIME_DIR`)

| Ruta | Qué |
|---|---|
| `niri-island.sock` | IPC de la isla |
| `niri-island-engine/engine.sock` | servidor del motor |
| `niri-island-shot.ppm` / `.png` | captura congelada en curso |
