# niri-island-engine

The renderer behind niri-island's animated wallpapers: Wallpaper Engine scenes
and videos, and plain videos, drawn with Vulkan on a background layer surface.

It is [skwd-paper](https://github.com/liixini/skwd-paper) by liixini (MIT,
see LICENSE), trimmed for niri-island: binaries renamed (`niri-island-engine`,
`niri-island-engine-vk`, `niri-island-engine-still`), its own socket
(`$XDG_RUNTIME_DIR/niri-island-engine/engine.sock`) and logs
(`~/.cache/niri-island/engine*.log`), and the private AV1 worker dropped.

    cargo build --release      # niri-island's CMake build does this when cargo is there
