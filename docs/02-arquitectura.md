# 02 · Arquitectura

## Procesos

```
                         ┌──────────────────────────── niri (compositor) ───────────────────────────┐
                         │  Wayland (layer-shell, session-lock, dmabuf…)       IPC $NIRI_SOCKET      │
                         └───────▲───────────────────────▲────────────────────────▲─────────────────┘
                                 │ superficies           │ superficies (Vulkan)   │ EventStream + acciones
┌────────────────────────────────┴───────────┐   ┌───────┴──────────────────┐     │
│ niri-island  (C++, 1 hilo de estado        │   │ niri-island-engine-vk    │     │
│              + 2-4 workers)                ├──►│ (un renderizador por     │     │
│  isla · barras · paneles · overlays ·      │CLI│  composición: vídeo o    │     │
│  fondo (imágenes) · bloqueo · protector    │   │  escena, V8 dentro)      │     │
│                                            │   └───────▲──────────────────┘     │
│  IPC: $XDG_RUNTIME_DIR/niri-island.sock ◄──┼─ «niri-island <comando>»           │
│                                            │   ┌───────┴──────────────────┐     │
│                                            ├──►│ niri-island-engine serve │ socket JSON
│                                            │   │ (gestor de renderizadores)│ $XDG_RUNTIME_DIR/
│                                            │   └──────────────────────────┘ niri-island-engine/
│                                            │   ┌──────────────────────────┐ engine.sock
│                                            ├──►│ niri-island-steam        │ JSON por stdin/stdout
│                                            │   │ (Steamworks ↔ Steam)     │ (solo con el Workshop abierto)
│                                            │   └──────────────────────────┘
│                                            │   ┌──────────────────────────┐
│                                            ├──►│ waywallen-weweb-renderer │ socket waywallen_ipc_v3,
│                                            │   │ (CEF: fondos web)        │ fotogramas dma-buf
│                                            │   └──────────────────────────┘
│  D-Bus de sesión y de sistema, sysfs, udev, inotify, programas auxiliares (nmcli, cava, grim…)
└────────────────────────────────────────────┘
```

- **`niri-island`** sin argumentos es el demonio; con argumentos es un **cliente** que manda la línea al socket
  IPC del demonio y escribe la respuesta (`src/main.cpp`). Si ya hay un demonio, un segundo arranque sale con
  «already running».
- **El motor** se lanza bajo demanda: la isla ejecuta `niri-island-engine apply …`; ese proceso cliente arranca el
  servidor (`niri-island-engine serve`) si no existe, le pasa la petición por el socket y sale. El servidor lanza un
  renderizador `niri-island-engine-vk` por composición y lo supervisa. `niri-island-engine stop` lo apaga todo.
- **niri-island-steam** lo arranca el selector cuando entra en el Workshop y muere al cerrar su stdin.
- **El renderizador web** lo arranca `WebWall` con su propio socket de escucha y `LD_LIBRARY_PATH` apuntando a
  `~/.local/lib/niri-island` (donde está `libwaywallen-bridge.so`).

## Hilos dentro de niri-island

- **Hilo principal**: todo el estado. Un bucle `poll` (`core/loop.cpp`) sobre: el socket de Wayland, los dos buses
  D-Bus (sd-bus), udev (batería, red), inotify (config, paleta), el socket de niri, el socket IPC, los pidfd de los
  hijos supervisados, el eventfd de despertar y los temporizadores.
- **Workers** (entre 2 y 4 según núcleos): `Loop::async(trabajo, hecho)` ejecuta `trabajo` en el pool y `hecho` de
  vuelta en el hilo principal. Ahí va lo bloqueante: `proc::run` (nmcli, matugen, el CLI del motor), descargas
  (libcurl), decodificar y escalar imágenes (gdk-pixbuf/Cairo), recortes y brillos del selector, PAM.
- **Hilo de libpulse**: lo crea la propia libpulse (`pa_threaded_mainloop`); cada callback copia los datos y los
  publica en el bucle con `Loop::post`.
