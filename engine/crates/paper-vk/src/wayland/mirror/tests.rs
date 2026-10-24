use super::*;
use std::os::fd::FromRawFd;

fn mirror() -> (SurfaceMirror, OwnedFd) {
    let mut fds = [0; 2];
    let made = unsafe {
        libc::socketpair(
            libc::AF_UNIX,
            libc::SOCK_SEQPACKET | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC,
            0,
            fds.as_mut_ptr(),
        )
    };
    assert_eq!(made, 0);
    let (ours, theirs) = unsafe { (OwnedFd::from_raw_fd(fds[0]), OwnedFd::from_raw_fd(fds[1])) };
    let mirror = SurfaceMirror {
        socket: String::new(),
        link: Link::from_socket(ours),
        sent: HashMap::new(),
        held: HashMap::new(),
        deferred: Vec::new(),
    };
    (mirror, theirs)
}

#[test]
fn held_buffer_defers() {
    let (mut mirror, _peer) = mirror();
    let now = Instant::now();
    mirror.held.insert(4, 1);
    mirror.deferred.push((1, now));
    let mut free = vec![0];
    mirror.settle(&mut free);
    assert_eq!(free, vec![0]);
    assert!(mirror.holds(1));
    mirror.held.remove(&4);
    mirror.settle(&mut free);
    assert_eq!(free, vec![0, 1]);
    assert!(mirror.deferred.is_empty());
}

#[test]
fn stalled_consumer_keeps_ownership() {
    let (mut mirror, _peer) = mirror();
    let then = Instant::now();
    mirror.held.insert(4, 1);
    mirror.deferred.push((1, then));
    let mut free = Vec::new();
    mirror.settle(&mut free);
    assert!(free.is_empty());
    assert!(mirror.stalled(then + HOLD_LIMIT));
    assert!(mirror.holds(1));
}

#[test]
fn free_slots_unique() {
    let (mut mirror, _peer) = mirror();
    let now = Instant::now();
    mirror.deferred.push((2, now));
    let mut free = vec![2];
    mirror.settle(&mut free);
    assert_eq!(free, vec![2]);
}

#[test]
fn layout_plane_formats() {
    let plane = Plane { stride: 7680, offset: 0 };
    let chroma = Plane { stride: 7680, offset: 8_294_400 };
    let nv12 = dmabuf_layout(1920, 1080, crate::dmabuf::FOURCC_NV12, 7, &[plane, chroma]);
    assert_eq!(nv12.plane_count, 2);
    assert_eq!(nv12.planes, [plane, chroma]);
    assert_eq!(nv12.memory, Memory::Dmabuf);
    let xr24 = dmabuf_layout(1920, 1080, crate::dmabuf::FOURCC_XR24, 0, &[plane]);
    assert_eq!(xr24.planes[1], Plane::default());
    assert_eq!(shm_fourcc(wl_shm::Format::Xrgb8888), crate::dmabuf::FOURCC_XR24);
    assert_eq!(shm_fourcc(wl_shm::Format::Argb8888), u32::from_le_bytes(*b"AR24"));
    assert_eq!(shm_fourcc(wl_shm::Format::Abgr8888), u32::from_le_bytes(*b"AB24"));
}

#[test]
fn requests_match_output() {
    request(MirrorTarget { output: "DP-9".into(), socket: "/run/x.sock".into() });
    assert!(take_requests(|output| output == "DP-8").iter().all(|target| target.output != "DP-9"));
    let taken = take_requests(|output| output == "DP-9");
    assert!(taken.iter().any(|target| target.output == "DP-9"));
    assert!(take_requests(|_| true).iter().all(|target| target.output != "DP-9"));
}
