//! The JavaScript engine behind SceneScript: V8, as Wallpaper Engine has.
//! (QuickJS ran scripts ~20× slower: fine for a clock, hopeless for a script
//! that simulates and draws a whole scene every frame.)
//!
//! One isolate and one context per scene. Calls run under a deadline: a
//! watchdog thread terminates execution once it passes, and the call returns
//! an "interrupted" error, as QuickJS's interrupt handler did.

use anyhow::{Result, anyhow};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Once};
use std::time::{Duration, Instant};

/// An argument for a call into the scripts.
pub enum Arg<'a> {
    Str(&'a str),
    Num(f64),
    F32s(&'a [f32]),
    Bools(&'a [bool]),
    Strs(&'a [String]),
}

/// Built-in modules scripts import (`WEMath`, …), compiled once.
struct Builtins(HashMap<String, v8::Global<v8::Module>>);

struct Watchdog {
    base: Instant,
    deadline_ns: Arc<AtomicU64>,
    fired: Arc<AtomicBool>,
    /// Inside a call: only then may it terminate (a deadline passing between
    /// calls must not kill the next one before it starts).
    inside: Arc<std::sync::Mutex<bool>>,
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for Watchdog {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

/// ArrayBuffers live outside V8's heap, so its limit does not see them: they
/// get a budget of their own (a script asking for more gets an exception).
struct BufferBudget {
    used: std::sync::atomic::AtomicUsize,
    max: usize,
}

const BUFFER_ALIGN: usize = 16;

unsafe extern "C" fn buffer_allocate(budget: &BufferBudget, len: usize) -> *mut std::ffi::c_void {
    if budget.used.fetch_add(len, Ordering::AcqRel) + len > budget.max {
        budget.used.fetch_sub(len, Ordering::AcqRel);
        return std::ptr::null_mut();
    }
    let Ok(layout) = std::alloc::Layout::from_size_align(len.max(1), BUFFER_ALIGN) else {
        budget.used.fetch_sub(len, Ordering::AcqRel);
        return std::ptr::null_mut();
    };
    // SAFETY: a non-zero layout
    let ptr = unsafe { std::alloc::alloc_zeroed(layout) };
    if ptr.is_null() {
        budget.used.fetch_sub(len, Ordering::AcqRel);
    }
    ptr.cast()
}

unsafe extern "C" fn buffer_free(budget: &BufferBudget, data: *mut std::ffi::c_void, len: usize) {
    if data.is_null() {
        return;
    }
    budget.used.fetch_sub(len, Ordering::AcqRel);
    if let Ok(layout) = std::alloc::Layout::from_size_align(len.max(1), BUFFER_ALIGN) {
        // SAFETY: allocated above with this very layout
        unsafe { std::alloc::dealloc(data.cast(), layout) };
    }
}

unsafe extern "C" fn buffer_drop(budget: *const BufferBudget) {
    // SAFETY: the Arc leaked into the allocator when it was made
    drop(unsafe { Arc::from_raw(budget) });
}

static BUFFER_VTABLE: v8::RustAllocatorVtable<BufferBudget> = v8::RustAllocatorVtable {
    allocate: buffer_allocate,
    allocate_uninitialized: buffer_allocate,
    free: buffer_free,
    drop: buffer_drop,
};

pub struct Js {
    // (handles before the isolate: they must go first)
    context: v8::Global<v8::Context>,
    watchdog: Watchdog,
    isolate: v8::OwnedIsolate,
}

fn init_v8() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let platform = v8::new_default_platform(0, false).make_shared();
        v8::V8::initialize_platform(platform);
        v8::V8::initialize();
    });
}

// What a caught exception says: its message and stack.
fn describe(tc: &mut v8::PinnedRef<'_, v8::TryCatch<v8::HandleScope>>) -> String {
    if tc.has_terminated() {
        return "interrupted".to_owned();
    }
    let message = tc.exception().map(|e| e.to_rust_string_lossy(tc)).unwrap_or_else(|| "exception".into());
    let stack = tc
        .stack_trace()
        .map(|s| s.to_rust_string_lossy(tc))
        .filter(|s| !s.is_empty() && *s != message)
        .unwrap_or_default();
    if stack.is_empty() { message } else { format!("{message}\n{stack}") }
}

fn resolve<'s>(
    context: v8::Local<'s, v8::Context>,
    specifier: v8::Local<'s, v8::String>,
    _attributes: v8::Local<'s, v8::FixedArray>,
    _referrer: v8::Local<'s, v8::Module>,
) -> Option<v8::Local<'s, v8::Module>> {
    v8::callback_scope!(unsafe scope, context);
    let name = specifier.to_rust_string_lossy(scope);
    let module = scope.get_slot::<Builtins>().and_then(|b| b.0.get(&name)).cloned();
    match module {
        Some(global) => Some(v8::Local::new(scope, global)),
        None => {
            let e = v8::String::new(scope, &format!("module {name} not found")).unwrap();
            scope.throw_exception(e.into());
            None
        }
    }
}

