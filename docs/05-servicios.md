# 05 · Servicios

`src/services/` contiene lo que habla con el sistema. Cada servicio publica su estado en una estructura y avisa
por un `std::function` (`on_change` o similar) **en el hilo principal**; las vistas solo leen ese estado. Lo que
bloquea va a workers.

## Tabla de dependencias externas

| Servicio | Archivo | Habla con | Mecanismo |
|---|---|---|---|
| D-Bus | `dbus.cpp` | bus de sesión y de sistema | sd-bus (libsystemd) integrado en el bucle; `Value` decodifica variantes |
| Multimedia | `mpris.cpp` | `org.mpris.MediaPlayer2.*` | `NameOwnerChanged`, `PropertiesChanged`, `Seeked` (se ignora `playerctld`) |
| Notificaciones | `notifications.cpp` | `org.freedesktop.Notifications` | servidor propio (sd-bus vtable) o monitor (`org.freedesktop.DBus.Monitoring.BecomeMonitor`) |
| Bandeja | `tray.cpp` | `org.kde.StatusNotifierWatcher/Item`, `com.canonical.dbusmenu` | la isla **es** el Watcher y el Host |
| Audio | `audio.cpp` | PulseAudio / pipewire-pulse | libpulse con `pa_threaded_mainloop`, suscripción a sinks, sources, sink-inputs, source-outputs y server |
| Energía | `power.cpp` | sysfs `power_supply` y `backlight`, udev, `org.freedesktop.login1.Session.SetBrightness`, `net.hadess.PowerProfiles` / `org.freedesktop.UPower.PowerProfiles` | udev netlink + lecturas de sysfs; brillo por login1 (sin root) o `brightnessctl` |
| Red | `network.cpp` | NetworkManager | señales D-Bus de NM para enterarse; **`nmcli -t`** para leer y actuar |
| Bluetooth | `bluetooth.cpp` | `org.bluez` | `ObjectManager.GetManagedObjects`, `InterfacesAdded/Removed`, propiedades; métodos asíncronos (emparejar → confiar → conectar) |
| niri | `niri.cpp` | `$NIRI_SOCKET` | ver [08 · niri](08-niri.md) |
| Clima | `weather.cpp` | caché de noctalia, `api.open-meteo.com` | cada 10 min; ver abajo |
| Monitor del sistema | `sysmon.cpp` | `/proc/stat`, `/proc/meminfo`, `/proc/net/dev`, hwmon, `statvfs`, `/sys/class/drm` | muestreo cada segundo mientras algo lo muestra |
| Imágenes | `images.cpp` | gdk-pixbuf, libcurl, tema de iconos | ver abajo |
| Lanzador | `apps.cpp` | archivos `.desktop` | `XDG_DATA_HOME`, `XDG_DATA_DIRS`, Flatpak |
| Espectro | `cava.cpp` | `cava` | salida `raw` ASCII por stdout |
| Grabación | `recorder.cpp` | `wf-recorder`, `ffmpeg`, `pactl` | ver abajo |
| Tareas y notas | `store.cpp` | SQLite `tasks.db` | |
| Temporizador | `timer.cpp` | — | fin como instante de reloj (no deriva) |
| Workshop en disco | `workshop.cpp` | carpetas de Steam | ver [06](06-fondos-y-suite.md) |
| Puente de Steam | `steam.cpp` | `niri-island-steam` | JSON por líneas |
| Motor | `engine.cpp` | `niri-island-engine` | su CLI; ver [07](07-motor.md) |

## Detalles por servicio

### `dbus` — sd-bus en el bucle

`Bus` posee una conexión (`sd_bus_open_user` / `sd_bus_open_system`), registra su fd en el `Loop` y en cada vuelta
procesa mensajes (`pump()` devuelve el plazo que sd-bus pide). `Value` es una variante decodificada (bool, enteros,
doble, cadena, arrays, diccionarios `a{sv}`) suficiente para metadatos MPRIS, *hints* de notificaciones y
propiedades de BlueZ/NetworkManager.

### `mpris`

