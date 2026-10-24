use super::*;

fn fixture() -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(MAGIC_23);
    out.resize(32, 0);
    out.extend_from_slice(MESH_SIGNATURE);
    out.extend_from_slice(&(VERTEX_STRIDE as u32 * 3).to_le_bytes());
    for (position, uv) in [
        ([1.0_f32, 2.0, 0.0], [0.1_f32, 0.2]),
        ([3.0, 4.0, 0.0], [0.3, 0.4]),
        ([5.0, 6.0, 0.0], [0.5, 0.6]),
    ] {
        let start = out.len();
        out.extend(position.into_iter().flat_map(f32::to_le_bytes));
        out.resize(start + 72, 0);
        out[start + 56..start + 60].copy_from_slice(&1.0_f32.to_le_bytes());
        out.extend(uv.into_iter().flat_map(f32::to_le_bytes));
    }
    out.extend_from_slice(&6_u32.to_le_bytes());
    out.extend([0_u16, 1, 2].into_iter().flat_map(u16::to_le_bytes));
    out.extend_from_slice(b"MDLS0004\0");
    let animation_offset = out.len();
    out.extend_from_slice(&0_u32.to_le_bytes());
    out.extend_from_slice(&1_u32.to_le_bytes());
    out.push(0);
    out.extend_from_slice(&1_u32.to_le_bytes());
    out.extend_from_slice(&u32::MAX.to_le_bytes());
    out.extend_from_slice(&64_u32.to_le_bytes());
    for value in
        [1.0_f32, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out.push(0);
    let animation = out.len();
    out[animation_offset..animation_offset + 4].copy_from_slice(&(animation as u32).to_le_bytes());
    out.extend_from_slice(b"MDLA0006\0");
    let animation_end = out.len();
    out.extend_from_slice(&0_u32.to_le_bytes());
    out.extend_from_slice(&1_u32.to_le_bytes());
    out.extend_from_slice(&1_u32.to_le_bytes());
    out.extend_from_slice(&0_u32.to_le_bytes());
    out.extend_from_slice(b"move\0loop\0");
    out.extend_from_slice(&30.0_f32.to_le_bytes());
    out.extend_from_slice(&2_u32.to_le_bytes());
    out.extend_from_slice(&0_u32.to_le_bytes());
    out.extend_from_slice(&1_u32.to_le_bytes());
    out.extend_from_slice(&0_u32.to_le_bytes());
    out.extend_from_slice(&108_u32.to_le_bytes());
    for x in [0.0_f32, 10.0, 0.0] {
        for value in [x, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0] {
            out.extend_from_slice(&value.to_le_bytes());
        }
    }
    out.resize(out.len() + 35, 0);
    let end = out.len();
    out[animation_end..animation_end + 4].copy_from_slice(&(end as u32).to_le_bytes());
    out
}

#[test]
fn parse_mesh_fields() {
    let mesh = parse(&fixture()).unwrap();
    assert_eq!(mesh.indices, [0, 1, 2]);
    assert_eq!(mesh.vertices[1].position, [3.0, 4.0, 0.0]);
    assert_eq!(mesh.vertices[2].uv, [0.5, 0.6]);
    assert_eq!(mesh.bones.len(), 1);
    assert_eq!(mesh.animations[0].id, 1);

    let mut version_21 = fixture();
    version_21[..8].copy_from_slice(MAGIC_21);
    assert_eq!(parse(&version_21).unwrap().vertices.len(), 3);
}

#[test]
fn rejects_bad_headers_indices() {
    let mut bad_header = fixture();
    bad_header[..8].copy_from_slice(b"MDLV0013");
    assert!(parse(&bad_header).unwrap_err().to_string().contains("header"));

    let mut bad_index = fixture();
    let index = 32 + 8 + VERTEX_STRIDE * 3 + 4;
    bad_index[index..index + 2].copy_from_slice(&3_u16.to_le_bytes());
    assert!(parse(&bad_index).unwrap_err().to_string().contains("outside"));

    let mut oversized_animation = fixture();
    let mode = find(&oversized_animation, b"move\0loop\0").unwrap();
    oversized_animation[mode + 14..mode + 18].copy_from_slice(&u32::MAX.to_le_bytes());
    assert!(parse(&oversized_animation).unwrap_err().to_string().contains("samples"));
}

#[test]
fn additive_skin_weights() {
    let mesh = parse(&fixture()).unwrap();
    let positions =
        skin(&mesh, &[AnimationLayer { id: 1, rate: 1.0, blend: 1.0, additive: true }], 1.0 / 30.0)
            .positions;
    assert!((positions[0][0] - 11.0).abs() < 0.001);
    assert!((positions[0][1] - 2.0).abs() < 0.001);
}

#[test]
fn mirrored_animation_reverses() {
    let mut mesh = parse(&fixture()).unwrap();
    mesh.animations[0].mirror = true;
    let layers = [AnimationLayer { id: 1, rate: 1.0, blend: 1.0, additive: true }];
    let forward = skin(&mesh, &layers, 1.0 / 30.0).positions;
    let reverse = skin(&mesh, &layers, 3.0 / 30.0).positions;
    assert_eq!(forward, reverse);
}

