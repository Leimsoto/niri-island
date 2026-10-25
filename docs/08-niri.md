# 08 · Integración con niri

niri-island está hecho para [niri](https://github.com/YaLTeR/niri). Usa tres vías: el **IPC** de niri, sus
**reglas de configuración** (un archivo KDL que la isla escribe) y los **protocolos Wayland** que niri implementa.

## IPC (`services/niri.cpp`)

niri publica `$NIRI_SOCKET`. La isla abre:

- **Una conexión larga** a la que manda `"EventStream"` y de la que lee una línea JSON por evento. Se reconecta sola
  si niri se reinicia. Eventos que consume:

  | Evento | Para qué |
  |---|---|
  | `WorkspacesChanged`, `WorkspaceActivated`, `WorkspaceActiveWindowChanged`, `WorkspaceUrgencyChanged` | módulo de espacios de trabajo |
  | `WindowsChanged`, `WindowOpenedOrChanged`, `WindowClosed`, `WindowFocusChanged`, `WindowUrgencyChanged` | ventana activa, `@` del lanzador, enfocar apps |
  | `WindowLayoutsChanged` | tamaño de cada ventana: ¿cubre la pantalla? (pausar fondos, ocultar la isla a pantalla completa) |
  | `OverviewOpenedOrClosed` | con la vista general abierta nada cuenta como «cubierto» |

- **Una conexión corta por acción**: `{"Action": {"FocusWorkspace": {"reference": {"Id": N}}}}`,
  `FocusWorkspaceDown/Up`, `{"Action": {"FocusWindow": {"id": N}}}`.

Además ejecuta `niri msg -j outputs` / `focused-output` (tamaño lógico de la pantalla, salida enfocada para
capturas y paneles) y `niri msg action screenshot-window --id N` para capturar una ventana.

`Niri::screen_covered(w, h)`: alguna ventana activa de un espacio visible mide al menos el 95 % del ancho y el 85 % del
alto de la pantalla. `fullscreen(w, h)`: una ventana no flotante ocupa la pantalla entera.

Si niri cambia el formato del IPC, todo esto está en `niri.cpp` (análisis con nlohmann::json; los campos que faltan
se ignoran).

## Reglas KDL (`views/bar.cpp::write_niri_rules`)

La isla escribe `~/.config/niri/cfg/niri-island.kdl` **solo si cambia** (cada escritura hace que niri recargue la
configuración) y solo si existe `~/.config/niri/cfg/`. Hay que incluirlo desde `config.kdl`:

```kdl
include "cfg/niri-island.kdl"
```

Contenido:

```kdl
// Generado por niri-island. Se reescribe solo.
layer-rule {
    match namespace="^niri-island-wallpaper$"
    match namespace="^mpvpaper$"
    match namespace="^niri-island-engine$"
    match namespace="^niri-island-web$"
    place-within-backdrop true
}
// solo con compositor_blur:
layer-rule {
    match namespace="^niri-island$"
    match namespace="^niri-island-bar-"
    match namespace="^niri-island-capbar$"
    background-effect {
        xray true|false
    }
}
```

### `place-within-backdrop` y la entrada

Con `place-within-backdrop true`, niri dibuja la superficie como **fondo de la vista general**: se ve una sola vez
detrás de todos los espacios de trabajo, en lugar de una copia dentro de cada uno. Pero niri **no envía entrada** a
esas superficies (en su código, `contents_under`/`layer_surface_under` saltan las que tienen
`place_within_backdrop()`). Por eso:

- el namespace `niri-island-engine-input` **no** está en la regla: son las superficies transparentes que el motor
  crea para recibir el puntero (escenas interactivas, como los peces de Koi Pond que huyen del cursor);
- si se añade otro namespace de fondo que necesite clics, debe quedar fuera de la regla o tener su gemela de entrada.

### Desenfoque

`background-effect { xray … }` pide a niri el desenfoque detrás de la isla y sus franjas (protocolo
`ext-background-effect-v1`, que la isla también usa por superficie con `set_blur`). `xray true` desenfoca solo el
fondo de pantalla; `false` también las ventanas de detrás. No se aplica a todo `niri-island-*`: la ventana de ajustes
a pantalla completa quedaba desenfocada hasta destruirse.

## Huecos y márgenes

`niri_gaps()` lee el `gaps N` de `config.kdl`, `layout.kdl` o `cfg/layout.kdl` (por defecto 16, el de niri) para
colocar la isla y su zona exclusiva: niri suma su propio hueco exterior debajo de cualquier zona reservada, y la isla
lo compensa para que `window_gap` sea la distancia real entre la isla y las ventanas.

## Protocolos Wayland de niri que se usan

wlr-layer-shell (todas las superficies), ext-session-lock (bloqueo), ext-idle-notify (protector), viewporter,
cursor-shape, linux-dmabuf (fondos web y motor), ext-background-effect (desenfoque; reciente, específico de niri
por ahora) y wp-presentation-time (motor). Ver [11 · Mantenimiento](11-mantenimiento.md#protocolos-wayland).

## Lanzamiento y atajos

```kdl
spawn-at-startup "niri-island"
spawn-at-startup "swayidle" "-w" "before-sleep" "niri-island lock"
```

Atajos sugeridos en el [README](../README.md#integración-con-niri). Todos los paneles se cierran con el mismo atajo
que los abre (el comando alterna).

## niri modificado

Este equipo usa una compilación propia de niri (rama `mi-niri-v26.04` en `~/programas/niri`, binario `lniri`). La
isla no depende de nada de esa rama: funciona con un niri estándar que soporte `place-within-backdrop` en las
`layer-rule` y, para el desenfoque, `background-effect` / `ext-background-effect-v1`. Con un niri que no los
conozca, la configuración daría error al recargar: en ese caso hay que quitar esas reglas de `write_niri_rules`.
