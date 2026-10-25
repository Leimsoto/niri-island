//! Models scripts build (`thisScene.createModelData`): each frame the
//! script's buffers go to the GPU, and every visible model layer becomes a
//! draw placed right before the scene layer it was sorted in front of,
//! alongside particles (so snapshots of "the scene so far" include them).

use std::collections::{BTreeMap, HashMap};

use anyhow::{Context, Result};
use paper_scene::script::VertexElement;

use super::{FxTargetId, FxTargetStorage, Group, LayerFx, OrderedParticleQuad, size4, vk};

/// Where a material's texture slot reads from.
enum MeshSlot {
    None,
    Texture(usize),
    Target(String),
}

pub(super) struct MeshMaterialGpu {
    pass: paper_scene::effects::EffectPass,
    meta: paper_scene::effects::PassMeta,
    slots: Vec<MeshSlot>,
    blend: paper_scene::particles::ParticleBlend,
    depth_test: bool,
    depth_write: bool,
    cull_back: bool,
    /// What the vertex shader takes: (location, name, components).
    attributes: Vec<(u32, String, u32)>,
}

struct MeshShapeGpu {
    version: u64,
    vertices: vk::DynBuffer,
    vertex_bytes: usize,
    indices: vk::DynBuffer,
    index_bytes: usize,
    index_count: u32,
}

/// One frame's model draw: before scene order `order`, with this pipeline.
pub(super) struct MeshDraw {
    order: usize,
    pipeline: (String, Vec<VertexElement>),
    shape: (u32, usize),
}

pub(super) struct Meshes {
    materials: HashMap<String, MeshMaterialGpu>,
    pipelines: HashMap<(String, Vec<VertexElement>), vk::EffectPipeline>,
    failed: std::collections::HashSet<(String, Vec<VertexElement>)>,
    shapes: HashMap<(u32, usize), MeshShapeGpu>,
    zero: vk::DynBuffer,
    object_order: BTreeMap<usize, usize>,
    pub(super) draws: Vec<MeshDraw>,
}

// `layout(location = N) in vecK name;` lines of a translated vertex shader.
fn shader_attributes(source: &str) -> Vec<(u32, String, u32)> {
    let mut out = Vec::new();
    for line in source.lines() {
        let line = line.trim();
        let Some(rest) = line.strip_prefix("layout(location = ") else { continue };
        let Some((location, rest)) = rest.split_once(')') else { continue };
        let Some(decl) = rest.trim().strip_prefix("in ") else { continue };
        let mut parts = decl.trim_end_matches(';').split_whitespace();
        let (Some(ty), Some(name)) = (parts.next(), parts.next()) else { continue };
        let width = match ty {
            "float" => 1,
            "vec2" => 2,
            "vec3" => 3,
            "vec4" => 4,
            _ => continue,
        };
        if let Ok(location) = location.trim().parse() {
            out.push((location, name.to_owned(), width));
        }
    }
    out
}

fn element_of(name: &str) -> Option<VertexElement> {
    match name {
        "a_Position" => Some(VertexElement::Position),
        "a_Normal" => Some(VertexElement::Normal),
        "a_Tangent" | "a_Tangent4" => Some(VertexElement::Tangent),
        "a_TexCoord" | "a_TexCoordVec4" => Some(VertexElement::Uv),
        "a_Color" => Some(VertexElement::Color),
        _ => None,
    }
}

fn float_format(components: u32) -> ash::vk::Format {
    match components {
        1 => ash::vk::Format::R32_SFLOAT,
        2 => ash::vk::Format::R32G32_SFLOAT,
        3 => ash::vk::Format::R32G32B32_SFLOAT,
        _ => ash::vk::Format::R32G32B32A32_SFLOAT,
    }
}

impl MeshMaterialGpu {
    fn layout(&self, format: &[VertexElement]) -> vk::MeshLayout {
        let stride: usize = format.iter().map(|e| e.floats()).sum();
        let attributes = self
            .attributes
            .iter()
            .map(|(location, name, width)| {
                let mut offset = 0;
                for element in format {
                    if Some(*element) == element_of(name) {
                        let components = (*width).min(element.floats() as u32);
                        return (*location, float_format(components), offset as u32 * 4, 0);
                    }
                    offset += element.floats();
                }
                (*location, float_format(*width), 0, 1)  // not in the model: zeros
            })
            .collect();
        vk::MeshLayout {
            stride: (stride.max(1) * 4) as u32,
            attributes,
            blend: self.blend,
            depth_test: self.depth_test,
            depth_write: self.depth_write,
            cull_back: self.cull_back,
        }
    }
}