- **Descargas sueltas** que pueden sobrevivir al bucle usan `Loop::post_if_alive` y consultan
  `Loop::shutting_down()` para abandonar.

Regla: **un worker nunca toca estado de la app**. Calcula, devuelve por captura (`std::shared_ptr` con el
resultado) y el callback `hecho` lo aplica. Para descartar resultados viejos se usa un contador de generación
(`gen_` en `Backdrop`, `wh_gen_`/`ws_gen_` en el selector): si cambió mientras tanto, el resultado se tira.

## El bucle y el fotograma

Cada vuelta de `Loop::run()`:

1. `before_poll()` (`App::before_poll`, `island/app.cpp`):
   - bombea los buses D-Bus y los eventos Wayland pendientes;
   - `pump()` del fondo, el selector, la captura, las barras de las esquinas, ajustes, protectores, burbuja de
     captura y bloqueo: cada uno dibuja **solo si está sucio y el compositor ya liberó su último fotograma**
     (`frame_pending()` es falso);
   - si la isla está sucia y libre, `render()`;
   - calcula cuánto puede dormir el `poll` (0 si alguien quiere fotograma ya; 33 ms si el espectro está vivo;
     si no, hasta el próximo temporizador; o indefinidamente).
2. `poll()` sobre todos los descriptores.
3. Callbacks de los descriptores listos, temporizadores vencidos y tareas publicadas desde otros hilos.

El ritmo lo marcan los **callbacks de fotograma** de Wayland (`wl_surface.frame`): una superficie no vuelve a
dibujar hasta que niri dice que mostró la anterior. Nada dibuja «a 60 fps» por su cuenta; si nada cambia, la isla
no consume CPU.

### Un fotograma de la isla (`App::render`)

1. `Ui::begin_frame(dt)`: empieza un registro nuevo de regiones de clic.
2. `decide()`: qué vista toca (paneles abiertos > notificación > OSD > música/resumen al pasar el ratón > reposo).
3. Si la vista cambió: se congela la saliente en una **instantánea** (`snap_prev_`) y se anima el *morph* (tamaño
   con muelles `w_`/`h_`, fundido cruzado `morph_`). La entrante se dibuja en vivo.
4. Se dibuja el contorno (cápsula o acoplada con «orejas»), la sombra (nine-slice cacheada), el cuerpo de cristal y
   el contenido de la vista. Cada widget registra su región de clic **mientras se dibuja**.
5. `end_frame(daño)`: se adjunta el búfer con el rectángulo de daño; región de entrada = la forma actual; desenfoque
   de niri (ext-background-effect) detrás de las formas si está activado.

El espectro de audio va en una **subsuperficie** propia (`SubSurface`): con música y la isla quieta solo se
redibujan esas barras (30 por segundo), no la isla entera.

## Superficies

Todas son **layer-shell** (wlr-layer-shell), salvo las del bloqueo (ext-session-lock):

| Namespace | Capa | Quién | Notas |
|---|---|---|---|
| `niri-island` | overlay | la isla | 940×640 lógicos; la región de entrada recorta lo visible |
| `niri-island-bar-left/right` | top | barras de las esquinas (distribución «extendida») | |
| `niri-island-wallpaper` | background | el fondo cuando es imagen | una por pantalla |
| `niri-island-engine` | background | renderizador del motor | una por pantalla; **sin entrada** (niri) |
| `niri-island-engine-input` | background | gemelas transparentes del motor | reciben el puntero para las escenas interactivas |
| `niri-island-web` | background | fondos web (dma-buf del CEF) | |
| `niri-island-wallpapers` | overlay | selector de fondos | |
| `niri-island-settings` | overlay | ajustes | |
| `niri-island-capture`, `niri-island-capbar` | overlay | editor y burbuja de captura | |
| `niri-island-screensaver` | overlay | protector, uno por pantalla | |
| `niri-island-shield` | overlay | escudo transparente que detecta clics fuera de un panel | 1×1 escalado con viewporter |
| `niri-island-lock` | — | superficies de ext-session-lock, una por pantalla | |

