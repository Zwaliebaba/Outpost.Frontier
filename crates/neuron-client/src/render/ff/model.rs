//! The state part of the device model: render states, texture-stage states
//! and pipeline properties, with the set and get rules of DX8RENDERER.
//!
//! A render state set by the caller is pending until the next draw in the
//! C++ (`SetRenderState` marks it, `FlushDrawState` writes it); its getter
//! returns the pending value, so for the model it is the value. A stage
//! state is written at once. Neither getter can fail once the device
//! exists. Both take any Direct3D 8 number in range, not only the ones in
//! DX8RENDERER's tables: the C++ forwards those to the device unchanged.

use std::fmt;

use super::defaults::Defaults;
use super::state::{
    PIPELINE_SLOTS, PipelineProperty, RenderState, RenderStateId, STAGE_STATE_SLOTS, STAGES,
    StageState, StageStateId,
};

/// The render-state numbers the cache holds
/// (`FDx8RendererRenderStateCache::StateCount`).
pub const RENDER_STATE_SLOTS: usize = 256;

/// A state call the model refuses. The C++ indexes its arrays with these
/// numbers unchecked, which is undefined behaviour (divergence D203).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StateError {
    RenderStateOutOfRange(u32),
    StageOutOfRange(u32),
    StageStateOutOfRange(u32),
}

impl fmt::Display for StateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::RenderStateOutOfRange(n) => write!(f, "render state {n} is out of range"),
            Self::StageOutOfRange(n) => write!(f, "texture stage {n} is out of range"),
            Self::StageStateOutOfRange(n) => write!(f, "texture-stage state {n} is out of range"),
        }
    }
}

impl std::error::Error for StateError {}

/// A pipeline-property slot (`this + 0x354 + id * 12`: value, enabled,
/// set).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
struct PipelineSlot {
    value: u32,
    enabled: bool,
}

/// Render, stage and pipeline state.
#[derive(Clone, Debug)]
pub struct StateModel {
    defaults: Defaults,
    render: [u32; RENDER_STATE_SLOTS],
    stages: [[u32; STAGE_STATE_SLOTS]; STAGES],
    pipeline: [PipelineSlot; PIPELINE_SLOTS],
}

impl StateModel {
    /// The state after `Startup` read `defaults` and `CreateBuffers`
    /// applied them.
    #[must_use]
    pub fn new(defaults: Defaults) -> StateModel {
        let mut pipeline = [PipelineSlot::default(); PIPELINE_SLOTS];
        for (slot, value) in pipeline.iter_mut().zip(defaults.pipeline) {
            if let Some(value) = value {
                *slot = PipelineSlot {
                    value,
                    enabled: true,
                };
            }
        }
        let mut model = StateModel {
            defaults,
            render: [0; RENDER_STATE_SLOTS],
            stages: [[0; STAGE_STATE_SLOTS]; STAGES],
            pipeline,
        };
        model.reset_render_states();
        model
    }

    #[must_use]
    pub fn defaults(&self) -> &Defaults {
        &self.defaults
    }

    /// `ResetRenderStatesToDefaults` (also the tail of `CreateBuffers`):
    /// the render-state defaults, then every stage's defaults. States that
    /// are not in the tables keep their values.
    pub fn reset_render_states(&mut self) {
        for id in RenderStateId::ALL {
            self.render[id.raw() as usize] = self.defaults.render_state(*id);
        }
        for stage in &mut self.stages {
            for id in StageStateId::ALL {
                stage[id.raw() as usize] = id.builtin_default();
            }
        }
    }

    /// `SetRenderState` with a Direct3D 8 number.
    pub fn set_render_state_raw(&mut self, state: u32, value: u32) -> Result<(), StateError> {
        let slot = self
            .render
            .get_mut(state as usize)
            .ok_or(StateError::RenderStateOutOfRange(state))?;
        *slot = value;
        Ok(())
    }

    /// `GetRenderState` with a Direct3D 8 number.
    pub fn render_state_raw(&self, state: u32) -> Result<u32, StateError> {
        self.render
            .get(state as usize)
            .copied()
            .ok_or(StateError::RenderStateOutOfRange(state))
    }

    pub fn set_render_state(&mut self, state: RenderState) {
        self.render[state.id().raw() as usize] = state.to_raw();
    }

    #[must_use]
    pub fn render_state(&self, id: RenderStateId) -> RenderState {
        RenderState::from_raw(id, self.render[id.raw() as usize])
    }

    fn stage_slot(&self, stage: u32, state: u32) -> Result<(usize, usize), StateError> {
        if stage as usize >= STAGES {
            return Err(StateError::StageOutOfRange(stage));
        }
        if state as usize >= STAGE_STATE_SLOTS {
            return Err(StateError::StageStateOutOfRange(state));
        }
        Ok((stage as usize, state as usize))
    }