impl Meshes {
    /// The scene's model materials, with their asset textures uploaded.
    pub(super) fn new(
        renderer: &mut vk::Renderer,
        materials: BTreeMap<String, paper_scene::effects::MeshMaterial>,
        object_order: BTreeMap<usize, usize>,
        mut intern: impl FnMut(&mut vk::Renderer, &mut paper_scene::model::Texture) -> Option<usize>,
    ) -> Result<Self> {
        let mut out = HashMap::new();
        for (path, mut material) in materials {
            let count = material
                .pass
                .vertex
                .samplers
                .iter()
                .chain(material.pass.fragment.samplers.iter())
                .map(|s| s.index as usize + 1)
                .max()
                .unwrap_or(0)
                .max(material.pass.textures.len());
            let mut slots: Vec<MeshSlot> = (0..count).map(|_| MeshSlot::None).collect();
            for (slot, texture) in material.pass.textures.iter_mut().enumerate() {
                if let Some(texture) = texture
                    && let Some(index) = intern(renderer, texture)
                {
                    slots[slot] = MeshSlot::Texture(index);
                }
            }
            for (slot, bind) in &material.pass.binds {
                if let paper_scene::effects::EffectBind::Named(name) = bind
                    && *slot < slots.len()
                {
                    slots[*slot] = MeshSlot::Target(name.clone());
                }
            }
            let attributes = shader_attributes(&material.pass.vertex.source);
            out.insert(
                path,
                MeshMaterialGpu {
                    meta: paper_scene::effects::PassMeta::of(&material.pass),
                    pass: material.pass,
                    slots,
                    blend: material.blend,
                    depth_test: material.depth_test,
                    depth_write: material.depth_write,
                    cull_back: material.cull_back,
                    attributes,
                },
            );
        }
        let zero = renderer.create_dyn_buffer(64, ash::vk::BufferUsageFlags::VERTEX_BUFFER)?;
        renderer.write_dyn_buffer(&zero, &[0u8; 64]);
        Ok(Self {
            materials: out,
            pipelines: HashMap::new(),
            failed: std::collections::HashSet::new(),
            shapes: HashMap::new(),
            zero,
            object_order,
            draws: Vec::new(),
        })
    }

    pub(super) fn destroy(self, renderer: &mut vk::Renderer) {
        for (_, pipeline) in self.pipelines {
            renderer.destroy_effect_pipeline(pipeline);
        }
        for (_, shape) in self.shapes {
            renderer.destroy_dyn_buffer(shape.vertices);
            renderer.destroy_dyn_buffer(shape.indices);
        }
        renderer.destroy_dyn_buffer(self.zero);
    }

    // The order a model sorted in front of object `before` draws at.
    fn order_before(&self, before: Option<usize>) -> usize {
        match before {
            Some(object) => self.object_order.range(object..).next().map_or(usize::MAX, |(_, order)| *order),
            None => usize::MAX,
        }
    }
}

// A buffer big enough for `bytes` (grown in steps; the old one waits for the GPU).
fn ensure_buffer(
    renderer: &mut vk::Renderer,
    buffer: &mut vk::DynBuffer,
    capacity: &mut usize,
    bytes: usize,
    usage: ash::vk::BufferUsageFlags,
) -> Result<()> {
    if bytes <= *capacity {
        return Ok(());
    }
    let size = bytes.next_power_of_two().max(256);
    let fresh = renderer.create_dyn_buffer(size, usage)?;
    // SAFETY: waits for the GPU to be done with the old buffer
    let _ = unsafe { renderer.device.device_wait_idle() };
    let old = std::mem::replace(buffer, fresh);
    renderer.destroy_dyn_buffer(old);
    *capacity = size;
    Ok(())
}

fn as_bytes<T: Copy>(values: &[T]) -> &[u8] {
    // SAFETY: plain numbers, read as the bytes they are
    unsafe { std::slice::from_raw_parts(values.as_ptr().cast(), std::mem::size_of_val(values)) }
}

impl LayerFx {
    /// A target declared by name and not unique (`"unique": false`): one per
    /// scene, written by one layer, read by others (and by models).
    pub(super) fn shared_name(&self, target: FxTargetId) -> bool {
        match target {
            FxTargetId::Fbo(index) => {
                self.fbo_owners.get(index).is_some_and(Option::is_none)
                    && self.fbos.get(index).is_some_and(|(name, _)| name.starts_with("_rt_"))
            }
            _ => false,
        }
    }
}

