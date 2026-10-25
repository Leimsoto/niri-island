# 11 · Mantenimiento

Qué depende de cosas externas que pueden cambiar, dónde está ese código y cómo actualizarlo. Al final, una lista
de comprobación para después de cualquier actualización.

## Mapa rápido: «cambió X, toca Y»

| Cambió… | Síntoma probable | Dónde mirar |
|---|---|---|
| IPC de niri (eventos, campos, acciones) | espacios de trabajo vacíos, la pausa de fondos no funciona | `src/services/niri.cpp` |
| Sintaxis de la config de niri | niri da error al recargar tras escribir la isla su archivo | `write_niri_rules` en `src/views/bar.cpp` |
| Protocolos Wayland *staging* | falla CMake (falta un XML) o la isla no arranca | `CMakeLists.txt`, `src/core/wayland.*` |
| Steamworks / SDK de Steam | el Workshop no conecta, `niri-island-steam` no arranca | `engine/crates/island-steam` |
| Formato de archivos de Steam | el selector no ve descargas o borra de más | `src/services/workshop.cpp` |
| Formato de Wallpaper Engine | escenas que no cargan, negras o mal | `engine/crates/paper-scene` |
| API de SceneScript | scripts que lanzan errores | `engine/crates/paper-scene/src/script/bootstrap.js` |
| Crate `v8` | no compila el motor | `engine/crates/paper-scene/src/script/js.rs` |
| ffmpeg (versión mayor) | no compila o no decodifica | `ffmpeg-the-third` en `paper-vk` y `paper-audio` |
| API de wallhaven | modo wallhaven vacío | `src/suite/slices.cpp` (`wh_fetch`) |
| open-meteo / caché de noctalia | clima vacío | `src/services/weather.cpp` |
| Releases de open-wallpaper-engine | «Instalar soporte web» falla | `WebWall::install_renderer` en `src/suite/webwall.cpp` |
| Protocolo de waywallen | fondos web negros o el renderizador se cierra | `third_party/waywallen-bridge`, `src/suite/webwall.cpp` |
| Salida de `nmcli` | Wi-Fi vacío o mal | `src/services/network.cpp` |
| Opciones de `matugen` | la paleta adaptativa no cambia | `Backdrop::make_palette` |
| Opciones de `wf-recorder`/`ffmpeg` | la grabación no empieza o no une | `src/services/recorder.cpp` |
| Nombre D-Bus de power-profiles | perfiles de energía no responden | `src/services/power.cpp` |
| skwd-paper (upstream) | mejoras que traer | ver [skwd-paper](#skwd-paper) |

## niri

**IPC.** `services/niri.cpp` lee el `EventStream` con nlohmann::json y tolera campos que falten. Si niri renombra un
evento o un campo, se nota en el módulo de espacios de trabajo y en `screen_covered`. Referencia: el crate
`niri-ipc` de niri (`niri-ipc/src/lib.rs`), que define `Event`, `Window`, `Workspace`, `WindowLayout` y `Action`.
Una copia del código de niri para leer está en `~/programas/ref/niri`.

**Configuración.** La isla genera `layer-rule` con `place-within-backdrop` y `background-effect { xray }`. Si niri
cambia esos nombres, actualizar `write_niri_rules`. La regla del fondo es la que hace que el fondo sea uno solo en la
vista general; si niri llegara a dar entrada a esas superficies, las gemelas `niri-island-engine-input` sobrarían
(se pueden desactivar con `SKWD_VK_INPUT=passthrough`).

**Comportamiento.** `screen_covered` usa los tamaños de `WindowLayoutsChanged` (`tile_size`). Si niri deja de
mandarlos, la pausa de fondos con ventanas maximizadas dejará de funcionar.

## Protocolos Wayland

Los XML se generan al compilar. `xdg-shell` y `wlr-layer-shell-unstable-v1` van copiados en `protocols/`; los demás
salen de `wayland-protocols` del sistema:

| Protocolo | Estado | Si cambia o falta |
|---|---|---|
| `ext-background-effect-v1` | *staging*, reciente | si la distro no lo trae: copiar el XML a `protocols/` y apuntar ahí la llamada a `generate_wayland_proto` |
| `cursor-shape-v1`, `ext-idle-notify-v1` | *staging* | ídem |
| `ext-session-lock-v1` | *staging* | ídem; sin él no hay bloqueo propio |
| `linux-dmabuf-v1`, `viewporter`, `tablet-v2` | estables | |

Cuando un protocolo *staging* pasa a estable, cambia su ruta (`staging/…` → `stable/…`) y a veces el sufijo; hay que
actualizar la ruta en `CMakeLists.txt` y, si cambió el prefijo de los símbolos, los usos en `src/core/wayland.cpp`.
El motor usa `wayland-protocols` de Rust (`0.32`, con `staging` y `unstable`): al subir de versión, revisar
`paper-vk/src/wayland/`.

## Steam y el Workshop

**Archivos en disco** (`services/workshop.cpp`):

- `steamapps/libraryfolders.vdf`: líneas `"path"  "<ruta>"` → bibliotecas.
- `steamapps/workshop/content/431960/<id>/project.json` → elementos.
- `userdata/<cuenta>/ugc/431960_subscriptions.vdf`: bloques con `"publishedfileid" "<id>"` y `"disabled_locally"`.
  **De esto depende la limpieza**: si Steam cambia el formato y el análisis devuelve una lista vacía, no se borra
  nada (salvaguarda); si devolviera una lista incompleta, se borrarían descargas válidas. Ante la duda, desactivar
  `wallpaper_engine_clean` y revisar `subscriptions()`.
- Raíces probadas: `~/.local/share/Steam`, `~/.steam/steam`, `~/.var/app/com.valvesoftware.Steam/data/Steam`
  (Flatpak). Una instalación nueva (Snap, otra ruta) se añade en `steam_libraries()`.

**Steamworks** (`engine/crates/island-steam`): crate `steamworks = "0.13.1"`. Al subirlo: el SDK y
`libsteam_api.so` cambian juntos (`copy-steam-lib.cmake` copia el que construye `steamworks-sys`); revisar la API de
`ugc()` que usa `main.rs` (`query_all`, `query_item`, `subscribe_item`, `unsubscribe_item`, `download_item`,
`subscribed_items`, `item_state`, `item_download_info`, `item_install_info`). El helper se identifica como la app
431960 con `Client::init_app(APP_ID)`, sin `steam_appid.txt`.

**Contenido del Workshop**: el tipo (`scene`/`video`/`web`/`application`) sale de las etiquetas del elemento
(`kind_of`), y «para adultos» de las etiquetas de clasificación (`mature`). Si Valve cambia esas etiquetas, ajustar
esas dos funciones.

## Wallpaper Engine

- **`project.json`**: `type`, `title`, `preview`, `file`, `general.properties`. Lo leen `workshop::read_item`
  (isla) y `paper-control/src/we_project.rs` (motor). Propiedades editables: `workshop::properties` (tipos `bool`,
  `slider`, `combo`, `color`; `condition` simple). Si aparecen tipos nuevos que merezca la pena editar, añadirlos
  ahí y en `draw_properties` (`suite/picker_steam.cpp`) y `engine_overrides`.
- **Escenas** (`scene.pkg`, `scene.json`, materiales, shaders): `engine/crates/paper-scene`. Los recursos comunes
  están en `steamapps/common/wallpaper_engine/assets` (o `SKWD_WE_ASSETS`).
- **Diagnóstico de una escena que falla**: `engine-vk.log` (errores de carga, de shader o de script); compararla
  con su vista previa; `capture-scenes` con `SKWD_DUMP_SHARED_TARGETS` para ver los pases; el corpus de tests de
  `paper-scene` (`corpus_tests.rs`).
- **API de SceneScript**: `paper-scene/src/script/bootstrap.js` implementa lo que ven los scripts. Una función que
  falte se nota en el registro como `TypeError: … is not a function`. Para añadir algo: implementarlo en JS en
  `bootstrap.js` y, si necesita al motor, una función nativa nueva en `runtime.rs` (`js.set_function("__nombre",
  …)`) con su efecto en el modelo de escena.

## V8 (`v8` crate)

`paper-scene` usa `v8 = "152.2.0"`. La API de rusty_v8 cambia a menudo entre versiones (ámbitos, `HandleScope`,
`Isolate::new`, `ArrayBuffer` y su asignador, callbacks de funciones). Todo el contacto con V8 está en
`paper-scene/src/script/js.rs`, a propósito: al subir de versión, solo ese archivo debería cambiar.

- La primera compilación descarga `librusty_v8` precompilada; en entornos sin red, `RUSTY_V8_ARCHIVE=<ruta>`.
- Tras actualizar: `cargo test --release -p paper-scene` (incluye tests del *watchdog*, presupuestos, módulos,
  getters de capas) y probar Koi Pond.

## ffmpeg, Vulkan y audio

- `ffmpeg-the-third = "6"` sigue a ffmpeg 6/7. Con una versión mayor nueva de ffmpeg puede hacer falta subir el
  crate; tocará `paper-vk/src/decode*`, `ffmpeg_vulkan*` y `paper-audio/src/decoder.rs`.
- `ash = "0.38"`: al subir, revisar `paper-vk/src/shared.rs` (creación del dispositivo, extensiones, prioridad de
  colas) y `vk/`.
- `cpal = "0.16"` en `paper-audio`.

## APIs web

| Servicio | Llamada | Notas |
|---|---|---|
| wallhaven | `GET /api/v1/search?categories=110&purity=…&sorting=…&order=desc&topRange=1M&page=N…` | de la respuesta se usan `data[].{id, path, thumbs.large, resolution, purity, …}` y `meta.{last_page, seed}`; límite de peticiones por minuto |
| open-meteo | `GET /v1/forecast?latitude&longitude&current=…&hourly=…&daily=…&timezone=auto&forecast_days=6` | sin clave |
| GitHub | `GET /repos/waywallen/open-wallpaper-engine/releases/latest` | asset cuyo nombre contiene `linux-x86_64` y acaba en `.zip`; dentro, `lib/weweb` |
| Steam Community | `https://steamcommunity.com/sharedfiles/filedetails/?id=<id>` | solo para abrir en el navegador |

## Fondos web: waywallen

- **Renderizador**: `waywallen-weweb-renderer` dentro de la release de open-wallpaper-engine. Si cambia el nombre
  del asset o la carpeta (`lib/weweb`), actualizar `install_renderer` y `WebWall::renderer()`.
- **Protocolo**: `third_party/waywallen-bridge` (MIT) es una copia de la biblioteca de waywallen:
  `WW_PROTOCOL_VERSION 3`. Si el renderizador sube de versión de protocolo, hay que **actualizar la copia completa**
  desde el repositorio de waywallen (su biblioteca `waywallen-bridge`: `include/`, `src/`, `waywallen-bridge.map`) y
  adaptar `webwall.cpp` a los mensajes nuevos. Ojo: el renderizador **carga** `libwaywallen-bridge.so` (la que
  instalamos en `~/.local/lib/niri-island`), así que la biblioteca y el renderizador deben ser de la misma versión.

## skwd-paper

`engine/` parte de skwd-paper (MIT, de liixini). Copia de referencia: `~/programas/ref/skwd-paper`
(`https://github.com/liixini/skwd-paper`; estaba en `d7cbbbf` cuando se copió). Para traer mejoras:

1. `git -C ~/programas/ref/skwd-paper log --oneline d7cbbbf..origin/main` para ver qué cambió.
2. Aplicar los cambios a mano o con `git format-patch` + `git am --directory=engine` (las rutas de crates coinciden:
   `crates/paper-*`). Los conflictos aparecerán en lo que niri-island cambió (lista en
   [07](07-motor.md#diferencias-con-skwd-paper)): nombres de binarios y rutas, `script/js.rs` (V8 en vez de
   QuickJS), mallas, targets compartidos, entrada, propiedades, policy de la CLI, prioridad de GPU, composiciones.
3. Ojo con `rsync --exclude target`: también excluye `paper-vk/src/wayland/target/` (código). Ya pasó una vez.
4. `cargo test --release --workspace` y probar varias escenas (ligera, pesada, con script, con audio, vídeo).

## Herramientas del sistema

- **matugen**: `matugen image <img> -c <conf> --json hex -m dark -t scheme-tonal-spot --source-color-index 0
  --dry-run`; se lee `colors.<rol>` del JSON. Con una versión que cambie el formato, la isla cae al color dominante.
- **nmcli**: siempre `-t -f CAMPOS` (salida estable separada por `:`).
- **wf-recorder**: `-c h264_vaapi -d /dev/dri/renderD128 -p qp=…` o `-c libx264 -p preset=veryfast -p crf=…`.
- **cliphist**: `list`, `decode`, `delete`, `wipe` por stdin.
- **grim**: `-t ppm` para la captura congelada (rápida, sin comprimir).

## Toolchains

- C++20 con GCC o Clang recientes; `-Wall -Wextra` sin avisos.
- Rust: `engine/rust-toolchain.toml`. Al subirla, `cargo clippy --workspace --release` (el workspace niega
  `clippy::pedantic`: los lints nuevos pueden romper la compilación de clippy, no la normal).

## Lista de comprobación tras actualizar algo

```bash
cmake --build build -j$(nproc) && build/niri-island-tests                # 17/17 o más
(cd engine && cargo test --release -p skwd-wall-vk -p paper-scene)       # salvo los fallos conocidos (01)
cmake --install build --prefix ~/.local
~/.local/bin/niri-island-engine stop; kill -TERM $(pgrep -x niri-island); setsid niri-island &
tests/smoke.sh
```

Y a mano:

- [ ] La isla arranca, la barra muestra espacios de trabajo (IPC de niri).
- [ ] Panel de sistema: CPU, GPU, memoria con datos.
- [ ] Fondo imagen → vídeo → escena → web → imagen: sin parpadeo de escritorio, la paleta cambia.
- [ ] En la vista general de niri el fondo se ve **una** vez.
- [ ] Una ventana maximizada pausa el fondo animado (`tools/gpu-procs.py` debe mostrar el motor a ~0).
- [ ] Koi Pond responde al ratón.
- [ ] Selector: carpetas, wallhaven (buscar, probar, añadir), Steam (descargas con miniaturas, panel con
      propiedades, Workshop: buscar, suscribir con progreso, desuscribir).
- [ ] Bloqueo con contraseña y `lock --preview 5`.
- [ ] Captura (Mod+S) y una grabación corta con audio.