#[test]
fn degenerate_bind_pose() {
    let zero = Transform { translation: [0.0; 3], rotation: [0.0; 3], scale: [0.0, 0.0, 1.0] };
    let bones = vec![Bone { parent: None, bind: zero }];
    let bind_inverse: Vec<Option<Affine>> = world(bones.iter().map(|bone| bone.bind), &bones)
        .into_iter()
        .map(Affine::inverse)
        .collect();
    assert_eq!(bind_inverse, vec![None]);
    let mesh = Mesh {
        vertices: vec![Vertex {
            position: [1.0, 2.0, 0.0],
            uv: [0.0, 0.0],
            bones: [0, u16::MAX, u16::MAX, u16::MAX],
            weights: [1.0, 0.0, 0.0, 0.0],
        }],
        indices: vec![0],
        bones,
        animations: Vec::new(),
        attachments: Vec::new(),
        bind_inverse,
    };
    let out = skin(&mesh, &[], 0.0).positions;
    assert_eq!(out, vec![[1.0, 2.0, 0.0]]);
}

#[test]
fn bind_inverse_cached() {
    let mesh = parse(&fixture()).unwrap();
    assert_eq!(mesh.bind_inverse.len(), mesh.bones.len());
    assert!(mesh.bind_inverse.iter().all(Option::is_some));
}

#[test]
fn legacy_puppet_decodes_multiple_animations_and_named_bones() {
    let current = fixture();
    let skeleton = find(&current, b"MDLS0004\0").unwrap();
    let animation = find(&current, b"MDLA0006\0").unwrap();
    let mut legacy = current[..animation].to_vec();
    legacy[..8].copy_from_slice(MAGIC_19);
    legacy[skeleton..skeleton + 8].copy_from_slice(b"MDLS0002");
    let name = b"root";
    legacy.splice(skeleton + 17..skeleton + 17, name.iter().copied());
    let animation = legacy.len();
    legacy[skeleton + 9..skeleton + 13].copy_from_slice(&(animation as u32).to_le_bytes());
    legacy.extend_from_slice(b"MDLA0005\0");
    legacy.extend_from_slice(&0_u32.to_le_bytes());
    legacy.extend_from_slice(&2_u32.to_le_bytes());
    let current_animation = find(&current, b"MDLA0006\0").unwrap();
    let record = &current[current_animation + 17..current.len() - 35];
    for id in [1_u32, 2] {
        legacy.extend_from_slice(&id.to_le_bytes());
        legacy.extend_from_slice(&record[4..]);
        legacy.resize(legacy.len() + 34, 0);
    }
    let end = legacy.len() as u32;
    legacy[animation + 9..animation + 13].copy_from_slice(&end.to_le_bytes());
    let mesh = parse(&legacy).unwrap();
    assert_eq!(mesh.vertices, parse(&current).unwrap().vertices);
    assert_eq!(mesh.animations.len(), 2);
    for id in [1, 2] {
        let positions = skin(
            &mesh,
            &[AnimationLayer { id, rate: 1.0, blend: 1.0, additive: true }],
            1.0 / 30.0,
        )
        .positions;
        assert!((positions[0][0] - 11.0).abs() < 0.001);
    }
    legacy.truncate(legacy.len() - 1);
    assert!(parse(&legacy).is_err());
}

#[test]
fn all_four_vertex_influences_participate_in_skinning() {
    let mut bytes = fixture();
    let first_vertex = 32 + 8;
    for (index, weight) in [0.0_f32, 0.0, 0.25, 0.75].into_iter().enumerate() {
        let at = first_vertex + 56 + index * 4;
        bytes[at..at + 4].copy_from_slice(&weight.to_le_bytes());
    }
    let mut mesh = parse(&bytes).unwrap();
    assert_eq!(mesh.vertices[0].weights, [0.0, 0.0, 0.25, 0.75]);
    mesh.vertices[0].bones = [u16::MAX, u16::MAX, 0, 1];
    let identity = Affine::of(mesh.bones[0].bind);
    let mut right = identity;
    right.x = 4.0;
    let mut up = identity;
    up.y = 8.0;
    let actual = skin_vertex(&mesh.vertices[0], &[Some(identity); 2], &[right, up]);
    assert_eq!(actual, [2.0, 8.0, 0.0]);
}

fn attachment_fixture() -> Vec<u8> {
    let mut bytes = fixture();
    let animation = find(&bytes, b"MDLA0006\0").unwrap();
    let mut attachment = b"MDAT0001\0".to_vec();
    attachment.extend_from_slice(&0_u32.to_le_bytes());
    attachment.extend_from_slice(&1_u16.to_le_bytes());
    attachment.extend_from_slice(&0_u16.to_le_bytes());
    attachment.extend_from_slice(b"Attachment\0");
    for value in
        [1.0_f32, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 654.09, -278.139, 0.0, 1.0]
    {
        attachment.extend_from_slice(&value.to_le_bytes());
    }
    let next = animation + attachment.len();
    attachment[9..13].copy_from_slice(&(next as u32).to_le_bytes());
    let end = bytes.len() + attachment.len();
    bytes[animation + 9..animation + 13].copy_from_slice(&(end as u32).to_le_bytes());
    bytes.splice(animation..animation, attachment);
    bytes
}