Las del fondo llevan una regla de niri `place-within-backdrop true` para que en la vista general (overview) se vea
**un** fondo detrás de todos los espacios de trabajo y no una copia dentro de cada uno. Efecto secundario: niri no
envía entrada a esas superficies; por eso el motor crea las gemelas `-input`. Detalles en [08 · niri](08-niri.md).

## Flujos principales

### Aplicar un fondo

```
selector / IPC «wallpaper set» / Ajustes
        │
        ▼
Backdrop::set(ruta)  (suite/backdrop.cpp) ── guarda ~/.local/state/niri-island/wallpaper.json
        │
        ├─ carpeta de Workshop de tipo scene|video, o vídeo suelto con motor instalado
        │     └─ worker: engine::apply(ruta, Engine|Video, silencio, propiedades)
        │            └─ «niri-island-engine apply * ruta --kind we|video --mute … --replace-all
        │                --scene-fps N [--scene-max PX] [--properties JSON]»
        │            hecho → handed_over(Engine): tras 0,3 s se retira el dibujante anterior
        ├─ carpeta de tipo web  → WebWall::start (CEF) → primer fotograma → handed_over(Web)
        ├─ vídeo sin motor      → mpvpaper -l background
        └─ imagen               → worker decodifica a tamaño completo → superficie propia por pantalla
                                   → se ajusta al tamaño de las pantallas (fit_to_screens) → retire(Own)
        │
        └─ make_palette: matugen (o color dominante) → ~/.cache/niri-island/colors.json
               → inotify → Theme::load() → fundido de toda la interfaz a la paleta nueva
```

Solo un dibujante a la vez (`Drawer::Own|Mpv|Engine|Web`). El nuevo se pone **encima** antes de retirar el viejo,
para que nunca se vea el escritorio vacío entre medias.

### Pausar los fondos animados

`niri` informa de cada ventana y su disposición (`WindowsChanged`, `WindowLayoutsChanged`…).
`App::update_wallpaper_pause` llama a `Niri::screen_covered(w, h)` (¿alguna ventana enfocada de un espacio visible
ocupa casi toda la pantalla, completa o maximizada?) y a `Backdrop::set_paused`, que según el dibujante hace
`niri-island-engine pause/resume`, `set_property pause` por el socket IPC de mpv, o el control de pausa del CEF.
En pausa el renderizador no dibuja: 0 % de CPU y GPU. Se desactiva con `wallpaper_pause_covered: false`.

### Un comando IPC

`niri-island cc` → `IpcServer::send` (socket de línea) → `App::command("cc")` en el hilo principal → respuesta de
texto. `lock` es especial: el cliente espera a que `lock-state` diga `locked` para que `swayidle before-sleep` no
suspenda con el escritorio visible.

### Una notificación

Si nadie posee `org.freedesktop.Notifications`, la isla lo toma (modo `server`); si otro demonio lo tiene, la isla
escucha con `org.freedesktop.DBus.Monitoring` (modo `monitor`) y solo las muestra. `Notifications` decide según
No molestar, la isla la muestra como capa `Notification`, y el historial se guarda en `notifications.json`.

## Rendimiento: dónde se va el tiempo

| Situación | Coste medido (Intel UHD, 1080p) |
|---|---|
| Isla en reposo, sin música | ~0 % CPU (no dibuja) |
| Isla en reposo con música | ~2 % de un núcleo (solo la subsuperficie del espectro) |
| Fotograma de la isla / de un overlay | 1-3 ms / 5-13 ms |
| Vídeo 1080p60 en el motor | ~10 % de un núcleo; 0 en pausa |
| Escena ligera (p. ej. «cat eating chips») | ~2-5 % de la GPU |
| Escena pesada («Sleeping District», 4K, 11 cadenas de efectos) | ~34 ms de GPU por fotograma a calidad completa, ~18 ms en «Ligera» |
| Escena con script 3D («Niwama – Koi Pond») | ~49 ms por fotograma (CPU y GPU en serie) → ~20 fps |

Cómo medir cada cosa: [10 · Depuración y rendimiento](10-depuracion.md).
