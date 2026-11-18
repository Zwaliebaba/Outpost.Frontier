//! The user's Documents folder (`SHGetKnownFolderPath`), under which
//! [`crate::user_state::UserPaths`] keeps the per-user files (P4.1).

use std::ffi::OsString;
use std::os::windows::ffi::OsStringExt;
use std::path::PathBuf;

use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::UI::Shell::{FOLDERID_Documents, KF_FLAG_DEFAULT, SHGetKnownFolderPath};

/// The Documents folder of the current user (redirected folders
/// included); `None` when the shell cannot say.
#[must_use]
pub fn documents() -> Option<PathBuf> {
    // SAFETY: a known-folder id constant and no token: the current user.
    let path = unsafe { SHGetKnownFolderPath(&FOLDERID_Documents, KF_FLAG_DEFAULT, None) }.ok()?;
    // SAFETY: on success the shell returns a NUL-terminated wide string it
    // allocated; it is copied, then freed once with CoTaskMemFree.
    let owned = unsafe {
        let wide = path.as_wide();
        let os = OsString::from_wide(wide);
        CoTaskMemFree(Some(path.0 as *const _));
        os
    };
    Some(PathBuf::from(owned))
}

#[cfg(test)]
mod tests {
    #[test]
    fn documents_is_an_absolute_path() {
        match super::documents() {
            Some(p) => assert!(p.is_absolute(), "{}", p.display()),
            None => println!("no Documents folder here; nothing to test"),
        }
    }
}
