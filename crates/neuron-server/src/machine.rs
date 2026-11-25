//! The machine queries and the clock-seeded generator of a dedicated
//! server's first start, when `InitServerAccount` writes `accounts.cfg`
//! (`neuron_server::account::init_server_account`).
//!
//! The C++ answers the machine queries from the registry
//! (`DigitalProductId`), `UuidCreate`, `GetUserNameA` and `timeGetTime`.
//! [`PortableMachine`] has no registry: it reports no product id, so the
//! account is derived from a fresh random UUID and the user name, the path
//! the C++ takes on a machine whose product id cannot be read (D914).

use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};
use std::time::{SystemTime, UNIX_EPOCH};

use neuron_core::account::{MachineIdentity, ProductIdKey};
use neuron_core::rng::CrtRand;

use crate::account::ServerAccountSeed;

/// Milliseconds of the system clock, cut to 32 bits as `timeGetTime`'s
/// counter is.
fn clock_milliseconds() -> u32 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| (d.as_millis() & 0xFFFF_FFFF) as u32)
}

/// Machine queries without platform calls.
#[derive(Clone, Debug, Default)]
pub struct PortableMachine {
    /// The user name; `None` reads `USERNAME`, then `USER`.
    pub user_name: Option<Vec<u8>>,
}

impl MachineIdentity for PortableMachine {
    fn product_id(&mut self, _key: ProductIdKey, _size: &mut u32) -> Option<Option<Vec<u8>>> {
        None
    }

    fn uuid(&mut self) -> [u8; 16] {
        let mut bytes = [0u8; 16];
        for half in bytes.chunks_exact_mut(8) {
            let mut hasher = RandomState::new().build_hasher();
            hasher.write_u32(clock_milliseconds());
            half.copy_from_slice(&hasher.finish().to_le_bytes());
        }
        // A version 4, variant 1 UUID, as `UuidCreate` makes.
        bytes[7] = (bytes[7] & 0x0F) | 0x40;
        bytes[8] = (bytes[8] & 0x3F) | 0x80;
        bytes
    }

    fn user_name(&mut self) -> Option<Vec<u8>> {
        if let Some(name) = &self.user_name {
            return Some(name.clone());
        }
        std::env::var("USERNAME")
            .or_else(|_| std::env::var("USER"))
            .ok()
            .filter(|name| !name.is_empty())
            .map(String::into_bytes)
    }

    fn milliseconds(&mut self) -> u32 {
        clock_milliseconds()
    }
}

/// The CRT generator `InitServerAccount` seeds from the clock. The C++
/// reseeds the process's one generator; the server's world keeps its own
/// ([`neuron_core::rng::CrtRand`] per world), so this one is separate.
#[derive(Clone, Debug, Default)]
pub struct ClockSeed {
    rand: CrtRand,
}

impl ServerAccountSeed for ClockSeed {
    fn reseed_from_clock(&mut self) {
        self.rand.seed(clock_milliseconds());
    }

    fn rand(&mut self) -> i32 {
        self.rand.rand()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::account::{AccountsRoot, init_server_account};

    #[test]
    fn a_portable_machine_makes_a_verified_server_account() {
        let dir = std::env::temp_dir().join(format!("outpost-machine-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let root = AccountsRoot::new(&dir, true);
        let mut machine = PortableMachine {
            user_name: Some(b"admin".to_vec()),
        };
        let account = init_server_account(&root, &mut ClockSeed::default(), &mut machine)
            .unwrap()
            .expect("an account");
        assert!(account.keys.verify());
        // The second start reads the same file.
        let again = init_server_account(&root, &mut ClockSeed::default(), &mut machine)
            .unwrap()
            .unwrap();
        assert_eq!(again, account);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn uuids_differ() {
        let mut machine = PortableMachine::default();
        assert_ne!(machine.uuid(), machine.uuid());
        assert_eq!(machine.uuid()[7] & 0xF0, 0x40);
    }
}