#[test]
fn attachments_preserve_animation() {
    let original = parse(&fixture()).unwrap();
    let mesh = parse(&attachment_fixture()).unwrap();
    assert_eq!(mesh.animations, original.animations);
    assert_eq!(mesh.attachments.len(), 1);
    let layers = [AnimationLayer { id: 1, rate: 1.0, blend: 1.0, additive: true }];
    assert!((skin(&mesh, &layers, 1.0 / 30.0).positions[0][0] - 11.0).abs() < 0.001);
}

#[test]
fn attachment_offsets_bounded() {
    let original = attachment_fixture();
    let start = find(&original, b"MDAT0001\0").unwrap();
    let animation = find(&original, b"MDLA0006\0").unwrap();
    for end in [0, start, start + 13, animation - 1, animation + 1, original.len() + 1] {
        let mut bytes = original.clone();
        bytes[start + 9..start + 13].copy_from_slice(&(end as u32).to_le_bytes());
        assert!(parse(&bytes).is_err(), "offset {end}");
    }
    for end in start..animation {
        assert!(parse(&original[..end]).is_err(), "length {end}");
    }
}

#[test]
fn attachment_records_validated() {
    let original = attachment_fixture();
    let start = find(&original, b"MDAT0001\0").unwrap();
    for (offset, replacement) in [(13, 0_u16), (13, u16::MAX), (15, 1), (15, u16::MAX)] {
        let mut bytes = original.clone();
        bytes[start + offset..start + offset + 2].copy_from_slice(&replacement.to_le_bytes());
        assert!(parse(&bytes).is_err());
    }
    let mut invalid_matrix = original.clone();
    invalid_matrix[start + 28..start + 32].copy_from_slice(&f32::NAN.to_le_bytes());
    assert!(parse(&invalid_matrix).unwrap_err().to_string().contains("not finite"));
    let mut invalid_name = original;
    let end = find(&invalid_name, b"MDLA0006\0").unwrap();
    invalid_name[start + 17..end].fill(b'A');
    assert!(parse(&invalid_name).unwrap_err().to_string().contains("unterminated"));
}

#[test]
fn attachment_counts_parse() {
    let original = attachment_fixture();
    let start = find(&original, b"MDAT0001\0").unwrap();
    let animation = find(&original, b"MDLA0006\0").unwrap();
    let record = &original[start + 15..animation];
    for count in [0_u16, 2] {
        let mut bytes = original[..start + 13].to_vec();
        bytes.extend_from_slice(&count.to_le_bytes());
        for _ in 0..count {
            bytes.extend_from_slice(record);
        }
        let next = bytes.len();
        bytes[start + 9..start + 13].copy_from_slice(&(next as u32).to_le_bytes());
        bytes.extend_from_slice(&original[animation..]);
        let end = bytes.len() as u32;
        bytes[next + 9..next + 13].copy_from_slice(&end.to_le_bytes());
        let mesh = parse(&bytes).unwrap();
        assert_eq!(mesh.animations, parse(&fixture()).unwrap().animations);
        assert_eq!(mesh.attachments.len(), usize::from(count));
    }
}

#[test]
fn attachment_tracks_pose() {
    let mut mesh = parse(&attachment_fixture()).unwrap();
    mesh.attachments[0].transform.translation = [3.0, 4.0, 0.0];
    mesh.attachments[0].bone = 1;
    let child = Transform { translation: [5.0, 0.0, 0.0], ..mesh.bones[0].bind };
    mesh.bones.push(Bone { parent: Some(0), bind: child });
    mesh.animations[0].tracks.push(vec![child; 3]);
    let track = &mut mesh.animations[0].tracks[0][1];
    track.rotation[2] = std::f32::consts::FRAC_PI_2;
    track.scale = [2.0, 2.0, 1.0];
    let layers = [AnimationLayer { id: 1, rate: 1.0, blend: 1.0, additive: false }];
    let initial = skin(&mesh, &layers, 0.0);
    let moved = skin(&mesh, &layers, 1.0 / 30.0);
    assert_eq!(initial.attachments[0].translation, [8.0, 4.0, 0.0]);
    let attachment = moved.attachments[0];
    assert!((attachment.translation[0] - 2.0).abs() < 0.001);
    assert!((attachment.translation[1] - 16.0).abs() < 0.001);
    assert!((attachment.rotation[2] - std::f32::consts::FRAC_PI_2).abs() < 0.001);
    assert_eq!(attachment.scale, [2.0, 2.0, 1.0]);
}
