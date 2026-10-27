# 04 · La isla

`src/island/` es la aplicación (`App`): crea las superficies, conecta servicios y entrada, decide qué se muestra y
atiende los comandos IPC. `src/views/` contiene los métodos de dibujo de `App`, repartidos por tema (todos son
`App::draw_*`; no son clases aparte).

## Vistas y arbitraje

`View` (`island/app.hpp`) enumera lo que la isla puede mostrar. Hay dos clases:

- **Capas automáticas**: `Rest` (reposo), `Glance` (resumen al pasar el ratón), `Music` (reproductor),
  `Notification`, `Osd` (volumen, brillo, micrófono, batería, mensajes).
- **Paneles** (todo lo que va de `Control` en adelante): `Control`, `Wifi`, `WifiPassword`, `WifiInfo`,
  `Bluetooth`, `Sound`, `Weather`, `Calendar`, `DatePicker`, `Board`, `Notes`, `NoteEditor`, `Launcher`, `Session`,
  `Stats`, `Timer`, `Record`, `Clipboard`, `Inbox`, `Tray`.

Los paneles se **apilan** en `nav_` (`open(v)` empuja, `back()` saca, `close_all()` vacía). `decide()` elige, por
prioridad:

```
panel en lo alto de la pila  >  notificación actual  >  OSD vigente  >  resumen (Music si hay MPRIS activo, si no Glance)  >  Rest
```

Abrir un panel descarta el OSD, el resumen y la notificación en curso. `on_view_enter/leave` arrancan y paran lo
que una vista necesita (escaneo Wi-Fi, cava, cargar el portapapeles…). `update_keyboard()` pide teclado
**exclusivo** solo mientras hay un campo de texto activo (lanzador, contraseña, tareas, notas…) y ninguno el resto
del tiempo, para no robar el foco a las ventanas.

### Tamaño y morph

`sizes_for(v, w, h)` da el tamaño objetivo de cada vista. El tamaño visible lo llevan dos muelles (`w_`, `h_`)
hacia ese objetivo. Al cambiar de vista, la saliente se dibuja **una vez** en una instantánea y solo se escala y
funde; la entrante se dibuja en vivo encima (`morph_` va de 0 a 1). Las notas tienen su propia transición
(la tarjeta crece hasta papel y vuelve a su hueco en la baraja).

### Entrada

- Puntero: `on_motion`, `on_button`, `on_scroll` pasan primero por `Ui` (regiones registradas al dibujar). Lo que
  ninguna región toma llega a la isla (`island_click`): en reposo o resumen abre el centro de control; sobre el
  reproductor enfoca la ventana de la app (o pide `Raise` por MPRIS); sobre una notificación la activa; sobre un
  OSD lo cierra.
- Pasar el ratón por la isla en reposo arma un temporizador (`glance_delay_ms`); al vencer, `summary_` muestra el
  resumen o el reproductor.
- Teclado: `on_key` lo reparte al panel activo (campos de texto, navegación con flechas en sesión y lanzador,
  Escape para atrás).
- Clic fuera de un panel: el **escudo** de `Wayland` lo detecta y cierra.

## La barra

`views/bar.cpp` dibuja los módulos (espacios de trabajo, ventana activa, buscador, bandeja, notificaciones, red,
Bluetooth, volumen, batería, rendimiento) y aplica la distribución:

| `bar_layout` | Qué hace |
|---|---|
| `grouped` | cápsulas a izquierda y derecha pegadas a la isla, en la misma superficie |
| `spread` | módulos en las esquinas, en dos superficies propias (`EdgeBar`, `island/edgebar.cpp`, 900×64) |
| `island` | todo dentro de la isla |

`island_style`: `floating` (cápsula con margen) o `attached` (acoplada al borde superior con «orejas»;
`dock_sides` decide si las laterales también se acoplan). `apply_layout()` abre o cierra las `EdgeBar`, ajusta
márgenes y zona exclusiva, y reescribe las reglas de niri (`write_niri_rules`, ver [08](08-niri.md)).

Las `EdgeBar` solo se redibujan cuando cambia lo que muestran: `bar_signature()` resume su contenido en una
cadena (incluidas las cifras del monitor de rendimiento), y solo si cambia se les pide fotograma.

El **monitor de rendimiento** (módulo `perf`) muestra las métricas de `perf_metrics` (`cpu`, `gpu`, `ram`,
`temp`, `net`, `disk`, `swap`) como números o anillos (`perf_style`), en una cápsula o en una por métrica
(`perf_split`). Clic abre el panel Sistema (`View::Stats`, `views/panels.cpp::draw_stats`), que muestra procesador,
gráficos (si la GPU es legible), memoria, red y disco con sus historiales.

## Ajustes

`views/settings.cpp` construye el centro de ajustes como una lista de filas (`SetRow`, tipos `Toggle`, `Choice`,
`Slider`, `Text`, `Info`, `Buttons`, `Preview`, `Custom`) agrupadas por sección:

