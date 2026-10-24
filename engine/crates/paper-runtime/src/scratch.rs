use std::{
    fs, io,
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    sync::OnceLock,
    time::{Duration, SystemTime},
};

const PREFIX: &str = "skwd-paper-";
const SEPARATOR: char = '-';
const PROC: &str = "/proc";
const STAT: &str = "stat";
const UPTIME: &str = "uptime";
const START_FIELD: usize = 19;
const NAME_FIELDS: usize = 3;
const PRIVATE: u32 = 0o700;

pub const VIDEO: &str = "skwd-scene-video-";
pub const AUDIO: &str = "skwd-wall-scene-audio-";
const KINDS: [&str; 2] = [VIDEO, AUDIO];

static DIR: OnceLock<Option<PathBuf>> = OnceLock::new();

pub fn file(kind: &str, suffix: &str) -> io::Result<tempfile::NamedTempFile> {
    let dir = DIR.get_or_init(|| {
        claim(&std::env::temp_dir())
            .inspect_err(|err| tracing::warn!(?err, "scratch folder unavailable"))
            .ok()
            .flatten()
    });
    let mut builder = tempfile::Builder::new();
    builder.prefix(kind).suffix(suffix);
    match dir {
        Some(dir) => builder.tempfile_in(dir),
        None => builder.tempfile(),
    }
}

pub fn sweep() {
    sweep_in(&std::env::temp_dir(), None);
}

pub fn release(pid: u32) {
    remove(&std::env::temp_dir(), pid);
}

fn claim(root: &Path) -> io::Result<Option<PathBuf>> {
    let pid = std::process::id();
    let Some(start) = started(pid) else {
        return Ok(None);
    };
    sweep_in(root, Some((pid, start)));
    let dir = tempfile::Builder::new()
        .prefix(&format!("{}{SEPARATOR}", name(pid, start)))
        .permissions(fs::Permissions::from_mode(PRIVATE))
        .tempdir_in(root)?;
    Ok(Some(dir.keep()))
}

fn sweep_in(root: &Path, own: Option<(u32, u64)>) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    let boot = booted();
    for entry in entries.flatten() {
        let name = entry.file_name();
        let Some(name) = name.to_str() else {
            continue;
        };
        if let Some(found) = owner(name) {
            if Some(found) == own || started(found.0) != Some(found.1) {
                let _ = fs::remove_dir_all(entry.path());
            }
        } else if KINDS.iter().any(|kind| name.starts_with(kind)) && before(&entry, boot) {
            let _ = fs::remove_file(entry.path());
        }
    }
}

fn remove(root: &Path, pid: u32) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        if entry
            .file_name()
            .to_str()
            .and_then(owner)
            .is_some_and(|(owner, start)| owner == pid && started(pid) != Some(start))
        {
            let _ = fs::remove_dir_all(entry.path());
        }
    }
}

fn name(pid: u32, start: u64) -> String {
    format!("{PREFIX}{pid}{SEPARATOR}{start}")
}

fn owner(name: &str) -> Option<(u32, u64)> {
    let mut fields = name.strip_prefix(PREFIX)?.splitn(NAME_FIELDS, SEPARATOR);
    Some((fields.next()?.parse().ok()?, fields.next()?.parse().ok()?))
}

fn started(pid: u32) -> Option<u64> {
    let stat = fs::read_to_string(Path::new(PROC).join(pid.to_string()).join(STAT)).ok()?;
    stat.rsplit_once(')')?.1.split_whitespace().nth(START_FIELD)?.parse().ok()
}

fn booted() -> Option<SystemTime> {
    let uptime = fs::read_to_string(Path::new(PROC).join(UPTIME)).ok()?;
    let seconds: f64 = uptime.split_whitespace().next()?.parse().ok()?;
    SystemTime::now().checked_sub(Duration::from_secs_f64(seconds))
}

fn before(entry: &fs::DirEntry, boot: Option<SystemTime>) -> bool {
    let modified = entry.metadata().and_then(|meta| meta.modified()).ok();
    matches!((modified, boot), (Some(modified), Some(boot)) if modified < boot)
}

#[cfg(test)]
mod tests;
