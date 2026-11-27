//! The interface windows `InitializeEngine` makes (`engine_init.cpp:927-944`),
//! in its order: the mission briefing's parent window (`MissionBriefing::
//! PreInit`), the group-request window (`ChatInit`), then the tool tip and
//! the rollover (`MenuUiInitToolTipAndRollover`). Each registers itself, so
//! this order is the interface registry's and the oracle's tree dumps'.
//!
//! Only the windows are made here: the briefing state's other fields, the
//! chat's channel table and message store come with their screens.

use neuron_client::ui::children::{StaticInit, init_static_control};
use neuron_client::ui::factory::Factory;
use neuron_client::ui::tree::{ACTIVE, NodeId, Payload, Ui, VISIBLE};
use neuron_core::math::Vector;

use crate::chat::group_control;

/// `kBriefingWindowMesh` (`mission_briefing.cpp`).
pub const BRIEFING_WINDOW_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\repairblackbox.3db";

/// The windows made, by their owners' fields.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BootWindows {
    /// `MissionBriefing +0x08`.
    pub briefing_parent: Option<NodeId>,
    /// `G_ChatGroupControl`.
    pub group_control: Option<NodeId>,
    pub tooltip: Option<NodeId>,
    pub rollover: Option<NodeId>,
}

/// Registers the game's classes these windows need.
pub fn register(factory: &mut Factory) {
    group_control::register(factory);
}

fn hide(ui: &mut Ui, id: NodeId) {
    if let Some(n) = ui.node_mut(id) {
        n.flags &= !(ACTIVE | VISIBLE);
    }
}

/// `MissionBriefing::PreInit`'s window: a "StaticControl" named
/// "BriefingParentWindow" with the black box mesh (no wire, no clear
/// render), at (-0.62, 0.3, -1.25), hidden until the briefing's first frame.
pub fn create_briefing_parent(ui: &mut Ui) -> Option<NodeId> {
    let id = ui.create_by_class(
        Some(b"BriefingParentWindow"),
        b"StaticControl",
        None,
        &mut Payload::None,
    )?;
    init_static_control(
        ui,
        id,
        Some(BRIEFING_WINDOW_MESH),
        &StaticInit {
            no_wire_render: true,
            no_clear_render: true,
            ..StaticInit::default()
        },
    );
    ui.set_position(id, Vector::new(-0.62, 0.3, -1.25));
    hide(ui, id);
    Some(id)
}

/// `ChatInit`'s window: "NN_GroupControl" at (0, 0, -1), hidden.
pub fn create_group_control(ui: &mut Ui) -> Option<NodeId> {
    let id = ui.create_by_class(
        Some(group_control::CLASS),
        group_control::CLASS,
        None,
        &mut Payload::None,
    )?;
    ui.set_position(id, Vector::new(0.0, 0.0, -1.0));
    hide(ui, id);
    Some(id)
}

/// The four windows, in `InitializeEngine`'s order.
pub fn create(ui: &mut Ui) -> BootWindows {
    let briefing_parent = create_briefing_parent(ui);
    let group_control = create_group_control(ui);
    // `MenuUiInitToolTipAndRollover`: the nickname is the class name.
    let tooltip = ui.create_by_class(Some(b"ToolTip"), b"ToolTip", None, &mut Payload::None);
    let rollover = ui.create_by_class(Some(b"Rollover"), b"Rollover", None, &mut Payload::None);
    BootWindows {
        briefing_parent,
        group_control,
        tooltip,
        rollover,
    }
}