    /// `SetTextureStageState` with Direct3D 8 numbers.
    pub fn set_stage_state_raw(
        &mut self,
        stage: u32,
        state: u32,
        value: u32,
    ) -> Result<(), StateError> {
        let (s, t) = self.stage_slot(stage, state)?;
        self.stages[s][t] = value;
        Ok(())
    }

    /// `GetTextureStageState` with Direct3D 8 numbers.
    pub fn stage_state_raw(&self, stage: u32, state: u32) -> Result<u32, StateError> {
        let (s, t) = self.stage_slot(stage, state)?;
        Ok(self.stages[s][t])
    }

    /// Sets a typed stage state; `stage` must be below [`STAGES`].
    pub fn set_stage_state(&mut self, stage: usize, state: StageState) {
        self.stages[stage][state.id().raw() as usize] = state.to_raw();
    }

    #[must_use]
    pub fn stage_state(&self, stage: usize, id: StageStateId) -> StageState {
        StageState::from_raw(id, self.stages[stage][id.raw() as usize])
    }

    /// `SetPipelineState`: changes only an enabled slot, that is one in
    /// the property table. Any other number is ignored, as in the C++ for
    /// the slots it has (divergence D203 for the rest).
    pub fn set_pipeline_raw(&mut self, id: i32, value: i32) {
        if let Some(slot) = usize::try_from(id)
            .ok()
            .and_then(|i| self.pipeline.get_mut(i))
            && slot.enabled
        {
            slot.value = value.cast_unsigned();
        }
    }

    /// `GetPipelineState`: the slot's value for 0 to 0xE, else `None` (the
    /// C++ leaves the output alone).
    #[must_use]
    pub fn pipeline_raw(&self, id: i32) -> Option<u32> {
        usize::try_from(id)
            .ok()
            .and_then(|i| self.pipeline.get(i))
            .map(|slot| slot.value)
    }

    pub fn set_pipeline(&mut self, property: PipelineProperty, value: u32) {
        self.set_pipeline_raw(property.raw().cast_signed(), value.cast_signed());
    }

    #[must_use]
    pub fn pipeline(&self, property: PipelineProperty) -> u32 {
        self.pipeline[property.raw() as usize].value
    }
}

#[cfg(test)]
mod tests {
    use super::super::state::{Compare, TextureOp};
    use super::*;

    #[test]
    fn starts_from_the_defaults_with_every_stage_disabled() {
        let mut d = Defaults::builtin();
        d.render[RenderStateId::ZFunc.index()] = 4;
        let m = StateModel::new(d);
        assert_eq!(
            m.render_state(RenderStateId::ZFunc),
            RenderState::ZFunc(Compare::LessEqual)
        );
        for stage in 0..STAGES {
            assert_eq!(
                m.stage_state(stage, StageStateId::ColorOp),
                StageState::ColorOp(TextureOp::Disable)
            );
            assert_eq!(m.stage_state_raw(stage as u32, 0x11).unwrap(), 2);
        }
        // A number outside the table starts at 0.
        assert_eq!(m.render_state_raw(0x8A).unwrap(), 0);
        assert_eq!(m.pipeline(PipelineProperty::ViewspaceLights), 1);
    }

    #[test]
    fn reset_restores_table_states_only() {
        let mut m = StateModel::new(Defaults::builtin());
        m.set_render_state_raw(0x17, 7).unwrap();
        m.set_render_state_raw(0x8A, 9).unwrap();
        m.set_stage_state_raw(1, 1, 4).unwrap();
        m.reset_render_states();
        assert_eq!(m.render_state_raw(0x17).unwrap(), 2);
        assert_eq!(m.render_state_raw(0x8A).unwrap(), 9);
        assert_eq!(m.stage_state_raw(1, 1).unwrap(), 1);
    }

    #[test]
    fn out_of_range_numbers_are_errors() {
        let mut m = StateModel::new(Defaults::builtin());
        assert_eq!(
            m.set_render_state_raw(0x100, 1),
            Err(StateError::RenderStateOutOfRange(0x100))
        );
        assert_eq!(m.stage_state_raw(8, 1), Err(StateError::StageOutOfRange(8)));
        assert_eq!(
            m.stage_state_raw(0, 0x1D),
            Err(StateError::StageStateOutOfRange(0x1D))
        );
    }

    #[test]
    fn pipeline_slots_follow_set_pipeline_state() {
        let mut m = StateModel::new(Defaults::builtin());
        m.set_pipeline_raw(8, 0x00ff_0000);
        assert_eq!(m.pipeline_raw(8), Some(0x00ff_0000));
        // Slot 0xE is not enabled: the value stays 0.
        m.set_pipeline_raw(0xE, 5);
        assert_eq!(m.pipeline_raw(0xE), Some(0));
        m.set_pipeline_raw(-1, 5);
        assert_eq!(m.pipeline_raw(-1), None);
        assert_eq!(m.pipeline_raw(0xF), None);
    }
}