fn origin<'s>(scope: &mut v8::PinScope<'s, '_>, name: &str, module: bool) -> v8::ScriptOrigin<'s> {
    let resource = v8::String::new(scope, name).unwrap();
    v8::ScriptOrigin::new(scope, resource.into(), 0, 0, false, 0, None, false, false, module, None)
}

impl Js {
    pub fn new(heap_limit: usize, buffer_limit: usize) -> Result<Self> {
        init_v8();
        let budget = Arc::new(BufferBudget { used: std::sync::atomic::AtomicUsize::new(0), max: buffer_limit });
        // SAFETY: the vtable is static and the handle an Arc the allocator owns (freed in buffer_drop)
        let allocator = unsafe { v8::new_rust_allocator(Arc::into_raw(budget), &BUFFER_VTABLE) };
        let params = v8::CreateParams::default().heap_limits(0, heap_limit).array_buffer_allocator(allocator);
        let mut isolate = v8::Isolate::new(params);
        isolate.set_slot(Builtins(HashMap::new()));
        let context = {
            v8::scope!(let scope, &mut isolate);
            let context = v8::Context::new(scope, Default::default());
            v8::Global::new(scope, context)
        };
        let base = Instant::now();
        let deadline_ns = Arc::new(AtomicU64::new(u64::MAX));
        let fired = Arc::new(AtomicBool::new(false));
        let stop = Arc::new(AtomicBool::new(false));
        let handle = isolate.thread_safe_handle();
        let inside = Arc::new(std::sync::Mutex::new(false));
        let thread = {
            let (deadline_ns, fired, stop, inside) = (deadline_ns.clone(), fired.clone(), stop.clone(), inside.clone());
            std::thread::Builder::new()
                .name("scenescript-watchdog".into())
                .spawn(move || {
                    while !stop.load(Ordering::Acquire) {
                        std::thread::sleep(Duration::from_millis(2));
                        let now = base.elapsed().as_nanos() as u64;
                        if now < deadline_ns.load(Ordering::Acquire) {
                            continue;
                        }
                        let inside = inside.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                        if *inside && !fired.swap(true, Ordering::AcqRel) {
                            handle.terminate_execution();
                        }
                    }
                })?
        };
        Ok(Self {
            context,
            watchdog: Watchdog { base, deadline_ns, fired, stop, inside, thread: Some(thread) },
            isolate,
        })
    }

    /// Calls after this have until `at` (then they are interrupted).
    pub fn set_deadline(&self, at: Instant) {
        let ns = at.saturating_duration_since(self.watchdog.base).as_nanos() as u64;
        self.watchdog.fired.store(false, Ordering::Release);
        self.watchdog.deadline_ns.store(ns, Ordering::Release);
    }

    pub fn deadline(&self) -> Instant {
        self.watchdog.base + Duration::from_nanos(self.watchdog.deadline_ns.load(Ordering::Acquire))
    }

    fn enter(&self) {
        *self.watchdog.inside.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = true;
    }

    // Out of a call: no more terminating, and after a terminated one the
    // isolate takes calls again.
    fn recover(&mut self) {
        *self.watchdog.inside.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = false;
        if self.watchdog.fired.swap(false, Ordering::AcqRel) {
            self.isolate.cancel_terminate_execution();
        }
    }

