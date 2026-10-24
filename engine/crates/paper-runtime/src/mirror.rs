use std::io;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::path::Path;

pub const PACKET: usize = 64;
pub const MAX_PLANES: usize = 2;
const MAGIC: &[u8; WORD] = b"SKDM";
const WORD: usize = 4;
const WIDE: usize = 8;
const CONTROL: usize = 64;
const SHM_FLAG: u8 = 1;
pub const AR24: u32 = u32::from_le_bytes(*b"AR24");
pub const XR24: u32 = u32::from_le_bytes(*b"XR24");

mod at {
    pub const KIND: usize = 4;
    pub const ID: usize = 5;
    pub const PLANES: usize = 6;
    pub const SHM: usize = 7;
    pub const WIDTH: usize = 8;
    pub const HEIGHT: usize = 12;
    pub const FOURCC: usize = 16;
    pub const MODIFIER: usize = 20;
    pub const PLANE_TABLE: usize = 28;
    pub const PLANE_OFFSET: usize = 4;
    pub const PLANE_STRIDE: usize = 8;
    pub const SOURCE: usize = 8;
}

mod kind {
    pub const BUFFER: u8 = 1;
    pub const SHOW: u8 = 2;
    pub const FORGET: u8 = 3;
    pub const RELEASE: u8 = 4;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Memory {
    Dmabuf,
    Shm,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Plane {
    pub stride: u32,
    pub offset: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    pub width: u32,
    pub height: u32,
    pub fourcc: u32,
    pub modifier: u64,
    pub memory: Memory,
    pub planes: [Plane; MAX_PLANES],
    pub plane_count: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Message {
    Buffer { id: u8, layout: Layout },
    Show { id: u8, source: [u32; 4] },
    Forget { id: u8 },
    Release { id: u8 },
}

fn put(bytes: &mut [u8; PACKET], at: usize, value: u32) {
    bytes[at..at + WORD].copy_from_slice(&value.to_le_bytes());
}

fn get(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes[at..at + WORD].try_into().expect("four bytes"))
}

impl Message {
    pub fn encode(&self) -> [u8; PACKET] {
        let mut bytes = [0u8; PACKET];
        bytes[..WORD].copy_from_slice(MAGIC);
        match *self {
            Message::Buffer { id, layout } => {
                bytes[at::KIND] = kind::BUFFER;
                bytes[at::ID] = id;
                bytes[at::PLANES] = layout.plane_count;
                bytes[at::SHM] = u8::from(layout.memory == Memory::Shm);
                put(&mut bytes, at::WIDTH, layout.width);
                put(&mut bytes, at::HEIGHT, layout.height);
                put(&mut bytes, at::FOURCC, layout.fourcc);
                bytes[at::MODIFIER..at::MODIFIER + WIDE]
                    .copy_from_slice(&layout.modifier.to_le_bytes());
                for (index, plane) in layout.planes.iter().enumerate() {
                    put(&mut bytes, at::PLANE_TABLE + index * at::PLANE_STRIDE, plane.stride);
                    put(
                        &mut bytes,
                        at::PLANE_TABLE + index * at::PLANE_STRIDE + at::PLANE_OFFSET,
                        plane.offset,
                    );
                }
            }
            Message::Show { id, source } => {
                bytes[at::KIND] = kind::SHOW;
                bytes[at::ID] = id;
                for (index, value) in source.iter().enumerate() {
                    put(&mut bytes, at::SOURCE + index * WORD, *value);
                }
            }
            Message::Forget { id } => {
                bytes[at::KIND] = kind::FORGET;
                bytes[at::ID] = id;
            }
            Message::Release { id } => {
                bytes[at::KIND] = kind::RELEASE;
                bytes[at::ID] = id;
            }
        }
        bytes
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        if bytes.len() != PACKET || &bytes[..WORD] != MAGIC {
            return None;
        }
        let id = bytes[at::ID];
        Some(match bytes[at::KIND] {
            kind::BUFFER => {
                let plane_count = bytes[at::PLANES];
                if plane_count == 0 || usize::from(plane_count) > MAX_PLANES {
                    return None;
                }
                let plane = |index: usize| Plane {
                    stride: get(bytes, at::PLANE_TABLE + index * at::PLANE_STRIDE),
                    offset: get(
                        bytes,
                        at::PLANE_TABLE + index * at::PLANE_STRIDE + at::PLANE_OFFSET,
                    ),
                };
                Message::Buffer {
                    id,
                    layout: Layout {
                        width: get(bytes, at::WIDTH),
                        height: get(bytes, at::HEIGHT),
                        fourcc: get(bytes, at::FOURCC),
                        modifier: u64::from_le_bytes(
                            bytes[at::MODIFIER..at::MODIFIER + WIDE].try_into().ok()?,
                        ),
                        memory: if bytes[at::SHM] == SHM_FLAG {
                            Memory::Shm
                        } else {
                            Memory::Dmabuf
                        },
                        planes: [plane(0), plane(1)],
                        plane_count,
                    },
                }
            }
            kind::SHOW => Message::Show {
                id,
                source: std::array::from_fn(|index| get(bytes, at::SOURCE + index * WORD)),
            },
            kind::FORGET => Message::Forget { id },
            kind::RELEASE => Message::Release { id },
            _ => return None,
        })
    }
}

pub struct Link {
    socket: OwnedFd,
    next: u8,
}

impl Link {
    pub fn peer_exists(path: &Path) -> bool {
        std::os::unix::net::UnixDatagram::unbound()
            .and_then(|probe| probe.connect(path))
            .is_err_and(|error| error.raw_os_error() == Some(libc::EPROTOTYPE))
    }

    pub fn connect(path: &Path) -> io::Result<Self> {
        use std::os::unix::ffi::OsStrExt;
        let raw = unsafe {
            libc::socket(
                libc::AF_UNIX,
                libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                0,
            )
        };
        if raw < 0 {
            return Err(io::Error::last_os_error());
        }
        let socket = unsafe { OwnedFd::from_raw_fd(raw) };
        let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
        address.sun_family = libc::AF_UNIX as libc::sa_family_t;
        let bytes = path.as_os_str().as_bytes();
        if bytes.len() >= address.sun_path.len() {
            return Err(io::ErrorKind::InvalidInput.into());
        }
        for (target, byte) in address.sun_path.iter_mut().zip(bytes) {
            *target = *byte as libc::c_char;
        }
        let connected = unsafe {
            libc::connect(
                raw,
                std::ptr::from_ref(&address).cast(),
                std::mem::size_of::<libc::sockaddr_un>() as libc::socklen_t,
            )
        };
        if connected < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self { socket, next: 0 })
    }

    pub fn from_socket(socket: OwnedFd) -> Self {
        Self { socket, next: 0 }
    }

    pub fn fd(&self) -> RawFd {
        self.socket.as_raw_fd()
    }

    pub fn next_id(&mut self) -> u8 {
        self.next = self.next.wrapping_add(1);
        self.next
    }

    pub fn send(&self, message: &Message, fds: &[BorrowedFd<'_>]) -> io::Result<()> {
        if fds.len() > MAX_PLANES {
            return Err(io::ErrorKind::InvalidInput.into());
        }
        let bytes = message.encode();
        let mut iov = libc::iovec { iov_base: bytes.as_ptr().cast_mut().cast(), iov_len: PACKET };
        let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
        msg.msg_iov = &raw mut iov;
        msg.msg_iovlen = 1;
        let mut control = [0usize; CONTROL / std::mem::size_of::<usize>()];
        if !fds.is_empty() {
            let size = std::mem::size_of_val(fds) as u32;
            msg.msg_control = control.as_mut_ptr().cast();
            msg.msg_controllen = unsafe { libc::CMSG_SPACE(size) } as usize;
            let header = unsafe { libc::CMSG_FIRSTHDR(&raw const msg) };
            unsafe {
                (*header).cmsg_level = libc::SOL_SOCKET;
                (*header).cmsg_type = libc::SCM_RIGHTS;
                (*header).cmsg_len = libc::CMSG_LEN(size) as usize;
                let data = libc::CMSG_DATA(header);
                for (index, fd) in fds.iter().enumerate() {
                    let bytes = fd.as_raw_fd().to_ne_bytes();
                    std::ptr::copy_nonoverlapping(
                        bytes.as_ptr(),
                        data.add(index * bytes.len()),
                        bytes.len(),
                    );
                }
            }
        }
        loop {
            let sent = unsafe { libc::sendmsg(self.fd(), &raw const msg, libc::MSG_NOSIGNAL) };
            if sent == PACKET as isize {
                return Ok(());
            }
            let error = io::Error::last_os_error();
            if sent < 0 && error.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(if sent < 0 { error } else { io::ErrorKind::WriteZero.into() });
        }
    }

    pub fn drain(&self) -> io::Result<Vec<u8>> {
        let mut released = Vec::new();
        loop {
            let mut bytes = [0u8; PACKET];
            let read = unsafe {
                libc::recv(self.fd(), bytes.as_mut_ptr().cast(), PACKET, libc::MSG_DONTWAIT)
            };
            if read == 0 {
                return Err(io::ErrorKind::BrokenPipe.into());
            }
            if read < 0 {
                let error = io::Error::last_os_error();
                return match error.kind() {
                    io::ErrorKind::WouldBlock => Ok(released),
                    io::ErrorKind::Interrupted => continue,
                    _ => Err(error),
                };
            }
            if let Some(Message::Release { id }) = Message::decode(&bytes[..read as usize]) {
                released.push(id);
            }
        }
    }
}

mod tests;