Busca reproductores al arrancar (`ListNames`) y sigue `NameOwnerChanged`. Elige el activo según quién reproduce y
cuándo reprodujo por última vez. Extrapola la posición (`position()`, µs) a partir de la última `Position` leída y
el instante monótono en que se leyó, para que la barra avance sin preguntar cada fotograma; mientras el usuario
arrastra la barra, la posición mostrada es la suya. La carátula (`mpris:artUrl`) se carga por
`ImageCache` (http(s) se descarga a `~/.cache/niri-island/art/`) y su color medio tiñe el reproductor.

### `notifications`

- `notification_mode: auto` toma el nombre si está libre y, si no, monitoriza; `server` lo reclama
  (`REPLACE_EXISTING`); `monitor` solo escucha; `off` nada.
- Implementa `Notify`, `CloseNotification`, `GetCapabilities`, `GetServerInformation` y emite
  `NotificationClosed`/`ActionInvoked`.
- No molestar: 0 apagado, 1 discreto (sonido + icono que vibra junto a la isla), 2 total. Las críticas pasan siempre.
- Historial (`notification_history` entradas) en `~/.local/state/niri-island/notifications.json`.
- La isla publica las suyas (temporizador, batería, capturas) con `post_local`, que lleva sus propias acciones.

### `tray`

La isla registra `org.kde.StatusNotifierWatcher` y un Host (`org.kde.StatusNotifierHost-<pid>`). Las apps llaman a
`RegisterStatusNotifierItem`; la isla lee sus propiedades (`IconName`, `IconPixmap`, `Title`, `Id`…) y, al pedir el
menú, `com.canonical.dbusmenu.GetLayout` (`AboutToShow` antes). `tray_title()` decide el nombre visible (título, o
el id sin los sufijos que añaden las apps, o el nombre del proceso).

### `audio`

Estado completo al estilo pavucontrol: salidas, entradas, apps que reproducen o graban, volumen y silencio de cada
uno, mover una app a otra salida. `on_change(user)` indica si el cambio merece OSD (volumen o silencio del sink por
defecto tras el arranque). Se ignoran como «micrófono en uso» los monitores propios (cava, pavucontrol, noctalia,
quickshell, la propia isla).

### `power`

Batería desde `/sys/class/power_supply/*` (capacidad, estado, salud = `energy_full/energy_full_design`), releída
por eventos udev y periódicamente (la telemetría deriva sin eventos). Brillo desde `/sys/class/backlight`, escrito
con `org.freedesktop.login1.Session.SetBrightness` (no necesita root); si falla, `brightnessctl`. Perfiles de
energía por D-Bus (`net.hadess.PowerProfiles` o el nombre nuevo `org.freedesktop.UPower.PowerProfiles`).

### `network`

NetworkManager avisa por D-Bus (`PropertiesChanged`, `StateChanged`) y la isla relee con `nmcli -t -f …` en un
worker (con rebote): radio, dispositivos, redes Wi-Fi con señal, conectividad, conexiones guardadas. Conectar,
desconectar, olvidar y la contraseña (para el QR) también van por `nmcli`. Si `nmcli` cambia su salida tabulada, es
aquí donde hay que tocar.

### `bluetooth`

BlueZ por el bus de sistema sin `bluetoothctl` ni hilos: `GetManagedObjects` al arrancar, `InterfacesAdded/Removed`
y `PropertiesChanged` después. Batería de los dispositivos por `org.bluez.Battery1`. Conectar uno nuevo hace
`Pair` → `Trusted=true` → `Connect`.

### `weather`

1. Lee la caché de noctalia (`weather_cache`, por defecto `~/.cache/noctalia/weather.json`) si existe.
2. Si su propia copia (`~/.cache/niri-island/weather.json`) es más reciente, gana: la de noctalia deja de
   actualizarse cuando noctalia no corre.
3. Si no hay ninguna fresca, pide a **open-meteo** (`/v1/forecast` con `current=…`, `hourly=…`, `daily=…`) con
   `latitude`/`longitude` de la config o, si faltan, las de `~/.cache/noctalia/location.json`.

Se repite cada 10 minutos.

### `sysmon`

Cada segundo (solo mientras el panel Sistema o el módulo de rendimiento están visibles): CPU (`/proc/stat`,
suavizado), memoria y swap (`/proc/meminfo`), temperatura (hwmon: `k10temp`, `coretemp`, `zenpower`,
`cpu_thermal`, `acpitz`, por ese orden), disco de `/` (`statvfs`), red (`/proc/net/dev`, sin `lo`, `veth*`,
`docker*`, `br-*`, `virbr*`), carga y tiempo encendido (`sysinfo`), y **GPU**:

