use std::io;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd};

const NAME: &std::ffi::CStr = c"skwd-paper-shm";

pub struct SharedMemory {
    fd: OwnedFd,
    map: *mut u8,
    len: usize,
}

impl SharedMemory {
    pub fn new(len: usize) -> io::Result<Self> {
        if len == 0 || len > isize::MAX as usize {
            return Err(io::ErrorKind::InvalidInput.into());
        }
        let raw = unsafe { libc::memfd_create(NAME.as_ptr(), libc::MFD_CLOEXEC) };
        if raw < 0 {
            return Err(io::Error::last_os_error());
        }
        let fd = unsafe { OwnedFd::from_raw_fd(raw) };
        if unsafe { libc::ftruncate(raw, len as libc::off_t) } < 0 {
            return Err(io::Error::last_os_error());
        }
        let map = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                len,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                raw,
                0,
            )
        };
        if map == libc::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        Ok(Self { fd, map: map.cast(), len })
    }

    pub fn bytes(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.map, self.len) }
    }

    pub fn drop_local_pages(&self) {
        unsafe {
            libc::madvise(self.map.cast(), self.len, libc::MADV_DONTNEED);
        }
    }
}

impl AsFd for SharedMemory {
    fn as_fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }
}

impl AsRawFd for SharedMemory {
    fn as_raw_fd(&self) -> std::os::fd::RawFd {
        self.fd.as_raw_fd()
    }
}

impl Drop for SharedMemory {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(self.map.cast(), self.len);
        }
    }
}

#[cfg(test)]
mod tests;
