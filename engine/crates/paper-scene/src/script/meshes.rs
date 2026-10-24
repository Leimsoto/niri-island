//! Models a SceneScript builds itself (`thisScene.createModelData`) and the
//! layers that show them (`thisScene.createLayer({model})`): vertex and index
//! buffers straight from the script's typed arrays, kept here for the
//! renderer, which draws each visible layer right before the scene layer it
//! was sorted in front of.

use std::collections::HashMap;

/// What a vertex holds, in order (Wallpaper Engine's `IModelData` formats).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum VertexElement {
    Position,
    Normal,
    Tangent,
    Uv,
    Color,
}

impl VertexElement {
    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "position" => Some(Self::Position),
            "normal" => Some(Self::Normal),
            "tangent" => Some(Self::Tangent),
            "uv" => Some(Self::Uv),
            "color" => Some(Self::Color),
            _ => None,
        }
    }

    /// Floats it takes in a vertex.
    pub fn floats(self) -> usize {
        match self {
            Self::Position | Self::Normal => 3,
            Self::Tangent | Self::Color => 4,
            Self::Uv => 2,
        }
    }
}

#[derive(Clone, Debug)]
pub struct MeshShape {
    /// The material's path in the package (`materials/….json`).
    pub material: String,
    pub format: Vec<VertexElement>,
    pub vertices: Vec<f32>,
    pub indices: Vec<u32>,
    /// Bumped on every change: the renderer uploads when it differs.
    pub version: u64,
}

impl MeshShape {
    pub fn stride(&self) -> usize {
        self.format.iter().map(|e| e.floats()).sum::<usize>().max(1)
    }
}

#[derive(Clone, Debug)]
pub struct MeshLayer {
    pub id: u32,
    pub model: u32,
    pub name: String,
    pub visible: bool,
    /// The scene layer (index into the scene's objects) it draws in front of;
    /// none: after everything.
    pub before: Option<usize>,
    /// Sort calls in order: later ones land after earlier ones in front of
    /// the same layer, as Wallpaper Engine's insertions do.
    pub sequence: u64,
}

#[derive(Default, Debug)]
pub struct MeshStore {
    pub models: HashMap<u32, Vec<MeshShape>>,
    /// Files the scripts registered (`engine.registerAsset`): the materials
    /// their models may use, built when the scene loads.
    pub assets: std::collections::BTreeSet<String>,
    pub layers: Vec<MeshLayer>,
    sequence: u64,
    version: u64,
}

/// Bounds against a runaway script: 64 Mi floats of vertices in all.
const MAX_FLOATS: usize = 64 * 1024 * 1024;

impl MeshStore {
    pub fn version(&self) -> u64 {
        self.version
    }

    fn total_floats(&self) -> usize {
        self.models.values().flatten().map(|s| s.vertices.len() + s.indices.len()).sum()
    }

    pub fn set_shape(
        &mut self,
        model: u32,
        index: usize,
        material: String,
        format: Vec<VertexElement>,
        vertices: &[f32],
        indices: Vec<u32>,
    ) -> bool {
        if index > 256 || self.total_floats() + vertices.len() + indices.len() > MAX_FLOATS {
            return false;
        }
        self.version += 1;
        let shapes = self.models.entry(model).or_default();
        if shapes.len() <= index {
            shapes.resize_with(index + 1, || MeshShape {
                material: String::new(),
                format: Vec::new(),
                vertices: Vec::new(),
                indices: Vec::new(),
                version: 0,
            });
        }
        let version = self.version;
        shapes[index] = MeshShape { material, format, vertices: vertices.to_vec(), indices, version };
        true
    }

    /// New vertices for a shape (same layout); a different length is taken too.
    pub fn set_vertices(&mut self, model: u32, index: usize, vertices: &[f32]) -> bool {
        let Some(shape) = self.models.get_mut(&model).and_then(|s| s.get_mut(index)) else {
            return false;
        };
        if vertices.len() > MAX_FLOATS {
            return false;
        }
        if shape.vertices.len() == vertices.len() {
            shape.vertices.copy_from_slice(vertices);
        } else {
            shape.vertices = vertices.to_vec();
        }
        self.version += 1;
        shape.version = self.version;
        true
    }

    pub fn add_layer(&mut self, id: u32, model: u32, name: String) {
        self.sequence += 1;
        self.version += 1;
        self.layers.push(MeshLayer { id, model, name, visible: true, before: None, sequence: self.sequence });
    }

    pub fn set_visible(&mut self, id: u32, visible: bool) {
        if let Some(layer) = self.layers.iter_mut().find(|l| l.id == id) {
            if layer.visible != visible {
                layer.visible = visible;
                self.version += 1;
            }
        }
    }

    pub fn sort(&mut self, id: u32, before: Option<usize>) {
        self.sequence += 1;
        let sequence = self.sequence;
        if let Some(layer) = self.layers.iter_mut().find(|l| l.id == id) {
            layer.before = before;
            layer.sequence = sequence;
            self.version += 1;
        }
    }

    /// The visible layers in drawing order.
    pub fn ordered(&self) -> Vec<&MeshLayer> {
        let mut out: Vec<&MeshLayer> = self.layers.iter().filter(|l| l.visible).collect();
        out.sort_by_key(|l| (l.before.unwrap_or(usize::MAX), l.sequence));
        out
    }
}
