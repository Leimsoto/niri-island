# niri-island 🏝️

Una **Dynamic Island** minimalista, ultra-rápida y autónoma construida en **C++20** nativo para el compositor Wayland **Niri** (y cualquier compositor compatible con `wlr-layer-shell`).

Inspirada en el concepto de iOS, Noctalia y k4, pero sin runtimes pesados: **sin QML, sin Qt, sin Node/JS ni Electron**.

---

## ⚡ Especificaciones Técnicas

- **Lenguaje:** C++20 puro.
- **Tamaño del binario:** ~52 KB (stripped).
- **Consumo de memoria:** ~10-15 MB RAM (vs 200+ MB de suites basadas en web/Qt).
- **Arquitectura:**
  - **Wayland Client & Layer-Shell:** Manejo directo de buffers compartidos (`memfd_create` + `wl_shm`) usando `zwlr_layer_shell_v1` en capa `OVERLAY`.
  - **Física de resortes (Spring Physics):** Oscilador armónico amortiguado (`stiffness: 260.0`, `damping: 22.0`) para transiciones fluidas de tamaño y forma.
  - **Renderizado vectorial:** Cairo + Pango para tipografía nítida y curvas continuas anti-aliasing con estética *glass/matte*.
  - **Integración Multimedia:** Escucha en tiempo real de reproductores MPRIS (Spotify, Psysonic, Brave, Firefox, etc.) mediante `sd-bus` de `systemd`.

---

## 🚀 Estados y Comportamiento

1. **Modo Idle (En reposo):**
   - Pastilla oscura redondeada centrada en el borde superior (110x32 px).
   - Muestra la hora actual y un indicador de estado verde sutil.
2. **Modo Compacto con Música:**
   - Detecta automáticamente el reproductor activo vía MPRIS.
   - Se expande suavemente con física fluida a 240x34 px.
   - Muestra onda de audio animada en tiempo real y el título del track actual.
3. **Modo Expandido (Hover / Click):**
   - Al pasar el cursor o hacer clic sobre la isla, se abre a 330x115 px.
   - Presenta título, artista, barra de progreso y controles táctiles/clicables:
     - Izquierda: `Previous`
     - Centro: `Play / Pause`
     - Derecha: `Next`

---

## 🛠️ Compilación

### Dependencias
```bash
sudo apt install build-essential cmake ninja-build libwayland-client-dev \
    wayland-protocols wayland-scanner libcairo2-dev libpango1.0-dev libsystemd-dev
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

Para iniciar automáticamente `niri-island` al iniciar Niri, agrega la siguiente línea a tu archivo `~/.config/niri/cfg/startup.kdl`:

```kdl
spawn-at-startup "/usr/local/bin/niri-island"
```
