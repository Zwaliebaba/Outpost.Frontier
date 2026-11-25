//! The trigger actions (`Content/MissionAct.h`, `MissionAct.cpp`; TU21):
//! one [`Action`] variant for each of the 85 `Act_*` classes (decision
//! record 0008 point 6), each a struct of its class's fields with its
//! constructor's values as [`Default`], and the parse of its line.
//!
//! [`FACTORY`] is the `is_value` chain of `LoadTrigger`
//! (`MissionScript.cpp:146-230`). Three parses reach beyond the line: the
//! comm actions add their speaker's costume to the mission's dialog
//! costumes (`AddDialogCostume`), `Act_PlayerForm` looks its formation up,
//! and `Act_NNPath` enumerates a system's objects. `Execute` is P3.5c's.

use game_sim::data::costume::{Costume, MAX_ACCESSORIES};
use game_sim::data::equip_desc::EquipDesc;
use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Quaternion, Transform, Vector, quaternion_to_matrix};

use super::MissionScript;
use super::condition::is_numeric_token;
use super::field::{Field, record};
use super::{
    base_id_of, f32_at, find_location, nickname_id, rep_value_from_name, system_id_into,
    value_tstring,
};
use crate::api;
use crate::content::tables;
use crate::server::Cx;

record! {
    /// `Act_ActTrig` (`ActActTrig`): activate a trigger.
    ActTrig {
        /// `CreateID(s0)` (`CreateID("")` first).
        target_trigger_id: u32 = create_id(b""),
    }
}

