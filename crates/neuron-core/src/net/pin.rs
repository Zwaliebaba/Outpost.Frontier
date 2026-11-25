//! The server certificate's fingerprint, which a client pins (decision
//! T16, work package P3.8).
//!
//! A server presents a self-signed certificate it generated on its first
//! run. The client trusts it on first use and keeps its fingerprint, the
//! SHA-256 digest of the certificate's DER bytes; on later connections it
//! accepts only a certificate with that digest, and the change of one is
//! reported so the player can be warned. The digest is computed by the
//! transports, which hold a TLS provider; this module is the value, its
//! text form and its comparison.

use core::fmt;
use core::str::FromStr;

/// Bytes of a fingerprint: one SHA-256 digest.
pub const FINGERPRINT_LEN: usize = 32;

/// The SHA-256 digest of a server certificate's DER encoding.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct CertificateFingerprint([u8; FINGERPRINT_LEN]);

impl CertificateFingerprint {
    /// The fingerprint of these digest bytes.
    #[must_use]
    pub const fn from_bytes(bytes: [u8; FINGERPRINT_LEN]) -> Self {
        Self(bytes)
    }

    /// The digest bytes.
    #[must_use]
    pub const fn as_bytes(&self) -> &[u8; FINGERPRINT_LEN] {
        &self.0
    }
}

impl fmt::Display for CertificateFingerprint {
    /// Upper-case hexadecimal pairs joined by colons, as certificate
    /// viewers show them.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for (index, byte) in self.0.iter().enumerate() {
            if index > 0 {
                f.write_str(":")?;
            }
            write!(f, "{byte:02X}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for CertificateFingerprint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "CertificateFingerprint({self})")
    }
}

/// Why a text is not a fingerprint.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParseFingerprintError;

impl fmt::Display for ParseFingerprintError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("not 32 hexadecimal byte pairs")
    }
}

impl std::error::Error for ParseFingerprintError {}

impl FromStr for CertificateFingerprint {
    type Err = ParseFingerprintError;

    /// 64 hexadecimal digits in either case, optionally with a colon
    /// between pairs.
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let digits: Vec<u8> = text.bytes().filter(|&byte| byte != b':').collect();
        if digits.len() != FINGERPRINT_LEN * 2 {
            return Err(ParseFingerprintError);
        }
        let mut bytes = [0; FINGERPRINT_LEN];
        let (pairs, _) = digits.as_chunks::<2>();
        for (byte, &[high, low]) in bytes.iter_mut().zip(pairs) {
            let high = hex_value(high).ok_or(ParseFingerprintError)?;
            let low = hex_value(low).ok_or(ParseFingerprintError)?;
            *byte = high << 4 | low;
        }
        Ok(Self(bytes))
    }
}

const fn hex_value(digit: u8) -> Option<u8> {
    match digit {
        b'0'..=b'9' => Some(digit - b'0'),
        b'a'..=b'f' => Some(digit - b'a' + 10),
        b'A'..=b'F' => Some(digit - b'A' + 10),
        _ => None,
    }
}

/// What a client accepts from the server it connects to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ServerTrust {
    /// The first connection to a server: any certificate whose handshake
    /// signature verifies. The client then reports the fingerprint to
    /// keep.
    FirstUse,
    /// Only a certificate with this fingerprint. Another one ends the
    /// attempt with [`super::transport::DisconnectReason::CertificateChanged`].
    Pinned(CertificateFingerprint),
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_form_round_trips() {
        let mut bytes = [0; FINGERPRINT_LEN];
        for (index, byte) in bytes.iter_mut().enumerate() {
            *byte = (index as u8).wrapping_mul(37);
        }
        let fingerprint = CertificateFingerprint::from_bytes(bytes);
        let text = fingerprint.to_string();
        assert_eq!(text.len(), 32 * 3 - 1);
        assert!(text.starts_with("00:25:4A:6F"));
        assert_eq!(text.parse(), Ok(fingerprint));
        assert_eq!(
            text.replace(':', "").to_lowercase().parse(),
            Ok(fingerprint)
        );
        assert_eq!(fingerprint.as_bytes(), &bytes);
    }

    #[test]
    fn malformed_text_is_refused() {
        assert_eq!(
            "00".parse::<CertificateFingerprint>(),
            Err(ParseFingerprintError)
        );
        let bad = "G".repeat(64);
        assert_eq!(
            bad.parse::<CertificateFingerprint>(),
            Err(ParseFingerprintError)
        );
    }
}
