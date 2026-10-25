# 01 · Compilación

## Qué se compila

| Objetivo | Lenguaje | Sale como | Instalado en |
|---|---|---|---|
| `niri-island` | C++20 | el shell (y su propio cliente IPC) | `bin/` |
| `island-core` | C++20 | biblioteca estática con todo menos `main.cpp`; la usan el shell y los tests | — |
| `niri-island-tests` | C++20 | tests unitarios | — (se ejecuta desde `build/`) |
| `ww-codec` | C99 | el códec del protocolo de waywallen (fondos web), enlazado en `island-core` | — |
| `waywallen-bridge` | C99 | `libwaywallen-bridge.so`, la biblioteca que **carga el renderizador CEF** de los fondos web | `lib/niri-island/` |
| `engine` (objetivo personalizado) | Rust | `niri-island-engine`, `-vk`, `-still`, `niri-island-steam` + `libsteam_api.so` | `bin/` |

Todo cuelga del `CMakeLists.txt` de la raíz. El motor es un *workspace* de cargo (`engine/Cargo.toml`) que CMake
compila con `cargo build --release --workspace --target-dir build/engine` cuando `cargo` está en el `PATH` y la
opción `NIRI_ISLAND_ENGINE` está activada (lo está por defecto).

## Dependencias

### El shell (C++)

Paquetes de Debian/Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libwayland-dev wayland-protocols libcairo2-dev libpango1.0-dev libgdk-pixbuf-2.0-dev \
    libsystemd-dev libpulse-dev libudev-dev libxkbcommon-dev \
    libsqlite3-dev libcurl4-openssl-dev libqrencode-dev nlohmann-json3-dev libpam0g-dev
```

CMake los pide por pkg-config: `wayland-client`, `wayland-scanner`, `wayland-protocols`, `cairo`, `pangocairo`,
`gdk-pixbuf-2.0` (decodificar imágenes), `libsystemd` (sd-bus, para D-Bus), `libpulse`, `libudev`, `xkbcommon`,
`sqlite3`, `libcurl`, `libqrencode`, `nlohmann_json`; y enlaza `pam` (pantalla de bloqueo).

`waywallen-bridge` solo se compila si además están `egl`, `gbm` y `vulkan` (pkg-config); sin ellos se omite y los
fondos web no funcionan, nada más.

Los protocolos Wayland se generan al compilar con `wayland-scanner` (macro `generate_wayland_proto` del
CMakeLists): `xdg-shell` y `wlr-layer-shell-unstable-v1` vienen en `protocols/`; el resto (`tablet-v2`,
`viewporter`, `cursor-shape-v1`, `ext-background-effect-v1`, `ext-idle-notify-v1`, `ext-session-lock-v1`,
`linux-dmabuf-v1`) se toman del paquete `wayland-protocols` del sistema. **`ext-background-effect-v1` es un
protocolo *staging* reciente**: si la distribución trae un `wayland-protocols` antiguo, falta ese XML y la
configuración de CMake falla. Ver [11 · Mantenimiento](11-mantenimiento.md#protocolos-wayland).

### El motor (Rust)

- **Toolchain**: fijada en `engine/rust-toolchain.toml` (`1.94.1`, con clippy y rustfmt). rustup la descarga sola
  la primera vez. La edición es 2024 (`rust-version = "1.93"` mínimo).
- **Bibliotecas del sistema**: `libvulkan-dev`, `libgbm-dev`, `libegl-dev`, `libwayland-dev`, ffmpeg completo
  (`libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libavfilter-dev
  libavdevice-dev`), `libasound2-dev` (cpal), `libdbus-1-dev` no hace falta (el crate `dbus` va *vendored*),
  `libclang-dev` (bindgen de ffmpeg y de shaderc), `cmake` y un compilador de C++ (shaderc compila su copia de
  glslang si no encuentra la del sistema).
- **V8**: el crate `v8 = "152.2.0"` descarga en la primera compilación una biblioteca estática precompilada
  (`librusty_v8`, ~100 MB) desde las *releases* de denoland/rusty_v8. Sin red en esa primera compilación, falla.
  Se puede apuntar a una copia local con la variable `RUSTY_V8_ARCHIVE`.
- **Steamworks**: `steamworks = "0.13.1"` (crate `steamworks-sys`) trae el SDK y su `libsteam_api.so`.
  `engine/copy-steam-lib.cmake` copia la más reciente junto a `niri-island-steam`, que la encuentra por su rpath.
- **dxc opcional**: los shaders HLSL de algunas escenas se compilan con `libdxcompiler.so` si está (junto al
  ejecutable, en `../lib/`, en `~/.local/lib/` o donde diga `SKWD_DXC_PATH`). Sin ella se usa la traducción a
  GLSL + shaderc, que cubre casi todo. Ver [07 · El motor](07-motor.md#shaders).

### En tiempo de ejecución (opcionales)

| Programa | Para qué |
|---|---|
| `nmcli` | Wi-Fi (lista, conectar, olvidar, QR) |
| `cava` | espectro de audio de la isla |
| `cliphist`, `wl-clipboard` (`wl-copy`, `wl-paste`), `wtype` | portapapeles y pegar en la ventana anterior |
| `matugen` | paleta adaptativa a partir del fondo (sin él: color dominante de la imagen) |
| `mpvpaper` | vídeos de fondo **solo si el motor no está compilado** |
| `wf-recorder`, `ffmpeg`, `pactl` | grabación de pantalla (pausa = segmentos unidos por ffmpeg; audio por un sumidero nulo temporal) |
| `grim` | capturas |
| `swayidle` | bloquear antes de suspender (`before-sleep`) |
| `brightnessctl`, `powerprofilesctl` / power-profiles-daemon | brillo y perfiles de energía |
| `paplay` | sonidos de notificaciones y temporizadores |
| Steam + Wallpaper Engine (431960) | escenas: sus recursos (`assets/`); el Workshop necesita Steam abierto |

## Compilar

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)          # shell + motor + tests
cmake --install build --prefix ~/.local
```

