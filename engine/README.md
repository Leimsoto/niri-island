# niri-island-engine

The renderer behind niri-island's animated wallpapers: Wallpaper Engine scenes
and videos, and plain videos, drawn with Vulkan on a background layer surface.

It is [skwd-paper](https://github.com/liixini/skwd-paper) by liixini (MIT,
see LICENSE), trimmed for niri-island: binaries renamed (`niri-island-engine`,
`niri-island-engine-vk`, `niri-island-engine-still`), its own socket
(`$XDG_RUNTIME_DIR/niri-island-engine/engine.sock`) and logs
(`~/.cache/niri-island/engine*.log`), and the private AV1 worker dropped.

Beyond skwd-paper: SceneScripts run on V8 instead of QuickJS (~20× faster:
scripts that simulate and draw a whole scene each frame become playable);
scripts can build models (`thisScene.createModelData`, `createLayer` with a
model, `sortLayer`), drawn with depth between scene layers; effects share
non-unique named targets across layers; a material's first texture named
after a target replaces the layer underneath. `SKWD_DUMP_SHARED_TARGETS=dir`
makes `capture-scenes` save every shared target as a PNG.

Also: transparent input surfaces (`niri-island-engine-input`) so scenes get
the pointer although niri gives none to backdrop layers; `niri-island-engine
properties <JSON>` (live property updates) and word combos passed by index;
`apply --scene-fps N --scene-max PX` (the renderer strips SKWD_* from its
environment, so policy is the only way in); low global queue priority, so a
heavy scene never starves the compositor; composition layers scaled with the
scene's raster; `SKWD_VK_SCENE_STATS=1` logs real fps and compose time.

Full technical notes (in Spanish): [../docs/07-motor.md](../docs/07-motor.md).

    cargo build --release      # niri-island's CMake build does this when cargo is there
