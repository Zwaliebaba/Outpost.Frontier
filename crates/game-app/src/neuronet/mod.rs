//! The NeuroNet windows the client makes on entering space
//! (`nn_modal.cpp`, `nn_main_button.cpp`): the NeuroNet bar
//! ([`main_button`], "NN_MainButton") and the arrival caption
//! ([`arrival_text`], "NN_ArrivalText").
//!
//! The reference keeps each window in a field (`G_NnMainButton`,
//! `G_NnArrivalText`) that the window's constructor sets and its destructor
//! clears. Here the caller holds the node in an `Option<NodeId>`; a node
//! that has been deleted no longer answers as the window, which is the
//! cleared field.

use neuron_client::ui::factory::Factory;

pub mod arrival_text;
pub mod main_button;

/// Registers the two factory classes (`WINFACTORY<NN_MainButton>` and
/// `WINFACTORY<NN_ArrivalText>`).
pub fn register(factory: &mut Factory) {
    main_button::register(factory);
    arrival_text::register(factory);
}
