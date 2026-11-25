//! The server's TLS identity (decision T16): a self-signed certificate and
//! its key, generated on the first run and kept, so that clients can pin
//! the certificate's fingerprint.

use std::fmt;
use std::fs;
use std::io;
use std::path::Path;

use neuron_core::net::pin::{CertificateFingerprint, FINGERPRINT_LEN};
use rustls::crypto::ring::cipher_suite::TLS13_AES_128_GCM_SHA256;

/// The name in the certificate. Clients address servers by IP address and
/// trust the pinned fingerprint, not a name, so it is only a label.
pub const CERTIFICATE_NAME: &str = "outpost-frontier-server";

/// The certificate's file in an identity directory, DER encoded.
pub const CERTIFICATE_FILE: &str = "server_certificate.der";

/// The key's file in an identity directory, PKCS #8 DER encoded.
pub const KEY_FILE: &str = "server_key.der";

/// Why an identity cannot be made or loaded.
#[derive(Debug)]
pub enum IdentityError {
    /// The key or the certificate could not be generated.
    Generate(rcgen::Error),
    /// The files could not be read or written.
    Io(io::Error),
}

impl fmt::Display for IdentityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Generate(error) => write!(f, "cannot generate the server certificate: {error}"),
            Self::Io(error) => write!(f, "cannot store the server certificate: {error}"),
        }
    }
}

impl std::error::Error for IdentityError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Generate(error) => Some(error),
            Self::Io(error) => Some(error),
        }
    }
}

/// A certificate and its private key.
#[derive(Clone)]
pub struct ServerIdentity {
    certificate: Vec<u8>,
    key: Vec<u8>,
}

impl fmt::Debug for ServerIdentity {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ServerIdentity")
            .field("fingerprint", &self.fingerprint())
            .finish_non_exhaustive()
    }
}

impl ServerIdentity {
    /// A new self-signed certificate with an ECDSA P-256 key.
    ///
    /// # Errors
    /// [`IdentityError::Generate`] when the key cannot be made.
    pub fn generate() -> Result<Self, IdentityError> {
        let certified = rcgen::generate_simple_self_signed(vec![CERTIFICATE_NAME.to_owned()])
            .map_err(IdentityError::Generate)?;
        Ok(Self {
            certificate: certified.cert.der().to_vec(),
            key: certified.signing_key.serialize_der(),
        })
    }

    /// The identity of a DER certificate and a PKCS #8 DER key. Whether
    /// they match is checked when a host starts with them.
    #[must_use]
    pub const fn from_der(certificate: Vec<u8>, key: Vec<u8>) -> Self {
        Self { certificate, key }
    }

    /// The identity kept in `directory` ([`CERTIFICATE_FILE`] and
    /// [`KEY_FILE`]), or a new one written there when either file is
    /// missing: the first run's certificate stays the server's.
    ///
    /// # Errors
    /// [`IdentityError::Io`] when the directory cannot be read or written,
    /// [`IdentityError::Generate`] as [`ServerIdentity::generate`].
    pub fn load_or_generate(directory: &Path) -> Result<Self, IdentityError> {
        let certificate_path = directory.join(CERTIFICATE_FILE);
        let key_path = directory.join(KEY_FILE);
        match (fs::read(&certificate_path), fs::read(&key_path)) {
            (Ok(certificate), Ok(key)) => return Ok(Self::from_der(certificate, key)),
            (Err(error), _) | (_, Err(error)) if error.kind() != io::ErrorKind::NotFound => {
                return Err(IdentityError::Io(error));
            }
            _ => {}
        }
        let identity = Self::generate()?;
        fs::create_dir_all(directory).map_err(IdentityError::Io)?;
        fs::write(&key_path, &identity.key).map_err(IdentityError::Io)?;
        fs::write(&certificate_path, &identity.certificate).map_err(IdentityError::Io)?;
        Ok(identity)
    }

    /// The certificate's DER bytes.
    #[must_use]
    pub fn certificate_der(&self) -> &[u8] {
        &self.certificate
    }

    /// The key's PKCS #8 DER bytes.
    #[must_use]
    pub fn key_der(&self) -> &[u8] {
        &self.key
    }

    /// The fingerprint clients pin.
    #[must_use]
    pub fn fingerprint(&self) -> CertificateFingerprint {
        fingerprint_of(&self.certificate)
    }
}

/// The SHA-256 digest of `certificate`, through the TLS provider's hash.
#[must_use]
pub fn fingerprint_of(certificate: &[u8]) -> CertificateFingerprint {
    let mut bytes = [0; FINGERPRINT_LEN];
    if let Some(suite) = TLS13_AES_128_GCM_SHA256.tls13() {
        let digest = suite.common.hash_provider.hash(certificate);
        bytes.copy_from_slice(digest.as_ref());
    }
    CertificateFingerprint::from_bytes(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identities_are_distinct_and_kept() {
        let first = ServerIdentity::generate().unwrap();
        let second = ServerIdentity::generate().unwrap();
        assert_ne!(first.fingerprint(), second.fingerprint());
        assert_eq!(
            ServerIdentity::from_der(first.certificate_der().to_vec(), first.key_der().to_vec())
                .fingerprint(),
            first.fingerprint()
        );

        let directory = std::env::temp_dir().join(format!(
            "neuron-server-identity-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let _ = fs::remove_dir_all(&directory);
        let created = ServerIdentity::load_or_generate(&directory).unwrap();
        let loaded = ServerIdentity::load_or_generate(&directory).unwrap();
        assert_eq!(created.fingerprint(), loaded.fingerprint());
        assert_eq!(created.key_der(), loaded.key_der());
        fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn the_fingerprint_is_the_sha256_of_the_der() {
        // FIPS 180-2, appendix B.1.
        assert_eq!(
            fingerprint_of(b"abc").to_string(),
            "BA:78:16:BF:8F:01:CF:EA:41:41:40:DE:5D:AE:22:23:\
             B0:03:61:A3:96:17:7A:9C:B4:10:FF:61:F2:00:15:AD"
        );
    }
}