0. Apariencia · 1. Barra e isla · 2. Cristal · 3. Notificaciones · 4. Capturas y grabación · 5. Fondos de
pantalla (carpetas, pausa con pantalla completa, estilo del selector y el grupo Wallpaper Engine: activar,
sonido, borrar los que dejas de seguir, **fotogramas** y **calidad** del motor, abrir el Workshop, instalar el
soporte web) · 6. Sistema (comandos, terminal, tipografías, protector de pantalla, bloqueo) · 7. Acerca de.

Cada fila tiene un *getter* y un *setter* sobre `config()`. El setter guarda con `save_config_soon()` (rebote corto
→ `write_file_atomic` de `to_json()`), y algunos aplican al momento (p. ej. `backdrop_.set_audio`,
`backdrop_.engine_settings_changed`). El buscador filtra filas por título, subtítulo y grupo. Se muestra en una
`HostedOverlay` (`niri-island-settings`) que pinta la propia `App`.

**Añadir un ajuste**: la clave en `Config` (ver [03](03-nucleo-y-ui.md#confighppcpp--configuración)) y una
llamada `toggle(...)`, `choice(...)`, `slider(...)` en `draw_settings` con su sección y grupo.

## Comandos IPC

`island/commands.cpp` (`App::command`) interpreta la línea. `niri-island help` imprime el resumen. Lista completa:

### Paneles y navegación
`cc` / `control` / `toggle-control-center`, `launcher [texto]` / `toggle-launcher` / `run`, `wifi` / `open-wifi`,
`bt` / `bluetooth` / `open-bt`, `sound` / `audio`, `weather`, `calendar` / `cal` (`cal-next`, `cal-prev`,
`cal-today`), `tasks` / `board`, `notes`, `note-new [texto]` / `quick-note`, `note-open <id>` / `open-note`,
`stats` / `system`, `session` / `power-menu`, `clipboard`, `notifications` / `inbox`, `tray`, `timer`, `stopwatch`,
`settings [sección]` (`appearance bar glass notifications capture wallpapers system about`), `glance` / `peek` /
`expand`, `close`, `back`.

### Suite
`wallpapers [wallhaven|engine]` / `wallpaper-picker` / `walls`; `wallpaper [current|next|prev|set <ruta>]`;
`capture [record]`; `screenshot [region|screen|window] [retardo] [--no-edit]` / `shot`;
`record [toggle|start|region|screen|pause|resume|stop|cancel]`; `screensaver [stop]`;
`lock [--preview <s>]`, `lock-state`; `night-light`.

### Multimedia y niveles
`play-pause` / `toggle-media`, `next`, `prev` / `previous`; `volume-up [n]`, `volume-down [n]`, `volume <n>`,
`mute` / `toggle-mute`, `mic-mute` / `toggle-mic`; `brightness-up [n]`, `brightness-down [n]`.

### Interruptores
`gaming [on|off|toggle|status]` / `gaming-mode` / `toggle-gaming`, `toggle-wifi`, `toggle-bluetooth` / `toggle-bt`,
`dnd` / `toggle-dnd [off|discreet|total|cycle]`, `toggle-airplane` / `airplane`, `power-profile [nombre]`.

### Wi-Fi, tareas, temporizador
`wifi-list`, `wifi-scan` / `rescan-wifi`, `wifi-status`, `wifi-connect <ssid> [clave]`, `wifi-password <ssid>`,
`wifi-disconnect`, `wifi-forget <ssid>`; `task-add [--date AAAA-MM-DD] <título>` / `add-task`;
`timer <25m|1h30|5:00> [etiqueta]`, `timer pause`, `timer cancel`.

### Otros
`notify <app> <resumen> <cuerpo>`, `notifications-clear`, `battery`, `battery-osd`, `osd-test`, `tray-list`,
`reload` (relee config, fuentes y tema), `status`, `test-notif` / `mock-notify`.

### Depuración (pruebas sin pulsaciones simuladas)

| Comando | Qué hace |
|---|---|
| `debug-move x y` / `debug-click x y [right\|middle]` | puntero en coordenadas relativas a la isla |
| `debug-drag x1 y1 x2 y2` | arrastre en 10 pasos |
| `debug-edge-click left\|right x y [right]` | clic en una barra de esquina |
| `debug-picker-click x y [right]` | clic en el selector de fondos (coordenadas de su superficie) |
| `debug-picker-key <Nombre\|texto>` | una tecla xkb (`Escape`, `Return`, `Left`…) o texto, al selector |
| `debug-settings-click x y`, `debug-settings-scroll x y pasos`, `debug-settings-search texto` | ajustes |
| `debug-capbar-click x y` | burbuja de captura |
| `debug-idle <segundos>` | rehace la vigilancia de inactividad con ese plazo (probar el protector) |
| `debug-demo [off]` | oculta datos personales para capturas de pantalla |

Las pruebas con la isla en marcha se hacen **siempre** con estos comandos, nunca con pulsaciones simuladas
(`wtype`, `ydotool`): esas irían a la ventana que tenga el foco.

**Añadir un comando**: un `if (c == "nombre")` en `App::command` (los argumentos llegan en `a`, el resto de la línea
en `rest`), y una línea en el texto de `help`.
