# Documentación técnica de niri-island

El [README](../README.md) de la raíz cuenta qué hace niri-island y cómo instalarlo. Esta carpeta explica **cómo
está hecho**: compilación, procesos, flujos de datos, formatos y protocolos. Sirve para retomar el proyecto
tiempo después, o para adaptarlo cuando cambie algo de lo que depende (niri, Wayland, Steam, Wallpaper Engine,
wallhaven, V8…).

| Documento | Qué cubre |
|---|---|
| [01 · Compilación](01-compilacion.md) | Dependencias, CMake, cargo, objetivos, instalación, compilaciones de depuración, tests |
| [02 · Arquitectura](02-arquitectura.md) | Procesos, hilos, el bucle de eventos, superficies Wayland, el ciclo de un fotograma |
| [03 · Núcleo e interfaz](03-nucleo-y-ui.md) | `core/` (bucle, procesos, IPC, Wayland, config, inotify) y `ui/` (tema, pintor, regiones, widgets) |
| [04 · La isla](04-isla.md) | `island/` y `views/`: vistas, arbitraje, morph, entrada, barras, ajustes, comandos IPC |
| [05 · Servicios](05-servicios.md) | `services/`: con qué habla cada uno (D-Bus, sysfs, programas, APIs web) |
| [06 · Fondos y suite](06-fondos-y-suite.md) | Fondo de pantalla, selector, Steam/Workshop, wallhaven, web, capturas, protector, bloqueo |
| [07 · El motor](07-motor.md) | `engine/`: niri-island-engine (Rust, Vulkan, V8): CLI, protocolo, renderizador, escenas, scripts |
| [08 · Integración con niri](08-niri.md) | IPC de niri, reglas KDL, namespaces de las capas, lo que niri hace con cada superficie |
| [09 · Configuración y datos](09-configuracion.md) | Cada clave de `config.json`, archivos de estado y cachés |
| [10 · Depuración y rendimiento](10-depuracion.md) | Comandos de depuración, registros, variables de entorno, cómo medir CPU y GPU |
| [11 · Mantenimiento](11-mantenimiento.md) | Qué puede romperse cuando cambia algo externo y cómo actualizarlo |

## En una frase cada pieza

- **`niri-island`** (C++20): un único proceso con un único hilo de estado. Dibuja con Cairo/Pango en superficies
  layer-shell: la isla, las barras, los paneles, los overlays (selector de fondos, ajustes, capturas, protector,
  bloqueo) y el fondo cuando es una imagen.
- **`niri-island-engine`** (Rust, en `engine/`, derivado de skwd-paper): un servidor que lanza renderizadores
  **Vulkan** para vídeos y escenas de Wallpaper Engine. Los scripts de las escenas corren en **V8**.
- **`niri-island-steam`** (Rust): el puente con el cliente de Steam (Steamworks) para buscar en el Workshop,
  suscribirse y descargar. Solo vive mientras el selector muestra el Workshop.
- **Fondos web**: el renderizador CEF de open-wallpaper-engine (descargado aparte), al que la isla habla
  directamente con el protocolo de waywallen (`third_party/waywallen-bridge`).

## Convenciones del código

- Los comentarios explican el **porqué** y van en inglés; la interfaz y estos documentos, en español.
- Nada de estado tocado fuera del hilo principal: el trabajo lento va a un worker (`Loop::async`) y su resultado
  vuelve al bucle.
- Procesos siempre sin shell (`proc::run` con argumentos separados), salvo los comandos que el usuario configura.
- Cada commit termina con `Co-Authored-By:` cuando lo escribe un asistente.
