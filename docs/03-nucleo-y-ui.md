# 03 · Núcleo e interfaz

`src/core/` es la plataforma (sin nada de la isla en sí) y `src/ui/` la capa de dibujo e interacción. Todo lo
demás se apoya en estas dos carpetas.

## core/

### `loop.hpp/.cpp` — el bucle de eventos

`Loop` es un singleton (`Loop::get()`), creado por `App` como miembro.

| API | Uso |
|---|---|
| `watch(fd, events, cb)` / `unwatch(fd)` | descriptores en el `poll` |
| `after(seg, fn, repetir=0)` → id; `cancel(id)` | temporizadores (monótonos) |
| `post(fn)` | desde cualquier hilo: ejecuta `fn` en el hilo principal (despierta el bucle con un eventfd) |
| `post_if_alive(fn)` (estático) | para hilos que pueden sobrevivir al bucle |
| `async(trabajo, hecho)` | `trabajo` en el pool de workers, `hecho` en el hilo principal |
| `before_poll` | callback que la app usa para dibujar y decir cuánto dormir |
| `shutting_down()` | las transferencias largas lo consultan para abandonar |

Los workers son entre 2 y 4 (`hardware_concurrency` acotado). Las tareas de `async` se encolan en orden, pero
pueden terminar en cualquier orden: quien encadena trabajos usa contadores de generación.

### `proc.hpp/.cpp` — procesos

- `proc::run(argv, timeout_ms, stdin?, merge_stderr?)`: `fork/exec` directo, sin shell; recoge stdout; mata al
  hijo si pasa el plazo. **Bloquea**: solo desde workers.
- `run_quiet`: igual sin capturar stdout (programas que se quedan sirviendo, como `wl-copy`).
- `start(argv, log)` / `start_piped(argv, in, out, log)`: hijo supervisado con su **pidfd** (legible al morir) y
  su salida a un log; `reap` lo recoge.
- `spawn(argv)`: doble fork, sesión propia, sin zombis (abrir apps, `xdg-open`).
- `spawn_shell(cmd)`: **solo** para comandos que configura el usuario (`cmd_lock`, `cmd_suspend`…).
- `has(nombre)`: ¿está en el `PATH`? (cacheado).
- `g_shutdown`: al salir, los hijos en curso se matan en vez de esperarse.

### `ipc.hpp/.cpp` — el socket de comandos

Socket Unix de flujo en `$XDG_RUNTIME_DIR/niri-island.sock`. Protocolo: el cliente escribe **una línea** (el
comando con sus argumentos separados por espacios; los que llevan espacios van entre comillas), cierra su lado de
escritura (`shutdown(SHUT_WR)`) y lee la respuesta hasta EOF. El servidor lee hasta el salto de línea (máximo
64 KiB o un plazo corto), llama al manejador en el hilo principal y responde con texto terminado en `\n`.
`instance_running()` prueba a conectar para saber si ya hay un demonio.

### `wayland.hpp/.cpp` — Wayland

`Wayland` enlaza los globales y gestiona el asiento (puntero, teclado con xkbcommon y repetición de teclas por
timerfd). Globales que usa:

| Global | Para qué | Si falta |
|---|---|---|
| `wl_compositor`, `wl_subcompositor`, `wl_shm` | superficies, subsuperficie del espectro, búferes | obligatorios |
| `zwlr_layer_shell_v1` | todas las superficies | obligatorio |
| `wp_viewporter` | escudo 1×1 escalado; fondos web | sin escudo/web |
| `wp_cursor_shape_manager_v1` | formas de cursor por nombre | cursor por defecto |
| `ext_background_effect_manager_v1` | desenfoque de niri detrás del cristal | sin desenfoque |
| `ext_idle_notifier_v1` | protector y bloqueo por inactividad | sin protector automático |
| `ext_session_lock_manager_v1` | pantalla de bloqueo | sin bloqueo propio |
| `zwp_linux_dmabuf_v1` | fotogramas GPU de los fondos web | sin fondos web |

`LayerSurface` es una superficie layer-shell (o de bloqueo) con **triple búfer en memoria compartida** (`wl_shm`
+ Cairo image surfaces), escala entera (`scale_`), callbacks de entrada y de fotograma, región de entrada
(`set_input_rects`), modo de teclado (`None`/`Exclusive`/`OnDemand`) y desenfoque (`set_blur`, que aproxima las
formas redondeadas con franjas). `begin_frame(área)` toma un búfer que el compositor no esté usando, borra solo
lo que ese búfer dibujó la última vez más el área nueva (el resto ya es transparente) y devuelve su `cairo_t*`
escalado; `end_frame(daño)` adjunta, marca el daño y pide el siguiente `frame`. Si los tres búferes están ocupados,
`begin_frame` devuelve nulo y se reintenta en la siguiente vuelta.

`SubSurface` es una subsuperficie con su propio doble búfer, colocada sobre su madre (`place` se aplica con el
siguiente commit de la madre).

`IdleWatch` envuelve una notificación de inactividad (`idled`/`resumed`). El compositor la retiene mientras haya
inhibidores (un vídeo a pantalla completa, una llamada).

El **escudo** (`set_shield`) es una superficie overlay transparente a pantalla completa que solo existe con un panel
abierto: un clic fuera del panel cae en ella y dispara `on_outside_click` (cerrar).

