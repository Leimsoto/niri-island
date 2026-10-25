# 07 · El motor (`engine/`)

**niri-island-engine** dibuja los fondos animados: vídeos y escenas de Wallpaper Engine. Es
[skwd-paper](https://github.com/liixini/skwd-paper) de liixini (MIT; ver `engine/LICENSE` y `engine/LICENSES/`)
adaptado a niri-island, más el puente de Steam (`island-steam`). Está en Rust (edición 2024, toolchain fijada en
`engine/rust-toolchain.toml`).

## Crates

| Crate (paquete) | Binario | Qué hace |
|---|---|---|
| `paper-cli` (`skwd-paper`) | `niri-island-engine` | CLI, cliente y **servidor** (gestor de renderizadores, tokio) |
| `paper-vk` (`skwd-wall-vk`) | `niri-island-engine-vk` | **renderizador Vulkan**: vídeo (ffmpeg + Vulkan Video/VA-API) y escenas |
| `paper-still` (`skwd-wall-still`) | `niri-island-engine-still` | renderizador de imágenes fijas en memoria compartida (la isla no lo usa: dibuja las imágenes ella misma) |
| `paper-scene` | — | formato de Wallpaper Engine: `scene.pkg`, modelo de escena, efectos, partículas, texto, marionetas, sonido, shaders, **scripts (V8)** |
| `paper-control` | — | contratos: protocolo del socket, `PaperCommand`, `project.json`, capturas, señales de *readiness* |
| `paper-audio` | — | audio (cpal + ffmpeg): reproducción, mezclador, espectro para los scripts |
| `paper-geom` | — | geometría de los modos de ajuste (fill, fit, stretch, center, tile, span) |
| `paper-runtime` | — | procesos, *watchdog*, seccomp, memoria compartida, *wake fds*, espejo |
| `paper-log` | — | registros a archivo y stderr (tracing) |
| `paper-shaders` | — | GLSL de las transiciones «sand» |
| `island-steam` | `niri-island-steam` | puente con el cliente de Steam (Steamworks) |

## Procesos y canales

```
niri-island ──(exec)──► niri-island-engine <cmd> …           (cliente: 1 petición, 1 respuesta, sale)
                              │ NDJSON por socket Unix
                              ▼
               niri-island-engine serve                        (servidor; se lanza solo si no está)
               $XDG_RUNTIME_DIR/niri-island-engine/engine.sock
                              │ spawn + stdin (NDJSON PaperCommand) + socket de readiness
                              ▼
               niri-island-engine-vk <salidas> <fuente> …      (un renderizador por composición)
                              │ Wayland (layer-shell, dmabuf, viewporter, presentation-time…)
                              ▼
                             niri
```

- **Cliente → servidor**: una línea JSON (`Request {id, method, params}`) y una línea de respuesta
  (`Response`). Si no hay servidor, el cliente lo arranca (`serve`) y espera al socket.
- **Servidor → renderizador**: lo lanza con argumentos (fuente, tamaños, fps…) y variables de entorno controladas
  (borra las `SKWD_*` heredadas y pone las de la *policy*), le habla por **stdin** con líneas `PaperCommand` (swap,
  pause, audio, pointer, capture, freeze, surface, mirror, properties…) y espera su señal de **listo** por el socket
  que indica `SKWD_PAPER_READY_SOCKET` (con número de generación).
- **Transacciones**: `begin_apply` valida, expande la petición en asignaciones por salida, lanza los renderizadores
  nuevos (`stage`) y solo cuando **todos** han presentado su primer fotograma hace `commit` y retira los viejos; si
  alguno falla, *rollback* y los anteriores siguen. Por eso un fondo roto nunca deja la pantalla vacía.
- **Registros**: `~/.cache/niri-island/engine.log` (servidor) y `~/.cache/niri-island/engine-vk.log`
  (renderizadores).

## CLI

```
niri-island-engine apply <salida|*> <ruta> [--kind static|video|we] [--engine …] [--frame-rate …]
                   [--fill-mode fill|fit|stretch|center|tile|span] [--mute true|false] [--volume N]
                   [--layer background|bottom] [--transition] [--effect …] [--duration-ms N]
                   [--idle-seconds N] [--properties JSON] [--scene-fps N] [--scene-max PX]
                   [--replace-all]
niri-island-engine apply --manifest <JSON|@archivo|->      # varias asignaciones + policy de una vez
niri-island-engine stop [salidas]
niri-island-engine pause | resume
niri-island-engine audio [salidas] [--mute true|false] [--volume N]
niri-island-engine properties <JSON>                        # valores nuevos para las escenas en marcha
niri-island-engine status | outputs | capabilities [--reset-cache]
niri-island-engine capture-scenes                           # miniaturas sin pantalla, peticiones NDJSON por stdin
niri-island-engine serve                                    # el servidor (normalmente lo lanza el cliente)
```

Lo que usa la isla (`src/services/engine.cpp`):

| Llamada | Comando |
|---|---|
| `engine::apply(ruta, Engine/Video, silencio, err, props)` | `apply * <ruta> --kind we\|video --mute … --replace-all --scene-fps N [--scene-max PX] [--properties JSON]` |
| `engine::set_properties(json)` | `properties <JSON>` |
| `engine::set_paused(b)` | `pause` / `resume` |
| `engine::set_muted(b)` | `audio --mute true\|false` |
| `engine::stop()` | `stop` |
| `engine::capture(fuente, png)` | `capture-scenes` con `{"source": …, "destination": …}` por stdin |

### `--scene-fps` y `--scene-max` (policy)

Son la `ScenePolicy` de la petición (`policy.scene.fps`, `policy.scene.max_dimension`). **No sirve exportar
`SKWD_PAPER_WE_FPS` o `SKWD_VK_SCENE_MAX` al cliente**: el servidor limpia esas variables antes de lanzar el
renderizador y solo pone las que trae la policy (`apply_policy` en `paper-cli/src/backend.rs`). Cambiar la policy
exige `--replace-all` (la isla lo pasa siempre).

| Ajuste de la isla | Policy |
|---|---|
| Fotogramas 30 / 60 | `--scene-fps 30` / `60` (por defecto del motor: 30) |
| Calidad Completa / Equilibrada / Ligera | sin límite / `--scene-max 1600` / `--scene-max 1280` |

### Protocolo del socket

`paper-control/src/protocol.rs`. Métodos (`"method"`):

| Método | Params | Respuesta |
|---|---|---|
| `paper.apply` | `ApplyRequest {assignments[], replace_all, policy?}` | estado de las asignaciones |
| `paper.stop` | `{outputs[]}` | cuántas paró |
| `paper.pause` | `{paused}` | |
| `paper.audio.set` | `{outputs[], mute?, volume?}` | |
| `paper.properties.set` | `{properties: {nombre: valor}}` | (añadido por niri-island) |
| `paper.surface.set` | `SurfacePolicy` | |
| `paper.mirror` | destino de espejo | |
| `paper.status` / `paper.capabilities` | | |
| `paper.ready` / `paper.failed` | (de los renderizadores al servidor) | |

Una `Assignment` lleva `outputs`, `source {kind: static|video|we, path, engine?, frame_rate?, properties?}`,
`fill_mode`, `mute`, `volume`, `layer`, `transition?`, `background?`.

## El renderizador (`paper-vk`)

### Dispositivo y presentación

- Elige la GPU (`SKWD_VK_DEVICE` para forzar), crea un dispositivo Vulkan con las extensiones que necesita
  (vídeo, memoria y semáforos externos, DRM, `push_descriptor`, `timeline_semaphore`, `synchronization2`…) y, si el
  driver ofrece `VK_KHR/EXT_global_priority`, **pide prioridad baja para todas sus colas**: en una GPU compartida
  niri se atiende primero (`SKWD_PAPER_GPU_PRIORITY=normal` lo desactiva). En el registro:
  `GPU queue priority low_priority=true`.
- Dibuja en imágenes propias y las **exporta como dma-buf** al compositor (`zwp_linux_dmabuf_v1`), sin copias por
  CPU («scene path = Vulkan DMA-BUF»). Si no se puede, cae a memoria compartida con lectura por CPU.
- Ritmo: tope de fps de la policy, con *presentation feedback* de Wayland; pausa total cuando el servidor lo pide
  (`pause`), y por inactividad si la policy trae `idle_seconds`.

### Superficies

`paper-vk/src/wayland/`: una superficie layer-shell por salida en la capa background con namespace
`niri-island-engine` (`SKWD_PAPER_NAMESPACE`). Como niri no da entrada a las superficies con
`place-within-backdrop` (ver [08](08-niri.md)), el renderizador crea además, por salida, una **superficie de
entrada** `niri-island-engine-input`: la misma capa, un búfer `wl_shm` de 1×1 transparente escalado a la pantalla
con `wp_viewporter`. El puntero que entra en ella cuenta como si entrara en la visual
(`INPUT_SURFACE` en `wayland/model.rs`, `configure_input` en `wayland/protocol.rs`, `pointer.rs`).
Con `SKWD_VK_INPUT=passthrough` no se crean (sin interacción).

### Escenas: de `scene.pkg` a píxeles

1. **Lectura** (`paper-scene`): `pkg.rs` abre `scene.pkg` (o la carpeta suelta), `scene.rs`/`model.rs` construyen
   el `SceneModel`: cámara y lienzo (`canvas`), capas (imágenes, composiciones, texto, partículas, modelos de
   marioneta), efectos con sus pases y materiales, animaciones por *keyframes*, sonido, propiedades del usuario
   (`project.json → general.properties` + las de la petición).
2. **Recursos de Wallpaper Engine**: muchos materiales y shaders viven en la instalación de Wallpaper Engine
   (`steamapps/common/wallpaper_engine/assets`); se buscan en las bibliotecas de Steam o en `SKWD_WE_ASSETS`.
   **Sin Wallpaper Engine instalado, muchas escenas no cargan.**
3. **Shaders** — <a id="shaders"></a>los de Wallpaper Engine son un dialecto propio. `shader.rs::translate` los
   traduce a GLSL Vulkan y `shaderc` los compila a SPIR-V. Los que vienen en HLSL se compilan con **dxc**
   (`hlsl.rs`, vía `hassle-rs`) si `libdxcompiler.so` está disponible; `SKWD_PAPER_SHADER_BACKEND=glsl` fuerza la
   ruta GLSL. El SPIR-V se cachea en disco (`~/.cache/skwd-wall/scene-shaders/`, nombre heredado de skwd-paper) por hash del fuente: la primera
   carga de una escena es más lenta que las siguientes.
4. **Plan de targets** (`app/scene.rs`): cada capa con efectos tiene una cadena de pases en targets intermedios. Se
   reservan con un presupuesto (`effect_target_budget`) y se **reutilizan** entre capas que no coinciden en el
   tiempo (*aliasing*). Los targets con nombre `_rt_*` que no son únicos se **comparten** entre capas (como hace
   Wallpaper Engine) y quedan fuera del aliasing.
5. **Resolución interna** (`scene_dimensions_for_outputs`): el lienzo lógico (p. ej. 3840×2160) se rasteriza a lo que
   cubre la pantalla y, como máximo, a `scene-max`. Las **composiciones** se renderizan a su tamaño de autor
   **escalado igual que la escena** (`raster_scale`): una composición de 1920 px en un lienzo 4K mostrado a 1080p se
   dibuja a 960 px, que es lo que ocupa en pantalla. Antes se dibujaban a su tamaño de autor (hasta 2048) y eso
   duplicaba el coste de escenas como «Sleeping District».
6. **Por fotograma** (`Group::compose`): scripts → animaciones → partículas (simulación en CPU) → textos dinámicos
   (relojes) → grabación de los pases de cada capa → modelos 3D de los scripts → composición final → presentación.

### Scripts (SceneScript en V8)

`paper-scene/src/script/`:

- **`js.rs`**: un *isolate* de V8 (crate `v8 = "152.2.0"`) y un contexto por escena. Cada llamada corre con un
  plazo: un hilo *watchdog* termina la ejecución si se pasa (solo mientras hay una llamada en curso) y la llamada
  devuelve «interrupted». Límite de *heap* de 128 MB y un asignador de `ArrayBuffer` con presupuesto de 128 MB.
- **`runtime.rs`**: presupuestos `FRAME_BUDGET` 100 ms por fotograma, `MODULE_INIT_BUDGET` 4 s por módulo,
  `INIT_BUDGET` 8 s en total. Funciones nativas expuestas al JS: `__log`, `__meshShape`, `__meshVertices`,
  `__meshLayer`, `__meshLayerVisible`, `__meshLayerSort`, `__registerAsset`. Módulos integrados que los scripts
  importan: `WEColor`, `WEMath`, `WEVector`.
- **`bootstrap.js`**: la API que ven los scripts: `thisScene` (`getLayer`, `enumerateLayers`, `createLayer`,
  `destroyLayer`, `sortLayer`, `getLayerIndex`, `createModelData`…), `thisLayer`, `engine` (`canvasSize`,
  `runtime`, `registerAsset`, temporizadores), `input` (`cursorWorldPosition`, `cursorScreenPosition`,
  `cursorLeftDown`…), `shared`, `localStorage` (`storage.js`, persistido), audio, vídeo y propiedades del usuario
  (`applyUserProperties`).
- **Propiedades**: `properties.rs` mezcla las de `project.json` con las de la petición. El motor maneja **números**:
  un combo cuyas opciones son palabras recibe el índice y el script recibe la palabra (`options[índice].value`).
  `update_properties` las aplica en caliente cuando el script define `applyUserProperties`; si no, la escena se
  recarga.

QuickJS (el motor de skwd-paper) era unas 20 veces más lento: un script que simula y dibuja toda la escena cada
fotograma (Koi Pond: ~130 ms) pasa a ~8 ms en V8.

### Modelos 3D desde scripts (`createModelData`)

- `paper-scene/src/script/meshes.rs` (`MeshStore`): modelos con varias formas (material, formato de vértice
  `position/normal/tangent/uv/color`, vértices `f32`, índices `u32`) y capas que los muestran (visible, «delante de
  la capa N», orden de inserción). Límite: 64 Mi floats en total.
- `paper-vk/src/app/scene/meshes.rs`: cada fotograma sube los búferes que cambiaron (`version`) y convierte cada capa
  visible en un lote de dibujo, insertado justo antes de la capa de escena delante de la que se ordenó.
- `paper-vk/src/vk/mesh.rs`: el pase de color de la escena termina, un pase con **profundidad** (`D32_SFLOAT`,
  invertida: gana el mayor, borrado a 0, `GREATER_OR_EQUAL`) dibuja los modelos y la escena sigue. Viewport
  invertido (los shaders de modelos colocan los vértices al estilo Direct3D, y arriba), uniformes con `d3d_clip`.
  Pipeline `PipelineKind::Mesh` con un búfer cero en el binding 1 para atributos que el formato no trae.
- Un material cuya primera textura se llama como un target (`_rt_FullFrameBuffer`, `_rt_…`) lee ese target.

### Rendimiento: perillas

| Variable / opción | Efecto |
|---|---|
| `--scene-fps` (policy) | tope de fps de escenas animadas (defecto 30) |
| `--scene-max` (policy) | lado máximo de la resolución interna |
| `SKWD_PAPER_GPU_PRIORITY=normal` | no pedir prioridad baja de GPU |
| `SKWD_VK_SCENE_STATS=1` | cada segundo: `scene stats fps=… compose_avg=…ms compose_max=…ms present_avg=…ms` |
| `SKWD_VK_PROFILE=1` | tiempo de GPU por parte del fotograma y de CPU por fase (ver abajo) |
| `SKWD_VK_OVERLAP=0` | no solapar CPU y GPU |
| `SKWD_VK_SCENE_BENCH=N` | renderiza N fotogramas sin pausa, registra ms/fotograma (y GPU con `SKWD_VK_SCENE_GPU_TIMESTAMPS`) y sale |
| `SKWD_VK_SCENE_GPU_TIMESTAMPS` | marcas de tiempo de GPU por pase |
| `SKWD_DUMP_SHARED_TARGETS=dir` | con `capture-scenes`: guarda cada target compartido como PNG |
| `SKWD_MESH_DEBUG` | trazas del pase de modelos |
| `SKWD_VK_FX_DEBUG`, `SKWD_VK_SCENE_DUMP` | depuración de efectos y volcado de la escena |

Las variables de depuración llegan al renderizador solo si están en el entorno **del servidor**; como el servidor
lo suele lanzar la isla, para usarlas: `pkill -x niri-island-eng; SKWD_VK_SCENE_STATS=1 niri-island-engine serve &`
y luego aplicar el fondo. Ver [10 · Depuración](10-depuracion.md#medir-una-escena).

Medidas en Intel UHD (iGPU, 1080p):

| Escena | Antes | Ahora (completa) | Ligera (1280) |
|---|---|---|---|
| Sleeping District (4K, 11 cadenas) | 68 ms → 14 fps, GPU al 100 %, niri al 6 % | 34 ms | 18 ms (~48 fps) |
| Niwama – Koi Pond (script 3D) | 49 ms | 47 ms | 40 ms |

(Tabla anterior al solapamiento CPU/GPU; cifras actuales más abajo.)

### CPU y GPU solapadas

`Group::compose` envía el fotograma a la GPU (`submit_scene_batch_async`) y, **mientras la GPU dibuja**, adelanta
el trabajo de CPU del siguiente: la llamada a V8 de los scripts (`prefetch_scripts` → `script_tick`) y la
simulación y empaquetado de partículas (`prepare_particles`). Solo después espera (`wait_scene_batch`). Lo que esas
fases piden y toca la GPU (texturas de texto, subir modelos, copiar sprites al búfer mapeado, comandos de sprites y
sonidos) se aplica en el siguiente `compose`, **después** de la espera, así que nunca se escribe algo que la GPU
esté leyendo. Coste: los scripts ven el tiempo del fotograma siguiente previsto (`time + dt`) y la entrada del
ratón llega un fotograma más tarde. `SKWD_VK_OVERLAP=0` vuelve al orden en serie.

Lo que no se solapa: la grabación de comandos (un solo command buffer y un UBO por pipeline, reutilizados cada
fotograma) y la presentación. Tener dos fotogramas completos en vuelo exigiría duplicar command buffers, UBOs,
descriptores y búferes dinámicos; con las escenas medidas el límite ya es la GPU, así que no compensa por ahora.

Además, `uniform_bytes_with` (`paper-scene/src/effects.rs`) resuelve solo los uniformes que declara el shader, en
vez de clonar todas las constantes e insertar ~40 valores por pase y fotograma (prioridad: audio > overrides >
valores del fotograma > constantes del pase > valores por defecto; test
`uniforms_resolve_with_the_documented_precedence`).

Resultado (iGPU Intel UHD, 1080p, tope 60):

| Escena | Antes de la sesión | Ahora |
|---|---|---|
| Sleeping District | 14 fps, 68 ms | **30 fps**, 30,5 ms (GPU ~24 ms) |
| Niwama – Koi Pond | 20 fps, 49 ms | **24,5 fps**, 38 ms (GPU ~31 ms) |

Lo que queda es coste **del contenido**: en Koi Pond un único material (`materials/draw_11_0.json`, pixel art a
480×270) se lleva 9 ms porque su shader lee decenas de valores de una textura de datos por cada píxel y hace ray
marching; en Sleeping District, dos capas de ruido procedural de 8-10 octavas cuestan ~6 ms cada una a 960×540.

### Perfilador (`SKWD_VK_PROFILE=1`)

Con la variable en el entorno del servidor, el renderizador escribe marcas de tiempo de GPU entre las partes del
fotograma (cada cadena de efectos, cada snapshot, cada modelo de script por material, el pase final) y cronometra
las fases de CPU de `compose`. Cada 120 fotogramas registra los promedios, de mayor a menor (`vk/profile.rs`,
`CpuProfile` en `app/scene.rs`):

```
profile 31.37 ms/frame over 120 frames
profile    9.07 ms    model materials/draw_11_0.json (27000 triangles)
profile    4.49 ms  fx 89: 4 passes at 1920x1080
profile    3.63 ms  scene layers at 1920x1080
cpu    8.16 ms  next frame's scripts (overlapped)
cpu   24.90 ms  wait for the GPU
```

Los números de `fx N` son ids de objeto de `scene.json`: para ver qué efectos son, extraer el `scene.pkg` (formato
`PKGV`: nombre, número de entradas, y por entrada nombre/offset/tamaño) y buscar ese id.

## Captura sin pantalla (`capture-scenes`)

`niri-island-engine capture-scenes` lee por stdin líneas `{"source": "<carpeta o vídeo>", "destination": "<png>"}`
(y opcionalmente propiedades), renderiza un fotograma sin superficie (como máximo 1280×720) y escribe el PNG. Lo usa
el selector para las miniaturas nítidas de los elementos del Workshop.

## `niri-island-steam`

`engine/crates/island-steam/src/main.rs`. Se inicializa con Steamworks como Wallpaper Engine (`APP_ID = 431960`):
necesita **Steam abierto** y la cuenta con Wallpaper Engine. Protocolo por líneas JSON:

```
→ {"op":"search","query":"…","sort":0..4,"page":1,"mature":false,"tag":"…"}
→ {"op":"subscribe","id":"123"}        suscribe, Steam descarga, informa del progreso
→ {"op":"unsubscribe","id":"123"}      desuscribe y borra la carpeta
→ {"op":"state"}                        ids suscritos
← {"ev":"ready","account":"…"}
← {"ev":"results","tag":"…","items":[{id,title,preview,size,subs,score,type,mature,subscribed,installed}]}
← {"ev":"progress","id":"…","progress":N,"total":N}
← {"ev":"done","id":"…","folder":"…"}
← {"ev":"unsubscribed","id":"…"} · {"ev":"state","subscribed":[…]} · {"ev":"error","id":"…","message":"…"}
```

`sort`: 1 más nuevos, 2 votos, 3 suscriptores; cualquier otro valor, tendencia. `type` sale de las etiquetas
(`scene`, `video`, `web`, `application`) y solo se devuelven elementos de Wallpaper Engine. Sale cuando se cierra su stdin (el selector se fue).
Registro en `~/.cache/niri-island/steam.log`. `libsteam_api.so` va junto al binario (rpath).

## Diferencias con skwd-paper

- Binarios, socket (`$XDG_RUNTIME_DIR/niri-island-engine/engine.sock`) y registros renombrados; sin el
  trabajador AV1 privado.
- SceneScript en **V8** en vez de QuickJS.
- `createModelData`/`applyData`/`createLayer({model})`/`sortLayer`/`getLayerIndex`, `engine.registerAsset`;
  modelos dibujados con profundidad entre capas.
- Targets `_rt_*` no únicos compartidos entre capas; la primera textura de un material con nombre de target lo lee.
- Superficies de entrada transparentes (interacción con niri y `place-within-backdrop`).
- Comando `properties` / método `paper.properties.set`; combos de palabras por índice.
- `--scene-fps` y `--scene-max` en `apply`.
- Prioridad de GPU baja; composiciones escaladas con la escena; `SKWD_VK_SCENE_STATS`.

Para traer cambios de skwd-paper, ver [11 · Mantenimiento](11-mantenimiento.md#skwd-paper).
