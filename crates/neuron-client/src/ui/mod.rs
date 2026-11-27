//! The client UI toolkit (work package P4.13): the interface objects of
//! `CppSource/OutpostFrontier/Client` and the machinery they stand on.
//!
//! # How the reference's toolkit is built
//!
//! The interface is a 3D scene. Every object is a `Win3D` (`win3d.cpp`): a
//! tree node with a position and orientation in the space of the Win3D
//! camera, which sits at the origin looking down -Z with planes 0.1 and
//! 1000 and a fixed horizontal angle ([`camera`]). Objects register in
//! creation order; a manager keeps the keyboard focus, the stack of mouse
//! captures, the object under the mouse and the mouse ray, picks objects
//! with that ray, routes the key-map events to them, updates the roots each
//! frame and draws them sorted by layer and depth ([`tree`], [`message`]).
//! The modal NeuroNet dialogs push a capture and count up a global that
//! makes the update run the captured control alone (the modal stack).
//!
//! `WinControl` (`wincontrol_base.cpp`, [`control`]) is the mesh-backed
//! control the NeuroNet windows, scroll bars and rich-text windows derive
//! from: a `.cmp`/`.3db` mesh with an offset, a hit box (`mouse_size`), a
//! tooltip, the capture flag, a parent hardpoint, a property switch and a
//! `[BaseFrame]` INI loader that builds sub-controls by class name through
//! the factory registry (`win_factory.cpp`, [`factory`]): 65 `WINFACTORY`
//! statics, one per concrete class.
//!
//! The front-end pages are text descriptors (`PAGE_*.page`,
//! `ui_page_desc.cpp`) instantiated by a `UIPage::MasterWin` into a frame
//! scaled by the visible height (`ui_page.cpp`, [`page`]). Text objects
//! (`text_win.cpp`) draw a `Font::String`, whose pages go as screen quads
//! to the batcher of `screen_quad_state.cpp` ([`quad_batch`]), which also
//! takes the HUD bars, effects, icons and the IME background. Hovered
//! controls pulse between palette colours (`nn_ui_pulse.cpp`, [`pulse`]).
//! The generic controls ([`controls`]: list boxes, scroll bars and cycles,
//! framed panels, text and edit controls, static, button and wire controls,
//! the mesh scroll bar, the table, the tooltip and the rollover, the
//! page's rich-text windows) draw their frames, arrows and rules through an
//! OpenGL-like immediate-mode layer (`win3d_immediate.cpp`: the calls
//! [`immediate`], the layer [`win3d_immediate`], its line batchers and
//! overlay pass [`win3d_renderables`]) and call the client's sounds, commands and options through
//! [`services`]. Buttons whose mesh names a tile of the button montage
//! draw it as a screen-space icon ([`montage`]).
//! The software cursor is a named record of `mouse.ini` drawn from a
//! texture shape at the end of the frame ([`shape`], [`cursor`]).
//!
//! The reference build's port-only widescreen support turns the cameras
//! "Hor+" and pushes the HUD's corner windows to the surface edges by
//! shearing the Win3D camera while they draw or take the mouse
//! ([`widescreen`]).
//!
//! # The port
//!
//! One [`tree::Ui`] value holds the manager's state, the camera, the
//! factory registry, the pulse and the fonts (decision T10). Objects are
//! nodes of an arena with generational ids; each may hold a
//! [`tree::Widget`] (the virtual slots) whose defaults are the `Win3D`
//! bodies. Game screens and game-specific controls register their own
//! factories from `game-app`.

// The x87 compares are written as the reference makes them: a negated
// ordered compare is true for NaN.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod camera;
pub mod children;
pub mod control;
pub mod controls;
pub mod cursor;
pub mod factory;
pub mod ime;
pub mod immediate;
pub mod message;
pub mod montage;
pub mod page;
pub mod pulse;
pub mod quad_batch;
pub mod services;
pub mod shape;
pub mod tree;
pub mod widescreen;
pub mod win3d_immediate;
pub mod win3d_renderables;
