use super::{Parallax, Properties, ancestor_chain, resolve_transform};
use crate::tests::{build_pkg, build_tex};
use serde_json::{Value, json};

fn image_package() -> crate::pkg::Package {
    let fallback = build_tex("TEXB0002", 0, 0, &[255; 64], false, None);
    let scene = br#"{"general":{"orthogonalprojection":{"width":100,"height":100}},"objects":[{"id":1,"name":"left","image":"models/left.json","origin":"25 50 0","size":"20 20"},{"id":2,"name":"right","image":"models/right.json","origin":"75 50 0","size":"20 20"}]}"#;
    crate::pkg::Package::parse(build_pkg(&[
        ("scene.json", scene),
        ("models/left.json", br#"{"material":"materials/left.json"}"#),
        ("models/right.json", br#"{"material":"materials/right.json"}"#),
        (
            "materials/left.json",
            br#"{"passes":[{"textures":["fallback"],"usertextures":["customleft"]}]}"#,
        ),
        (
            "materials/right.json",
            br#"{"passes":[{"textures":["fallback"],"usertextures":["customright"]}]}"#,
        ),
        ("materials/fallback.tex", &fallback),
    ]))
    .unwrap()
}

fn image_presets(root: &std::path::Path) -> (std::path::PathBuf, std::path::PathBuf) {
    let base = root.join("1");
    let middle = root.join("2");
    let selected = root.join("3");
    for dir in [&base, &middle, &selected] {
        std::fs::create_dir(dir).unwrap();
    }
    std::fs::write(base.join("project.json"), r#"{"type":"scene","general":{"properties":{"customleft":{"type":"scenetexture","value":""},"customright":{"type":"scenetexture","value":""}}}}"#).unwrap();
    std::fs::write(
        middle.join("project.json"),
        r#"{"dependency":"1","preset":{"customleft":"files/über.jpeg"}}"#,
    )
    .unwrap();
    std::fs::write(
        selected.join("project.json"),
        r#"{"dependency":"2","preset":{"customright":"files/dance.gif"}}"#,
    )
    .unwrap();
    (middle, selected)
}

fn sample_jpeg(color: [u8; 3]) -> Vec<u8> {
    let mut bytes = Vec::new();
    image::codecs::jpeg::JpegEncoder::new(&mut bytes)
        .encode(&color, 1, 1, image::ExtendedColorType::Rgb8)
        .unwrap();
    bytes
}

fn sample_gif() -> Vec<u8> {
    gif_frames(2)
}

fn gif_frames(count: usize) -> Vec<u8> {
    let mut bytes = Vec::new();
    let frames = (0..count).map(|index| {
        let (ms, color) =
            if index % 2 == 0 { (30, [0, 250, 0, 255]) } else { (70, [0, 0, 250, 255]) };
        image::Frame::from_parts(
            image::RgbaImage::from_pixel(2, 2, image::Rgba(color)),
            0,
            0,
            image::Delay::from_numer_denom_ms(ms, 1),
        )
    });
    image::codecs::gif::GifEncoder::new(&mut bytes).encode_frames(frames).unwrap();
    bytes
}

#[test]
fn oversized_gif_dimensions_rejected() {
    let mut bytes = sample_gif();
    bytes[6..8].copy_from_slice(&(super::tex::MAX_TEXTURE_EDGE as u16 + 1).to_le_bytes());
    assert!(super::load_user_texture(&bytes).is_none());
}

#[test]
fn gif_frame_budget_rejected() {
    let mut bytes = gif_frames(5);
    bytes[6..8].copy_from_slice(&4096u16.to_le_bytes());
    bytes[8..10].copy_from_slice(&4096u16.to_le_bytes());
    assert!(super::load_user_texture(&bytes).is_none());
}

#[test]
fn preset_images_bind_materials() {
    let root = tempfile::tempdir().unwrap();
    let (middle, selected) = image_presets(root.path());
    std::fs::create_dir(middle.join("files")).unwrap();
    std::fs::create_dir(selected.join("files")).unwrap();
    std::fs::write(middle.join("files/über.jpeg"), sample_jpeg([240, 20, 10])).unwrap();
    std::fs::write(selected.join("files/dance.gif"), sample_gif()).unwrap();
    let model = super::load_from_dir(&image_package(), &selected).unwrap();
    assert!(model.skipped.is_empty(), "{:?}", model.skipped);
    let left = &model.layers[0].texture;
    assert_eq!((left.width, left.height), (1, 1));
    assert!(left.pixels.base_rgba().unwrap()[0] > 100);
    let right = &model.layers[1].texture;
    assert_eq!((right.width, right.height), (2, 2));
    assert_eq!((right.frames.len(), right.pages.len()), (2, 1));
    assert_eq!(right.frames.iter().map(|frame| frame.time).collect::<Vec<_>>(), [0.03, 0.07]);
    assert!(right.pixels.base_rgba().unwrap()[1] > 200);
    assert!(right.pages[0].base_rgba().unwrap()[2] > 200);
}

#[test]
fn selected_image_wins_chain() {
    let root = tempfile::tempdir().unwrap();
    let (middle, selected) = image_presets(root.path());
    std::fs::create_dir(middle.join("files")).unwrap();
    std::fs::create_dir(selected.join("files")).unwrap();
    std::fs::write(middle.join("files/über.jpeg"), sample_jpeg([240, 20, 10])).unwrap();
    std::fs::write(selected.join("files/über.jpeg"), sample_jpeg([10, 20, 240])).unwrap();
    std::fs::write(
        selected.join("project.json"),
        r#"{"dependency":"2","preset":{"customleft":"files/über.jpeg"}}"#,
    )
    .unwrap();
    let model = super::load_from_dir(&image_package(), &selected).unwrap();
    assert!(model.layers[0].texture.pixels.base_rgba().unwrap()[2] > 100);
}

#[test]
fn empty_image_uses_default() {
    let root = tempfile::tempdir().unwrap();
    let (_, selected) = image_presets(root.path());
    std::fs::write(
        selected.join("project.json"),
        r#"{"dependency":"2","preset":{"customleft":""}}"#,
    )
    .unwrap();
    let model = super::load_from_dir(&image_package(), &selected).unwrap();
    assert_eq!(model.layers[0].texture.width, 4);
}

#[test]
fn lenient_base_image_loads() {
    let root = tempfile::tempdir().unwrap();
    std::fs::create_dir(root.path().join("files")).unwrap();
    std::fs::write(root.path().join("files/picture.jpeg"), sample_jpeg([240, 20, 10])).unwrap();
    std::fs::write(root.path().join("project.json"), r#"{"type":"scene","general":{"properties":{"customleft":{"type":"scenetexture","value":"files/picture.jpeg",},}},}"#).unwrap();
    let model = super::load_from_dir(&image_package(), root.path()).unwrap();
    assert_eq!(model.layers[0].texture.width, 1);
    assert!(model.layers[0].texture.pixels.base_rgba().unwrap()[0] > 100);
}

#[test]
fn escaped_preset_uses_default() {
    let root = tempfile::tempdir().unwrap();
    let (_, selected) = image_presets(root.path());
    std::fs::write(
        selected.join("project.json"),
        r#"{"dependency":"2","preset":{"customright":"../outside.gif"}}"#,
    )
    .unwrap();
    std::fs::write(root.path().join("outside.gif"), sample_gif()).unwrap();
    let model = super::load_from_dir(&image_package(), &selected).unwrap();
    let right = &model.layers[1].texture;
    assert_eq!((right.width, right.height), (4, 4));
    assert!(right.frames.is_empty());
}

#[cfg(unix)]
#[test]
fn linked_preset_uses_default() {
    let root = tempfile::tempdir().unwrap();
    let (_, selected) = image_presets(root.path());
    std::fs::create_dir(selected.join("files")).unwrap();
    std::fs::write(root.path().join("outside.gif"), sample_gif()).unwrap();
    std::os::unix::fs::symlink(root.path().join("outside.gif"), selected.join("files/dance.gif"))
        .unwrap();
    let model = super::load_from_dir(&image_package(), &selected).unwrap();
    assert_eq!(model.layers[1].texture.width, 4);
}

#[test]
fn oversized_preset_uses_default() {
    let root = tempfile::tempdir().unwrap();
    let (_, selected) = image_presets(root.path());
    std::fs::create_dir(selected.join("files")).unwrap();
    let file = std::fs::File::create(selected.join("files/dance.gif")).unwrap();
    file.set_len((super::MAX_USER_TEXTURE_FILE_BYTES + 1) as u64).unwrap();
    let model = super::load_from_dir(&image_package(), &selected).unwrap();
    assert_eq!(model.layers[1].texture.width, 4);
}

#[test]
fn preset_scene_load_uses_parent_then_preset_then_explicit_properties() {
    let root = tempfile::tempdir().unwrap();
    let base = root.path().join("1");
    let preset = root.path().join("2");
    std::fs::create_dir(&base).unwrap();
    std::fs::create_dir(&preset).unwrap();
    std::fs::write(
        base.join("project.json"),
        r#"{"type":"scene","general":{"properties":{"tint":{"type":"color","value":"1 0 0"}}}}"#,
    )
    .unwrap();
    std::fs::write(preset.join("project.json"), r#"{"dependency":"1","preset":{"tint":"0 1 0"}}"#)
        .unwrap();
    let scene =
        json!({"general":{"clearcolor":{"user":"tint","value":"1 1 1"}},"objects":[]}).to_string();
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&8u32.to_le_bytes());
    bytes.extend_from_slice(b"PKGV0007");
    bytes.extend_from_slice(&1u32.to_le_bytes());
    bytes.extend_from_slice(&10u32.to_le_bytes());
    bytes.extend_from_slice(b"scene.json");
    bytes.extend_from_slice(&0u32.to_le_bytes());
    bytes.extend_from_slice(&(scene.len() as u32).to_le_bytes());
    bytes.extend_from_slice(scene.as_bytes());
    let package = crate::pkg::Package::parse(bytes).unwrap();
    assert_eq!(super::load_from_dir(&package, &base).unwrap().clear, [1.0, 0.0, 0.0]);
    assert_eq!(super::load_from_dir(&package, &preset).unwrap().clear, [0.0, 1.0, 0.0]);
    let properties = Properties::from([("tint".into(), vec![0.0, 0.0, 1.0])]);
    assert_eq!(
        super::load_from_dir_with(&package, &preset, &properties).unwrap().clear,
        [0.0, 0.0, 1.0]
    );
}

fn scene(amount: f64) -> Value {
    json!({"general": {"cameraparallax": true, "cameraparallaxamount": amount}})
}

fn by_id(objects: &[Value]) -> std::collections::HashMap<String, &Value> {
    objects.iter().filter_map(|object| Some((object.get("id")?.to_string(), object))).collect()
}

#[test]
fn parallax_pushes_a_layer_away_from_the_canvas_centre_by_amount_times_depth() {
    let props = Properties::new();
    let canvas = (1000.0, 800.0);
    let parallax = Parallax::of(&scene(0.5), canvas, &props).expect("enabled");
    let object = json!({"id": 1, "origin": "700 500 0", "parallaxDepth": "2 0.5"});
    let objects = [object.clone()];
    let placed = resolve_transform(
        &object,
        &by_id(&objects),
        &props,
        Some(&parallax),
        &crate::model::Attachments::new(),
    );
    assert!((placed.origin.0 - (700.0 + (700.0 - 500.0) * 0.5 * 2.0)).abs() < 1e-3);
    assert!((placed.origin.1 - (500.0 + (500.0 - 400.0) * 0.5 * 0.5)).abs() < 1e-3);
}

#[test]
fn a_layer_at_the_focus_point_and_a_layer_without_depth_do_not_move() {
    let props = Properties::new();
    let canvas = (1000.0, 800.0);
    let parallax = Parallax::of(&scene(0.5), canvas, &props).expect("enabled");
    let centred = json!({"id": 1, "origin": "500 400 0", "parallaxDepth": "3 3"});
    let depthless = json!({"id": 2, "origin": "700 500 0"});
    for object in [&centred, &depthless] {
        let objects = [(*object).clone()];
        let base = resolve_transform(
            object,
            &by_id(&objects),
            &props,
            None,
            &crate::model::Attachments::new(),
        );
        let placed = resolve_transform(
            object,
            &by_id(&objects),
            &props,
            Some(&parallax),
            &crate::model::Attachments::new(),
        );
        assert!((placed.origin.0 - base.origin.0).abs() < 1e-3);
        assert!((placed.origin.1 - base.origin.1).abs() < 1e-3);
    }
}

#[test]
fn a_child_inherits_the_root_ancestors_offset_and_ignores_its_own_depth() {
    let props = Properties::new();
    let canvas = (1000.0, 800.0);
    let parallax = Parallax::of(&scene(0.5), canvas, &props).expect("enabled");
    let root = json!({"id": 1, "origin": "700 400 0", "parallaxDepth": "2 2"});
    let child = json!({"id": 2, "parent": 1, "origin": "10 0 0", "parallaxDepth": "50 50"});
    let objects = [root.clone(), child.clone()];
    let map = by_id(&objects);
    assert_eq!(ancestor_chain(&child, &map).len(), 2);
    let base = resolve_transform(&child, &map, &props, None, &crate::model::Attachments::new());
    let placed =
        resolve_transform(&child, &map, &props, Some(&parallax), &crate::model::Attachments::new());
    assert!((placed.origin.0 - base.origin.0 - (700.0 - 500.0) * 0.5 * 2.0).abs() < 1e-3);
    assert!((placed.origin.1 - base.origin.1).abs() < 1e-3);
}

#[test]
fn parallax_is_off_unless_the_scene_enables_it_and_defaults_to_half_amount() {
    let props = Properties::new();
    let canvas = (1000.0, 800.0);
    assert!(Parallax::of(&json!({"general": {}}), canvas, &props).is_none());
    assert!(Parallax::of(&json!({"general": {"cameraparallax": false}}), canvas, &props).is_none());
    let defaulted =
        Parallax::of(&json!({"general": {"cameraparallax": true}}), canvas, &props).expect("on");
    let object = json!({"id": 1, "origin": "600 400 0", "parallaxDepth": "1 1"});
    assert!((defaulted.offset(&object, &props).0 - 50.0).abs() < 1e-3);
}

#[test]
fn animated_texture_retains_pages_and_uses_each_pages_dimensions() {
    let mut bytes = b"TEXV0005\0TEXI0001\0".to_vec();
    for value in [0i32, 4, 4, 2, 4, 2, 0] {
        bytes.extend(value.to_le_bytes());
    }
    bytes.extend(b"TEXB0001\0");
    bytes.extend(2i32.to_le_bytes());
    for (width, height, color) in [(4i32, 2i32, [255u8, 0, 0, 255]), (2, 2, [0, 255, 0, 255])] {
        for value in [1, width, height, width * height * 4] {
            bytes.extend(value.to_le_bytes());
        }
        for _ in 0..width * height {
            bytes.extend(color);
        }
    }
    bytes.extend(b"TEXS0003\0");
    for value in [2i32, 2, 2] {
        bytes.extend(value.to_le_bytes());
    }
    for image in [0i32, 1] {
        bytes.extend(image.to_le_bytes());
        for value in [0.1f32, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0] {
            bytes.extend(value.to_le_bytes());
        }
    }
    let texture = super::load_texture_bytes(&bytes).unwrap();
    assert_eq!(texture.pages.len(), 1);
    assert_eq!(texture.payload_bytes(), 48);
    let frames = texture.atlas_frames().unwrap();
    assert_eq!(frames[0].uv, [0.0, 0.0, 0.5, 1.0]);
    assert_eq!(frames[1].uv, [0.0, 0.0, 1.0, 1.0]);
    assert_eq!(frames[1].image, 1);
    assert_eq!(texture.pages[0].base_rgba().unwrap(), [0, 255, 0, 255].repeat(4));
}

#[test]
fn hidden_script_layer_preserves_effect_source_alpha() {
    let mut scene = json!({"objects":[{"id":1,"visible":false},{"id":2,"parent":1,"color":"0.2 0.4 0.6","alpha":0.5}]});
    let layout = super::script::Layout {
        id: "2".into(),
        size: [64.0; 2],
        offset: [0.0; 2],
        text: false,
        hidden_without_fx: false,
    };
    let frame = super::script::frames(
        &scene,
        std::slice::from_ref(&layout),
        (64.0, 64.0),
        &Properties::new(),
        &crate::model::Attachments::new(),
    )
    .remove(0)
    .unwrap();
    assert_eq!(frame.tint, [0.2, 0.4, 0.6, 0.0]);
    assert_eq!(frame.source_tint, [0.2, 0.4, 0.6, 0.5]);
    scene["objects"][0]["visible"] = json!(true);
    let shown = super::script::frames(
        &scene,
        &[layout],
        (64.0, 64.0),
        &Properties::new(),
        &crate::model::Attachments::new(),
    )
    .remove(0)
    .unwrap();
    assert_eq!(shown.tint, frame.source_tint);
    assert_eq!(shown.source_tint, frame.source_tint);
}

#[test]
fn playback_parallax_ignores_saved_editor_camera_offset() {
    let mut scene = scene(1.27);
    scene["camera"] = json!({"eye":"20.05524 289.45834 1", "center":"20.05524 289.45834 0"});
    let props = Properties::new();
    let parallax = Parallax::of(&scene, (3840.0, 2160.0), &props).unwrap();
    let background = json!({"origin":"1920 1080 0", "parallaxDepth":"-1.71 -1.71"});
    assert_eq!(parallax.offset(&background, &props), (0.0, 0.0));
}

#[test]
fn text_defaults_to_unit_depth_for_placement_and_pointer_motion() {
    let props = Properties::new();
    let parallax = Parallax::of(&scene(0.5), (1000.0, 800.0), &props).unwrap();
    let root = json!({"id":1,"text":"Clock", "origin":"700 500 0"});
    let child =
        json!({"id":2,"parent":1,"image":"child.json","origin":"10 0 0","parallaxDepth":"0 0"});
    let objects = [root.clone(), child.clone()];
    let map = by_id(&objects);
    let transform =
        resolve_transform(&child, &map, &props, Some(&parallax), &crate::model::Attachments::new());
    assert_eq!(transform.origin, (810.0, 550.0, 0.0));
    let mouse = crate::mouse::Parallax { amount: 0.5, influence: 1.0, ..Default::default() };
    assert_eq!(
        super::layer_mouse(&child, &map, &props, mouse, transform, false).parallax,
        [1.0; 2]
    );
    let flat = json!({"text":"Clock", "origin":"700 500 0","parallaxDepth":"0 0"});
    assert_eq!(parallax.offset(&flat, &props), (0.0, 0.0));
}

#[test]
fn image_parallax_defaults_to_unit_depth_and_preserves_authored_depth() {
    let props = Properties::new();
    let parallax = Parallax::of(&scene(0.5), (1000.0, 800.0), &props).unwrap();
    let mouse = crate::mouse::Parallax { amount: 0.5, influence: 1.0, ..Default::default() };
    for (depth, expected) in
        [(None, [1.0, 1.0]), (Some("0 0"), [0.0, 0.0]), (Some("-2 0.5"), [-2.0, 0.5])]
    {
        let mut object = json!({"id":1,"image":"image.json","origin":"700 500 0"});
        if let Some(depth) = depth {
            object["parallaxDepth"] = json!(depth);
        }
        let objects = [object.clone()];
        let map = by_id(&objects);
        let transform = resolve_transform(
            &object,
            &map,
            &props,
            Some(&parallax),
            &crate::model::Attachments::new(),
        );
        assert_eq!(
            transform.origin,
            (700.0 + 100.0 * expected[0], 500.0 + 50.0 * expected[1], 0.0)
        );
        assert_eq!(
            super::layer_mouse(&object, &map, &props, mouse, transform, false).parallax,
            expected
        );
        assert_eq!(
            super::layer_mouse(
                &object,
                &map,
                &props,
                crate::mouse::Parallax::default(),
                transform,
                false
            )
            .parallax,
            [0.0; 2]
        );
    }
}

#[test]
fn non_image_parallax_defaults_are_preserved() {
    let props = Properties::new();
    for object in [json!({"text":"Clock"}), json!({"particle":"particle.json"})] {
        assert_eq!(super::parallax_depth(&object, &props), (1.0, 1.0));
    }
    for object in [json!({}), json!({"sound":"sound.json"})] {
        assert_eq!(super::parallax_depth(&object, &props), (0.0, 0.0));
    }
}

#[test]
fn child_inherits_omitted_image_parent_depth() {
    let props = Properties::new();
    let parallax = Parallax::of(&scene(0.5), (1000.0, 800.0), &props).unwrap();
    let root = json!({"id":1,"image":"parent.json","origin":"700 500 0"});
    let child =
        json!({"id":2,"parent":1,"image":"child.json","origin":"10 0 0","parallaxDepth":"0 0"});
    let objects = [root, child.clone()];
    let map = by_id(&objects);
    let transform =
        resolve_transform(&child, &map, &props, Some(&parallax), &crate::model::Attachments::new());
    assert_eq!(transform.origin, (810.0, 550.0, 0.0));
    let mouse = crate::mouse::Parallax { amount: 0.5, influence: 1.0, ..Default::default() };
    assert_eq!(
        super::layer_mouse(&child, &map, &props, mouse, transform, false).parallax,
        [1.0; 2]
    );
}
