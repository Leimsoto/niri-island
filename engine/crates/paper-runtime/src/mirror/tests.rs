#![cfg(test)]

use super::*;
use std::os::fd::AsFd;
use std::os::unix::ffi::OsStrExt;

fn layout() -> Layout {
    Layout {
        width: 2560,
        height: 1440,
        fourcc: u32::from_le_bytes(*b"XR24"),
        modifier: 0x0300_0000_0000_0014,
        memory: Memory::Dmabuf,
        planes: [Plane { stride: 10240, offset: 0 }, Plane { stride: 10240, offset: 3_686_400 }],
        plane_count: 2,
    }
}

#[test]
fn messages_round_trip() {
    for message in [
        Message::Buffer { id: 7, layout: layout() },
        Message::Buffer {
            id: 255,
            layout: Layout { memory: Memory::Shm, plane_count: 1, ..layout() },
        },
        Message::Show { id: 7, source: [0, 1440, 2560, 1440] },
        Message::Forget { id: 3 },
        Message::Release { id: 9 },
    ] {
        assert_eq!(Message::decode(&message.encode()), Some(message));
    }
}

#[test]
fn wire_layout_stable() {
    let bytes = Message::Buffer { id: 2, layout: layout() }.encode();
    assert_eq!(&bytes[..8], b"SKDM\x01\x02\x02\x00");
    assert_eq!(&bytes[8..12], &2560u32.to_le_bytes());
    assert_eq!(&bytes[16..20], b"XR24");
    assert_eq!(&bytes[36..40], &10240u32.to_le_bytes());
    assert_eq!(&bytes[40..44], &3_686_400u32.to_le_bytes());
    let show = Message::Show { id: 2, source: [1, 2, 3, 4] }.encode();
    assert_eq!(&show[4..6], &[2, 2]);
    assert_eq!(&show[20..24], &4u32.to_le_bytes());
}

#[test]
fn rejects_malformed_packets() {
    let mut bytes = Message::Forget { id: 1 }.encode();
    assert!(Message::decode(&bytes[..32]).is_none());
    bytes[0] = b'X';
    assert!(Message::decode(&bytes).is_none());
    let mut buffer = Message::Buffer { id: 1, layout: layout() }.encode();
    buffer[6] = 3;
    assert!(Message::decode(&buffer).is_none());
    buffer[6] = 0;
    assert!(Message::decode(&buffer).is_none());
}

#[test]
fn passes_descriptors_and_releases() {
    let directory = std::env::temp_dir().join(format!("paper-mirror-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("mirror.sock");
    let _ = std::fs::remove_file(&path);
    let listener =
        unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) };
    assert!(listener >= 0);
    let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    address.sun_family = libc::AF_UNIX as libc::sa_family_t;
    for (target, byte) in address.sun_path.iter_mut().zip(path.as_os_str().as_bytes()) {
        *target = *byte as libc::c_char;
    }
    let size = std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t;
    assert_eq!(unsafe { libc::bind(listener, std::ptr::from_ref(&address).cast(), size) }, 0);
    assert_eq!(unsafe { libc::listen(listener, 1) }, 0);
    assert!(Link::peer_exists(&path));
    let mut events = libc::pollfd { fd: listener, events: libc::POLLIN, revents: 0 };
    assert_eq!(unsafe { libc::poll(&raw mut events, 1, 0) }, 0);
    let mut link = Link::connect(&path).unwrap();
    let peer = unsafe { libc::accept(listener, std::ptr::null_mut(), std::ptr::null_mut()) };
    assert!(peer >= 0);
    let file = std::fs::File::open("/dev/null").unwrap();
    let id = link.next_id();
    link.send(&Message::Buffer { id, layout: layout() }, &[file.as_fd()]).unwrap();
    let mut bytes = [0u8; PACKET];
    let mut control = [0u8; 64];
    let mut iov = libc::iovec { iov_base: bytes.as_mut_ptr().cast(), iov_len: PACKET };
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &raw mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.as_mut_ptr().cast();
    msg.msg_controllen = control.len();
    assert_eq!(unsafe { libc::recvmsg(peer, &raw mut msg, 0) }, PACKET as isize);
    let header = unsafe { libc::CMSG_FIRSTHDR(&raw const msg) };
    assert!(!header.is_null());
    let mut fd_bytes = [0u8; 4];
    unsafe { std::ptr::copy_nonoverlapping(libc::CMSG_DATA(header), fd_bytes.as_mut_ptr(), 4) };
    let received = RawFd::from_ne_bytes(fd_bytes);
    assert!(received >= 0);
    assert_eq!(Message::decode(&bytes), Some(Message::Buffer { id, layout: layout() }));
    let release = Message::Release { id }.encode();
    assert_eq!(unsafe { libc::send(peer, release.as_ptr().cast(), PACKET, 0) }, PACKET as isize);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    let mut released = Vec::new();
    while released.is_empty() && std::time::Instant::now() < deadline {
        released = link.drain().unwrap();
    }
    assert_eq!(released, vec![id]);
    unsafe {
        libc::close(received);
        libc::close(peer);
    }
    assert!(link.drain().is_err());
    unsafe { libc::close(listener) };
    assert!(path.exists());
    assert!(!Link::peer_exists(&path));
    let _ = std::fs::remove_dir_all(directory);
}