- `-DNIRI_ISLAND_ENGINE=OFF` omite el motor: los vídeos vuelven a `mpvpaper` y no hay escenas de Wallpaper Engine.
- Objetivos sueltos: `--target niri-island`, `--target niri-island-tests`, `--target engine`.
- El motor también se compila por su cuenta desde `engine/` con `cargo build --release` (sale en
  `engine/target/release/`); para instalarlo hay que pasar por CMake o copiar los binarios a mano.
- La primera compilación del motor tarda varios minutos (V8, ffmpeg, shaderc); las siguientes, ~20 s – 2 min con
  `lto = "thin"` y `codegen-units = 1` (perfil `release` del workspace).

### Qué deja la instalación

```
~/.local/bin/niri-island
~/.local/bin/niri-island-engine          # controlador / servidor del motor
~/.local/bin/niri-island-engine-vk       # renderizador Vulkan (vídeo y escenas)
~/.local/bin/niri-island-engine-still    # renderizador de imágenes fijas por memoria compartida
~/.local/bin/niri-island-steam           # puente con Steam
~/.local/bin/libsteam_api.so
~/.local/lib/niri-island/libwaywallen-bridge.so
```

El renderizador web (CEF, ~340 MB) no se compila: se descarga desde Ajustes → Fondos a
`~/.local/share/niri-island/weweb` (ver [06](06-fondos-y-suite.md#fondos-web-webwall)).

La pantalla de bloqueo autentica con PAM como servicio `niri-island`:

```bash
sudo cp /etc/pam.d/swaylock /etc/pam.d/niri-island    # o un archivo con «auth include login»
```

### Reiniciar tras instalar

```bash
kill -TERM $(pgrep -x niri-island); setsid niri-island &     # la isla
~/.local/bin/niri-island-engine stop                         # el motor: la isla lo vuelve a lanzar al aplicar
```

Usar `pgrep -x`/`pkill -x` (nombre exacto). `pkill -f niri-island` también mata cualquier shell cuya línea de
órdenes contenga «niri-island».

## Compilaciones especiales

```bash
# Depuración (símbolos, sin optimizar) y ejecución bajo gdb: el backtrace queda en ~/.cache/niri-island/gdb.log
cmake -B build-dbg -DCMAKE_BUILD_TYPE=Debug -DNIRI_ISLAND_ENGINE=OFF && cmake --build build-dbg
kill -TERM $(pgrep -x niri-island); setsid tools/run-dbg.sh &

# Sanitizers (memoria y comportamiento indefinido)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DNIRI_ISLAND_ENGINE=OFF \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && build-asan/niri-island-tests
```

`CMAKE_EXPORT_COMPILE_COMMANDS` está activado: `build/compile_commands.json` sirve a clangd.

## Tests

### Shell

```bash
cmake --build build --target niri-island-tests && ctest --test-dir build --output-on-failure
# o directamente: build/niri-island-tests
```

El marco es propio y mínimo (`tests/check.hpp`: `TEST(nombre)`, `CHECK(cond)`). Cada `tests/test_*.cpp` se
compila en el mismo ejecutable. Cubren: utilidades, la calculadora del lanzador, los nombres de la bandeja, la
configuración (valores por defecto, módulos nuevos, guardado, JSON roto), el cierre con una descarga en curso, y
el Workshop (lectura de `project.json`, suscripciones, limpieza, propiedades editables). Los tests que tocan
archivos cambian `HOME`/`XDG_CONFIG_HOME` a un directorio temporal.

Con la isla en marcha:

```bash
tests/smoke.sh                  # abre y cierra cada panel y overlay; comprueba que la isla sigue viva
python3 tests/fake_mpris.py &   # un reproductor MPRIS falso «reproduciendo»
```

### Motor

```bash
cd engine
cargo test --release -p skwd-wall-vk      # renderizador: geometría, escenas, efectos, mallas
cargo test --release -p paper-scene       # formato de escenas, shaders, scripts V8, propiedades
cargo test --release -p skwd-paper        # controlador, protocolo, gestor de renderizadores
cargo test --release --workspace          # todo
```

Fallos conocidos que **no** son regresiones (fallaban igual antes de los cambios de niri-island):

- `paper-scene`: `corpus_shader_ratchet` y `packed_audio_spectrum_indices_compile_with_scalar_uniform_layout`
  necesitan `libdxcompiler.so` (dxc) instalado.
- `skwd-paper`: `still_package_works_without_vk_package`, `vk_package_works_without_still_package`,
  `missing_still_replacement_keeps_video_incumbent`, `spawn_failure_keeps_incumbents` y
  `parent_and_lock_private` esperan los nombres de binario y rutas originales de skwd-paper.

Los paquetes del workspace conservan los nombres de skwd-paper (`skwd-paper` es el controlador, `skwd-wall-vk`
el renderizador, `skwd-wall-still` el de imágenes); los **binarios** se renombran en sus `Cargo.toml`
(`[[bin]] name = "niri-island-engine…"`).

### Lints

El workspace del motor declara `clippy::pedantic = deny`. `cargo clippy --workspace --release` debe quedar
limpio; los avisos de C++ (`-Wall -Wextra`) también deberían quedar a cero.