impl Group {
    /// Effects that declare the same non-unique `_rt_` target share one: the
    /// first declaration's image stays (moved among the shared ones), the
    /// others go and point at it. Runs after scratch aliasing, which leaves
    /// these alone.
    pub(super) fn share_named_targets(&mut self) {
        let mut shared: HashMap<String, usize> = HashMap::new();
        for layer in 0..self.fx.len() {
            for index in 0..self.fx[layer].fbos.len() {
                if !self.fx[layer].shared_name(FxTargetId::Fbo(index)) {
                    continue;
                }
                let name = self.fx[layer].fbos[index].0.clone();
                let storage = &mut self.fx[layer].fbos[index].1;
                match shared.get(&name) {
                    Some(&slot) => {
                        if let Some(target) = storage.take_owned() {
                            self.renderer.destroy_scene_target(target);
                        }
                        *storage = FxTargetStorage::Shared(slot);
                    }
                    None => {
                        if let Some(target) = storage.take_owned() {
                            self.fx_scratch.push(target);
                            let slot = self.fx_scratch.len() - 1;
                            *storage = FxTargetStorage::Shared(slot);
                            shared.insert(name, slot);
                        }
                    }
                }
            }
        }
        if !shared.is_empty() {
            tracing::info!(targets = shared.len(), "skwd-wall-vk: named effect targets shared across layers");
        }
        self.shared_targets = shared;
    }

    /// Debugging: every shared target as a PNG in `dir`.
    pub(super) fn dump_shared_targets(&mut self, dir: &std::path::Path) {
        let _ = std::fs::create_dir_all(dir);
        if let Some(snapshot) = &self.scene_snapshot
            && let Ok((w, h, rgba)) = self.renderer.read_scene_target(snapshot)
            && let Some(image) = image::RgbaImage::from_raw(w, h, rgba)
        {
            let _ = image.save(dir.join("snapshot.png"));
        }
        let mut names: Vec<(String, usize)> = self.shared_targets.iter().map(|(n, &i)| (n.clone(), i)).collect();
        names.sort();
        for (name, index) in names {
            let Some(target) = self.fx_scratch.get(index) else { continue };
            match self.renderer.read_scene_target(target) {
                Ok((w, h, rgba)) => {
                    if let Some(image) = image::RgbaImage::from_raw(w, h, rgba) {
                        let _ = image.save(dir.join(format!("{name}.png")));
                    }
                }
                Err(err) => tracing::warn!("dump {name}: {err:#}"),
            }
        }
    }

