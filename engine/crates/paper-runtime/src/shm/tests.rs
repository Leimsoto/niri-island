use super::*;
use std::os::unix::fs::FileExt;

#[test]
fn descriptors_share_pixels() {
    let mut memory = SharedMemory::new(4096).unwrap();
    memory.bytes()[..4].copy_from_slice(b"test");
    let peer = std::fs::File::from(memory.as_fd().try_clone_to_owned().unwrap());
    memory.drop_local_pages();
    let mut pixels = [0; 4];
    peer.read_exact_at(&mut pixels, 0).unwrap();
    assert_eq!(&pixels, b"test");
    assert_eq!(&memory.bytes()[..4], b"test");
    drop(memory);
    peer.read_exact_at(&mut pixels, 0).unwrap();
    assert_eq!(&pixels, b"test");
}

#[test]
fn rejects_invalid_lengths() {
    assert!(SharedMemory::new(0).is_err());
    assert!(SharedMemory::new(usize::MAX).is_err());
}
