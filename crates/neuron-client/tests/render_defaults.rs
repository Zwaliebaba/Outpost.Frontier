//! The render-state defaults from the shipped engine INI
//! (`outdir/NeuronComponents.ini`, decision O15), section `[RenderPipeline]`.

use std::fs;
use std::path::Path;

use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::model::StateModel;
use neuron_client::render::ff::state::{Compare, RenderState, RenderStateId};
use neuron_core::ini::{EngineIni, NoIncludes};

#[test]
fn the_shipped_engine_ini_sets_alpha_test_and_zfunc() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/NeuronComponents.ini");
    let Ok(bytes) = fs::read(&path) else {
        println!("{} is missing: nothing to read", path.display());
        return;
    };
    let mut ini = EngineIni::new();
    ini.load_buffer(&bytes, &mut NoIncludes)
        .expect("the engine INI parses");
    let defaults = Defaults::from_section(ini.section(b"RenderPipeline"));
    let model = StateModel::new(defaults);
    assert_eq!(
        model.render_state(RenderStateId::AlphaTestEnable),
        RenderState::AlphaTestEnable(true)
    );
    assert_eq!(
        model.render_state(RenderStateId::AlphaFunc),
        RenderState::AlphaFunc(Compare::Greater)
    );
    assert_eq!(
        model.render_state(RenderStateId::ZFunc),
        RenderState::ZFunc(Compare::LessEqual)
    );
    assert_eq!(model.render_state_raw(0x18), Ok(0));
    assert_eq!(
        model.render_state(RenderStateId::LocalViewer),
        RenderState::LocalViewer(false)
    );
    // Keys the section does not set keep the table's value.
    assert_eq!(model.render_state_raw(RenderStateId::ZEnable.raw()), Ok(1));
    assert!(!model.defaults().lockable_back_buffer);
}