- **Intel** (i915 o xe): ocupación = 1 − (tiempo dormido / tiempo transcurrido), con el contador de residencia en
  RC6 (`gt/gt0/rc6_residency_ms`, `power/rc6_residency_ms` o, en xe, `device/tile0/gt0/gtidle/idle_residency_ms`);
  frecuencia actual y máxima de `rps_act_freq_mhz`/`rps_max_freq_mhz` (o `gt_*` / `freq0/*`).
- **AMD** (amdgpu): `device/gpu_busy_percent`.
- **NVIDIA** (driver propietario): no hay nada en sysfs; `gpu = -1` y la tarjeta de gráficos no aparece.

Historiales de 48 muestras para las sparklines.

### `images`

- `load_image_blocking(src, max_px)`: archivo, `file://` o http(s) (descargado a la caché con un único manejador
  curl compartido: DNS, sesiones TLS y conexiones reutilizadas), decodificado con gdk-pixbuf y escalado.
- `ImageCache` (hilo principal): devuelve la superficie si está lista; si no, la carga en un worker y avisa una vez.
  Prefijo `icon:` para resolver por el tema de iconos.
- Tema de iconos: `gtk-icon-theme-name` de `~/.config/gtk-3.0/settings.ini` (o `hicolor`), siguiendo `Inherits=`
  de cada `index.theme`.
- `dominant_color`: color medio con saturación reforzada (carátulas, fondos sin matugen).

### `apps` (lanzador)

Lee los `.desktop` de `XDG_DATA_HOME/applications`, cada `XDG_DATA_DIRS/applications` y las exportaciones de
Flatpak (sistema y usuario); descarta `NoDisplay`/`Hidden`. Búsqueda difusa con puntuación (inicio de palabra,
rachas) más la frecuencia de uso guardada en `~/.local/state/niri-island/launches.json`. También la calculadora
(`=`): `2+3*4`, `sqrt(2)`, `2^10`, `15% of 80`.

### `cava`

Solo corre mientras algo suena **y** la isla muestra el espectro. Se le pasa una config generada (barras, 40 fps,
salida `raw` ASCII a stdout, rango 0-1000). Si no llegan datos reales (silencio), la isla dibuja una animación
sintética en vez de barras planas.

### `recorder`

`wf-recorder` con lo que le falta:

- **Pausa**: cada tramo es un segmento `seg-NN.mkv`; al terminar, `ffmpeg` los concatena.
- **Audio del sistema + micrófono**: un sumidero nulo temporal (`module-null-sink` `niri_island_mix`) con dos
  `module-loopback` (el micrófono y el monitor de la salida), que se descargan al acabar.
- **GPU**: `h264_vaapi` en `/dev/dri/renderD128`; si falla, `libx264` (`veryfast`, CRF según calidad).
- **GIF**: `ffmpeg` a 15 fps, máximo 960 px de ancho, con `palettegen`/`paletteuse` (difuminado bayer).
- Opciones guardadas en `~/.local/state/niri-island/recorder.json`.

### `store`

`~/.local/state/niri-island/tasks.db` (SQLite), compatible con el esquema anterior:

```sql
tasks (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, date_str TEXT NOT NULL, due_label TEXT,
       done INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0, completed_at INTEGER NOT NULL DEFAULT 0,
       notes TEXT DEFAULT '', status INTEGER DEFAULT 0)          -- status: columna del tablero kanban
notes (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, content TEXT DEFAULT '', color INTEGER DEFAULT 0,
       created_at INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0, archived INTEGER DEFAULT 0)
```

Las columnas añadidas en versiones posteriores se crean con `ALTER TABLE … ADD COLUMN` (inofensivo si ya existen).

### `timer`

`Timer`: una cuenta atrás cuyo fin es un instante de reloj de pared (no deriva si el equipo va cargado). Formatos:
`25m`, `1h30`, `90s`, `5` (minutos), `1:30`. `Stopwatch`: cronómetro con vueltas sobre el reloj monótono.
