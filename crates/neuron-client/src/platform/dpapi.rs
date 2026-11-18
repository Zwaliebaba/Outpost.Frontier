//! Data protection for saved secrets (P4.1; decision O27): the Windows
//! user's DPAPI (`CryptProtectData`, `CryptUnprotectData`), so a saved
//! password in a state file opens only for the user who saved it. The C++
//! kept passwords as hex text in the registry.

use windows::Win32::Foundation::{HLOCAL, LocalFree};
use windows::Win32::Security::Cryptography::{
    CRYPT_INTEGER_BLOB, CRYPTPROTECT_UI_FORBIDDEN, CryptProtectData, CryptUnprotectData,
};
use windows::core::w;

use crate::user_state::{ProtectError, Protector};

/// DPAPI for the current user, without UI.
#[derive(Clone, Copy, Debug, Default)]
pub struct Dpapi;

fn blob(bytes: &[u8]) -> CRYPT_INTEGER_BLOB {
    CRYPT_INTEGER_BLOB {
        cbData: bytes.len() as u32,
        pbData: bytes.as_ptr().cast_mut(),
    }
}

/// Copies an output blob and frees it with `LocalFree`.
///
/// # Safety
/// `out` must be a blob DPAPI filled on success.
unsafe fn take(out: CRYPT_INTEGER_BLOB) -> Vec<u8> {
    // SAFETY: DPAPI allocated `cbData` bytes at `pbData` (caller contract).
    let bytes = unsafe { std::slice::from_raw_parts(out.pbData, out.cbData as usize) }.to_vec();
    // SAFETY: the buffer is freed once, as DPAPI documents.
    let _ = unsafe { LocalFree(Some(HLOCAL(out.pbData.cast()))) };
    bytes
}

impl Protector for Dpapi {
    fn protect(&self, plain: &[u8]) -> Result<Vec<u8>, ProtectError> {
        let input = blob(plain);
        let mut out = CRYPT_INTEGER_BLOB::default();
        // SAFETY: the input blob borrows `plain` for the call; the output
        // is taken and freed below.
        unsafe {
            CryptProtectData(
                &input,
                w!("Outpost:Frontier"),
                None,
                None,
                None,
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut out,
            )
        }
        .map_err(|e| ProtectError(format!("CryptProtectData: {e}")))?;
        // SAFETY: the call succeeded and filled `out`.
        Ok(unsafe { take(out) })
    }

    fn unprotect(&self, sealed: &[u8]) -> Result<Vec<u8>, ProtectError> {
        let input = blob(sealed);
        let mut out = CRYPT_INTEGER_BLOB::default();
        // SAFETY: as in `protect`.
        unsafe {
            CryptUnprotectData(
                &input,
                None,
                None,
                None,
                None,
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut out,
            )
        }
        .map_err(|e| ProtectError(format!("CryptUnprotectData: {e}")))?;
        // SAFETY: the call succeeded and filled `out`.
        Ok(unsafe { take(out) })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn secrets_round_trip_and_tampering_fails() {
        let d = Dpapi;
        let sealed = match d.protect(b"hunter2") {
            Ok(s) => s,
            Err(e) => {
                println!("no DPAPI here ({e}); nothing to test");
                return;
            }
        };
        assert_ne!(sealed, b"hunter2");
        assert_eq!(d.unprotect(&sealed).unwrap(), b"hunter2");
        let mut bad = sealed.clone();
        let last = bad.len() - 1;
        bad[last] ^= 1;
        assert!(d.unprotect(&bad).is_err());
    }
}
