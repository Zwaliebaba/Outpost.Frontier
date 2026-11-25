//! Certificate pinning (decision T16): the client accepts the server's
//! self-signed certificate by its fingerprint, not by a certificate
//! authority or a host name.
//!
//! The handshake signature is still verified against the certificate's
//! key, so a server must hold the key of the certificate it presents. A
//! certificate is checked only for its fingerprint: it names no host
//! (servers are addressed by IP address) and its validity dates are not
//! read (the pin, not the date, is the trust; a server keeps its first
//! certificate for good).

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, PoisonError};

use neuron_core::net::pin::{CertificateFingerprint, FINGERPRINT_LEN, ServerTrust};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::ring::cipher_suite::TLS13_AES_128_GCM_SHA256;
use rustls::crypto::{WebPkiSupportedAlgorithms, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{CertificateError, DigitallySignedStruct, SignatureScheme};

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

/// The verifier of one connection attempt: remembers the certificate it
/// was shown and whether it refused it for its pin.
#[derive(Debug)]
pub(super) struct PinVerifier {
    trust: ServerTrust,
    presented: Mutex<Option<CertificateFingerprint>>,
    mismatch: AtomicBool,
    algorithms: WebPkiSupportedAlgorithms,
}

impl PinVerifier {
    pub(super) fn new(trust: ServerTrust) -> Self {
        Self {
            trust,
            presented: Mutex::new(None),
            mismatch: AtomicBool::new(false),
            algorithms: rustls::crypto::ring::default_provider().signature_verification_algorithms,
        }
    }

    /// The fingerprint of the certificate the server presented.
    pub(super) fn presented(&self) -> Option<CertificateFingerprint> {
        *self
            .presented
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
    }

    /// Whether the certificate was refused because it is not the pinned
    /// one.
    pub(super) fn mismatched(&self) -> bool {
        self.mismatch.load(Ordering::Relaxed)
    }
}

impl ServerCertVerifier for PinVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let fingerprint = fingerprint_of(end_entity);
        *self
            .presented
            .lock()
            .unwrap_or_else(PoisonError::into_inner) = Some(fingerprint);
        match self.trust {
            ServerTrust::FirstUse => Ok(ServerCertVerified::assertion()),
            ServerTrust::Pinned(pin) if pin == fingerprint => Ok(ServerCertVerified::assertion()),
            ServerTrust::Pinned(_) => {
                self.mismatch.store(true, Ordering::Relaxed);
                Err(rustls::Error::InvalidCertificate(
                    CertificateError::ApplicationVerificationFailure,
                ))
            }
        }
    }

    /// QUIC runs TLS 1.3 only.
    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::General("TLS 1.2 is not used".to_owned()))
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algorithms.supported_schemes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_fingerprint_is_the_sha256_of_the_der() {
        // FIPS 180-2, appendix B.1.
        assert_eq!(
            fingerprint_of(b"abc").to_string(),
            "BA:78:16:BF:8F:01:CF:EA:41:41:40:DE:5D:AE:22:23:\
             B0:03:61:A3:96:17:7A:9C:B4:10:FF:61:F2:00:15:AD"
        );
    }

    #[test]
    fn a_pin_refuses_another_certificate() {
        let certificate = CertificateDer::from(vec![1, 2, 3]);
        let name = ServerName::try_from("outpost-frontier").unwrap();
        let now = UnixTime::now();

        let first = PinVerifier::new(ServerTrust::FirstUse);
        assert!(
            first
                .verify_server_cert(&certificate, &[], &name, &[], now)
                .is_ok()
        );
        assert_eq!(first.presented(), Some(fingerprint_of(&[1, 2, 3])));
        assert!(!first.mismatched());

        let pinned = PinVerifier::new(ServerTrust::Pinned(fingerprint_of(&[1, 2, 3])));
        assert!(
            pinned
                .verify_server_cert(&certificate, &[], &name, &[], now)
                .is_ok()
        );
        let other = PinVerifier::new(ServerTrust::Pinned(fingerprint_of(&[9])));
        assert!(
            other
                .verify_server_cert(&certificate, &[], &name, &[], now)
                .is_err()
        );
        assert!(other.mismatched());
        assert_eq!(other.presented(), Some(fingerprint_of(&[1, 2, 3])));
    }
}