    pub fn set_slot<T: 'static>(&mut self, value: T) {
        self.isolate.set_slot(value);
    }

    pub fn heap_bytes(&mut self) -> usize {
        self.isolate.get_heap_statistics().used_heap_size()
    }

    /// A global function backed by Rust (its state goes in isolate slots).
    pub fn set_function(&mut self, name: &str, callback: impl v8::MapFnTo<v8::FunctionCallback>) -> Result<()> {
        v8::scope_with_context!(let scope, &mut self.isolate, &self.context);
        let function = v8::Function::new(scope, callback).ok_or_else(|| anyhow!("function {name}"))?;
        let key = v8::String::new(scope, name).unwrap();
        scope.get_current_context().global(scope).set(scope, key.into(), function.into());
        Ok(())
    }

    fn run_script<T>(&mut self, source: &str, read: impl FnOnce(&mut v8::PinScope<'_, '_>, v8::Local<'_, v8::Value>) -> T) -> Result<T> {
        self.enter();
        let result = {
            v8::scope_with_context!(let scope, &mut self.isolate, &self.context);
            v8::tc_scope!(let tc, scope);
            let code = v8::String::new(tc, source).ok_or_else(|| anyhow!("source too large"))?;
            let script_origin = origin(tc, "eval_script", false);
            match v8::Script::compile(tc, code, Some(&script_origin)).and_then(|s| s.run(tc)) {
                Some(value) => Ok(read(tc, value)),
                None => Err(anyhow!("{}", describe(tc))),
            }
        };
        self.recover();
        result
    }

    pub fn eval(&mut self, source: &str) -> Result<()> {
        self.run_script(source, |_, _| ())
    }

    pub fn eval_bool(&mut self, source: &str) -> Result<bool> {
        self.run_script(source, |scope, value| value.boolean_value(scope))
    }

    fn call_with<T>(
        &mut self,
        name: &str,
        args: &[Arg<'_>],
        read: impl FnOnce(&mut v8::PinScope<'_, '_>, v8::Local<'_, v8::Value>) -> T,
    ) -> Result<T> {
        let started = Instant::now();
        let margin_ms = self.deadline().saturating_duration_since(started).as_millis();
        let was_terminating = self.isolate.is_execution_terminating();
        self.enter();
        let result = {
            v8::scope_with_context!(let scope, &mut self.isolate, &self.context);
            v8::tc_scope!(let tc, scope);
            let global = tc.get_current_context().global(tc);
            let key = v8::String::new(tc, name).unwrap();
            let function = global
                .get(tc, key.into())
                .and_then(|f| v8::Local::<v8::Function>::try_from(f).ok())
                .ok_or_else(|| anyhow!("{name} is not a function"))?;
            let mut values: Vec<v8::Local<v8::Value>> = Vec::with_capacity(args.len());
            for arg in args {
                values.push(match arg {
                    Arg::Str(s) => v8::String::new(tc, s).ok_or_else(|| anyhow!("string too large"))?.into(),
                    Arg::Num(n) => v8::Number::new(tc, *n).into(),
                    Arg::F32s(list) => {
                        let items: Vec<v8::Local<v8::Value>> = list.iter().map(|&v| v8::Number::new(tc, f64::from(v)).into()).collect();
                        v8::Array::new_with_elements(tc, &items).into()
                    }
                    Arg::Bools(list) => {
                        let items: Vec<v8::Local<v8::Value>> = list.iter().map(|&v| v8::Boolean::new(tc, v).into()).collect();
                        v8::Array::new_with_elements(tc, &items).into()
                    }
                    Arg::Strs(list) => {
                        let items: Vec<v8::Local<v8::Value>> =
                            list.iter().filter_map(|s| v8::String::new(tc, s)).map(Into::into).collect();
                        v8::Array::new_with_elements(tc, &items).into()
                    }
                });
            }
            let receiver: v8::Local<v8::Value> = global.into();
            match function.call(tc, receiver, &values) {
                Some(value) => Ok(read(tc, value)),
                None => Err(anyhow!("{}", describe(tc))),
            }
        };
        if result.as_ref().is_err_and(|e| e.to_string() == "interrupted") {
            tracing::debug!(name, margin_ms, elapsed_ms = started.elapsed().as_millis(), was_terminating, "SceneScript call interrupted");
        }
        self.recover();
        result
    }

    pub fn call(&mut self, name: &str, args: &[Arg<'_>]) -> Result<()> {
        self.call_with(name, args, |_, _| ())
    }

    pub fn call_string(&mut self, name: &str, args: &[Arg<'_>]) -> Result<String> {
        self.call_with(name, args, |scope, value| value.to_rust_string_lossy(scope))
    }

    pub fn call_bool(&mut self, name: &str, args: &[Arg<'_>]) -> Result<bool> {
        self.call_with(name, args, |scope, value| value.boolean_value(scope))
    }

    fn compile_module<'s>(
        scope: &mut v8::PinScope<'s, '_>,
        name: &str,
        source: &str,
    ) -> Option<v8::Local<'s, v8::Module>> {
        let code = v8::String::new(scope, source)?;
        let module_origin = origin(scope, name, true);
        let mut source = v8::script_compiler::Source::new(code, Some(&module_origin));
        v8::script_compiler::compile_module(scope, &mut source)
    }

    /// A module scripts can import by `name`.
    pub fn declare_module(&mut self, name: &str, source: &str) -> Result<()> {
        self.enter();
        let global = (|| {
            v8::scope_with_context!(let scope, &mut self.isolate, &self.context);
            v8::tc_scope!(let tc, scope);
            let module = Self::compile_module(tc, name, source).ok_or_else(|| anyhow!("{name}: {}", describe(tc)))?;
            if module.instantiate_module(tc, resolve) != Some(true) || module.evaluate(tc).is_none() {
                return Err(anyhow!("{name}: {}", describe(tc)));
            }
            Ok(v8::Global::new(tc, module))
        })();
        self.recover();
        let global = global?;
        if let Some(builtins) = self.isolate.get_slot_mut::<Builtins>() {
            builtins.0.insert(name.to_owned(), global);
        }
        Ok(())
    }

    /// Runs a script's module, hands its namespace to `register(index, ns,
    /// path)` (a global function) and says whether it exports `update`.
    pub fn run_module(&mut self, name: &str, source: &str, register: &str, index: usize, path: &str) -> Result<bool> {
        self.enter();
        let result = {
            v8::scope_with_context!(let scope, &mut self.isolate, &self.context);
            v8::tc_scope!(let tc, scope);
            (|| -> Result<bool> {
                let module = Self::compile_module(tc, name, source).ok_or_else(|| anyhow!("{}", describe(tc)))?;
                if module.instantiate_module(tc, resolve) != Some(true) {
                    return Err(anyhow!("{}", describe(tc)));
                }
                let evaluated = module.evaluate(tc).ok_or_else(|| anyhow!("{}", describe(tc)))?;
                if let Ok(promise) = v8::Local::<v8::Promise>::try_from(evaluated)
                    && promise.state() == v8::PromiseState::Rejected
                {
                    let reason = promise.result(tc).to_rust_string_lossy(tc);
                    return Err(anyhow!("{reason}"));
                }
                let namespace = module.get_module_namespace();
                let ns = v8::Local::<v8::Object>::try_from(namespace).map_err(|_| anyhow!("no namespace"))?;
                let update_key = v8::String::new(tc, "update").unwrap();
                let has_update = ns.get(tc, update_key.into()).is_some_and(|v| v.is_function());
                let global = tc.get_current_context().global(tc);
                let key = v8::String::new(tc, register).unwrap();
                let function = global
                    .get(tc, key.into())
                    .and_then(|f| v8::Local::<v8::Function>::try_from(f).ok())
                    .ok_or_else(|| anyhow!("{register} is not a function"))?;
                let index_value = v8::Number::new(tc, index as f64);
                let path_value = v8::String::new(tc, path).unwrap();
                function
                    .call(tc, global.into(), &[index_value.into(), namespace, path_value.into()])
                    .ok_or_else(|| anyhow!("{}", describe(tc)))?;
                Ok(has_update)
            })()
        };
        self.recover();
        result
    }
}
