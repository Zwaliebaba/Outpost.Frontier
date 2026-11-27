//! The client's object database (`ClientObjDb_*`, `client_objdb.cpp`; work
//! package P5.4, record 0009 point 4): the game objects the client knows,
//! by id, in the two buckets its finders take (0 any object, 1 ships).
//!
//! This first part holds what the event receiver's create, launch and
//! equipment slots look up: an entry per registered object with its kind
//! and archetype, and in single player the shared world's handle the create
//! message found (record 0002: the client adds its own state beside the
//! server's object rather than a second object). The game objects' own
//! state (engine instances, effects, the remote physics block) comes with
//! the rest of P5.4.

use std::collections::BTreeMap;

use game_sim::object::ObjHandle;

/// The kind of a client game object (`FClientShipGameobj`,
/// `FClientSolarGameobj`, ...).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClientObjectKind {
    /// A ship (bucket 1).
    Ship,
    /// A solar.
    Solar,
}

/// One registered game object.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClientObject {
    /// The object's id.
    pub id: u32,
    /// Its kind.
    pub kind: ClientObjectKind,
    /// The archetype's id (the nickname hash).
    pub archetype: u32,
    /// Single player: the shared world's object (`Gameobj::find`).
    pub handle: Option<ObjHandle>,
}

/// `ClientObjDb`: the registered game objects.
#[derive(Clone, Debug, Default)]
pub struct ClientObjects {
    by_id: BTreeMap<u32, ClientObject>,
    /// `PhySys::AnyActive()`: a system is entered, so creates are taken
    /// (`StarSysEnter` makes it true, leaving the system false).
    pub physics_active: bool,
}

impl ClientObjects {
    /// `ClientObjDb_RegisterGameobj`: the object under its id; one already
    /// there is replaced.
    pub fn register(&mut self, object: ClientObject) {
        self.by_id.insert(object.id, object);
    }

    /// `ClientObjDb_FindObject(id, bucket)`: bucket 1 finds ships only,
    /// any other bucket every kind.
    #[must_use]
    pub fn find(&self, id: u32, bucket: u32) -> Option<&ClientObject> {
        self.by_id
            .get(&id)
            .filter(|o| bucket != 1 || o.kind == ClientObjectKind::Ship)
    }

    /// Removes the object with `id`.
    pub fn remove(&mut self, id: u32) -> Option<ClientObject> {
        self.by_id.remove(&id)
    }

    /// The objects in id order.
    pub fn iter(&self) -> impl Iterator<Item = &ClientObject> {
        self.by_id.values()
    }

    /// How many objects are registered.
    #[must_use]
    pub fn len(&self) -> usize {
        self.by_id.len()
    }

    /// True when none is.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.by_id.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bucket_one_finds_ships_only() {
        let mut db = ClientObjects::default();
        for (id, kind) in [(11, ClientObjectKind::Ship), (40, ClientObjectKind::Solar)] {
            db.register(ClientObject {
                id,
                kind,
                archetype: 0,
                handle: None,
            });
        }
        assert!(db.find(11, 1).is_some() && db.find(11, 0).is_some());
        assert!(db.find(40, 1).is_none() && db.find(40, 0).is_some());
        assert!(db.find(41, 0).is_none());
        assert_eq!(db.remove(40).map(|o| o.id), Some(40));
        assert_eq!(db.len(), 1);
    }
}
