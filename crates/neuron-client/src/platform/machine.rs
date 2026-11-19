//! The machine queries of a new install's account (`SeedMachineHash` in
//! `common_playeraccount.cpp`; work package P4.1, decision O27), the
//! memory figures of the low-machine warning (`GlobalMemoryStatus` in
//! `engine_init.cpp`) and the desktop's registry mode
//! (`WidescreenGetDesktopSize`).
//!
//! The C++ is a 32-bit process, so its `HKEY_LOCAL_MACHINE\Software` reads
//! see the 32-bit view of the registry; [`WinMachine`] asks for that view
//! (`KEY_WOW64_32KEY`). `UuidCreate` is reached through `CoCreateGuid`,
//! which returns the same kind of random UUID. The millisecond counter is
//! `GetTickCount`, the system's time since it started, as the C++'s
//! counter is (divergence D531).

use windows::Win32::Foundation::ERROR_SUCCESS;
use windows::Win32::Graphics::Gdi::{DEVMODEW, ENUM_REGISTRY_SETTINGS, EnumDisplaySettingsW};
use windows::Win32::System::Com::CoCreateGuid;
use windows::Win32::System::Registry::{
    HKEY, HKEY_LOCAL_MACHINE, KEY_QUERY_VALUE, KEY_WOW64_32KEY, RegCloseKey, RegOpenKeyExA,
    RegQueryValueExA,
};
use windows::Win32::System::SystemInformation::{
    GetTickCount, GlobalMemoryStatusEx, MEMORYSTATUSEX,
};
use windows::Win32::System::WindowsProgramming::GetUserNameA;
use windows::core::{PCSTR, PSTR, s};

use crate::user_state::{MachineIdentity, PRODUCT_ID_CAPACITY, ProductIdKey, USER_NAME_CAPACITY};

/// [`MachineIdentity`] on this machine.
#[derive(Clone, Copy, Debug, Default)]
pub struct WinMachine;

/// An open key, closed on drop.
struct Key(HKEY);

impl Drop for Key {
    fn drop(&mut self) {
        // SAFETY: the key this value opened, closed once.
        let _ = unsafe { RegCloseKey(self.0) };
    }
}

impl MachineIdentity for WinMachine {
    fn product_id(&mut self, key: ProductIdKey, size: &mut u32) -> Option<Option<Vec<u8>>> {
        let path: Vec<u8> = key.path().bytes().chain([0]).collect();
        let mut handle = HKEY::default();
        // SAFETY: a NUL-terminated path and an out handle closed by `Key`.
        let status = unsafe {
            RegOpenKeyExA(
                HKEY_LOCAL_MACHINE,
                PCSTR(path.as_ptr()),
                None,
                KEY_QUERY_VALUE | KEY_WOW64_32KEY,
                &raw mut handle,
            )
        };
        if status != ERROR_SUCCESS {
            return None;
        }
        let key = Key(handle);
        // The C++'s buffer holds PRODUCT_ID_CAPACITY bytes whatever size
        // the previous query left behind; a larger claim is cut to it.
        *size = (*size).min(PRODUCT_ID_CAPACITY);
        let mut buffer = vec![0u8; PRODUCT_ID_CAPACITY as usize];
        // SAFETY: an open key, a NUL-terminated name, and a buffer of at
        // least `*size` bytes with its in/out size.
        let status = unsafe {
            RegQueryValueExA(
                key.0,
                s!("DigitalProductId"),
                None,
                None,
                Some(buffer.as_mut_ptr()),
                Some(std::ptr::from_mut(size)),
            )
        };
        if status != ERROR_SUCCESS {
            return Some(None);
        }
        buffer.truncate((*size).min(PRODUCT_ID_CAPACITY) as usize);
        Some(Some(buffer))
    }

    fn uuid(&mut self) -> [u8; 16] {
        // SAFETY: a plain call; it fails only without memory.
        let guid = unsafe { CoCreateGuid() }.unwrap_or_default();
        let mut bytes = [0u8; 16];
        bytes[..4].copy_from_slice(&guid.data1.to_le_bytes());
        bytes[4..6].copy_from_slice(&guid.data2.to_le_bytes());
        bytes[6..8].copy_from_slice(&guid.data3.to_le_bytes());
        bytes[8..].copy_from_slice(&guid.data4);
        bytes
    }

    fn user_name(&mut self) -> Option<Vec<u8>> {
        let mut buffer = vec![0u8; USER_NAME_CAPACITY as usize + 3];
        let mut size = USER_NAME_CAPACITY;
        // SAFETY: a buffer of more than `size` bytes and its size.
        unsafe { GetUserNameA(Some(PSTR(buffer.as_mut_ptr())), &raw mut size) }.ok()?;
        let end = buffer.iter().position(|&b| b == 0).unwrap_or(buffer.len());
        buffer.truncate(end);
        Some(buffer)
    }

    fn milliseconds(&mut self) -> u32 {
        // SAFETY: a plain query.
        unsafe { GetTickCount() }
    }
}

/// The memory figures of the low-machine warning in MiB: the physical
/// memory installed and the virtual address space still free to this
/// process (`dwTotalPhys >> 20`, `dwAvailVirtual >> 20`); `None` when the
/// system cannot say.
#[must_use]
pub fn memory_mib() -> Option<(u64, u64)> {
    let mut status = MEMORYSTATUSEX {
        dwLength: size_of::<MEMORYSTATUSEX>() as u32,
        ..Default::default()
    };
    // SAFETY: a sized out structure.
    unsafe { GlobalMemoryStatusEx(&raw mut status) }.ok()?;
    Some((status.ullTotalPhys >> 20, status.ullAvailVirtual >> 20))
}

/// The primary display's mode as the registry holds it
/// (`EnumDisplaySettings(NULL, ENUM_REGISTRY_SETTINGS)`), in pixels;
/// `None` when it cannot be read or a side is 0.
#[must_use]
pub fn desktop_size() -> Option<(u32, u32)> {
    let mut mode = DEVMODEW {
        dmSize: size_of::<DEVMODEW>() as u16,
        ..Default::default()
    };
    // SAFETY: the primary display and a sized out structure.
    if !unsafe { EnumDisplaySettingsW(None, ENUM_REGISTRY_SETTINGS, &raw mut mode) }.as_bool() {
        return None;
    }
    (mode.dmPelsWidth != 0 && mode.dmPelsHeight != 0)
        .then_some((mode.dmPelsWidth, mode.dmPelsHeight))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The queries answer on any Windows machine: the NT key opens, a
    /// UUID is not all zeros, the user has a name or the counter runs.
    #[test]
    fn the_machine_answers() {
        let mut machine = WinMachine;
        let mut size = PRODUCT_ID_CAPACITY;
        assert!(
            machine
                .product_id(ProductIdKey::WindowsNt, &mut size)
                .is_some()
        );
        assert_ne!(machine.uuid(), [0; 16]);
        let name = machine.user_name();
        assert!(name.is_none_or(|n| !n.is_empty() && !n.contains(&0)));
        let (physical, _) = memory_mib().unwrap();
        assert!(physical > 0);
        if let Some((w, h)) = desktop_size() {
            assert!(w > 0 && h > 0);
        }
    }
}
