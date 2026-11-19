//! The control factory registry (`win_factory.cpp`, `WINFACTORY<T>`).
//!
//! In the reference every concrete class has a static `WINFACTORY<T>`
//! object that pushes itself onto one global list at static
//! initialisation; `WinFactoryCreate(nickname, class, parent, payload)`
//! finds the first factory whose name `strcmp`-equals the class (names cut
//! to 31 bytes), makes the object, sets its class name, calls its `Create`
//! (deleting it on failure) and then gives it the nickname. The class names
//! are the tokens of the page descriptors and the `class_name` keys of the
//! `[BaseFrame]` files.
//!
//! Here the registry is a value in [`super::tree::Ui`]: the toolkit
//! registers its generic controls ([`Factory::with_toolkit`]) and the game
//! registers its own (the NeuroNet windows, HUD panels and dialogs) at
//! start-up. [`CLASSES`] lists every factory of the reference with the
//! crate that owns it.

use super::control::PlainControl;
use super::tree::{NodeId, Payload, Ui, Widget};

/// Makes a new, unregistered object of a class.
pub type Constructor = fn() -> Box<dyn Widget>;

/// Who owns a factory of the reference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Owner {
    /// A generic control of the UI toolkit (`neuron-client::ui`).
    Toolkit,
    /// A game screen, dialog or game-specific control (`game-app`).
    Game,
}

/// Every `WINFACTORY` of the reference (61 `WINFACTORY<T>` statics, the
/// `RichTextWireButtonControl`, `NN_PlayerInfo`, `ToolTip` and `Rollover`
/// factories of their own types): 65 class names. The HUD elements have a
/// registry of their own (`HUDFACTORY`, P5.7).
pub const CLASSES: [(&str, Owner); 65] = [
    ("WinControl", Owner::Toolkit),
    ("ButtonControl", Owner::Toolkit),
    ("ToggleButtonControl", Owner::Toolkit),
    ("RichTextButtonControl", Owner::Toolkit),
    ("StaticControl", Owner::Toolkit),
    ("TextControl", Owner::Toolkit),
    ("EditControl", Owner::Toolkit),
    ("ScrollControl", Owner::Toolkit),
    ("WireStaticControl", Owner::Toolkit),
    ("WireButtonControl", Owner::Toolkit),
    ("RichTextWireButtonControl", Owner::Toolkit),
    ("RichTextWin", Owner::Toolkit),
    ("RichTextEditWin", Owner::Toolkit),
    ("MsgWin", Owner::Toolkit),
    ("UIStaticText", Owner::Toolkit),
    ("UITextEdit", Owner::Toolkit),
    ("UITextMsgButton", Owner::Toolkit),
    ("UIListBox", Owner::Toolkit),
    ("UIWinObject", Owner::Toolkit),
    ("UIRichTextWin", Owner::Toolkit),
    ("UITable", Owner::Toolkit),
    ("UIFastTable", Owner::Toolkit),
    ("UIScrollTable", Owner::Toolkit),
    ("UIScroll", Owner::Toolkit),
    ("UICycle", Owner::Toolkit),
    ("ToolTip", Owner::Toolkit),
    ("Rollover", Owner::Toolkit),
    ("UIWinDeform", Owner::Game),
    ("CargoBarControl", Owner::Game),
    ("ShipViewControl", Owner::Game),
    ("UIShipViewControl", Owner::Game),
    ("ChatControl", Owner::Game),
    ("ChatTextInputDisplay", Owner::Game),
    ("NavBar", Owner::Game),
    ("BlockingDialog", Owner::Game),
    ("ServerFilterDialog", Owner::Game),
    ("MissionObjectiveWindow", Owner::Game),
    ("MissionLogMoveToNNWindow", Owner::Game),
    ("MissionLogMoveFromNNWindow", Owner::Game),
    ("NeuroNetMissionMessage", Owner::Game),
    ("NeuroNetStoryStar", Owner::Game),
    ("NeuroNetNavMap", Owner::Game),
    ("NN_MainButton", Owner::Game),
    ("NN_ArrivalText", Owner::Game),
    ("NN_CommodityByBase", Owner::Game),
    ("NN_Confirm", Owner::Game),
    ("NN_Dealer", Owner::Game),
    ("NN_Error", Owner::Game),
    ("NN_Exit", Owner::Game),
    ("NN_GroupControl", Owner::Game),
    ("NN_Help", Owner::Game),
    ("NN_Inventory", Owner::Game),
    ("NN_LoadSave", Owner::Game),
    ("NN_MissionVendor", Owner::Game),
    ("NN_NetworkOperation", Owner::Game),
    ("NN_News", Owner::Game),
    ("NN_OfferDialog", Owner::Game),
    ("NN_Pause", Owner::Game),
    ("NN_PlayerInfo", Owner::Game),
    ("NN_Preferences", Owner::Game),
    ("NN_Prompt", Owner::Game),
    ("NN_Repair", Owner::Game),
    ("NN_Scanner", Owner::Game),
    ("NN_ShipTrader", Owner::Game),
    ("NN_Trade", Owner::Game),
];

