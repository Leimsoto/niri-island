# 06 · Fondos y suite

`src/suite/` contiene los overlays a pantalla completa y el fondo de pantalla. Los overlays heredan de `Overlay`
(`suite/overlay.hpp`): una superficie layer-shell a pantalla completa con su propio bucle de fotogramas, fundido de
entrada y salida (con instantánea del contenido mientras se funde), su `Ui`, y métodos `inject_click/key/scroll`
para probarlos por IPC. `HostedOverlay` es un overlay cuyo contenido pinta la `App` (ajustes, burbuja de captura).

## El fondo de pantalla (`backdrop.*`)

`Backdrop` decide **quién dibuja** cada fondo y se encarga de que el cambio no deje ver el escritorio:

| Fuente | Dibujante | Superficie |
|---|---|---|
| Imagen (png, jpg, webp…) | la propia isla (`Drawer::Own`), Cairo | `niri-island-wallpaper`, una por pantalla |
| Vídeo suelto | el motor (`Drawer::Engine`), o `mpvpaper` si no está compilado | `niri-island-engine` / `mpvpaper` |
| Carpeta de Workshop tipo `scene` o `video` | el motor | `niri-island-engine` (+ `-input`) |
| Carpeta de Workshop tipo `web` | `WebWall` (CEF de open-wallpaper-engine) | `niri-island-web` |
| Otros tipos (`application`…) | — (error «no se puede mostrar») | |

