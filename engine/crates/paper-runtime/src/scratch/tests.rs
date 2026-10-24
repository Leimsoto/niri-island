use super::*;

#[test]
fn owner_parses_names() {
    assert_eq!(owner(&name(42, 7)), Some((42, 7)));
    assert_eq!(owner("skwd-paper-42-7-x1Yz"), Some((42, 7)));
    assert_eq!(owner("skwd-paper-stationary"), None);
    assert_eq!(owner("skwd-paper-12"), None);
}

#[test]
fn sweep_keeps_live() {
    let root = tempfile::tempdir().unwrap();
    let pid = std::process::id();
    let start = started(pid).unwrap();
    let live = root.path().join(format!("{}-a", name(pid, start)));
    let reused = root.path().join(name(pid, start + 1));
    let unrelated = root.path().join("skwd-paper-stationary");
    for dir in [&live, &reused, &unrelated] {
        fs::create_dir(dir).unwrap();
    }
    fs::write(reused.join("video-a.mp4"), b"x").unwrap();
    let stale = root.path().join("skwd-scene-video-a.mp4");
    let fresh = root.path().join("skwd-wall-scene-audio-b.ogg");
    let other = root.path().join("notes.txt");
    for path in [&stale, &fresh, &other] {
        fs::write(path, b"x").unwrap();
    }
    fs::File::options()
        .write(true)
        .open(&stale)
        .unwrap()
        .set_modified(SystemTime::UNIX_EPOCH)
        .unwrap();
    sweep_in(root.path(), None);
    assert!(live.exists());
    assert!(!reused.exists());
    assert!(unrelated.exists());
    assert!(!stale.exists());
    assert!(fresh.exists());
    assert!(other.exists());
}

#[test]
fn claim_is_private() {
    let root = tempfile::tempdir().unwrap();
    let first = claim(root.path()).unwrap().unwrap();
    assert_eq!(fs::metadata(&first).unwrap().permissions().mode() & 0o777, 0o700);
    let name = first.file_name().unwrap().to_str().unwrap();
    assert_eq!(owner(name).map(|(pid, _)| pid), Some(std::process::id()));
    let second = claim(root.path()).unwrap().unwrap();
    assert!(!first.exists());
    assert!(second.is_dir());
}

#[test]
fn remove_targets_pid() {
    let root = tempfile::tempdir().unwrap();
    let gone = root.path().join(name(41, 9));
    let kept = root.path().join(name(4, 9));
    for dir in [&gone, &kept] {
        fs::create_dir(dir).unwrap();
        fs::write(dir.join("audio-a.mp3"), b"x").unwrap();
    }
    remove(root.path(), 41);
    assert!(!gone.exists());
    assert!(kept.exists());
}

#[test]
fn remove_keeps_live() {
    let root = tempfile::tempdir().unwrap();
    let pid = std::process::id();
    let live = root.path().join(name(pid, started(pid).unwrap()));
    fs::create_dir(&live).unwrap();
    remove(root.path(), pid);
    assert!(live.exists());
}
