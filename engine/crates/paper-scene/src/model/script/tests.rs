use super::{Properties, particle_frames};
use serde_json::json;

#[test]
fn particles_inherit_ancestor_placement_scale_and_visibility() {
    let scene = json!({"objects":[
        {"id":1,"origin":"1920 1080 4","scale":"2 3 4","visible":{"user":{"name":"language","condition":"1"},"value":true}},
        {"id":2,"parent":1,"origin":"10 -20 5","scale":"5 6 7","particle":"particles/stars.json"},
        {"id":3,"origin":"80 90 0","visible":false},
        {"id":4,"parent":3,"particle":"particles/stars.json"}
    ]});
    let props = Properties::from([("language".into(), vec![1.0])]);
    let states = particle_frames(
        &scene,
        ["2", "4"].into_iter(),
        (3840.0, 2160.0),
        &props,
        &crate::model::Attachments::new(),
    );
    let first = states[0].as_ref().unwrap();
    assert_eq!(first.origin, (1940.0, 1020.0, 9.0));
    assert_eq!(first.scale, [10.0, 18.0, 28.0]);
    assert!(first.visible);
    assert!(!states[1].as_ref().unwrap().visible);
    let props = Properties::from([("language".into(), vec![2.0])]);
    let states = particle_frames(
        &scene,
        ["2"].into_iter(),
        (3840.0, 2160.0),
        &props,
        &crate::model::Attachments::new(),
    );
    assert!(!states[0].as_ref().unwrap().visible);
}

#[test]
fn particle_parent_rotation_and_script_changes_update_the_same_system() {
    let mut scene = json!({"objects":[
        {"id":1,"origin":"100 200 0","angles":format!("0 0 {}",std::f32::consts::FRAC_PI_2)},
        {"id":2,"parent":1,"origin":"10 0 0","particle":"particles/stars.json"}
    ]});
    let props = Properties::new();
    let states = particle_frames(
        &scene,
        ["2"].into_iter(),
        (800.0, 600.0),
        &props,
        &crate::model::Attachments::new(),
    );
    let first = states[0].as_ref().unwrap();
    assert!((first.origin.0 - 100.0).abs() < 0.001);
    assert!((first.origin.1 - 210.0).abs() < 0.001);
    assert!((first.angle - std::f32::consts::FRAC_PI_2).abs() < 0.001);
    scene["objects"][0]["visible"] = json!(false);
    scene["objects"][0]["origin"] = json!("300 400 0");
    let states = particle_frames(
        &scene,
        ["2"].into_iter(),
        (800.0, 600.0),
        &props,
        &crate::model::Attachments::new(),
    );
    let changed = states[0].as_ref().unwrap();
    assert!(!changed.visible);
    assert!((changed.origin.0 - 300.0).abs() < 0.001);
    assert!((changed.origin.1 - 410.0).abs() < 0.001);
}

#[test]
fn particles_use_unit_parallax_depth_and_keep_sprite_scale_unchanged() {
    let mut scene = serde_json::json!({
        "general":{"cameraparallax":true,"cameraparallaxamount":0.5},
        "objects":[
            {"id":1,"origin":"1500 600 0","particle":"particles/test.json"},
            {"id":2,"origin":"1500 900 0","particle":"particles/test.json","parallaxDepth":"0 0"},
            {"id":3,"origin":"1500 1200 0","particle":"particles/test.json","parallaxDepth":"1 1"},
            {"id":4,"origin":"1500 1500 0","particle":"particles/test.json","parallaxDepth":"2 2"}
        ]
    });
    let props = Properties::new();
    let states = particle_frames(
        &scene,
        ["1", "2", "3", "4"].into_iter(),
        (3840.0, 2160.0),
        &props,
        &crate::model::Attachments::new(),
    );
    for (state, expected) in states.iter().zip([
        (1290.0, 360.0, 0.0),
        (1500.0, 900.0, 0.0),
        (1290.0, 1260.0, 0.0),
        (1080.0, 1920.0, 0.0),
    ]) {
        let state = state.as_ref().unwrap();
        assert_eq!(state.origin, expected);
        assert_eq!(state.scale, [1.0; 3]);
    }
    scene["objects"][0]["origin"] = serde_json::json!("1800 900 0");
    let states = particle_frames(
        &scene,
        ["1"].into_iter(),
        (3840.0, 2160.0),
        &props,
        &crate::model::Attachments::new(),
    );
    assert_eq!(states[0].as_ref().unwrap().origin, (1740.0, 810.0, 0.0));
}

#[test]
fn attached_frames_inherit() {
    let scene = json!({"objects":[
        {"id":1,"origin":"100 200 0","scale":"2 2 1"},
        {"id":2,"parent":1,"attachment":"sword","origin":"3 0 0"},
        {"id":3,"parent":2,"origin":"0 5 0"},
        {"id":4,"parent":1,"attachment":"missing","origin":"3 0 0"}
    ]});
    let props = Properties::new();
    let mut attachments = crate::model::Attachments::from([(
        ("1", "sword"),
        crate::puppet::Transform {
            translation: [10.0, 20.0, 0.0],
            rotation: [0.0, 0.0, std::f32::consts::FRAC_PI_2],
            scale: [1.0; 3],
        },
    )]);
    let layout = super::Layout {
        id: "3".into(),
        size: [8.0, 6.0],
        offset: [0.0; 2],
        text: false,
        hidden_without_fx: false,
    };
    let states = super::frames(&scene, &[layout], (800.0, 600.0), &props, &attachments);
    let layer = states[0].as_ref().unwrap();
    assert!((layer.rect[0] - 110.0).abs() < 0.001);
    assert!((layer.rect[1] - 354.0).abs() < 0.001);
    assert_eq!(&layer.rect[2..], &[16.0, 12.0]);
    let particles =
        particle_frames(&scene, ["3", "4"].into_iter(), (800.0, 600.0), &props, &attachments);
    let particle = particles[0].as_ref().unwrap();
    assert!((particle.origin.0 - layer.rect[0]).abs() < 0.001);
    assert!((particle.origin.1 - (600.0 - layer.rect[1])).abs() < 0.001);
    assert_eq!(particles[1].as_ref().unwrap().origin, (106.0, 200.0, 0.0));
    attachments.get_mut(&("1", "sword")).unwrap().translation[0] += 5.0;
    let moved = particle_frames(&scene, ["3"].into_iter(), (800.0, 600.0), &props, &attachments);
    assert!((moved[0].as_ref().unwrap().origin.0 - 120.0).abs() < 0.001);
}