El flujo completo está en [02 · Arquitectura](02-arquitectura.md#aplicar-un-fondo). Puntos finos:

- **Generación** (`gen_`): cada `set()` incrementa un contador; los resultados de trabajos más viejos se
  descartan. Si el motor termina de aplicar algo que ya no es lo pedido, se retira.
- **Relevo** (`handed_over`): el nuevo dibujante ya está encima cuando el viejo se retira (0,3 s para el motor,
  0,1 s para web, 0,6 s para mpvpaper).
- **Imágenes grandes**: se decodifican a tamaño completo y, una vez mostradas en todas las pantallas, se
  sustituyen por una copia del tamaño justo para cubrirlas (`fit_to_screens`; un PNG 4K son 33 MB decodificado) y
  se devuelve la memoria con `malloc_trim`.
- **Estado**: `~/.local/state/niri-island/wallpaper.json` (`{"path", "thumb", "type"}`). Al arrancar se restaura;
  si no hay, se toma el último de skwd-wall (`~/.cache/skwd-wall-v2/last-wallpaper.json`).
- **Paleta**: `make_palette` ejecuta `matugen image <src> --json hex -m dark -t scheme-tonal-spot --dry-run` con una
  config mínima propia (`~/.cache/niri-island/matugen.toml`, para no depender de la del usuario) y escribe
  `~/.cache/niri-island/colors.json`. Sin matugen, el color dominante hace de acento. Para vídeos se usa un
  fotograma (`wallpaper-frame.jpg`); para elementos del Workshop, su `preview`.
- **Propiedades** de un elemento del Workshop: `workshop::engine_overrides(ruta)` se pasa en cada `apply`.
- **Ajustes del motor** (`wallpaper_engine_fps`, `wallpaper_engine_quality`): `engine::configure()` al arrancar y
  `engine_settings_changed()` al cambiarlos, que vuelve a aplicar el fondo actual.

## El selector (`wallpicker.*`, `slices.cpp`, `picker_steam.cpp`)

`WallPicker` es un overlay (`niri-island-wallpapers`) que «sale» de la isla. Archivos:

- `wallpicker.cpp`: escaneo, filtros, aplicar, teclado, caché de miniaturas.
- `slices.cpp`: los tres estilos (carrusel de *slices* inclinados y retroiluminados, panal de hexágonos, tarjetas),
  la barra de filtros, la animación de entrada/salida y de «insertar» el elegido, y **wallhaven**.
- `picker_steam.cpp`: la fuente **Steam** (descargas de Wallpaper Engine y el Workshop), el **panel lateral**
  (vista previa animada, botones, propiedades editables) y **probar en el escritorio**.

### Fuentes

| Fuente | Qué muestra | Vista |
|---|---|---|
| Carpetas (por defecto) | imágenes y vídeos de `wallpaper_dirs` | el estilo elegido (`wallpaper_picker_style`) |
| wallhaven | resultados de la API de wallhaven | cuadrícula desplazable de tarjetas |
| Steam · descargas | los elementos del Workshop en disco (`workshop::items()`) | el estilo elegido; clic derecho abre el panel |
| Steam · Workshop | resultados de búsqueda del Workshop (vía `niri-island-steam`) | cuadrícula como wallhaven |

`grid_mode()` = wallhaven o Workshop. El clic en una tarjeta de la cuadrícula abre el panel; en las vistas de
estilo, clic aplica y clic derecho abre el panel (solo en Steam).

### Escaneo y miniaturas

`scan()` en un worker: recorre `wallpaper_dirs` (imágenes y vídeos) y, con Wallpaper Engine activado, los elementos
del Workshop (antes ejecuta `workshop::clean_workshop()` si `wallpaper_engine_clean`). Las miniaturas (520 px) van a
`~/.cache/niri-island/wallthumbs/`:

- imagen: gdk-pixbuf escalado;
- vídeo: `ffmpeg -ss 1.5 … -frames:v 1` (o el primer fotograma si es muy corto);
- elemento de Workshop: `niri-island-engine capture-scenes` renderiza un fotograma nítido sin pantalla
  (`engine::capture`); si sale casi negro, se usa el `preview` del elemento (un GIF pequeño).

Colores dominantes y `mtime` se guardan en `~/.cache/niri-island/wallpapers.json` para no recalcular. Las cachés
de recortes y brillos (`slice_cache_`, `glow_cache_`, `shape_cache_`) se hacen en workers y se liberan un rato
después de cerrar (`release_timer_`).

### wallhaven

`GET https://wallhaven.cc/api/v1/search?categories=110&purity=<100|110|111>&sorting=<toplist|date_added|random|views>
&order=desc&topRange=1M&page=N[&atleast=WxH][&q=…][&colors=<hex>][&seed=…][&apikey=…]`.

- `purity`: 100 SFW; 110 + sketchy; 111 + NSFW (necesita `wallhaven_key`). Lo marcado como +18 se resalta en rojo.
- `atleast`: el tamaño de la pantalla en píxeles reales.
- `seed`: para que «aleatorio» pagine de forma estable.
- Miniaturas de `th.wallhaven.cc`; la imagen completa se descarga a `~/.cache/niri-island/wallhaven/full-*` para
  probar, y a `wallhaven_dir` al añadirla.

### Steam: descargas y Workshop

- **En disco** (`services/workshop.*`): las bibliotecas de Steam salen de `libraryfolders.vdf` (raíces
  `~/.local/share/Steam`, `~/.steam/steam` y la de Flatpak `~/.var/app/com.valvesoftware.Steam/data/Steam`). Los
  elementos son `steamapps/workshop/content/431960/<id>/project.json`. Solo cuentan los que las cuentas siguen
  suscritas según `userdata/<cuenta>/ugc/431960_subscriptions.vdf` (y los más nuevos que esa lista, recién
  descargados). También aparecen los proyectos propios y los incluidos de Wallpaper Engine
  (`common/wallpaper_engine/projects/myprojects` y `defaultprojects`, filtrados por su `visibility.json`).
- **Limpieza** (`clean_workshop`): Steam deja en disco lo que dejas de seguir. Se borran las carpetas que no están
  en la lista y son anteriores (más de 60 s) a la última reescritura de la lista; sin lista o con lista vacía no se
  borra nada.
- **Workshop en línea** (`services/steam.*` + `engine/crates/island-steam`): ver el protocolo en
  [07 · El motor](07-motor.md#niri-island-steam). Suscribirse descarga con progreso; al terminar
  (`done`) se puede aplicar solo (`ws_apply_after_`). Desuscribirse borra también la carpeta.
- **Cancelar una descarga**: con el puntero sobre la barra de progreso del panel aparece ✕; `ws_cancel` manda
  `cancel` (el ayudante deja de seguirla y desuscribe) y, al llegar `unsubscribed`, la isla borra lo que hubiera
  llegado en `<workshop>/<id>`. Los `progress`/`done` que aún vengan de ese id se ignoran (`ws_cancelled_`).
- **Autor**: los resultados del Workshop traen la cuenta (`owner`) y el nombre (`author`); para las descargas,
  `project.json` no lo dice, así que al abrir el panel se pide `details` de todas a la vez (una vez por visita a
  Steam, sin aviso si Steam está cerrado: `ws_quiet_`). Mapas `ws_owner_of_` (id → cuenta) y `ws_names_`
  (cuenta → nombre); el panel muestra «por …» bajo el título.
- **Buscador**: el espacio escribe un espacio cuando ya hay texto; con el buscador vacío no hace nada en el
  Workshop ni en wallhaven (antes aplicaba, es decir, descargaba, la primera tarjeta). En las descargas y las
  carpetas, con el buscador vacío, sigue aplicando.

### Panel lateral y propiedades

`open_panel(vis)` muestra: la vista previa (el GIF animado del elemento, decodificado por un worker en fotogramas),
título, chips (tipo, tamaño, suscriptores, +18) y botones (principal ✓/▶/⬇, probar, desuscribir o guardar, abrir
en Steam o wallhaven).

Para elementos del Workshop **descargados**, el panel lista sus **propiedades editables**
(`general.properties` de `project.json`), vía `workshop::properties(dir)`:

| Tipo en `project.json` | Control | Valor guardado |
|---|---|---|
| `bool` | interruptor | `true`/`false` |
| `slider` (`min`, `max`, `step`) | deslizador con su valor | número (redondeado al paso) |
| `combo` (`options: [{label, value}]`) | píldora: clic avanza, clic derecho retrocede | el `value` de la opción |
| `color` | muestra del color | cadena `"r g b"` (solo lectura por ahora) |

- Se omiten los tipos `text`/`textinput`, las entradas cuyo texto empieza por `ui_` (las de la propia interfaz de
  Wallpaper Engine, como `ui_browse_properties_scheme_color`) y se quita el marcado HTML de las etiquetas. Orden: el
  campo `order`.
- `condition` del tipo `"<nombre>.value == <literal>"` o `!=` oculta la fila cuando no se cumple (p. ej. «hora fija»
  solo con «seguir el reloj» apagado). Otras condiciones se muestran siempre.
- Los valores del usuario se guardan por carpeta en `~/.config/niri-island/wallpaper-properties.json`:
  `{"<ruta de la carpeta>": {"<propiedad>": valor}}`.
- **Al motor** (`engine_overrides`): bool → 1/0, combo de palabras → **índice** de la opción (el motor solo maneja
  números y lo traduce de vuelta a la palabra para el script), color → cadena, números tal cual.
- Si el elemento editado es el fondo actual, 0,3 s después del último cambio se llama a
  `niri-island-engine properties <JSON>`: la escena los toma **en caliente** si puede; si el motor no lo está
  mostrando, se vuelve a aplicar entero.

### Probar en el escritorio

`try_on(i)`: el selector se aparta, el fondo se aplica de verdad (wallhaven descarga antes la imagen completa a la
caché) y aparece una barra ✓ / ✕. ✓ lo deja (y para wallhaven, lo copia a `wallhaven_dir`); ✕ vuelve al anterior
(`try_prev_`).

## Fondos web (`webwall.*`)

Los fondos web de Wallpaper Engine son páginas HTML/JS. Los dibuja **waywallen-weweb-renderer**, el renderizador
CEF del plugin open-wallpaper-engine de waywallen. La isla hace el papel del demonio de waywallen:

1. Instalación (Ajustes → Fondos → «Instalar soporte web»): `install_renderer()` consulta
   `https://api.github.com/repos/waywallen/open-wallpaper-engine/releases/latest`, descarga el asset
   `*linux-x86_64*.zip` a `~/.cache/niri-island/open-wallpaper-engine.zip` y extrae su `lib/weweb` a
   `~/.local/share/niri-island/weweb` (~340 MB).
2. `start(dir)`: crea un socket de escucha y lanza
   `env LD_LIBRARY_PATH=~/.local/lib/niri-island WW_LOG=warn <renderer> --ipc <socket> --path <dir> …`.
   El renderizador carga `libwaywallen-bridge.so` (compilada desde `third_party/waywallen-bridge`).
3. Protocolo `waywallen_ipc_v3` (códec en `ww-codec`): `init` → negociación de capacidades (formatos dma-buf que
   acepta niri) → `bind` de un *pool* de búferes (fds dma-buf) → por cada fotograma, `frame_ready(índice, punto,
   fence)`.
4. Cada fotograma trae un **fence** de sincronización: se espera (fd en el bucle) antes de mostrarlo. El búfer se
   envuelve como `wl_buffer` con `zwp_linux_dmabuf_v1` (uno por slot y pantalla), se pone en la superficie
   `niri-island-web` con un viewport que lo ajusta a la pantalla, y cuando niri lo suelta se señala el punto de la
   **línea temporal de liberación** del renderizador (un *syncobj* DRM importado en nuestro nodo de render).
5. Control: `play`, `pause`, `mute`, `unmute`, `shutdown`.

Registro: `~/.cache/niri-island/web.log`.

## Capturas (`capture.*`, `views/capture.cpp`)

- **Mod+S** (`niri-island capture`): `grim -t ppm` de la pantalla enfocada a `$XDG_RUNTIME_DIR/niri-island-shot.ppm`
  **antes** de mostrar nada (la pantalla «se congela» tal cual estaba, islas incluidas), el overlay
  `niri-island-capture` muestra esa imagen y la burbuja (`niri-island-capbar`) sube desde abajo con las opciones.
- Mientras dura, el teclado es solo de la captura (`capture_keys_`; los demás overlays suspenden el suyo).
- Editor: región, formas, flechas, texto, pixelado, contador, deshacer. Guardar en `screenshot_dir`
  (`<Imágenes>/Capturas`) y copiar con `wl-copy` si `screenshot_copy`.
- Grabación: ver `recorder` en [05](05-servicios.md#recorder).

## Protector de pantalla (`screensaver.*`)

«Burbujas» a lo Windows Vista, un overlay `niri-island-screensaver` por pantalla. Lo lanza una `IdleWatch` de
`screensaver_minutes` (el compositor la retiene con inhibidores: vídeo a pantalla completa, llamadas). Opcionalmente
bloquea tras `screensaver_lock_minutes`. Cualquier entrada lo cierra.

## Pantalla de bloqueo (`lock.*`)

Sobre `ext-session-lock-v1`: una superficie de bloqueo por pantalla (fondo difuminado, fecha, reloj, avatar
`~/.face`, campo de contraseña). La contraseña se comprueba con **PAM** en un worker, con el servicio
`niri-island` si existe `/etc/pam.d/niri-island`, si no `swaylock`, si no `login`. Si la isla muriera con la
pantalla bloqueada, el protocolo garantiza que la sesión **sigue bloqueada**. `cmd_lock` distinto de `internal`
usa otro bloqueador. `lock --preview <s>` bloquea y se desbloquea solo tras esos segundos, para probar el diseño sin escribir la
contraseña.