    /// After the scripts' frame: models to the GPU, uniforms written, and
    /// this frame's draws listed.
    pub(super) fn mesh_frame(&mut self, time: f32, dt: f32) -> Result<()> {
        let Some(meshes) = self.meshes.as_mut() else { return Ok(()) };
        meshes.draws.clear();
        let Some(host) = self.scripts.as_ref().and_then(|s| s.host.as_ref()) else { return Ok(()) };
        let store = host.meshes();
        let clock = match self.fixed_daytime {
            Some(daytime) => paper_scene::effects::FrameClock { time, dt, daytime },
            None => paper_scene::effects::FrameClock::now(time, dt),
        };
        let screen = (self.target.extent.width, self.target.extent.height);
        let format = self.target.format;
        let mut written = std::collections::HashSet::new();
        for layer in store.ordered() {
            let order = meshes.order_before(layer.before);
            let Some(shapes) = store.models.get(&layer.model) else { continue };
            for (index, shape) in shapes.iter().enumerate() {
                if shape.indices.is_empty() || shape.vertices.is_empty() {
                    continue;
                }
                let key = (shape.material.clone(), shape.format.clone());
                if meshes.failed.contains(&key) {
                    continue;
                }
                let Some(material) = meshes.materials.get(&shape.material) else {
                    tracing::warn!("skwd-wall-vk: script model material {} not loaded", shape.material);
                    meshes.failed.insert(key);
                    continue;
                };
                if !meshes.pipelines.contains_key(&key) {
                    let layout = material.layout(&shape.format);
                    let label = format!("model:{}", shape.material);
                    match self.renderer.create_mesh_pipeline(
                        &material.pass.vertex,
                        &material.pass.fragment,
                        material.pass.hlsl.as_ref(),
                        &label,
                        layout,
                        format,
                    ) {
                        Ok(pipeline) => {
                            meshes.pipelines.insert(key.clone(), pipeline);
                        }
                        Err(err) => {
                            tracing::warn!("skwd-wall-vk: script model {label} skipped: {err:#}");
                            meshes.failed.insert(key);
                            continue;
                        }
                    }
                }
                // buffers: uploaded when the script changed them
                let id = (layer.model, index);
                if !meshes.shapes.contains_key(&id) {
                    let vertices = self.renderer.create_dyn_buffer(256, ash::vk::BufferUsageFlags::VERTEX_BUFFER)?;
                    let indices = self.renderer.create_dyn_buffer(256, ash::vk::BufferUsageFlags::INDEX_BUFFER)?;
                    meshes.shapes.insert(
                        id,
                        MeshShapeGpu { version: 0, vertices, vertex_bytes: 256, indices, index_bytes: 256, index_count: 0 },
                    );
                }
                let gpu = meshes.shapes.get_mut(&id).context("model shape")?;
                if gpu.version != shape.version {
                    let vertex_bytes = as_bytes(&shape.vertices);
                    let index_bytes = as_bytes(&shape.indices);
                    ensure_buffer(&mut self.renderer, &mut gpu.vertices, &mut gpu.vertex_bytes, vertex_bytes.len(), ash::vk::BufferUsageFlags::VERTEX_BUFFER)?;
                    ensure_buffer(&mut self.renderer, &mut gpu.indices, &mut gpu.index_bytes, index_bytes.len(), ash::vk::BufferUsageFlags::INDEX_BUFFER)?;
                    self.renderer.write_dyn_buffer(&gpu.vertices, vertex_bytes);
                    self.renderer.write_dyn_buffer(&gpu.indices, index_bytes);
                    gpu.index_count = shape.indices.len() as u32;
                    gpu.version = shape.version;
                }
                // uniforms and textures, once per pipeline per frame
                if written.insert(key.clone()) {
                    let pipeline = &meshes.pipelines[&key];
                    let fallback = &self.textures[0];
                    let mut views = Vec::with_capacity(material.slots.len());
                    let mut sizes = Vec::with_capacity(material.slots.len());
                    for slot in &material.slots {
                        match slot {
                            MeshSlot::Texture(t) => {
                                let texture = &self.textures[*t];
                                views.push((texture.view, texture.sampler));
                                sizes.push(None);
                            }
                            MeshSlot::Target(name) => match self.shared_targets.get(name).map(|&i| &self.fx_scratch[i]) {
                                Some(target) => {
                                    views.push((target.view, target.sampler));
                                    sizes.push(Some(size4(target.extent)));
                                }
                                None => {
                                    views.push((fallback.view, fallback.sampler));
                                    sizes.push(None);
                                }
                            },
                            MeshSlot::None => {
                                views.push((fallback.view, fallback.sampler));
                                sizes.push(None);
                            }
                        }
                    }
                    // (drawn with a flipped viewport: matrices the Direct3D way too)
                    let d3d_clip = true;
                    let _ = pipeline;
                    let uniforms = material.meta.uniform_bytes_with(
                        clock, None, screen.0, screen.1, screen, &sizes, &self.scene_uniforms, d3d_clip,
                    );
                    self.renderer.write_effect_inputs(&meshes.pipelines[&key], &views, &uniforms);
                }
                meshes.draws.push(MeshDraw { order, pipeline: key, shape: id });
            }
        }
        if std::env::var_os("SKWD_MESH_DEBUG").is_some() {
            let layers: Vec<String> = store.ordered().iter().map(|l| format!("{}@{:?}->{}", l.name, l.before, meshes.order_before(l.before))).collect();
            tracing::info!(draws = meshes.draws.len(), pipelines = meshes.pipelines.len(), failed = meshes.failed.len(), ?layers, "skwd-wall-vk: model frame");
        }
        Ok(())
    }
}

/// This frame's models due in a range of the scene (like particle_batches:
/// `after` counts the quads drawn before each).
pub(super) fn mesh_batches<'a>(
    meshes: Option<&'a Meshes>,
    image_orders: &[usize],
    particle_quads: &[OrderedParticleQuad],
    from: usize,
    before: Option<usize>,
) -> Vec<vk::ParticleDraw<'a>> {
    let Some(meshes) = meshes else { return Vec::new() };
    meshes
        .draws
        .iter()
        .filter(|draw| (draw.order > from || from == 0) && before.is_none_or(|limit| draw.order <= limit))
        .filter_map(|draw| {
            let pipeline = meshes.pipelines.get(&draw.pipeline)?;
            let shape = meshes.shapes.get(&draw.shape)?;
            let after = image_orders
                .iter()
                .filter(|o| **o >= from && before.is_none_or(|limit| **o < limit) && **o < draw.order)
                .count()
                + particle_quads
                    .iter()
                    .filter(|q| {
                        q.scene_order >= from && before.is_none_or(|limit| q.scene_order < limit) && q.scene_order < draw.order
                    })
                    .count();
            Some(vk::ParticleDraw {
                after,
                pipeline,
                vertices: shape.vertices.buffer,
                indices: shape.indices.buffer,
                index_count: shape.index_count,
                grab: None,
                mesh: Some(meshes.zero.buffer),
                label: draw.pipeline.0.as_str(),
            })
        })
        .collect()
}
