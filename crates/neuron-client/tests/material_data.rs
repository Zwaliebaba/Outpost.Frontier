//! The material map and batching rules of the shipped engine INI
//! (`outdir/NeuronComponents.ini`, decision O15, sections `[MaterialMap]`,
//! `[BatchedMaterials]` and `[MaterialBatcher]`) through the material
//! batcher of work package P4.5. Nothing is compared with the oracle (the
//! synthetic suites of `oracle_vectors.rs` do that for the parser, the
//! regular expressions and the rules); the test checks that every line of
//! the game's map becomes a rule whose pattern compiles, and that names and
//! types the game uses resolve as the comments in the INI say they should.

use std::fs;
use std::path::Path;

use neuron_client::render::material::{Material, MaterialBatcher, MaterialFactory};
use neuron_core::ini::{EngineIni, NoIncludes};

/// Materials are never made here.
struct NoFactory;

impl MaterialFactory<()> for NoFactory {
    fn create(&mut self, _: &mut (), _: &[u8]) -> Result<Box<dyn Material<()>>, i32> {
        Err(-3)
    }
}

#[test]
fn the_shipped_material_map_compiles_and_resolves() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/NeuronComponents.ini");
    let Ok(bytes) = fs::read(&path) else {
        println!("{} is missing: nothing to read", path.display());
        return;
    };
    let mut ini = EngineIni::new();
    ini.load_buffer(&bytes, &mut NoIncludes)
        .expect("the engine INI parses");
    let section = ini.section(b"MaterialMap").expect("[MaterialMap]");
    let mapped = section.entries.iter().filter(|e| e.value.is_some()).count();
    let batcher = MaterialBatcher::new(Box::new(NoFactory), Some(&ini));
    let library = batcher.library();
    assert_eq!(library.rules().len(), mapped, "one rule per map line");
    for rule in library.rules() {
        assert!(
            rule.expression.is_some(),
            "{} does not compile",
            String::from_utf8_lossy(&rule.pattern)
        );
    }
    assert!(!batcher.rules().is_empty());
    for rule in batcher.rules() {
        assert!(rule.expression.is_some());
    }
    let remap = |name: &str, ty: &str| {
        library
            .remap_type(name.as_bytes(), ty.as_bytes())
            .map(|t| String::from_utf8_lossy(&t).into_owned())
            .unwrap_or_default()
    };
    // "exporter generates EcEtOcOt but there is no implementation of it".
    assert_eq!(remap("hull", "EcEtOcOt"), "DcDtOcOt");
    assert_eq!(remap("hull", "DcDtEcEt"), "DcDtEt");
    // "materials that would otherwise get mapped to translucent textures".
    assert_eq!(remap("alpha_mask_fin", "DcDtOcOt"), "DcDt");
    assert_eq!(remap("alpha_mask_fin2side", "DcDtOcOt"), "DcDtTwo");
    assert_eq!(remap("null", "DcDt"), "NullMaterial");
    assert_eq!(remap("plain_hull", "DcDt"), "");
    // The batching rule wraps translucent types.
    let batched = batcher.rules().iter().any(|r| {
        r.kind == 1
            && r.expression
                .as_ref()
                .is_some_and(|e| e.is_match(b"DcDtOcOt"))
    });
    assert!(batched, "[BatchedMaterials] batches DcDtOcOt");
}