/// The owner of a reference class name.
#[must_use]
pub fn owner(class_name: &[u8]) -> Option<Owner> {
    CLASSES
        .iter()
        .find(|(name, _)| name.as_bytes() == class_name)
        .map(|&(_, owner)| owner)
}

/// The registry: name and constructor, newest first in lookups.
#[derive(Clone, Debug, Default)]
pub struct Factory {
    entries: Vec<(&'static str, Constructor)>,
}

/// The name as the factory stores it: at most 31 bytes.
fn clamp(name: &[u8]) -> &[u8] {
    let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
    &name[..end.min(31)]
}

impl Factory {
    /// An empty registry.
    #[must_use]
    pub fn new() -> Factory {
        Factory::default()
    }

    /// The registry with the toolkit's controls.
    #[must_use]
    pub fn with_toolkit() -> Factory {
        let mut factory = Factory::new();
        factory.register("WinControl", || Box::new(PlainControl::default()));
        super::controls::register(&mut factory);
        factory
    }

    /// `WinFactoryRegister`: in front of every earlier factory.
    pub fn register(&mut self, name: &'static str, constructor: Constructor) {
        self.entries.push((name, constructor));
    }

    /// `WinFactoryUnregister`.
    pub fn unregister(&mut self, name: &str) {
        if let Some(i) = self.entries.iter().rposition(|(n, _)| *n == name) {
            self.entries.remove(i);
        }
    }

    /// The first factory whose (clamped) name equals the class name.
    #[must_use]
    pub fn find(&self, class_name: &[u8]) -> Option<(&'static str, Constructor)> {
        self.entries
            .iter()
            .rev()
            .find(|(name, _)| clamp(name.as_bytes()) == class_name)
            .copied()
    }

    /// The registered names, newest first.
    pub fn names(&self) -> impl Iterator<Item = &'static str> + '_ {
        self.entries.iter().rev().map(|(n, _)| *n)
    }
}

impl Ui {
    /// `WinFactoryCreate`: the class's object made, created under `parent`
    /// with `payload`, then named (`None` gives the empty name). `None`
    /// when no factory has the class or its `Create` failed.
    pub fn create_by_class(
        &mut self,
        nickname: Option<&[u8]>,
        class_name: &[u8],
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> Option<NodeId> {
        let (name, constructor) = self.factory.find(class_name)?;
        let id = self.create(constructor(), Some(name), parent, payload)?;
        if let Some(n) = self.node_mut(id) {
            n.set_name(nickname);
        }
        Some(id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_class_table_has_every_reference_factory_once() {
        let mut names: Vec<&str> = CLASSES.iter().map(|(n, _)| *n).collect();
        names.sort_unstable();
        names.dedup();
        assert_eq!(names.len(), CLASSES.len());
        assert!(CLASSES.iter().all(|(n, _)| n.len() <= 31));
        assert_eq!(owner(b"UIScroll"), Some(Owner::Toolkit));
        assert_eq!(owner(b"NN_Trade"), Some(Owner::Game));
        assert_eq!(owner(b"uiscroll"), None);
    }

    #[test]
    fn later_registrations_shadow_earlier_ones() {
        let mut f = Factory::new();
        f.register("A", || Box::new(PlainControl::default()));
        let first = f.find(b"A").unwrap().1 as usize;
        f.register("A", || Box::new(PlainControl::default()));
        let second = f.find(b"A").unwrap().1 as usize;
        assert_ne!(first, second);
        f.unregister("A");
        assert_eq!(f.find(b"A").unwrap().1 as usize, first);
        assert!(f.find(b"a").is_none());
    }
}