### `config.hpp/.cpp` — configuración

Un `Config` global (`config()`), cargado de `~/.config/niri-island/config.json`. Cada clave es opcional; si falta el
archivo se escribe con los valores por defecto. `load()` es tolerante: JSON roto → valores por defecto con un
aviso. `to_json()` lo serializa entero. Lista completa de claves en [09 · Configuración](09-configuracion.md).

El archivo está agrupado en secciones (`appearance`, `notifications`, `commands`, `suite`, `weather`). Para añadir
una clave: campo con valor por defecto y comentario en `config.hpp`, una línea `get(*sección, "clave", c.campo)` en
el bloque de su sección en `load()` y otra en el objeto de esa sección en `to_json()` (`config.cpp`); si se edita
desde Ajustes, una fila en `views/settings.cpp`. Hay un test (`tests/test_config.cpp`) que comprueba que lo guardado
se relee igual.

### `filewatch.hpp/.cpp` — inotify

Vigila el **directorio padre** de cada archivo, para seguir viendo los archivos que se reemplazan de forma atómica
(escribir temporal + `rename`, como hacen casi todas las herramientas y la propia isla). Rebota los eventos para no
recargar dos veces. Lo usan la configuración (recarga en caliente) y la paleta.

### `spring.hpp` — muelles

Oscilador armónico amortiguado integrado en pasos fijos de 1/240 s, así que el movimiento es idéntico a cualquier
tasa de fotogramas. `Spring::speed` (de `motion` en la config) escala el tiempo; 0 desactiva las animaciones. Toda
animación de la interfaz (morph de la isla, paneles, selector, scroll) es un `Spring`.

### `util.hpp/.cpp`

`Rect`, `Color`, `mix`, `lerp`, `smoothstep`; `ID(nombre, índice)` (FNV-1a) para identificadores estables de
widgets; cadenas (`trim`, `split`, `starts_with`, `sanitize_markup`…); UTF-8; tiempo (`now_mono`, `fmt_time`,
`time_ago`…); rutas XDG (`config_dir`, `state_dir`, `cache_dir`, `runtime_dir`, `user_dir("PICTURES")`);
`read_file`, `write_file_atomic` (temporal + rename), `make_dirs`, `shell_quote`.

## ui/

### `theme.hpp/.cpp` — tema

`Palette` son los *tokens* de diseño (`island`, `surface`, `text`, `text_dim`, `accent`, `good`, `warn`, `bad`…).
**Ninguna vista escribe un color a mano**: todas leen `T()`. `Theme::load()` lee la paleta configurada:
`adaptive` toma `~/.cache/niri-island/colors.json` (lo escribe el fondo con matugen) y `from_accent` construye el
resto; las fijas (`lavender`, `ocean`, `rose`, `mint`, `amber`, `mono`) salen de `named()`. Al cambiar, `update(dt)`
funde de la paleta vieja a la nueva.

### `paint.hpp/.cpp` — pintor

Funciones libres sobre Cairo/Pango: rectángulos redondeados, rellenos, trazos, sombras (dentro o fuera de la forma,
para cuerpos translúcidos), brillo diagonal (`sheen`), texto con ajuste/elipsis/alineación (`Text`), medición,
posición del cursor de texto, glifos centrados en su tinta (iconos de Nerd Font), imágenes recortadas, anillos,
barras, sparklines. Las fuentes salen de la config: `ui()`, `mono()`, `hand()` (notas). Los `PangoLayout` y las
descripciones de fuente se cachean (`reset_fonts()` al cambiar la config).

### `ui.hpp/.cpp` — regiones e interacción

El corazón de la interacción: **las regiones de clic se registran mientras se dibuja**. `Ui::add(id, rect)` guarda
la región con la transformación actual (`push`/`pop`, que imitan `cairo_translate/scale`) y el recorte (`push_clip`).
La comprobación de clics usa las regiones del **fotograma que está en pantalla**, así que lo que se pulsa es
exactamente lo que se ve; no hay tablas de coordenadas que mantener.

Una `Region` tiene `click`, `right`, `middle`, `drag` (con `DragEvent`: posición absoluta, local y relativa 0..1),
`release`, `scroll` y forma de cursor. `hover(id)` y `press(id)` devuelven estados animados 0..1 por id; `pulse(id)`
hace rebotar un control (para teclado e IPC). `set_enabled(false)` evita registrar regiones mientras algo se funde.

### `widgets.hpp/.cpp` — widgets

Sobre `Ui` + `paint`: `hit`, `icon_button`, `pill` (estilos Plain/Tonal/Accent), `header` (panel con botón atrás),
`card`, `toggle`, `slider` (con glifo y etiqueta derecha), `row`, `scrollbar`, `press_scale`. Todos devuelven o
registran su propia región.

### `textfield.hpp/.cpp`

Búfer de texto de una o varias líneas con cursor, consciente de UTF-8: insertar, borrar (también palabra a
palabra), moverse por caracteres o palabras, inicio/fin. `key()` devuelve qué pasó (`Changed`, `Submit`, `Cancel`,
`Paste`, `Up`, `Down`, `Tab`…); `Paste` (Ctrl+V) lo resuelve la app pidiendo el portapapeles de forma asíncrona. Lo usan el lanzador, la contraseña Wi-Fi, las tareas, las notas, el buscador de ajustes y el del selector.
