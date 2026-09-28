# niri-island 🏝️

Una **Dynamic Island** minimalista, ultra-rápida y autónoma construida en **C++20** nativo para el compositor Wayland **Niri** (y cualquier compositor compatible con `wlr-layer-shell`).

Inspirada en el concepto de iOS, Noctalia y k4, pero sin runtimes pesados: **sin QML, sin Qt, sin Node/JS ni Electron**.

---

## ⚡ Especificaciones Técnicas

- **Lenguaje:** C++20 puro.
- **Tamaño del binario:** ~80 KB (stripped).
- **Consumo de memoria:** ~12-18 MB RAM (vs 200+ MB de suites basadas en web/Qt).
- **Arquitectura:**
  - **Wayland Client & Layer-Shell:** Manejo directo de buffers compartidos (`memfd_create` + `wl_shm`) usando `zwlr_layer_shell_v1` en capa `OVERLAY`.
  - **Física de resortes (Spring Physics):** Oscilador armónico amortiguado (`stiffness: 260.0`, `damping: 22.0`) para transiciones fluidas de tamaño y forma.
  - **Renderizado vectorial:** Cairo + Pango para tipografía nítida y curvas continuas anti-aliasing con estética *glass/matte*. Centrado óptico pixel-perfect con `ink_rect`.
  - **Carátulas de Álbum Reales:** Soporte integrado vía `gdk-pixbuf` + `webp-pixbuf-loader` para imágenes `.webp`, `.png`, `.jpg`, `.svg`.
  - **Integración Multimedia:** Escucha en tiempo real de reproductores MPRIS (Spotify, Psysonic, Brave, Firefox, etc.) mediante `sd-bus` de `systemd`.
  - **HUD OSD Integrado:** Monitoreo en tiempo real de **Volumen** vía `libpulse` (PipeWire/PulseAudio) y **Brillo** vía uevents del kernel con `libudev`.

---

## 🚀 Estados y Comportamiento

1. **Modo Idle (En reposo):**
   - Pastilla oscura redondeada centrada en el borde superior (110x32 px).
   - Muestra la hora actual y un indicador de estado verde sutil perfectamente centrados.
2. **Modo Compacto con Música:**
   - Detecta automáticamente el reproductor activo vía MPRIS.
   - Muestra la **mini carátula de álbum (22x22 px)** a la izquierda, título/artista centrado al píxel y ecualizador de ondas animado en verde neón a la derecha.
3. **Modo Expandido (Hover / Click):**
   - Al pasar el cursor o hacer clic sobre la isla, se abre suavemente a 330x115 px.
   - Presenta carátula completa (48x48 px), título, artista, barra de progreso y controles táctiles/clicables (`Prev`, `Play/Pause`, `Next`).
4. **Modo HUD OSD (Volumen y Brillo):**
   - Al pulsar tus teclas de volumen o brillo, la isla se morphing elásticamente a 210x34 px.
   - Muestra el icono temático (󰕾 / 󰝟 / 󰃠), una barra de progreso líquida con acento de color y el porcentaje exacto.
   - A los 2 segundos de inactividad, se encoge suavemente de regreso a su estado normal.

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