record! {
    /// `Act_AddAmbient` (`ActAddAmbient`): add an ambient scene.
    AddAmbient {
        /// s0 (`TString<0x80>`).
        path: Vec<u8> = Vec::new(),
        /// Run time.
        handle: i32 = -1,
        /// `pub::GF::FindLocation(base, s1)`.
        room_id: u32 = 0,
        /// `pub::GetBaseID(s2)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Act_AddRTC` (`ActAddRtc`): add a cut scene.
    AddRtc {
        /// s0 (`TString<0x80>`).
        path: Vec<u8> = Vec::new(),
        /// s1 is `repeatable`.
        repeatable: bool = false,
    }
}

record! {
    /// `Act_AdjAcct` (`ActAdjAcct`): adjust the player's account.
    AdjAcct {
        /// i0.
        amount: i32 = 0,
    }
}

record! {
    /// `Act_AdjHealth` (`ActAdjHealth`): adjust a formation's health.
    AdjHealth {
        /// `CreateID(s0)`.
        formation_id: u32 = 0,
        /// f1.
        delta: f32 = 0.0,
    }
}

record! {
    /// `Act_CallSceneScript` (`ActCallSceneScript`): play a scene script.
    CallSceneScript {
        /// s1, or `Player`.
        target: Vec<u8> = Vec::new(),
        /// s0 (`TString<0x80>`).
        script: Vec<u8> = Vec::new(),
        /// (i1, i2, i3) as floats.
        position: Vector = Vector::ZERO,
        /// 1 an object, 2 a position.
        mode: i32 = 0,
    }
}

record! {
    /// `Act_ChangeState` (`ActChangeState`): change the mission's state.
    ChangeState {
        /// `MissionStateFromName(s0)`.
        state: i32 = -1,
        /// Optional i1: the mad lib shown with the change.
        field_0x10: u32 = 1,
    }
}

record! {
    /// `Act_Cloak` (`ActCloak`): cloak a formation.
    Cloak {
        /// s0.
        formation: Vec<u8> = Vec::new(),
        /// b1.
        cloak: bool = false,
    }
}

record! {
    /// `Act_DeactTrig` (`ActDeactTrig`): deactivate a trigger.
    DeactTrig {
        /// `CreateID(s0)` (`CreateID("")` first).
        target_trigger_id: u32 = create_id(b""),
    }
}

record! {
    /// `Act_DebugMsg` (`ActDebugMsg`): a debug message.
    DebugMsg {
        /// s0 (`TString<0x80>`).
        text: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_Destroy` (`ActDestroy`): destroy an object.
    Destroy {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// `EXPLODE` 1, `SILENT` 0.
        destroy_mode: i32 = 1,
    }
}

record! {
    /// `Act_DisableEnc` (`ActDisableEnc`): disable a random encounter.
    DisableEnc {
        /// s0 (`TString<0x20>`).
        encounter: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_DisableFriendlyFire` (`ActDisableFriendlyFire`): disable friendly fire.
    DisableFriendlyFire {
        /// s0..
        labels: Vec<Vec<u8>> = Vec::new(),
    }
}

record! {
    /// `Act_DisableTradelane` (`ActDisableTradelane`): disable a trade lane.
    DisableTradelane {
        /// `pub::GetNicknameId(s0)`.
        ring_id: u32 = 0,
    }
}

record! {
    /// `Act_DockRequest` (`ActDockRequest`): request a dock.
    DockRequest {
        /// `pub::GetNicknameId(s0)`.
        target_id: u32 = 0,
        /// +0x10, not read.
        field_0x10: u32 = 0,
    }
}

record! {
    /// `Act_EnableEnc` (`ActEnableEnc`): enable a random encounter.
    EnableEnc {
        /// s0 (`TString<0x20>`).
        encounter: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_EnableManeuver` (`ActEnableManeuver`): enable a maneuver.
    EnableManeuver {
        /// `BehaviorTypes::get_behavior_id(s0)`.
        maneuver_id: i32 = -1,
        /// b1.
        enable: bool = true,
    }
}

record! {
    /// `Act_EtherComm` (`ActEtherComm`): a comm from no object.
    EtherComm {
        /// s2.
        receiver: Vec<u8> = Vec::new(),
        /// s5 (head), s6 (body), s7.. (accessories).
        speaker_costume: Costume = Costume::default(),
        /// `pub::GetVoiceID(s0)`.
        voice_id: u32 = 0,
        /// i1.
        comm_type: u32 = 0,
        /// `pub::GetNicknameId(s3)`.
        text_id: u32 = 0,
        /// i4, kept for -1 and 1..10.
        duration: i32 = -1,
    }
}

record! {
    /// `Act_ForceLand` (`ActForceLand`): land the player.
    ForceLand {
        /// `pub::GetBaseID(s0)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Act_GCSClamp` (`ActGcsClamp`): clamp the comm system.
    GcsClamp {
        /// b0.
        clamp: bool = false,
    }
}

record! {
    /// `Act_GiveMB` (`ActGiveMb`): give a meta behavior.
    GiveMb {
        /// s0.
        npc: Vec<u8> = Vec::new(),
        /// `CreateID(s1)`.
        list_id: u32 = 0,
    }
}

record! {
    /// `Act_GiveNNObjs` (`ActGiveNnObjs`): give the objectives.
    GiveNnObjs {
    }
}

record! {
    /// `Act_GiveObjList` (`ActGiveObjList`): give an object list.
    GiveObjList {
        /// s0.
        npc: Vec<u8> = Vec::new(),
        /// `CreateID(s1)`.
        list_id: u32 = 0,
    }
}

record! {
    /// `Act_HostileClamp` (`ActHostileClamp`): clamp the hostiles.
    HostileClamp {
        /// The negation of b0.
        clamp: bool = true,
    }
}

record! {
    /// `Act_Invulnerable` (`ActInvulnerable`): make an object invulnerable.
    Invulnerable {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// b1.
        invulnerable: bool = false,
        /// Optional b2.
        param3: bool = false,
        /// Optional f3.
        duration: f32 = 0.0,
    }
}

record! {
    /// `Act_Jumper` (`ActJumper`): make ships follow the player through jumps.
    Jumper {
        /// `CreateID(s0)`.
        label_id: u32 = 0,
        /// b1.
        jumper: bool = false,
    }
}

record! {
    /// `Act_LightFuse` (`ActLightFuse`): light a fuse.
    LightFuse {
        /// s1 (`TString<0x40>`).
        fuse: Vec<u8> = Vec::new(),
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// Optional f2.
        delay: f32 = 0.0,
    }
}

record! {
    /// `Act_LockDock` (`ActLockDock`): lock a dock.
    LockDock {
        /// s0.
        target: Vec<u8> = Vec::new(),
        /// s1.
        gate: Vec<u8> = Vec::new(),
        /// `lock` 1, `unlock` 0.
        mode: i32 = -1,
    }
}

record! {
    /// `Act_LockManeuvers` (`ActLockManeuvers`): lock the player's maneuvers.
    LockManeuvers {
        /// b0.
        lock: bool = false,
    }
}

record! {
    /// `Act_MarkObj` (`ActMarkObj`): mark an object.
    MarkObj {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// i1.
        mode: i32 = 0,
    }
}

record! {
    /// `Act_MovePlayer` (`ActMovePlayer`): move the player.
    MovePlayer {
        /// f0, f1, f2.
        position: Vector = Vector::ZERO,
        /// f3 times f3.
        radius_sq: f32 = 0.0,
    }
}

record! {
    /// `Act_NagClamp` (`ActNagClamp`): clamp the nags.
    NagClamp {
        /// b0.
        clamp: bool = false,
        /// +0x0d, not read.
        field_0x0d: bool = true,
        /// +0x0e, not read.
        field_0x0e: bool = true,
    }
}

record! {
    /// `Act_NagDistLeaving` (`ActNagDistLeaving`): nag when leaving.
    NagDistLeaving {
        /// `CreateID(s0)`.
        nag_id: u32 = 0,
        /// s1.
        speaker: Vec<u8> = Vec::new(),
        /// s2, a label or `position`.
        target: Vec<u8> = Vec::new(),
        /// f3, f4, f5 with `position`.
        position: Vector = Vector::ZERO,
        /// The `position` form.
        has_position: bool = false,
        /// Optional, -1 the default.
        distance: f32 = 4000.0,
        /// Optional.
        fail_mad_lib_id: u32 = 1,
        /// The combat mode.
        nag_in_combat: bool = false,
        /// The combat mode.
        nag_out_of_combat: bool = true,
    }
}

record! {
    /// `Act_NagDistTowards` (`ActNagDistTowards`): nag when not approaching.
    NagDistTowards {
        /// `CreateID(s1)`.
        nag_id: u32 = 0,
        /// s2.
        speaker: Vec<u8> = Vec::new(),
        /// s3 (the `OBJ` form).
        target: Vec<u8> = Vec::new(),
        /// Optional.
        distance: f32 = 4000.0,
        /// Optional.
        fail_mad_lib_id: u32 = 1,
        /// f3, f4, f5 (the `POS` form).
        position: Vector = Vector::ZERO,
        /// The combat mode.
        nag_in_combat: bool = false,
        /// The combat mode.
        nag_out_of_combat: bool = true,
    }
}

record! {
    /// `Act_NagGreet` (`ActNagGreet`): a greeting nag.
    NagGreet {
        /// `CreateID(s0)`.
        nag_id: u32 = 0,
        /// s1.
        speaker: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_NagOff` (`ActNagOff`): switch a nag off.
    NagOff {
        /// `CreateID(s0)`.
        nag_id: u32 = 0,
    }
}

record! {
    /// `Act_NNPath` (`ActNnPath`): a navigation path.
    NnPath {
        /// f2..f4, or the named object's position.
        position: Vector = Vector::ZERO,
        /// s2 (the label form).
        label: Vec<u8> = Vec::new(),
        /// `pub::GetSystemID(s5 / s3)`.
        system_id: u32 = 0,
        /// i0.
        travel_ids: i32 = 0,
        /// i1.
        target_ids: i32 = 0,
    }
}

record! {
    /// `Act_NNIds` (`ActNnIds`): an objective text.
    NnIds {
        /// i0.
        text_id: i32 = 1,
        /// `OBJECTIVE` 7, `HISTORY` 8, `OBJECTIVE_HISTORY` 9.
        message_type: i32 = 9,
    }
}

record! {
    /// `Act_PlayerCanDock` (`ActPlayerCanDock`): whether the player may dock.
    PlayerCanDock {
        /// b0.
        can_dock: bool = false,
        /// `pub::GetNicknameId(s1..)`.
        exceptions: Vec<u32> = Vec::new(),
    }
}

record! {
    /// `Act_PlayerCanTradelane` (`ActPlayerCanTradelane`): whether the player may use trade lanes.
    PlayerCanTradelane {
        /// b0.
        can_tradelane: bool = true,
        /// Ring pairs from s1.
        exceptions: Vec<[u32; 2]> = Vec::new(),
    }
}

record! {
    /// `Act_PlayerEnemyClamp` (`ActPlayerEnemyClamp`): clamp the player's enemies.
    PlayerEnemyClamp {
        /// i0.
        min_clamp: i32 = 2,
        /// i1.
        max_clamp: i32 = 2,
    }
}

record! {
    /// `Act_PlayerForm` (`ActPlayerForm`): put the player in a formation.
    PlayerForm {
        /// s1.
        npc: Vec<u8> = Vec::new(),
        /// The formation of `CreateID(s0)` (its key).
        formation: Option<u32> = None,
    }
}

record! {
    /// `Act_PlayMusic` (`ActPlayMusic`): play music.
    PlayMusic {
        /// `pub::GetNicknameId(s0..s3)` unless `none`.
        music: [u32; 4] = [0; 4],
        /// b5 unless `none`.
        field_10: bool = false,
        /// f4 unless `none`.
        fade: f32 = -1.0,
        /// No values, or `no_params`.
        cancel: bool = false,
    }
}

record! {
    /// `Act_PlayNN` (`ActPlayNn`): play a navigation message.
    PlayNn {
        /// `pub::GetNicknameId(s0)`.
        message_id: u32 = 0,
    }
}

record! {
    /// `Act_PlaySoundEffect` (`ActPlaySoundEffect`): play a sound effect.
    PlaySoundEffect {
        /// `pub::GetNicknameId(s0)`.
        sound_id: u32 = 0,
    }
}

record! {
    /// `Act_PilotParams` (`ActPilotParams`): change a pilot's flee mode.
    PilotParams {
        /// s1: `no_flee` 0, `normal_flee` 1.
        flee_mode: i32 = 0,
        /// s0.
        label: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_PobjIdle` (`ActPobjIdle`): idle the player.
    PobjIdle {
    }
}

record! {
    /// `Act_PopUpDialog` (`ActPopUpDialog`): a pop-up dialog.
    PopUpDialog {
        /// i0.
        title_id: u32 = 1,
        /// i1.
        text_id: u32 = 1,
        /// `YES_NO` 3, `YES_NO_LATER` 7, `CLOSE` 8.
        buttons: u32 = 0,
    }
}

record! {
    /// `Act_SetPriority` (`ActSetPriority`): a formation's priority.
    SetPriority {
        /// `CreateID(s0)`.
        formation_id: u32 = 0,
        /// `NORMAL` 1, `ALWAYS_EXECUTE` 0.
        priority: i32 = 0,
    }
}

record! {
    /// `Act_RandomPop` (`ActRandomPop`): switch the random population.
    RandomPop {
        /// b0.
        enabled: bool = false,
    }
}

record! {
    /// `Act_RandomPopSphere` (`ActRandomPopSphere`): an exclusion sphere of the random population.
    RandomPopSphere {
        /// f0, f1, f2.
        center: Vector = Vector::ZERO,
        /// f3.
        radius: f32 = 0.0,
        /// `on` 0, `off` 1.
        mode: i32 = 0,
    }
}

record! {
    /// `Act_RelocateForm` (`ActRelocateForm`): relocate a formation.
    RelocateForm {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// Position f1..f3, rotation f4..f7.
        placement: Transform = Transform::IDENTITY,
    }
}

record! {
    /// `Act_RelocateShip` (`ActRelocateShip`): relocate a ship.
    RelocateShip {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// Position f1..f3, rotation f4..f7.
        placement: Transform = Transform::IDENTITY,
    }
}

record! {
    /// `Act_RemoveAmbient` (`ActRemoveAmbient`): remove an ambient scene.
    RemoveAmbient {
        /// s0 (`TString<0x80>`).
        path: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_RemoveCargo` (`ActRemoveCargo`): remove mission cargo.
    RemoveCargo {
        /// s0.
        loot: Vec<u8> = Vec::new(),
        /// Optional i1.
        count: i32 = 1,
    }
}

record! {
    /// `Act_RemoveRTC` (`ActRemoveRtc`): remove a cut scene.
    RemoveRtc {
        /// s0 (`TString<0x80>`).
        rtc_file: Vec<u8> = Vec::new(),
    }
}

record! {
    /// `Act_RepChangeRequest` (`ActRepChangeRequest`): offer a reputation change.
    RepChangeRequest {
        /// Not parsed.
        shown: bool = false,
        /// Set by the parse.
        armed: bool = false,
        /// `pub::GetSystemID(s0)`.
        system_id: u32 = 0,
        /// `GetReputationGroup(s1..s49)`, each once.
        groups: Vec<u32> = Vec::new(),
    }
}

record! {
    /// `Act_RevertCam` (`ActRevertCam`): revert the camera.
    RevertCam {
    }
}

record! {
    /// `Act_RpopAttClamp` (`ActRpopAttClamp`): clamp the random population's attacks.
    RpopAttClamp {
        /// b0.
        clamp: bool = false,
    }
}

record! {
    /// `Act_RpopTLAttacksEnabled` (`ActRpopTlAttacksEnabled`): switch the trade-lane attacks.
    RpopTlAttacksEnabled {
        /// b0.
        enabled: bool = false,
    }
}

record! {
    /// `Act_Save` (`ActSave`): save the game.
    Save {
        /// `CreateID(s0)`.
        save_trigger_id: u32 = 0,
        /// Optional i1.
        save_type: u32 = 1,
    }
}

record! {
    /// `Act_SendComm` (`ActSendComm`): a comm between objects.
    SendComm {
        /// s0.
        sender: Vec<u8> = Vec::new(),
        /// s1.
        receiver: Vec<u8> = Vec::new(),
        /// `pub::GetNicknameId(s2)`.
        text_id: u32 = 0,
        /// s6 (head), s7 (body), s8.. (accessories).
        speaker_costume: Costume = Costume::default(),
        /// `pub::GetVoiceID(s4)`.
        voice_id: u32 = 0,
        /// Optional i5.
        comm_type: u32 = 0,
        /// Optional i3, kept for -1 and 1..10.
        duration: i32 = -1,
    }
}

record! {
    /// `Act_SetFlee` (`ActSetFlee`): an NPC's flee target.
    SetFlee {
        /// s1.
        npc: Vec<u8> = Vec::new(),
        /// `SET_POS` 0, `SET_OBJ` 1, `CLEAR` 2.
        mode: i32 = 2,
        /// f2, f3, f4 (`SET_POS`).
        position: Vector = Vector::ZERO,
        /// s2 (`SET_OBJ`).
        target: Vec<u8> = Vec::new(),
        /// f5 / f3.
        distance: f32 = 0.0,
        /// b6 / b4.
        flag: bool = false,
    }
}

record! {
    /// `Act_SetInitialPlayerPos` (`ActSetInitialPlayerPos`): the player's initial position.
    SetInitialPlayerPos {
        /// The quaternion f3..f6.
        orientation: Matrix = Matrix::IDENTITY,
        /// f0, f1, f2.
        position: Vector = Vector::ZERO,
    }
}

record! {
    /// `Act_SetLifeTime` (`ActSetLifeTime`): an object's life time.
    SetLifeTime {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// f1.
        life_time: f32 = 1.0,
    }
}

record! {
    /// `Act_SetNNHidden` (`ActSetNnHidden`): hide an objective.
    SetNnHidden {
        /// The negation of b1.
        visible: bool = false,
        /// `CreateID(s0)`.
        objective_id: u32 = create_id(b""),
    }
}

record! {
    /// `Act_SetNNObj` (`ActSetNnObj`): set the objective.
    SetNnObj {
        /// `CreateID(s0)`.
        objective_id: u32 = create_id(b""),
        /// `HISTORY` 0x12a, `OBJECTIVE` 0x11a, `OBJECTIVE_HISTORY` 0x13a.
        state: i32 = 0x3a,
    }
}

record! {
    /// `Act_SetNNState` (`ActSetNnState`): an objective's state.
    SetNnState {
        /// `CreateID(s0)`.
        objective_id: u32 = create_id(b""),
        /// `ACTIVE` 1, `INACTIVE` 4, `COMPLETE` 3, `FAILED` 2.
        state: i32 = 4,
    }
}

record! {
    /// `Act_SetOffer` (`ActSetOffer`): the mission's offer text.
    SetOffer {
        /// i0.
        offer_id: u32 = 0,
    }
}

record! {
    /// `Act_SetOrient` (`ActSetOrient`): an object's orientation.
    SetOrient {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// f1..f4 (W, X, Y, Z).
        orientation: Quaternion = Quaternion::new(0.0, 0.0, 0.0, 0.0),
    }
}

record! {
    /// `Act_SetRep` (`ActSetRep`): a reputation.
    SetRep {
        /// s0.
        label: Vec<u8> = Vec::new(),
        /// s1.
        faction: Vec<u8> = Vec::new(),
        /// `GetReputationGroup(s1)`.
        group_id: u32 = 0,
        /// f2, or the value of a `REP_*` name.
        value: f32 = 0.0,
    }
}

record! {
    /// `Act_SetShipAndLoadout` (`ActSetShipAndLoadout`): the player's ship and loadout.
    SetShipAndLoadout {
        /// With s1: s0 again (`TString<0x20>`).
        loadout_name: Vec<u8> = Vec::new(),
        /// s0 (`TString<0x20>`).
        ship_name: Vec<u8> = Vec::new(),
        /// `pub::GetLoadout` of s1 unless `none`.
        loadout: Vec<EquipDesc> = Vec::new(),
        /// `pub::GetShipID(s0)` unless `none`.
        ship_arch_id: u32 = 0,
    }
}

record! {
    /// `Act_SetTitle` (`ActSetTitle`): the mission's title.
    SetTitle {
        /// i0.
        title_id: u32 = 0,
    }
}

record! {
    /// `Act_SetVibe` (`ActSetVibe`): an attitude between two objects.
    SetVibe {
        /// s0.
        label_a: Vec<u8> = Vec::new(),
        /// s1.
        label_b: Vec<u8> = Vec::new(),
        /// Value 2.
        lo: f32 = 0.0,
        /// Value 3, else value 2.
        hi: f32 = 0.0,
    }
}

record! {
    /// `Act_SetVibeLbl` (`ActSetVibeLbl`): an attitude between two labels.
    SetVibeLbl {
        /// `CreateID(s0)`.
        label_a: u32 = 0,
        /// `CreateID(s1)`.
        label_b: u32 = 0,
        /// Value 2.
        lo: f32 = 0.0,
        /// Value 3, else value 2.
        hi: f32 = 0.0,
    }
}

record! {
    /// `Act_SetVibeLblToShip` (`ActSetVibeLblToShip`): an attitude of a label towards a ship.
    SetVibeLblToShip {
        /// `CreateID(s0)`.
        formation_id: u32 = 0,
        /// s1.
        ship: Vec<u8> = Vec::new(),
        /// Value 2.
        lo: f32 = 0.0,
        /// Value 3, else value 2.
        hi: f32 = 0.0,
    }
}

record! {
    /// `Act_SetVibeShipToLbl` (`ActSetVibeShipToLbl`): an attitude of a ship towards a label.
    SetVibeShipToLbl {
        /// s0.
        ship: Vec<u8> = Vec::new(),
        /// `CreateID(s1)`.
        formation_id: u32 = 0,
        /// Value 2.
        lo: f32 = 0.0,
        /// Value 3, else value 2.
        hi: f32 = 0.0,
    }
}

record! {
    /// `Act_SetVibeOfferBaseHack` (`ActSetVibeOfferBaseHack`): the offer base's attitude.
    SetVibeOfferBaseHack {
        /// `pub::GetNicknameId(s0)`.
        base_id: u32 = 0,
    }
}

record! {
    /// `Act_SpawnFormation` (`ActSpawnFormation`): spawn a formation.
    SpawnFormation {
        /// s0.
        name: Vec<u8> = Vec::new(),
        /// Position f1..f3, rotation f4..f7.
        placement: Transform = Transform::IDENTITY,
        /// Value 1 present.
        position: bool = false,
        /// Value 4 present.
        orientation: bool = false,
    }
}

record! {
    /// `Act_SpawnLoot` (`ActSpawnLoot`): spawn loot.
    SpawnLoot {
        /// s0.
        name: Vec<u8> = Vec::new(),
        /// Position f1..f3, rotation f4..f7.
        placement: Transform = Transform::IDENTITY,
        /// Value 1 present.
        position: bool = false,
        /// Value 4 present.
        orientation: bool = false,
    }
}

record! {
    /// `Act_SpawnShip` (`ActSpawnShip`): spawn a ship.
    SpawnShip {
        /// s0.
        name: Vec<u8> = Vec::new(),
        /// No writer.
        new_nickname: Vec<u8> = Vec::new(),
        /// Position f2..f4, rotation f5..f8.
        placement: Transform = Transform::IDENTITY,
        /// Value 2 present.
        position: bool = false,
        /// Value 5 present.
        orientation: bool = false,
        /// No writer.
        rename: bool = false,
        /// `CreateID(s1)`.
        obj_list: u32 = 0,
    }
}

record! {
    /// `Act_SpawnShipRel` (`ActSpawnShipRel`): spawn a ship relative to an object.
    SpawnShipRel {
        /// s0.
        name: Vec<u8> = Vec::new(),
        /// s2.
        rel_obj: Vec<u8> = Vec::new(),
        /// s8.
        new_nickname: Vec<u8> = Vec::new(),
        /// Offset f4..f6.
        offset: Transform = Transform::IDENTITY,
        /// `POINT` 0, `DIRECTIONAL` 1, `RELATIVE` 2.
        mode: i32 = -1,
        /// +0xdc, not read.
        field_0xdc: bool = false,
        /// Value 8 present.
        rename: bool = false,
        /// `CreateID(s1)`.
        obj_list: u32 = 0,
        /// Optional f7.
        heading: f32 = -1.0,
    }
}

record! {
    /// `Act_SpawnSolar` (`ActSpawnSolar`): spawn a solar.
    SpawnSolar {
        /// s0.
        name: Vec<u8> = Vec::new(),
        /// Position f1..f3, rotation f4..f7.
        placement: Transform = Transform::IDENTITY,
        /// Value 1 present.
        position: bool = false,
        /// Value 4 present.
        orientation: bool = false,
    }
}

record! {
    /// `Act_StartDialog` (`ActStartDialog`): start a dialog.
    StartDialog {
        /// `CreateID(s0)`.
        dialog_id: u32 = 0,
        /// Optional i1: -1 and 1..10.
        delay_seconds: i32 = -1,
        /// Set with a kept delay.
        delay_pending: bool = false,
    }
}

record! {
    /// `Act_StaticCam` (`ActStaticCam`): a static camera.
    StaticCam {
        /// Position f0..f2, rotation f3..f6.
        camera: Transform = Transform::IDENTITY,
        /// Optional f7.
        fov: f32 = 0.0,
        /// Optional f8.
        aspect: f32 = 0.0,
    }
}

/// An action: one variant per C++ class, in the factory's order.
#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    /// `Act_ActTrig`.
    ActTrig(ActTrig),
    /// `Act_AddAmbient`.
    AddAmbient(AddAmbient),
    /// `Act_AddRTC`.
    AddRtc(AddRtc),
    /// `Act_AdjAcct`.
    AdjAcct(AdjAcct),
    /// `Act_AdjHealth`.
    AdjHealth(AdjHealth),
    /// `Act_CallSceneScript`.
    CallSceneScript(CallSceneScript),
    /// `Act_ChangeState`.
    ChangeState(ChangeState),
    /// `Act_Cloak`.
    Cloak(Cloak),
    /// `Act_DeactTrig`.
    DeactTrig(DeactTrig),
    /// `Act_DebugMsg`.
    DebugMsg(DebugMsg),
    /// `Act_Destroy`.
    Destroy(Destroy),
    /// `Act_DisableEnc`.
    DisableEnc(DisableEnc),
    /// `Act_DisableFriendlyFire`.
    DisableFriendlyFire(DisableFriendlyFire),
    /// `Act_DisableTradelane`.
    DisableTradelane(DisableTradelane),
    /// `Act_DockRequest`.
    DockRequest(DockRequest),
    /// `Act_EnableEnc`.
    EnableEnc(EnableEnc),
    /// `Act_EnableManeuver`.
    EnableManeuver(EnableManeuver),
    /// `Act_EtherComm`.
    EtherComm(EtherComm),
    /// `Act_ForceLand`.
    ForceLand(ForceLand),
    /// `Act_GCSClamp`.
    GcsClamp(GcsClamp),
    /// `Act_GiveMB`.
    GiveMb(GiveMb),
    /// `Act_GiveNNObjs`.
    GiveNnObjs(GiveNnObjs),
    /// `Act_GiveObjList`.
    GiveObjList(GiveObjList),
    /// `Act_HostileClamp`.
    HostileClamp(HostileClamp),
    /// `Act_Invulnerable`.
    Invulnerable(Invulnerable),
    /// `Act_Jumper`.
    Jumper(Jumper),
    /// `Act_LightFuse`.
    LightFuse(LightFuse),
    /// `Act_LockDock`.
    LockDock(LockDock),
    /// `Act_LockManeuvers`.
    LockManeuvers(LockManeuvers),
    /// `Act_MarkObj`.
    MarkObj(MarkObj),
    /// `Act_MovePlayer`.
    MovePlayer(MovePlayer),
    /// `Act_NagClamp`.
    NagClamp(NagClamp),
    /// `Act_NagDistLeaving`.
    NagDistLeaving(NagDistLeaving),
    /// `Act_NagDistTowards`.
    NagDistTowards(NagDistTowards),
    /// `Act_NagGreet`.
    NagGreet(NagGreet),
    /// `Act_NagOff`.
    NagOff(NagOff),
    /// `Act_NNPath`.
    NnPath(NnPath),
    /// `Act_NNIds`.
    NnIds(NnIds),
    /// `Act_PlayerCanDock`.
    PlayerCanDock(PlayerCanDock),
    /// `Act_PlayerCanTradelane`.
    PlayerCanTradelane(PlayerCanTradelane),
    /// `Act_PlayerEnemyClamp`.
    PlayerEnemyClamp(PlayerEnemyClamp),
    /// `Act_PlayerForm`.
    PlayerForm(PlayerForm),
    /// `Act_PlayMusic`.
    PlayMusic(PlayMusic),
    /// `Act_PlayNN`.
    PlayNn(PlayNn),
    /// `Act_PlaySoundEffect`.
    PlaySoundEffect(PlaySoundEffect),
    /// `Act_PilotParams`.
    PilotParams(PilotParams),
    /// `Act_PobjIdle`.
    PobjIdle(PobjIdle),
    /// `Act_PopUpDialog`.
    PopUpDialog(PopUpDialog),
    /// `Act_SetPriority`.
    SetPriority(SetPriority),
    /// `Act_RandomPop`.
    RandomPop(RandomPop),
    /// `Act_RandomPopSphere`.
    RandomPopSphere(RandomPopSphere),
    /// `Act_RelocateForm`.
    RelocateForm(RelocateForm),
    /// `Act_RelocateShip`.
    RelocateShip(RelocateShip),
    /// `Act_RemoveAmbient`.
    RemoveAmbient(RemoveAmbient),
    /// `Act_RemoveCargo`.
    RemoveCargo(RemoveCargo),
    /// `Act_RemoveRTC`.
    RemoveRtc(RemoveRtc),
    /// `Act_RepChangeRequest`.
    RepChangeRequest(RepChangeRequest),
    /// `Act_RevertCam`.
    RevertCam(RevertCam),
    /// `Act_RpopAttClamp`.
    RpopAttClamp(RpopAttClamp),
    /// `Act_RpopTLAttacksEnabled`.
    RpopTlAttacksEnabled(RpopTlAttacksEnabled),
    /// `Act_Save`.
    Save(Save),
    /// `Act_SendComm`.
    SendComm(SendComm),
    /// `Act_SetFlee`.
    SetFlee(SetFlee),
    /// `Act_SetInitialPlayerPos`.
    SetInitialPlayerPos(SetInitialPlayerPos),
    /// `Act_SetLifeTime`.
    SetLifeTime(SetLifeTime),
    /// `Act_SetNNHidden`.
    SetNnHidden(SetNnHidden),
    /// `Act_SetNNObj`.
    SetNnObj(SetNnObj),
    /// `Act_SetNNState`.
    SetNnState(SetNnState),
    /// `Act_SetOffer`.
    SetOffer(SetOffer),
    /// `Act_SetOrient`.
    SetOrient(SetOrient),
    /// `Act_SetRep`.
    SetRep(SetRep),
    /// `Act_SetShipAndLoadout`.
    SetShipAndLoadout(SetShipAndLoadout),
    /// `Act_SetTitle`.
    SetTitle(SetTitle),
    /// `Act_SetVibe`.
    SetVibe(SetVibe),
    /// `Act_SetVibeLbl`.
    SetVibeLbl(SetVibeLbl),
    /// `Act_SetVibeLblToShip`.
    SetVibeLblToShip(SetVibeLblToShip),
    /// `Act_SetVibeShipToLbl`.
    SetVibeShipToLbl(SetVibeShipToLbl),
    /// `Act_SetVibeOfferBaseHack`.
    SetVibeOfferBaseHack(SetVibeOfferBaseHack),
    /// `Act_SpawnFormation`.
    SpawnFormation(SpawnFormation),
    /// `Act_SpawnLoot`.
    SpawnLoot(SpawnLoot),
    /// `Act_SpawnShip`.
    SpawnShip(SpawnShip),
    /// `Act_SpawnShipRel`.
    SpawnShipRel(SpawnShipRel),
    /// `Act_SpawnSolar`.
    SpawnSolar(SpawnSolar),
    /// `Act_StartDialog`.
    StartDialog(StartDialog),
    /// `Act_StaticCam`.
    StaticCam(StaticCam),
}

impl Action {
    /// The factory name of the line that made it.
    #[must_use]
    pub fn name(&self) -> &'static str {
        match self {
            Self::ActTrig(_) => "Act_ActTrig",
            Self::AddAmbient(_) => "Act_AddAmbient",
            Self::AddRtc(_) => "Act_AddRTC",
            Self::AdjAcct(_) => "Act_AdjAcct",
            Self::AdjHealth(_) => "Act_AdjHealth",
            Self::CallSceneScript(_) => "Act_CallSceneScript",
            Self::ChangeState(_) => "Act_ChangeState",
            Self::Cloak(_) => "Act_Cloak",
            Self::DeactTrig(_) => "Act_DeactTrig",
            Self::DebugMsg(_) => "Act_DebugMsg",
            Self::Destroy(_) => "Act_Destroy",
            Self::DisableEnc(_) => "Act_DisableEnc",
            Self::DisableFriendlyFire(_) => "Act_DisableFriendlyFire",
            Self::DisableTradelane(_) => "Act_DisableTradelane",
            Self::DockRequest(_) => "Act_DockRequest",
            Self::EnableEnc(_) => "Act_EnableEnc",
            Self::EnableManeuver(_) => "Act_EnableManeuver",
            Self::EtherComm(_) => "Act_EtherComm",
            Self::ForceLand(_) => "Act_ForceLand",
            Self::GcsClamp(_) => "Act_GCSClamp",
            Self::GiveMb(_) => "Act_GiveMB",
            Self::GiveNnObjs(_) => "Act_GiveNNObjs",
            Self::GiveObjList(_) => "Act_GiveObjList",
            Self::HostileClamp(_) => "Act_HostileClamp",
            Self::Invulnerable(_) => "Act_Invulnerable",
            Self::Jumper(_) => "Act_Jumper",
            Self::LightFuse(_) => "Act_LightFuse",
            Self::LockDock(_) => "Act_LockDock",
            Self::LockManeuvers(_) => "Act_LockManeuvers",
            Self::MarkObj(_) => "Act_MarkObj",
            Self::MovePlayer(_) => "Act_MovePlayer",
            Self::NagClamp(_) => "Act_NagClamp",
            Self::NagDistLeaving(_) => "Act_NagDistLeaving",
            Self::NagDistTowards(_) => "Act_NagDistTowards",
            Self::NagGreet(_) => "Act_NagGreet",
            Self::NagOff(_) => "Act_NagOff",
            Self::NnPath(_) => "Act_NNPath",
            Self::NnIds(_) => "Act_NNIds",
            Self::PlayerCanDock(_) => "Act_PlayerCanDock",
            Self::PlayerCanTradelane(_) => "Act_PlayerCanTradelane",
            Self::PlayerEnemyClamp(_) => "Act_PlayerEnemyClamp",
            Self::PlayerForm(_) => "Act_PlayerForm",
            Self::PlayMusic(_) => "Act_PlayMusic",
            Self::PlayNn(_) => "Act_PlayNN",
            Self::PlaySoundEffect(_) => "Act_PlaySoundEffect",
            Self::PilotParams(_) => "Act_PilotParams",
            Self::PobjIdle(_) => "Act_PobjIdle",
            Self::PopUpDialog(_) => "Act_PopUpDialog",
            Self::SetPriority(_) => "Act_SetPriority",
            Self::RandomPop(_) => "Act_RandomPop",
            Self::RandomPopSphere(_) => "Act_RandomPopSphere",
            Self::RelocateForm(_) => "Act_RelocateForm",
            Self::RelocateShip(_) => "Act_RelocateShip",
            Self::RemoveAmbient(_) => "Act_RemoveAmbient",
            Self::RemoveCargo(_) => "Act_RemoveCargo",
            Self::RemoveRtc(_) => "Act_RemoveRTC",
            Self::RepChangeRequest(_) => "Act_RepChangeRequest",
            Self::RevertCam(_) => "Act_RevertCam",
            Self::RpopAttClamp(_) => "Act_RpopAttClamp",
            Self::RpopTlAttacksEnabled(_) => "Act_RpopTLAttacksEnabled",
            Self::Save(_) => "Act_Save",
            Self::SendComm(_) => "Act_SendComm",
            Self::SetFlee(_) => "Act_SetFlee",
            Self::SetInitialPlayerPos(_) => "Act_SetInitialPlayerPos",
            Self::SetLifeTime(_) => "Act_SetLifeTime",
            Self::SetNnHidden(_) => "Act_SetNNHidden",
            Self::SetNnObj(_) => "Act_SetNNObj",
            Self::SetNnState(_) => "Act_SetNNState",
            Self::SetOffer(_) => "Act_SetOffer",
            Self::SetOrient(_) => "Act_SetOrient",
            Self::SetRep(_) => "Act_SetRep",
            Self::SetShipAndLoadout(_) => "Act_SetShipAndLoadout",
            Self::SetTitle(_) => "Act_SetTitle",
            Self::SetVibe(_) => "Act_SetVibe",
            Self::SetVibeLbl(_) => "Act_SetVibeLbl",
            Self::SetVibeLblToShip(_) => "Act_SetVibeLblToShip",
            Self::SetVibeShipToLbl(_) => "Act_SetVibeShipToLbl",
            Self::SetVibeOfferBaseHack(_) => "Act_SetVibeOfferBaseHack",
            Self::SpawnFormation(_) => "Act_SpawnFormation",
            Self::SpawnLoot(_) => "Act_SpawnLoot",
            Self::SpawnShip(_) => "Act_SpawnShip",
            Self::SpawnShipRel(_) => "Act_SpawnShipRel",
            Self::SpawnSolar(_) => "Act_SpawnSolar",
            Self::StartDialog(_) => "Act_StartDialog",
            Self::StaticCam(_) => "Act_StaticCam",
        }
    }

