# niri-island 🏝️

Una **Dynamic Island** minimalista, ultra-rápida y autónoma construida en **C++20** nativo para el compositor Wayland **Niri** (y cualquier compositor compatible con `wlr-layer-shell`).

Inspirada en el concepto de iOS, Noctalia y k4, pero sin runtimes pesados: **sin QML, sin Qt, sin Node/JS ni Electron**.

---

## ⚡ Especificaciones Técnicas

- **Lenguaje:** C++20 puro.
- **Tamaño del binario:** ~104 KB (stripped).
- **Consumo de memoria:** ~15-20 MB RAM (vs 200+ MB de suites basadas en web/Qt).
- **Arquitectura:**
  - **Wayland Client & Layer-Shell:** Manejo directo de buffers compartidos (`memfd_create` + `wl_shm`) usando `zwlr_layer_shell_v1` en capa `OVERLAY`.
  - **Física de resortes (Spring Physics):** Oscilador armónico amortiguado (`stiffness: 260.0`, `damping: 22.0`) para transiciones fluidas de tamaño y forma.
  - **Renderizado vectorial:** Cairo + Pango para tipografía nítida y curvas continuas anti-aliasing con estética *glass/matte*. Centrado óptico pixel-perfect con `ink_rect`.
  - **Carátulas de Álbum Reales:** Soporte integrado vía `gdk-pixbuf` + `webp-pixbuf-loader` para imágenes `.webp`, `.png`, `.jpg`, `.svg`.
  - **Integración Multimedia:** Escucha en tiempo real de reproductores MPRIS (Spotify, Psysonic, Brave, Firefox, etc.) mediante `sd-bus` de `systemd`, con posición y seek interactivo.
  - **HUD OSD Integrado:** Monitoreo en tiempo real de **Volumen** vía `libpulse` (PipeWire/PulseAudio), **Brillo** vía uevents del kernel con `libudev` y **Batería/Cargador** (`power_supply`).
  - **Notificaciones Flotantes:** Interceptación nativa de `org.freedesktop.Notifications` sin interferir con otros demonios.
  - **Indicadores de Privacidad:** Detección de micrófono activo mediante `libpulse` (punto naranja estilo iOS).

---

## 🚀 Estados y Comportamiento

1. **Modo Idle (En reposo):**
   - Pastilla oscura redondeada centrada en el borde superior (110x32 px).
   - Muestra la hora actual y un indicador de estado verde sutil perfectamente centrados.
2. **Modo Compacto con Música:**
   - Detecta automáticamente el reproductor activo vía MPRIS.
   - Muestra la **mini carátula de álbum (22x22 px)** a la izquierda, título/artista centrado al píxel y ecualizador de ondas animado en verde neón a la derecha.
3. **Modo Expandido con Seek (Hover / Click):**
   - Al pasar el cursor o hacer clic sobre la isla, se abre suavemente a 330x125 px.
   - Presenta carátula completa (48x48 px), título, artista, **barra de progreso real con minuteros transcurridos/totales** y controles clicables (`Prev`, `Play/Pause`, `Next`).
   - **Seek táctil/ratón:** Haz clic en cualquier punto de la barra de progreso para saltar al segundo exacto de la canción.
4. **Modo Notificación:**
   - Al llegar un mensaje (Telegram, Discord, alertas de terminal, etc.), la isla se despliega elásticamente a 330x70 px mostrando el ícono de la app, el remitente y el mensaje.
   - Desaparece automáticamente a los 4.5 segundos o al hacer clic sobre ella.
5. **Modo HUD OSD (Volumen, Brillo y Batería):**
   - Al pulsar tus teclas de volumen o brillo, o al conectar el cargador, la isla se morphing elásticamente a 210x34 px.
   - Muestra el icono temático (󰕾 / 󰝟 / 󰃠 / 󰂄), una barra de progreso líquida con acento de color y el porcentaje exacto.
   - A los 2 segundos de inactividad, se encoge suavemente de regreso a su estado normal.
6. **Punto Naranja de Privacidad:**
   - Si alguna aplicación activa el micrófono, se enciende un punto naranja sutil en la esquina superior derecha de la pastilla con un suave resplandor.

---

## 🛠️ Compilación

### Dependencias
```bash
sudo apt install build-essential cmake ninja-build libwayland-client-dev \
    wayland-protocols wayland-scanner libcairo2-dev libpango1.0-dev \
    libgdk-pixbuf-2.0-dev webp-pixbuf-loader libsystemd-dev libpulse-dev libudev-dev
```

### Compilar e Instalar
```bash
cd ~/Projects/niri-island
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Para instalar el binario globalmente:
```bash
sudo cp build/niri-island /usr/local/bin/
```

---

## 🖥️ Integración con Niri

Para iniciar automáticamente `niri-island` al iniciar sesión en Niri, agrega la siguiente línea a tu archivo `~/.config/niri/cfg/startup.kdl`:

```kdl
spawn-at-startup "/home/leimsoto/Projects/niri-island/build/niri-island"
```
