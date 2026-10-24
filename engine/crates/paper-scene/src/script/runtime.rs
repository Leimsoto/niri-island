use super::js::{Arg, Js};
use super::source;
use anyhow::{Result, anyhow};
use serde_json::Value;
use std::cell::RefCell;
use std::rc::Rc;
use std::time::{Duration, Instant};

// (scripts that build their own geometry keep it in typed arrays: room for them)
const HEAP_LIMIT: usize = 128 * 1024 * 1024;
const BUFFER_LIMIT: usize = 128 * 1024 * 1024;
const FRAME_BUDGET: Duration = Duration::from_millis(100);
// Scripts may build a whole scene in init (geometry, tables): Wallpaper Engine
// gives them time once, at load. Frames keep their 100 ms.
const MODULE_INIT_BUDGET: Duration = Duration::from_secs(4);
const INIT_BUDGET: Duration = Duration::from_secs(8);

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SoundOp {
    Play,
    Stop,
    Pause,
    Gain(f32),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SpriteOp {
    Play,
    Stop,
    Pause,
    Frame(usize),
    Rate(f32),
    Join,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ScriptCommand {
    Sound { id: String, op: SoundOp },
    Sprite { object: usize, op: SpriteOp },
}

fn parse_command(kind: &str, target: &str, op: &str, value: &Value) -> Option<ScriptCommand> {
    let number = value.as_f64().map(|v| v as f32);
    match kind {
        "sound" => {
            let op = match op {
                "play" => SoundOp::Play,
                "stop" => SoundOp::Stop,
                "pause" => SoundOp::Pause,
                _ => return None,
            };
            Some(ScriptCommand::Sound { id: target.to_owned(), op })
        }
        "sprite" => {
            let op = match op {
                "play" => SpriteOp::Play,
                "stop" => SpriteOp::Stop,
                "pause" => SpriteOp::Pause,
                "join" => SpriteOp::Join,
                "frame" => SpriteOp::Frame(number?.max(0.0) as usize),
                "rate" => SpriteOp::Rate(number?),
                _ => return None,
            };
            Some(ScriptCommand::Sprite { object: target.parse().ok()?, op })
        }
        _ => None,
    }
}

pub struct SceneScripts {
    property_bindings: super::properties::PropertyBindings,
    properties_changed: bool,
    storage: Option<super::Storage>,
    commands: Vec<ScriptCommand>,
    general: Vec<(String, Value)>,
    js: RefCell<Js>,
    pub scene: Value,
    pub properties: crate::model::Properties,
    updates: Vec<usize>,
    pub diagnostics: Vec<String>,
    pub bindings: usize,
    disabled: bool,
    audio_registered: bool,
    timers_pending: bool,
    meshes: Rc<RefCell<super::MeshStore>>,
}

// ── native functions (their state lives in the isolate's slots) ──

type Store = Rc<RefCell<super::MeshStore>>;

fn arg_u32(scope: &mut v8::PinScope<'_, '_>, args: &v8::FunctionCallbackArguments, i: i32) -> u32 {
    args.get(i).uint32_value(scope).unwrap_or(0)
}

fn arg_string(scope: &mut v8::PinScope<'_, '_>, args: &v8::FunctionCallbackArguments, i: i32) -> String {
    args.get(i).to_rust_string_lossy(scope)
}

// A typed array's bytes, copied out (Float32Array: f32; indices: u32/u16; or a plain array).
fn view_bytes(value: v8::Local<'_, v8::Value>) -> Option<Vec<u8>> {
    let view = v8::Local::<v8::ArrayBufferView>::try_from(value).ok()?;
    let mut bytes = vec![0u8; view.byte_length()];
    view.copy_contents(&mut bytes);
    Some(bytes)
}

fn floats_of(value: v8::Local<'_, v8::Value>) -> Option<Vec<f32>> {
    if !value.is_float32_array() {
        return None;
    }
    Some(view_bytes(value)?.chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect())
}

fn indices_of(scope: &mut v8::PinScope<'_, '_>, value: v8::Local<'_, v8::Value>) -> Vec<u32> {
    if value.is_uint32_array() {
        return view_bytes(value)
            .map(|b| b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect())
            .unwrap_or_default();
    }
    if value.is_uint16_array() {
        return view_bytes(value)
            .map(|b| b.chunks_exact(2).map(|c| u32::from(u16::from_le_bytes([c[0], c[1]]))).collect())
            .unwrap_or_default();
    }
    if let Ok(array) = v8::Local::<v8::Array>::try_from(value) {
        return (0..array.length())
            .filter_map(|i| array.get_index(scope, i))
            .map(|v| v.number_value(scope).unwrap_or(0.0).max(0.0) as u32)
            .collect();
    }
    Vec::new()
}

fn native_log(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, _rv: v8::ReturnValue<v8::Value>) {
    let message = arg_string(scope, &args, 0);
    tracing::debug!("SceneScript: {}", message.chars().take(1024).collect::<String>());
}

fn native_mesh_shape(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, mut rv: v8::ReturnValue<v8::Value>) {
    let model = arg_u32(scope, &args, 0);
    let shape = arg_u32(scope, &args, 1) as usize;
    let material = arg_string(scope, &args, 2);
    let mut format = Vec::new();
    if let Ok(list) = v8::Local::<v8::Array>::try_from(args.get(3)) {
        for i in 0..list.length() {
            if let Some(item) = list.get_index(scope, i) {
                format.extend(super::VertexElement::parse(&item.to_rust_string_lossy(scope)));
            }
        }
    }
    let ok = match floats_of(args.get(4)) {
        Some(vertices) => {
            let indices = indices_of(scope, args.get(5));
            scope.get_slot::<Store>().is_some_and(|store| {
                store.borrow_mut().set_shape(model, shape, material, format, &vertices, indices)
            })
        }
        None => false,
    };
    rv.set_bool(ok);
}

fn native_mesh_vertices(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, mut rv: v8::ReturnValue<v8::Value>) {
    let model = arg_u32(scope, &args, 0);
    let shape = arg_u32(scope, &args, 1) as usize;
    let ok = floats_of(args.get(2)).is_some_and(|vertices| {
        scope.get_slot::<Store>().is_some_and(|store| store.borrow_mut().set_vertices(model, shape, &vertices))
    });
    rv.set_bool(ok);
}

fn native_register_asset(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, _rv: v8::ReturnValue<v8::Value>) {
    let file = arg_string(scope, &args, 0);
    if let Some(store) = scope.get_slot::<Store>() {
        let mut store = store.borrow_mut();
        if store.assets.len() < 4096 {
            store.assets.insert(file);
        }
    }
}

fn native_mesh_layer(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, _rv: v8::ReturnValue<v8::Value>) {
    let (id, model, name) = (arg_u32(scope, &args, 0), arg_u32(scope, &args, 1), arg_string(scope, &args, 2));
    if let Some(store) = scope.get_slot::<Store>() {
        store.borrow_mut().add_layer(id, model, name);
    }
}

fn native_mesh_layer_visible(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, _rv: v8::ReturnValue<v8::Value>) {
    let id = arg_u32(scope, &args, 0);
    let visible = args.get(1).boolean_value(scope);
    if let Some(store) = scope.get_slot::<Store>() {
        store.borrow_mut().set_visible(id, visible);
    }
}

fn native_mesh_layer_sort(scope: &mut v8::PinScope, args: v8::FunctionCallbackArguments, _rv: v8::ReturnValue<v8::Value>) {
    let id = arg_u32(scope, &args, 0);
    let before = args.get(1).int32_value(scope).and_then(|i| usize::try_from(i).ok());
    if let Some(store) = scope.get_slot::<Store>() {
        store.borrow_mut().sort(id, before);
    }
}

const WE_MATH: &str = "export const mix=(a,b,t)=>a+(b-a)*t; export const clamp=(x,a,b)=>Math.min(b,Math.max(a,x)); export const smoothstep=(a,b,x)=>{const t=clamp((x-a)/(b-a),0,1);return t*t*(3-2*t)}; export const random=(a=0,b=1)=>mix(a,b,Math.random()); export const radians=x=>x*Math.PI/180; export const degrees=x=>x*180/Math.PI; export const deg2rad=radians; export const rad2deg=degrees; export const smoothStep=smoothstep;";
const WE_VECTOR: &str = "export const lerp=(a,b,t)=>a.add(b.subtract(a).multiply(t));";

impl SceneScripts {
    pub fn load(
        scene: &mut Value,
        props: &crate::model::Properties,
        project: &Value,
    ) -> Result<Option<Self>> {
        Self::load_with_storage(scene, props, project, None)
    }

    pub fn load_with_storage(
        scene: &mut Value,
        props: &crate::model::Properties,
        project: &Value,
        storage: Option<super::Storage>,
    ) -> Result<Option<Self>> {
        if !source::present(scene) || !source::needs_runtime(scene, "") {
            return Ok(None);
        }
        let mut bindings = Vec::new();
        let original = scene.clone();
        source::collect(scene, "", &mut bindings, props);
        let property_bindings =
            super::properties::PropertyBindings::new(&original, project, &bindings);
        if bindings.len() > 4096
            || bindings.iter().map(|b| b.source.len()).sum::<usize>() > 4 * 1024 * 1024
        {
            return Err(anyhow!("SceneScript source exceeds the scene budget"));
        }
        let mut js = Js::new(HEAP_LIMIT, BUFFER_LIMIT)?;
        let meshes: Store = Rc::new(RefCell::new(super::MeshStore::default()));
        js.set_slot(meshes.clone());
        let mut host = Self {
            property_bindings,
            properties_changed: false,
            storage,
            commands: Vec::new(),
            general: Vec::new(),
            js: RefCell::new(js),
            scene: Value::Null,
            properties: props.clone(),
            updates: Vec::new(),
            diagnostics: Vec::new(),
            bindings: bindings.len(),
            disabled: false,
            audio_registered: false,
            timers_pending: false,
            meshes,
        };
        host.deadline(Duration::from_secs(2));
        host.setup(scene, props)?;
        let initialization_deadline = Instant::now() + INIT_BUDGET;
        for (index, binding) in bindings.iter().enumerate() {
            if Instant::now() >= initialization_deadline {
                host.diagnostics
                    .push("remaining scripts skipped: initialization budget exhausted".into());
                break;
            }
            host.js.borrow().set_deadline(initialization_deadline.min(Instant::now() + MODULE_INIT_BUDGET));
            if let Err(error) = host.compile(index, binding) {
                host.diagnostics.push(format!("{}: {error:#}", binding.path));
                host.disable_module(index);
            }
        }
        host.apply_user_properties();
        host.scene = scene.clone();
        host.drain()?;
        host.tick(0.0, 0.0, [0.5; 2])?;
        for diagnostic in &host.diagnostics {
            tracing::warn!("SceneScript: {diagnostic}");
        }
        tracing::info!(
            bindings = host.bindings,
            heap_bytes = host.heap_bytes(),
            models = host.meshes.borrow().models.len(),
            model_layers = host.meshes.borrow().layers.len(),
            "SceneScript runtime created"
        );
        *scene = host.scene.clone();
        Ok(Some(host))
    }

    fn deadline(&self, budget: Duration) {
        self.js.borrow().set_deadline(Instant::now() + budget);
    }

    fn call(&self, name: &str, args: &[Arg<'_>]) -> Result<()> {
        self.js.borrow_mut().call(name, args)
    }

    fn setup(&self, scene: &Value, props: &crate::model::Properties) -> Result<()> {
        let mut resolved = scene.clone();
        source::resolve_wrappers(&mut resolved, props);
        let mut js = self.js.borrow_mut();
        js.set_function("__log", native_log)?;
        js.set_function("__meshShape", native_mesh_shape)?;
        js.set_function("__meshVertices", native_mesh_vertices)?;
        js.set_function("__meshLayer", native_mesh_layer)?;
        js.set_function("__registerAsset", native_register_asset)?;
        js.set_function("__meshLayerVisible", native_mesh_layer_visible)?;
        js.set_function("__meshLayerSort", native_mesh_layer_sort)?;
        js.eval(include_str!("bootstrap.js"))?;
        js.eval(include_str!("storage.js"))?;
        if let Some(storage) = &self.storage {
            js.call("__storageLoad", &[Arg::Str(&storage.data.to_string())])?;
        }
        let values = self.property_bindings.user_properties(props);
        js.call("__setup", &[Arg::Str(&resolved.to_string()), Arg::Str(&values.to_string())])?;
        js.declare_module("WEColor", include_str!("color.js"))?;
        js.declare_module("WEMath", WE_MATH)?;
        js.declare_module("WEVector", WE_VECTOR)?;
        Ok(())
    }

    fn compile(&mut self, index: usize, binding: &source::Binding) -> Result<()> {
        let mut properties = binding.properties.clone();
        source::resolve_wrappers(&mut properties, &self.properties);
        let layer = binding.layer.map_or("undefined".to_owned(), |i| format!("__layers[{i}]"));
        let source = format!(
            "const thisLayer = {layer}; const thisObject = __owner({})[0]; const createScriptProperties = () => __createScriptProperties({properties}, {index});\n{}",
            serde_json::to_string(&binding.path).unwrap(),
            binding.source
        );
        let has_update = {
            let mut js = self.js.borrow_mut();
            let has_update = js.run_module(&format!("scene-{index}.js"), &source, "__register", index, &binding.path)?;
            js.call("__invoke", &[Arg::Num(index as f64), Arg::Str("init")])?;
            has_update
        };
        if has_update {
            self.updates.push(index);
        }
        Ok(())
    }

    fn apply_user_properties(&mut self) {
        self.deadline(Duration::from_millis(100));
        if let Err(error) = self.call("__applyUserProperties", &[]) {
            self.diagnostics.push(format!("applyUserProperties: {error:#}"));
        }
    }

    pub(crate) fn restrict_property_updates(
        &mut self,
        package: &crate::pkg::Package,
        assets: &crate::effects::Assets,
    ) {
        self.property_bindings.restrict_package(package);
        self.property_bindings.restrict_assets(assets.property_dependencies().as_ref());
    }

    pub fn update_properties(&mut self, overrides: &crate::model::Properties) -> Result<bool> {
        if self.disabled {
            return Ok(false);
        }
        let desired = self.property_bindings.desired(overrides);
        let Some(update) = self.property_bindings.update(&self.properties, &desired) else {
            return Ok(false);
        };
        self.deadline(Duration::from_millis(100));
        self.call("__updateUserProperties", &[Arg::Str(&update.to_string())])?;
        self.properties = desired;
        self.properties_changed = true;
        Ok(true)
    }

    pub fn restrict_hidden_layer_updates(&mut self, layers: &[String]) {
        if layers.is_empty() {
            return;
        }
        self.deadline(FRAME_BUDGET);
        let callbacks = self
            .js
            .borrow_mut()
            .eval_bool("__modules.some(m => m && !m.disabled && typeof m.ns.applyUserProperties === 'function')")
            .unwrap_or(true);
        self.property_bindings.restrict_hidden_layers(&self.scene, layers, callbacks);
    }

    pub fn set_keyframes(&mut self, bindings: &crate::keyframe::Bindings) -> Result<()> {
        self.deadline(FRAME_BUDGET);
        let paths = serde_json::to_string(&bindings.paths())?;
        self.call("__setKeyframes", &[Arg::Str(&paths)]).map_err(|error| anyhow!("keyframe bindings: {error}"))
    }

    pub fn sync_keyframes(&mut self, bindings: &crate::keyframe::Bindings) -> Result<()> {
        if bindings.is_empty() {
            return Ok(());
        }
        self.deadline(FRAME_BUDGET);
        for (index, values, count) in bindings.values() {
            self.call(
                "__keyframe",
                &[
                    Arg::Num(index as f64),
                    Arg::Num(f64::from(values[0])),
                    Arg::Num(f64::from(values[1])),
                    Arg::Num(f64::from(values[2])),
                    Arg::Num(f64::from(values[3])),
                    Arg::Num(count as f64),
                ],
            )
            .map_err(|error| anyhow!("keyframe values: {error}"))?;
        }
        Ok(())
    }

    pub fn set_sprites(&mut self, sprites: &[(usize, usize, f32)]) {
        if self.disabled || sprites.is_empty() {
            return;
        }
        let json = serde_json::to_string(sprites).unwrap_or_else(|_| "[]".into());
        self.deadline(FRAME_BUDGET);
        if let Err(error) = self.call("__setSprites", &[Arg::Str(&json)]) {
            self.stop(format!("sprite table stopped: {error:#}"));
        }
    }

    pub fn take_commands(&mut self) -> Vec<ScriptCommand> {
        std::mem::take(&mut self.commands)
    }

    pub fn take_general_changes(&mut self) -> Vec<(String, Value)> {
        std::mem::take(&mut self.general)
    }

    pub fn destroy(&mut self) {
        if self.disabled {
            return;
        }
        self.deadline(FRAME_BUDGET);
        let result = self.call("__destroyAll", &[]);
        match result.and_then(|()| self.drain()) {
            Ok(_) => {}
            Err(error) => self.diagnostics.push(format!("destroy: {error:#}")),
        }
    }

    pub fn needs_audio(&self) -> bool {
        !self.disabled && self.audio_registered
    }

    pub fn audio(&mut self, count: usize, left: &[f32], right: &[f32]) -> Result<()> {
        if self.disabled {
            return Ok(());
        }
        self.deadline(FRAME_BUDGET);
        if let Err(error) = self.call("__audioUpdate", &[Arg::Num(count as f64), Arg::F32s(left), Arg::F32s(right)]) {
            self.stop(format!("audio stopped: {error:#}"));
        }
        Ok(())
    }

    pub fn needs_media(&self) -> bool {
        self.deadline(FRAME_BUDGET);
        self.js.borrow_mut().call_bool("__needsMedia", &[]).unwrap_or(false)
    }

    pub fn media(&mut self, events: &Value) -> Result<()> {
        if self.disabled {
            return Ok(());
        }
        self.deadline(FRAME_BUDGET);
        if let Err(error) = self.call("__media", &[Arg::Str(&events.to_string())]) {
            self.stop(format!("media event stopped: {error:#}"));
        }
        Ok(())
    }

    pub fn pointer(
        &mut self,
        position: [f32; 2],
        buttons: [bool; 3],
        hits: Vec<String>,
    ) -> Result<()> {
        if self.disabled {
            return Ok(());
        }
        self.deadline(FRAME_BUDGET);
        let args = [
            Arg::Num(f64::from(position[0])),
            Arg::Num(f64::from(position[1])),
            Arg::Bools(&buttons),
            Arg::Strs(&hits),
        ];
        if let Err(error) = self.call("__pointer", &args) {
            self.stop(format!("cursor event stopped: {error:#}"));
        }
        Ok(())
    }

    pub fn animated(&self) -> bool {
        !self.disabled && (!self.updates.is_empty() || self.timers_pending)
    }

    /// The models and layers the scripts built (`createModelData`).
    pub fn meshes(&self) -> std::cell::Ref<'_, super::MeshStore> {
        self.meshes.borrow()
    }

    pub fn heap_bytes(&self) -> usize {
        self.js.borrow_mut().heap_bytes()
    }

    pub fn tick(&mut self, time: f32, dt: f32, pointer: [f32; 2]) -> Result<bool> {
        if self.disabled {
            return Ok(false);
        }
        let started = Instant::now();
        self.deadline(FRAME_BUDGET);
        let expected = self.updates.len();
        let mut invoked = 0;
        let mut failed = Vec::new();
        let result = (|| -> Result<()> {
            self.call(
                "__frame",
                &[Arg::Num(f64::from(time)), Arg::Num(f64::from(dt)), Arg::Num(f64::from(pointer[0])), Arg::Num(f64::from(pointer[1]))],
            )?;
            for &index in &self.updates {
                if Instant::now() >= self.js.borrow().deadline() {
                    break;
                }
                invoked += 1;
                if let Err(error) = self.call("__invoke", &[Arg::Num(index as f64), Arg::Str("update")]) {
                    self.diagnostics.push(format!("script {index} stopped: {error:#}"));
                    tracing::warn!("SceneScript {index} stopped: {error:#}");
                    failed.push(index);
                }
            }
            self.updates.retain(|index| !failed.contains(index));
            if Instant::now() >= self.js.borrow().deadline() {
                return Ok(());
            }
            self.call("__tickTimers", &[])
        })();
        tracing::debug!(target: "paper_scene::script::frame", invoked, expected, elapsed_us = started.elapsed().as_micros(), "SceneScript updates");
        for index in failed {
            self.disable_module(index);
        }
        if let Err(error) = result {
            self.stop(format!("scripts stopped: {error:#}"));
            return Ok(false);
        }
        match self.drain() {
            Ok(changed) => {
                let properties_changed = std::mem::take(&mut self.properties_changed);
                Ok(changed || properties_changed)
            }
            Err(error) => {
                self.stop(format!("changes stopped: {error:#}"));
                Ok(false)
            }
        }
    }

    fn stop(&mut self, diagnostic: String) {
        self.disabled = true;
        tracing::warn!("SceneScript: {diagnostic}");
        self.diagnostics.push(diagnostic);
    }

    fn disable_module(&self, index: usize) {
        let deadline = self.js.borrow().deadline();
        self.deadline(FRAME_BUDGET);
        let _ = self.call("__disable", &[Arg::Num(index as f64)]);
        self.js.borrow().set_deadline(deadline);
    }

    fn drain(&mut self) -> Result<bool> {
        self.deadline(FRAME_BUDGET);
        let json = self.js.borrow_mut().call_string("__drain", &[])?;
        if json.len() > 1024 * 1024 {
            return Err(anyhow!("SceneScript changes exceed 1 MiB per frame"));
        }
        self.audio_registered = self.js.borrow_mut().eval_bool("__audio.size > 0").unwrap_or(false);
        self.timers_pending = self.js.borrow_mut().eval_bool("__timers.size > 0").unwrap_or(false);
        if let Some(storage) = &mut self.storage {
            let data = self.js.borrow_mut().call_string("__storageDrain", &[])?;
            if !data.is_empty() {
                storage.save(&data)?;
            }
        }
        let changes: Vec<(String, Value)> = serde_json::from_str(&json)?;
        for (path, value) in &changes {
            if let Some(target) = self.scene.pointer_mut(path) {
                *target = value.clone();
            } else if let Some((parent, key)) = path.rsplit_once('/')
                && let Some(Value::Object(map)) = self.scene.pointer_mut(parent)
            {
                map.insert(key.replace("~1", "/").replace("~0", "~"), value.clone());
            }
            if let Some(command) = self.volume_command(path, value) {
                self.commands.push(command);
            }
            if let Some(key) = path.strip_prefix("/general/")
                && !key.contains('/')
            {
                self.general.push((key.to_owned(), value.clone()));
            }
        }
        let queued = self.js.borrow_mut().call_string("__drainCommands", &[])?;
        let queued: Vec<(String, String, String, Value)> = serde_json::from_str(&queued)?;
        self.commands.extend(
            queued
                .iter()
                .filter_map(|(kind, target, op, value)| parse_command(kind, target, op, value)),
        );
        Ok(!changes.is_empty())
    }

    fn volume_command(&self, path: &str, value: &Value) -> Option<ScriptCommand> {
        let (parent, key) = path.rsplit_once('/')?;
        if key != "volume" {
            return None;
        }
        let object = self.scene.pointer(parent)?;
        object.get("sound")?;
        let id = object.get("id").map(|id| id.to_string().trim_matches('"').to_owned())?;
        let gain = crate::effects::json_numbers(value)?.first().copied()?;
        Some(ScriptCommand::Sound { id, op: SoundOp::Gain(gain.max(0.0)) })
    }
}