    /// The fields in the oracle dump's order.
    #[must_use]
    pub fn fields(&self) -> Vec<(&'static str, Field)> {
        match self {
            Self::ActTrig(r) => r.fields(),
            Self::AddAmbient(r) => r.fields(),
            Self::AddRtc(r) => r.fields(),
            Self::AdjAcct(r) => r.fields(),
            Self::AdjHealth(r) => r.fields(),
            Self::CallSceneScript(r) => r.fields(),
            Self::ChangeState(r) => r.fields(),
            Self::Cloak(r) => r.fields(),
            Self::DeactTrig(r) => r.fields(),
            Self::DebugMsg(r) => r.fields(),
            Self::Destroy(r) => r.fields(),
            Self::DisableEnc(r) => r.fields(),
            Self::DisableFriendlyFire(r) => r.fields(),
            Self::DisableTradelane(r) => r.fields(),
            Self::DockRequest(r) => r.fields(),
            Self::EnableEnc(r) => r.fields(),
            Self::EnableManeuver(r) => r.fields(),
            Self::EtherComm(r) => r.fields(),
            Self::ForceLand(r) => r.fields(),
            Self::GcsClamp(r) => r.fields(),
            Self::GiveMb(r) => r.fields(),
            Self::GiveNnObjs(r) => r.fields(),
            Self::GiveObjList(r) => r.fields(),
            Self::HostileClamp(r) => r.fields(),
            Self::Invulnerable(r) => r.fields(),
            Self::Jumper(r) => r.fields(),
            Self::LightFuse(r) => r.fields(),
            Self::LockDock(r) => r.fields(),
            Self::LockManeuvers(r) => r.fields(),
            Self::MarkObj(r) => r.fields(),
            Self::MovePlayer(r) => r.fields(),
            Self::NagClamp(r) => r.fields(),
            Self::NagDistLeaving(r) => r.fields(),
            Self::NagDistTowards(r) => r.fields(),
            Self::NagGreet(r) => r.fields(),
            Self::NagOff(r) => r.fields(),
            Self::NnPath(r) => r.fields(),
            Self::NnIds(r) => r.fields(),
            Self::PlayerCanDock(r) => r.fields(),
            Self::PlayerCanTradelane(r) => r.fields(),
            Self::PlayerEnemyClamp(r) => r.fields(),
            Self::PlayerForm(r) => r.fields(),
            Self::PlayMusic(r) => r.fields(),
            Self::PlayNn(r) => r.fields(),
            Self::PlaySoundEffect(r) => r.fields(),
            Self::PilotParams(r) => r.fields(),
            Self::PobjIdle(r) => r.fields(),
            Self::PopUpDialog(r) => r.fields(),
            Self::SetPriority(r) => r.fields(),
            Self::RandomPop(r) => r.fields(),
            Self::RandomPopSphere(r) => r.fields(),
            Self::RelocateForm(r) => r.fields(),
            Self::RelocateShip(r) => r.fields(),
            Self::RemoveAmbient(r) => r.fields(),
            Self::RemoveCargo(r) => r.fields(),
            Self::RemoveRtc(r) => r.fields(),
            Self::RepChangeRequest(r) => r.fields(),
            Self::RevertCam(r) => r.fields(),
            Self::RpopAttClamp(r) => r.fields(),
            Self::RpopTlAttacksEnabled(r) => r.fields(),
            Self::Save(r) => r.fields(),
            Self::SendComm(r) => r.fields(),
            Self::SetFlee(r) => r.fields(),
            Self::SetInitialPlayerPos(r) => r.fields(),
            Self::SetLifeTime(r) => r.fields(),
            Self::SetNnHidden(r) => r.fields(),
            Self::SetNnObj(r) => r.fields(),
            Self::SetNnState(r) => r.fields(),
            Self::SetOffer(r) => r.fields(),
            Self::SetOrient(r) => r.fields(),
            Self::SetRep(r) => r.fields(),
            Self::SetShipAndLoadout(r) => r.fields(),
            Self::SetTitle(r) => r.fields(),
            Self::SetVibe(r) => r.fields(),
            Self::SetVibeLbl(r) => r.fields(),
            Self::SetVibeLblToShip(r) => r.fields(),
            Self::SetVibeShipToLbl(r) => r.fields(),
            Self::SetVibeOfferBaseHack(r) => r.fields(),
            Self::SpawnFormation(r) => r.fields(),
            Self::SpawnLoot(r) => r.fields(),
            Self::SpawnShip(r) => r.fields(),
            Self::SpawnShipRel(r) => r.fields(),
            Self::SpawnSolar(r) => r.fields(),
            Self::StartDialog(r) => r.fields(),
            Self::StaticCam(r) => r.fields(),
        }
    }
}

/// `GActFactory` (`MissionScript.cpp:146-230`): the `Act_*` names in the
/// order of the `is_value` chain.
pub const FACTORY: [&[u8]; 85] = [
    b"Act_ActTrig",
    b"Act_AddAmbient",
    b"Act_AddRTC",
    b"Act_AdjAcct",
    b"Act_AdjHealth",
    b"Act_CallSceneScript",
    b"Act_ChangeState",
    b"Act_Cloak",
    b"Act_DeactTrig",
    b"Act_DebugMsg",
    b"Act_Destroy",
    b"Act_DisableEnc",
    b"Act_DisableFriendlyFire",
    b"Act_DisableTradelane",
    b"Act_DockRequest",
    b"Act_EnableEnc",
    b"Act_EnableManeuver",
    b"Act_EtherComm",
    b"Act_ForceLand",
    b"Act_GCSClamp",
    b"Act_GiveMB",
    b"Act_GiveNNObjs",
    b"Act_GiveObjList",
    b"Act_HostileClamp",
    b"Act_Invulnerable",
    b"Act_Jumper",
    b"Act_LightFuse",
    b"Act_LockDock",
    b"Act_LockManeuvers",
    b"Act_MarkObj",
    b"Act_MovePlayer",
    b"Act_NagClamp",
    b"Act_NagDistLeaving",
    b"Act_NagDistTowards",
    b"Act_NagGreet",
    b"Act_NagOff",
    b"Act_NNPath",
    b"Act_NNIds",
    b"Act_PlayerCanDock",
    b"Act_PlayerCanTradelane",
    b"Act_PlayerEnemyClamp",
    b"Act_PlayerForm",
    b"Act_PlayMusic",
    b"Act_PlayNN",
    b"Act_PlaySoundEffect",
    b"Act_PilotParams",
    b"Act_PobjIdle",
    b"Act_PopUpDialog",
    b"Act_SetPriority",
    b"Act_RandomPop",
    b"Act_RandomPopSphere",
    b"Act_RelocateForm",
    b"Act_RelocateShip",
    b"Act_RemoveAmbient",
    b"Act_RemoveCargo",
    b"Act_RemoveRTC",
    b"Act_RepChangeRequest",
    b"Act_RevertCam",
    b"Act_RpopAttClamp",
    b"Act_RpopTLAttacksEnabled",
    b"Act_Save",
    b"Act_SendComm",
    b"Act_SetFlee",
    b"Act_SetInitialPlayerPos",
    b"Act_SetLifeTime",
    b"Act_SetNNHidden",
    b"Act_SetNNObj",
    b"Act_SetNNState",
    b"Act_SetOffer",
    b"Act_SetOrient",
    b"Act_SetRep",
    b"Act_SetShipAndLoadout",
    b"Act_SetTitle",
    b"Act_SetVibe",
    b"Act_SetVibeLbl",
    b"Act_SetVibeLblToShip",
    b"Act_SetVibeShipToLbl",
    b"Act_SetVibeOfferBaseHack",
    b"Act_SpawnFormation",
    b"Act_SpawnLoot",
    b"Act_SpawnShip",
    b"Act_SpawnShipRel",
    b"Act_SpawnSolar",
    b"Act_StartDialog",
    b"Act_StaticCam",
];

/// `GFleeModeNames` (`MissionAct.cpp:2518-2521`): the names of
/// `Act_PilotParams`; the eight empty entries are skipped.
const FLEE_MODES: [(&[u8], i32); 2] = [(b"no_flee", 0), (b"normal_flee", 1)];

/// `stricmp(value, text) == 0`.
fn value_is(reader: &IniReader, index: u32, text: &[u8]) -> bool {
    eq_ignore_case(c_str(&reader.value_string_at(index)), text)
}

/// `MissionStateFromName` (`FMissionScript`, slot 26): `FAIL` 0, `SUCCEED`
/// 1, `NO_STATE` 2, else -1 (`_stricmp`).
#[must_use]
pub fn mission_state_from_name(name: &[u8]) -> i32 {
    let name = c_str(name);
    if eq_ignore_case(name, b"FAIL") {
        0
    } else if eq_ignore_case(name, b"SUCCEED") {
        1
    } else if eq_ignore_case(name, b"NO_STATE") {
        2
    } else {
        -1
    }
}

/// `NormalizeQuaternion`: `1 / sqrt(W² + X² + Y² + Z²)` in the x87's 53
/// bits, each part times it stored as a float.
fn normalize_quaternion(w: f32, x: f32, y: f32, z: f32) -> Quaternion {
    let (w, x, y, z) = (f64::from(w), f64::from(x), f64::from(y), f64::from(z));
    let scale = 1.0 / (w * w + x * x + y * y + z * z).sqrt();
    Quaternion::new(
        (scale * w) as f32,
        (scale * x) as f32,
        (scale * y) as f32,
        (scale * z) as f32,
    )
}

/// `ParseSpawnRotation`: values `first..first + 3` are W, X, Y, Z (read
/// last to first), normalised, then `QuaternionToMatrix`.
fn spawn_rotation(reader: &mut IniReader, first: u32) -> Matrix {
    let z = f32_at(reader, first + 3);
    let y = f32_at(reader, first + 2);
    let x = f32_at(reader, first + 1);
    let w = f32_at(reader, first);
    quaternion_to_matrix(&normalize_quaternion(w, x, y, z))
}

/// `ParseRelocation` (`Act_RelocateForm`, `Act_RelocateShip`): s0, the
/// position f1..f3 (read 3, 2, 1), and with value 4 the rotation f4..f7.
fn relocation(reader: &mut IniReader, label: &mut Vec<u8>, placement: &mut Transform) {
    *label = value_tstring(reader, 0, 0x30);
    let z = f32_at(reader, 3);
    let y = f32_at(reader, 2);
    placement.translation = Vector::new(f32_at(reader, 1), y, z);
    if reader.is_value_empty(4) {
        return;
    }
    placement.rotation = spawn_rotation(reader, 4);
}

/// `ParseSpawnRecord` (`Act_SpawnSolar`, `Act_SpawnFormation`,
/// `Act_SpawnLoot`): s0; with value 1 the position f1..f3 (read 3, 2, 1);
/// with value 4 the rotation f4..f7.
fn spawn_record(
    reader: &mut IniReader,
    name: &mut Vec<u8>,
    placement: &mut Transform,
    position: &mut bool,
    orientation: &mut bool,
) {
    *name = value_tstring(reader, 0, 0x30);
    if reader.is_value_empty(1) {
        return;
    }
    *position = true;
    let z = f32_at(reader, 3);
    let y = f32_at(reader, 2);
    placement.translation = Vector::new(f32_at(reader, 1), y, z);
    if reader.is_value_empty(4) {
        return;
    }
    *orientation = true;
    placement.rotation = spawn_rotation(reader, 4);
}

/// `ParseVibeValue`: a value starting with a digit or `-` as a float, else
/// the value of its `REP_*` name.
fn vibe_value(reader: &mut IniReader, index: u32) -> f32 {
    if is_numeric_token(&reader.value_string_at(index)) {
        f32_at(reader, index)
    } else {
        rep_value_from_name(&value_tstring(reader, index, 0x40))
    }
}

/// `ParseVibeRange`: `lo = hi =` value 2; value 3 present, `hi =` value 3.
fn vibe_range(reader: &mut IniReader, lo: &mut f32, hi: &mut f32) {
    let first = vibe_value(reader, 2);
    *lo = first;
    *hi = first;
    if !reader.is_value_empty(3) {
        *hi = vibe_value(reader, 3);
    }
}

/// `ParseNagCombatMode`: `NAG_IN_COMBAT` (1, 0), `NAG_OUT_OF_COMBAT` (0, 1),
/// `NAG_ALWAYS` (1, 1), anything else leaves both.
fn nag_combat_mode(reader: &IniReader, index: u32, in_combat: &mut bool, out_of_combat: &mut bool) {
    if value_is(reader, index, b"NAG_IN_COMBAT") {
        (*in_combat, *out_of_combat) = (true, false);
    } else if value_is(reader, index, b"NAG_OUT_OF_COMBAT") {
        (*in_combat, *out_of_combat) = (false, true);
    } else if value_is(reader, index, b"NAG_ALWAYS") {
        (*in_combat, *out_of_combat) = (true, true);
    }
}

/// `NagDistanceOrDefault`: only an exact -1 becomes 4000.
fn nag_distance(value: f32) -> f32 {
    if value == -1.0 { 4000.0 } else { value }
}

/// A comm's duration: kept for -1 and 1..10, else -1.
fn comm_duration(value: i32) -> i32 {
    if value < -1 || value == 0 || value > 10 {
        -1
    } else {
        value
    }
}

/// The head, body and accessories of a comm's speaker from value `head`
/// on: `NO_HEAD` (`_stricmp`) gives head 0. The accessories go on while
/// present; the C++ writes past the eight slots, the port keeps eight
/// (D782).
fn comm_costume(cx: &mut Cx<'_>, reader: &IniReader, head: u32, costume: &mut Costume) {
    costume.part_ids[0] = if value_is(reader, head, b"NO_HEAD") {
        0
    } else {
        api::gf::body_part(cx, &reader.value_string_at(head), 1) as u32
    };
    costume.part_ids[1] = api::gf::body_part(cx, &reader.value_string_at(head + 1), 0) as u32;
    let mut index = head + 2;
    while !reader.is_value_empty(index) {
        let accessory = api::gf::accessory(cx, &reader.value_string_at(index)) as u32;
        if costume.accessory_ids.len() < MAX_ACCESSORIES {
            costume.accessory_ids.push(accessory);
        }
        index += 1;
    }
}

/// The position of the first object of `system`'s file named `label`
/// (`_stricmp`), through `pub::System::EnumerateObjects`; `Err` for an
/// unknown system, `Ok(None)` when no object has the name.
fn find_system_object(cx: &mut Cx<'_>, system: u32, label: &[u8]) -> Result<Option<Vector>, ()> {
    let source = crate::content::data_source(cx);
    let mut found = None;
    api::system::enumerate_objects(cx, &*source, system, |object| {
        if found.is_some() || !eq_ignore_case(c_str(&object.nickname), label) {
            return true;
        }
        found = Some(object.pos);
        false
    })
    .map_err(|_| ())?;
    Ok(found)
}

/// The action of factory entry `index` ([`FACTORY`]), made with its
/// constructor's values and parsed from the rest of the line
/// (`Act::Parse`). `script` is the mission being loaded: its ships, NPCs and
/// solars give a comm's speaker costume, and its dialog costumes take it.
#[must_use]
pub fn parse(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    index: usize,
    script: &mut MissionScript,
) -> Action {
    match index {
        0 => Action::ActTrig(ActTrig {
            target_trigger_id: create_id(&reader.value_string_at(0)),
        }),
        1 => {
            let path = value_tstring(reader, 0, 0x80);
            let base_id = base_id_of(cx, &reader.value_string_at(2));
            let room_id = find_location(base_id, &reader.value_string_at(1));
            Action::AddAmbient(AddAmbient {
                path,
                handle: -1,
                room_id,
                base_id,
            })
        }
        2 => Action::AddRtc(AddRtc {
            path: value_tstring(reader, 0, 0x80),
            repeatable: !reader.is_value_empty(1) && value_is(reader, 1, b"repeatable"),
        }),
        3 => Action::AdjAcct(AdjAcct {
            amount: reader.value_int(0),
        }),
        4 => Action::AdjHealth(AdjHealth {
            formation_id: create_id(&reader.value_string_at(0)),
            delta: f32_at(reader, 1),
        }),
        5 => {
            let mut a = CallSceneScript {
                script: value_tstring(reader, 0, 0x80),
                ..CallSceneScript::default()
            };
            if reader.is_value_empty(2) {
                a.target = if reader.is_value_empty(1) {
                    b"Player".to_vec()
                } else {
                    value_tstring(reader, 1, 0x30)
                };
                a.mode = 1;
            } else {
                a.target = b"Player".to_vec();
                let z = reader.value_int(3) as f32;
                let y = reader.value_int(2) as f32;
                let x = reader.value_int(1) as f32;
                a.position = Vector::new(x, y, z);
                a.mode = 2;
            }
            Action::CallSceneScript(a)
        }
        6 => {
            let mut a = ChangeState {
                state: mission_state_from_name(&reader.value_string_at(0)),
                ..ChangeState::default()
            };
            if !reader.is_value_empty(1) {
                a.field_0x10 = reader.value_int(1) as u32;
            }
            Action::ChangeState(a)
        }
        7 => {
            let formation = value_tstring(reader, 0, 0x30);
            Action::Cloak(Cloak {
                formation,
                cloak: reader.value_bool(1),
            })
        }
        8 => Action::DeactTrig(DeactTrig {
            target_trigger_id: create_id(&reader.value_string_at(0)),
        }),
        9 => Action::DebugMsg(DebugMsg {
            text: value_tstring(reader, 0, 0x80),
        }),
        10 => {
            let mut a = Destroy {
                label: value_tstring(reader, 0, 0x30),
                ..Destroy::default()
            };
            if !reader.is_value_empty(1) {
                if value_is(reader, 1, b"EXPLODE") {
                    a.destroy_mode = 1;
                } else if value_is(reader, 1, b"SILENT") {
                    a.destroy_mode = 0;
                }
            }
            Action::Destroy(a)
        }
        11 => Action::DisableEnc(DisableEnc {
            encounter: value_tstring(reader, 0, 0x20),
        }),
        12 => {
            let mut labels = Vec::new();
            let mut slot = 0;
            while !reader.is_value_empty(slot) {
                labels.push(value_tstring(reader, slot, 0x30));
                slot += 1;
            }
            Action::DisableFriendlyFire(DisableFriendlyFire { labels })
        }
        13 => Action::DisableTradelane(DisableTradelane {
            ring_id: nickname_id(&reader.value_string_at(0)),
        }),
        14 => Action::DockRequest(DockRequest {
            target_id: nickname_id(&reader.value_string_at(0)),
            field_0x10: 0,
        }),
        15 => Action::EnableEnc(EnableEnc {
            encounter: value_tstring(reader, 0, 0x20),
        }),
        16 => {
            let maneuver_id =
                game_sim::ai::state_graph::behavior_id(c_str(&reader.value_string_at(0)));
            Action::EnableManeuver(EnableManeuver {
                maneuver_id,
                enable: reader.value_bool(1),
            })
        }
        17 => {
            let mut a = EtherComm::default();
            tables::voice_id(&mut a.voice_id, &reader.value_string_at(0));
            a.comm_type = reader.value_int(1) as u32;
            a.receiver = value_tstring(reader, 2, 0x30);
            a.text_id = nickname_id(&reader.value_string_at(3));
            a.duration = comm_duration(reader.value_int(4));
            comm_costume(cx, reader, 5, &mut a.speaker_costume);
            script.add_dialog_costume(&a.speaker_costume);
            Action::EtherComm(a)
        }
        18 => {
            let name = value_tstring(reader, 0, 0x20);
            Action::ForceLand(ForceLand {
                base_id: base_id_of(cx, &name),
            })
        }
        19 => Action::GcsClamp(GcsClamp {
            clamp: reader.value_bool(0),
        }),
        20 => Action::GiveMb(GiveMb {
            npc: value_tstring(reader, 0, 0x30),
            list_id: create_id(&reader.value_string_at(1)),
        }),
        21 => Action::GiveNnObjs(GiveNnObjs {}),
        22 => Action::GiveObjList(GiveObjList {
            npc: value_tstring(reader, 0, 0x30),
            list_id: create_id(&reader.value_string_at(1)),
        }),
        23 => Action::HostileClamp(HostileClamp {
            clamp: !reader.value_bool(0),
        }),
        24 => {
            let mut a = Invulnerable {
                label: value_tstring(reader, 0, 0x30),
                ..Invulnerable::default()
            };
            a.invulnerable = reader.value_bool(1);
            if !reader.is_value_empty(2) {
                a.param3 = reader.value_bool(2);
            }
            if !reader.is_value_empty(3) {
                a.duration = f32_at(reader, 3);
            }
            Action::Invulnerable(a)
        }
        25 => {
            let label_id = create_id(&reader.value_string_at(0));
            Action::Jumper(Jumper {
                label_id,
                jumper: reader.value_bool(1),
            })
        }
        26 => {
            let mut a = LightFuse {
                label: value_tstring(reader, 0, 0x30),
                fuse: value_tstring(reader, 1, 0x40),
                ..LightFuse::default()
            };
            if !reader.is_value_empty(2) {
                a.delay = f32_at(reader, 2);
            }
            Action::LightFuse(a)
        }
        27 => {
            let mut a = LockDock {
                target: value_tstring(reader, 0, 0x30),
                gate: value_tstring(reader, 1, 0x30),
                ..LockDock::default()
            };
            if value_is(reader, 2, b"lock") {
                a.mode = 1;
            } else if value_is(reader, 2, b"unlock") {
                a.mode = 0;
            }
            Action::LockDock(a)
        }
        28 => Action::LockManeuvers(LockManeuvers {
            lock: reader.value_bool(0),
        }),
        29 => Action::MarkObj(MarkObj {
            label: value_tstring(reader, 0, 0x30),
            mode: reader.value_int(1),
        }),
        30 => {
            let position = Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
            let radius = f32_at(reader, 3);
            let radius_sq = f32_at(reader, 3) * radius;
            Action::MovePlayer(MovePlayer {
                position,
                radius_sq,
            })
        }
        31 => Action::NagClamp(NagClamp {
            clamp: reader.value_bool(0),
            ..NagClamp::default()
        }),
        32 => {
            let mut a = NagDistLeaving {
                nag_id: create_id(&reader.value_string_at(0)),
                speaker: value_tstring(reader, 1, 0x30),
                target: value_tstring(reader, 2, 0x30),
                ..NagDistLeaving::default()
            };
            let (fail, distance, combat) = if eq_ignore_case(&a.target, b"position") {
                let z = f32_at(reader, 5);
                let y = f32_at(reader, 4);
                let x = f32_at(reader, 3);
                a.position = Vector::new(x, y, z);
                a.has_position = true;
                (6, 7, 8)
            } else {
                (3, 4, 5)
            };
            if !reader.is_value_empty(fail) {
                a.fail_mad_lib_id = reader.value_int(fail) as u32;
            }
            if !reader.is_value_empty(distance) {
                a.distance = nag_distance(f32_at(reader, distance));
            }
            if !reader.is_value_empty(combat) {
                nag_combat_mode(
                    reader,
                    combat,
                    &mut a.nag_in_combat,
                    &mut a.nag_out_of_combat,
                );
            }
            Action::NagDistLeaving(a)
        }
        33 => {
            let object_form = value_is(reader, 0, b"OBJ");
            let mut a = NagDistTowards {
                nag_id: create_id(&reader.value_string_at(1)),
                speaker: value_tstring(reader, 2, 0x30),
                ..NagDistTowards::default()
            };
            if object_form {
                a.target = value_tstring(reader, 3, 0x30);
                if !reader.is_value_empty(4) {
                    a.fail_mad_lib_id = reader.value_int(4) as u32;
                }
                if !reader.is_value_empty(5) {
                    a.distance = nag_distance(f32_at(reader, 5));
                }
                if !reader.is_value_empty(6) {
                    nag_combat_mode(reader, 6, &mut a.nag_in_combat, &mut a.nag_out_of_combat);
                }
            } else {
                let z = f32_at(reader, 5);
                let y = f32_at(reader, 4);
                let x = f32_at(reader, 3);
                a.position = Vector::new(x, y, z);
                if !reader.is_value_empty(6) {
                    a.fail_mad_lib_id = reader.value_int(6) as u32;
                }
                if !reader.is_value_empty(7) {
                    a.distance = f32_at(reader, 7);
                }
                if !reader.is_value_empty(8) {
                    nag_combat_mode(reader, 8, &mut a.nag_in_combat, &mut a.nag_out_of_combat);
                }
            }
            Action::NagDistTowards(a)
        }
        34 => Action::NagGreet(NagGreet {
            nag_id: create_id(&reader.value_string_at(0)),
            speaker: value_tstring(reader, 1, 0x30),
        }),
        35 => Action::NagOff(NagOff {
            nag_id: create_id(&reader.value_string_at(0)),
        }),
        36 => {
            let mut a = NnPath {
                travel_ids: reader.value_int(0),
                target_ids: reader.value_int(1),
                ..NnPath::default()
            };
            if !reader.is_value_empty(4) {
                a.position = Vector::new(f32_at(reader, 2), f32_at(reader, 3), f32_at(reader, 4));
                if !reader.is_value_empty(5) {
                    system_id_into(cx, &mut a.system_id, &reader.value_string_at(5));
                }
            } else if !reader.is_value_empty(2) {
                a.label = value_tstring(reader, 2, 0x30);
                if !reader.is_value_empty(3) {
                    system_id_into(cx, &mut a.system_id, &reader.value_string_at(3));
                    if let Ok(found) = find_system_object(cx, a.system_id, &a.label) {
                        a.position = found.unwrap_or(Vector::ZERO);
                    }
                }
            }
            Action::NnPath(a)
        }
        37 => {
            let mut a = NnIds {
                text_id: reader.value_int(0),
                ..NnIds::default()
            };
            if !reader.is_value_empty(1) {
                if value_is(reader, 1, b"OBJECTIVE") {
                    a.message_type = 7;
                } else if value_is(reader, 1, b"HISTORY") {
                    a.message_type = 8;
                } else if value_is(reader, 1, b"OBJECTIVE_HISTORY") {
                    a.message_type = 9;
                }
            }
            Action::NnIds(a)
        }
        38 => {
            let can_dock = reader.value_bool(0);
            let mut exceptions = Vec::new();
            let mut slot = 1;
            while !reader.is_value_empty(slot) {
                exceptions.push(nickname_id(&reader.value_string_at(slot)));
                slot += 1;
            }
            Action::PlayerCanDock(PlayerCanDock {
                can_dock,
                exceptions,
            })
        }
        39 => {
            let can_tradelane = reader.value_bool(0);
            let mut exceptions = Vec::new();
            let mut slot = 1;
            while !reader.is_value_empty(slot) {
                exceptions.push([
                    nickname_id(&reader.value_string_at(slot)),
                    nickname_id(&reader.value_string_at(slot + 1)),
                ]);
                slot += 2;
            }
            Action::PlayerCanTradelane(PlayerCanTradelane {
                can_tradelane,
                exceptions,
            })
        }
        40 => Action::PlayerEnemyClamp(PlayerEnemyClamp {
            min_clamp: reader.value_int(0),
            max_clamp: reader.value_int(1),
        }),
        41 => {
            let id = create_id(&reader.value_string_at(0));
            let formation = cx.logic.content.tables.formation(id).map(|_| id);
            Action::PlayerForm(PlayerForm {
                npc: value_tstring(reader, 1, 0x30),
                formation,
            })
        }
        42 => {
            let mut a = PlayMusic::default();
            if reader.is_value_empty(0) || value_is(reader, 0, b"no_params") {
                a.cancel = true;
                return Action::PlayMusic(a);
            }
            for slot in 0..4 {
                if !reader.is_value_empty(slot) && !value_is(reader, slot, b"none") {
                    a.music[slot as usize] = nickname_id(&reader.value_string_at(slot));
                }
            }
            if !reader.is_value_empty(4) && !value_is(reader, 4, b"none") {
                a.fade = f32_at(reader, 4);
            }
            if !reader.is_value_empty(5) && !value_is(reader, 5, b"none") {
                a.field_10 = reader.value_bool(5);
            }
            Action::PlayMusic(a)
        }
        43 => Action::PlayNn(PlayNn {
            message_id: nickname_id(&reader.value_string_at(0)),
        }),
        44 => Action::PlaySoundEffect(PlaySoundEffect {
            sound_id: nickname_id(&reader.value_string_at(0)),
        }),
        45 => {
            let mut a = PilotParams {
                label: value_tstring(reader, 0, 0x30),
                ..PilotParams::default()
            };
            let mode = reader.value_string_at(1);
            if let Some(&(_, value)) = FLEE_MODES
                .iter()
                .find(|(name, _)| eq_ignore_case(c_str(&mode), name))
            {
                a.flee_mode = value;
            }
            Action::PilotParams(a)
        }
        46 => Action::PobjIdle(PobjIdle {}),
        47 => {
            let mut a = PopUpDialog {
                title_id: reader.value_int(0) as u32,
                text_id: reader.value_int(1) as u32,
                ..PopUpDialog::default()
            };
            if !reader.is_value_empty(2) {
                let kind = value_tstring(reader, 2, 0x40);
                if eq_ignore_case(&kind, b"YES_NO") {
                    a.buttons = 3;
                } else if eq_ignore_case(&kind, b"YES_NO_LATER") {
                    a.buttons = 7;
                } else if eq_ignore_case(&kind, b"CLOSE") {
                    a.buttons = 8;
                }
            }
            Action::PopUpDialog(a)
        }
        48 => {
            let mut a = SetPriority {
                formation_id: create_id(&reader.value_string_at(0)),
                ..SetPriority::default()
            };
            if value_is(reader, 1, b"NORMAL") {
                a.priority = 1;
            } else if value_is(reader, 1, b"ALWAYS_EXECUTE") {
                a.priority = 0;
            }
            Action::SetPriority(a)
        }
        49 => Action::RandomPop(RandomPop {
            enabled: reader.value_bool(0),
        }),
        50 => {
            let mut a = RandomPopSphere {
                center: Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2)),
                radius: f32_at(reader, 3),
                ..RandomPopSphere::default()
            };
            let mode = value_tstring(reader, 4, 0x40);
            if eq_ignore_case(&mode, b"on") {
                a.mode = 0;
            } else if eq_ignore_case(&mode, b"off") {
                a.mode = 1;
            }
            Action::RandomPopSphere(a)
        }
        51 => {
            let mut a = RelocateForm::default();
            relocation(reader, &mut a.label, &mut a.placement);
            Action::RelocateForm(a)
        }
        52 => {
            let mut a = RelocateShip::default();
            relocation(reader, &mut a.label, &mut a.placement);
            Action::RelocateShip(a)
        }
        53 => Action::RemoveAmbient(RemoveAmbient {
            path: value_tstring(reader, 0, 0x80),
        }),
        54 => {
            let mut a = RemoveCargo {
                loot: value_tstring(reader, 0, 0x30),
                ..RemoveCargo::default()
            };
            if !reader.is_value_empty(1) {
                a.count = reader.value_int(1);
            }
            Action::RemoveCargo(a)
        }
        55 => Action::RemoveRtc(RemoveRtc {
            rtc_file: value_tstring(reader, 0, 0x80),
        }),
        56 => {
            let mut a = RepChangeRequest::default();
            system_id_into(cx, &mut a.system_id, &reader.value_string_at(0));
            let mut slot = 1;
            if !reader.is_value_empty(slot) {
                while slot < 0x32 {
                    let group = tables::reputation_group(cx, &reader.value_string_at(slot));
                    if !a.groups.contains(&group) {
                        a.groups.push(group);
                    }
                    slot += 1;
                    if reader.is_value_empty(slot) {
                        break;
                    }
                }
            }
            a.armed = true;
            Action::RepChangeRequest(a)
        }
        57 => Action::RevertCam(RevertCam {}),
        58 => Action::RpopAttClamp(RpopAttClamp {
            clamp: reader.value_bool(0),
        }),
        59 => Action::RpopTlAttacksEnabled(RpopTlAttacksEnabled {
            enabled: reader.value_bool(0),
        }),
        60 => {
            let mut a = Save {
                save_trigger_id: create_id(&reader.value_string_at(0)),
                ..Save::default()
            };
            if !reader.is_value_empty(1) {
                a.save_type = reader.value_int(1) as u32;
            }
            Action::Save(a)
        }
        61 => {
            let mut a = SendComm {
                sender: value_tstring(reader, 0, 0x30),
                receiver: value_tstring(reader, 1, 0x30),
                text_id: nickname_id(&reader.value_string_at(2)),
                ..SendComm::default()
            };
            if !reader.is_value_empty(3) {
                a.duration = comm_duration(reader.value_int(3));
            }
            let to_player = eq_ignore_case(&a.receiver, b"Player");
            if !reader.is_value_empty(4) {
                tables::voice_id(&mut a.voice_id, &reader.value_string_at(4));
                if !reader.is_value_empty(5) {
                    a.comm_type = reader.value_int(5) as u32;
                }
                if !reader.is_value_empty(6) {
                    comm_costume(cx, reader, 6, &mut a.speaker_costume);
                    if to_player {
                        script.add_dialog_costume(&a.speaker_costume);
                    }
                }
            }
            if reader.is_value_empty(6)
                && to_player
                && let Some(costume) = script.mission_costume(&a.sender)
            {
                script.add_dialog_costume(&costume);
            }
            Action::SendComm(a)
        }
        62 => {
            let mut a = SetFlee::default();
            if value_is(reader, 0, b"CLEAR") {
                a.mode = 2;
            } else if value_is(reader, 0, b"SET_POS") {
                a.mode = 0;
            } else if value_is(reader, 0, b"SET_OBJ") {
                a.mode = 1;
            }
            a.npc = value_tstring(reader, 1, 0x30);
            if a.mode == 0 {
                let z = f32_at(reader, 4);
                let y = f32_at(reader, 3);
                let x = f32_at(reader, 2);
                a.position = Vector::new(x, y, z);
                a.distance = f32_at(reader, 5);
                a.flag = reader.value_bool(6);
            } else if a.mode == 1 {
                a.target = value_tstring(reader, 2, 0x30);
                a.distance = f32_at(reader, 3);
                a.flag = reader.value_bool(4);
            }
            Action::SetFlee(a)
        }
        63 => {
            let z = f32_at(reader, 2);
            let y = f32_at(reader, 1);
            let x = f32_at(reader, 0);
            let qz = f32_at(reader, 6);
            let qy = f32_at(reader, 5);
            let qx = f32_at(reader, 4);
            let qw = f32_at(reader, 3);
            Action::SetInitialPlayerPos(SetInitialPlayerPos {
                orientation: quaternion_to_matrix(&Quaternion::new(qw, qx, qy, qz)),
                position: Vector::new(x, y, z),
            })
        }
        64 => Action::SetLifeTime(SetLifeTime {
            label: value_tstring(reader, 0, 0x30),
            life_time: f32_at(reader, 1),
        }),
        65 => {
            let objective_id = create_id(&reader.value_string_at(0));
            Action::SetNnHidden(SetNnHidden {
                visible: !reader.value_bool(1),
                objective_id,
            })
        }
        66 => {
            let mut a = SetNnObj {
                objective_id: create_id(&reader.value_string_at(0)),
                ..SetNnObj::default()
            };
            if !reader.is_value_empty(1) {
                if value_is(reader, 1, b"HISTORY") {
                    a.state = 0x12a;
                }
                if value_is(reader, 1, b"OBJECTIVE") {
                    a.state = 0x11a;
                }
                if value_is(reader, 1, b"OBJECTIVE_HISTORY") {
                    a.state = 0x13a;
                }
            }
            Action::SetNnObj(a)
        }
        67 => {
            let mut a = SetNnState {
                objective_id: create_id(&reader.value_string_at(0)),
                ..SetNnState::default()
            };
            if value_is(reader, 1, b"ACTIVE") {
                a.state = 1;
            } else if value_is(reader, 1, b"INACTIVE") {
                a.state = 4;
            } else if value_is(reader, 1, b"COMPLETE") {
                a.state = 3;
            } else if value_is(reader, 1, b"FAILED") {
                a.state = 2;
            }
            Action::SetNnState(a)
        }
        68 => Action::SetOffer(SetOffer {
            offer_id: reader.value_int(0) as u32,
        }),
        69 => Action::SetOrient(SetOrient {
            label: value_tstring(reader, 0, 0x30),
            orientation: Quaternion::new(
                f32_at(reader, 1),
                f32_at(reader, 2),
                f32_at(reader, 3),
                f32_at(reader, 4),
            ),
        }),
        70 => {
            let mut a = SetRep {
                label: value_tstring(reader, 0, 0x30),
                faction: value_tstring(reader, 1, 0x30),
                ..SetRep::default()
            };
            a.group_id = tables::reputation_group(cx, &reader.value_string_at(1));
            a.value = if reader.is_number(&reader.value_string_at(2)) {
                f32_at(reader, 2)
            } else {
                rep_value_from_name(&value_tstring(reader, 2, 0x40))
            };
            Action::SetRep(a)
        }
        71 => {
            let mut a = SetShipAndLoadout::default();
            if !reader.is_value_empty(0) {
                a.ship_name = value_tstring(reader, 0, 0x20);
                let name = reader.value_string_at(0);
                if !eq_ignore_case(c_str(&name), b"none") && !c_str(&name).is_empty() {
                    a.ship_arch_id = create_id(c_str(&name));
                }
            }
            if !reader.is_value_empty(1) {
                a.loadout_name = value_tstring(reader, 0, 0x20);
                if !value_is(reader, 1, b"none")
                    && let Ok(id) = api::misc::loadout_id(&reader.value_string_at(1))
                    && let Ok(items) = api::misc::loadout(cx, id)
                {
                    a.loadout = items;
                }
            }
            Action::SetShipAndLoadout(a)
        }
        72 => Action::SetTitle(SetTitle {
            title_id: reader.value_int(0) as u32,
        }),
        73 => {
            let mut a = SetVibe {
                label_a: value_tstring(reader, 0, 0x30),
                label_b: value_tstring(reader, 1, 0x30),
                ..SetVibe::default()
            };
            vibe_range(reader, &mut a.lo, &mut a.hi);
            Action::SetVibe(a)
        }
        74 => {
            let mut a = SetVibeLbl {
                label_a: create_id(&reader.value_string_at(0)),
                label_b: create_id(&reader.value_string_at(1)),
                ..SetVibeLbl::default()
            };
            vibe_range(reader, &mut a.lo, &mut a.hi);
            Action::SetVibeLbl(a)
        }
        75 => {
            let mut a = SetVibeLblToShip {
                formation_id: create_id(&reader.value_string_at(0)),
                ship: value_tstring(reader, 1, 0x30),
                ..SetVibeLblToShip::default()
            };
            vibe_range(reader, &mut a.lo, &mut a.hi);
            Action::SetVibeLblToShip(a)
        }
        76 => {
            let mut a = SetVibeShipToLbl {
                ship: value_tstring(reader, 0, 0x30),
                formation_id: create_id(&reader.value_string_at(1)),
                ..SetVibeShipToLbl::default()
            };
            vibe_range(reader, &mut a.lo, &mut a.hi);
            Action::SetVibeShipToLbl(a)
        }
        77 => Action::SetVibeOfferBaseHack(SetVibeOfferBaseHack {
            base_id: nickname_id(&reader.value_string_at(0)),
        }),
        78 => {
            let mut a = SpawnFormation::default();
            spawn_record(
                reader,
                &mut a.name,
                &mut a.placement,
                &mut a.position,
                &mut a.orientation,
            );
            Action::SpawnFormation(a)
        }
        79 => {
            let mut a = SpawnLoot::default();
            spawn_record(
                reader,
                &mut a.name,
                &mut a.placement,
                &mut a.position,
                &mut a.orientation,
            );
            Action::SpawnLoot(a)
        }
        80 => {
            let mut a = SpawnShip {
                name: value_tstring(reader, 0, 0x30),
                ..SpawnShip::default()
            };
            if !reader.is_value_empty(1) {
                a.obj_list = create_id(&reader.value_string_at(1));
                if !reader.is_value_empty(2) {
                    a.position = true;
                    let z = f32_at(reader, 4);
                    let y = f32_at(reader, 3);
                    a.placement.translation = Vector::new(f32_at(reader, 2), y, z);
                    if !reader.is_value_empty(5) {
                        a.orientation = true;
                        a.placement.rotation = spawn_rotation(reader, 5);
                    }
                }
            }
            Action::SpawnShip(a)
        }
        81 => {
            let mut a = SpawnShipRel {
                name: value_tstring(reader, 0, 0x30),
                obj_list: create_id(&reader.value_string_at(1)),
                rel_obj: value_tstring(reader, 2, 0x30),
                ..SpawnShipRel::default()
            };
            if value_is(reader, 3, b"DIRECTIONAL") {
                a.mode = 1;
            } else if value_is(reader, 3, b"POINT") {
                a.mode = 0;
            } else if value_is(reader, 3, b"RELATIVE") {
                a.mode = 2;
            }
            let z = f32_at(reader, 6);
            let y = f32_at(reader, 5);
            a.offset.translation = Vector::new(f32_at(reader, 4), y, z);
            if !reader.is_value_empty(7) {
                a.heading = f32_at(reader, 7);
                if !reader.is_value_empty(8) {
                    a.rename = true;
                    a.new_nickname = value_tstring(reader, 8, 0x30);
                }
            }
            Action::SpawnShipRel(a)
        }
        82 => {
            let mut a = SpawnSolar::default();
            spawn_record(
                reader,
                &mut a.name,
                &mut a.placement,
                &mut a.position,
                &mut a.orientation,
            );
            Action::SpawnSolar(a)
        }
        83 => {
            let mut a = StartDialog {
                dialog_id: create_id(&reader.value_string_at(0)),
                ..StartDialog::default()
            };
            if !reader.is_value_empty(1) {
                let delay = reader.value_int(1);
                if delay < -1 || delay == 0 || delay > 10 {
                    a.delay_seconds = -1;
                } else {
                    a.delay_seconds = delay;
                    a.delay_pending = true;
                }
            }
            Action::StartDialog(a)
        }
        _ => {
            let mut a = StaticCam::default();
            a.camera.translation =
                Vector::new(f32_at(reader, 0), f32_at(reader, 1), f32_at(reader, 2));
            let z = f32_at(reader, 6);
            let y = f32_at(reader, 5);
            let x = f32_at(reader, 4);
            let w = f32_at(reader, 3);
            a.camera.rotation = quaternion_to_matrix(&normalize_quaternion(w, x, y, z));
            if !reader.is_value_empty(7) {
                a.fov = f32_at(reader, 7);
            }
            if !reader.is_value_empty(8) {
                a.aspect = f32_at(reader, 8);
            }
            Action::StaticCam(a)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_factories_have_the_class_counts() {
        assert_eq!(FACTORY.len(), 85);
        assert_eq!(super::super::condition::FACTORY.len(), 44);
    }

    #[test]
    fn mission_states_by_name() {
        assert_eq!(mission_state_from_name(b"succeed"), 1);
        assert_eq!(mission_state_from_name(b"FAIL"), 0);
        assert_eq!(mission_state_from_name(b"No_State"), 2);
        assert_eq!(mission_state_from_name(b"done"), -1);
    }

    #[test]
    fn comm_durations() {
        assert_eq!(comm_duration(-1), -1);
        assert_eq!(comm_duration(0), -1);
        assert_eq!(comm_duration(5), 5);
        assert_eq!(comm_duration(11), -1);
        assert_eq!(comm_duration(-2), -1);
    }

    #[test]
    fn a_unit_quaternion_stays_unit() {
        let q = normalize_quaternion(2.0, 0.0, 0.0, 0.0);
        assert_eq!(q, Quaternion::new(1.0, 0.0, 0.0, 0.0));
    }
}
