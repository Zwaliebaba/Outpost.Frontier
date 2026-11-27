use neuron_client::ui::widescreen::Surface;

use super::*;

fn load(text: &str) -> (Ui, HudElements, Vec<String>) {
    let mut ui = Ui::new(Surface::new(1024, 768));
    let mut warnings = Vec::new();
    let hud = load_hud_elements(
        &mut ui,
        b".\\data\\interface\\hud.ini",
        Some(text.as_bytes().to_vec()),
        FACTORIES,
        &mut |w| warnings.push(w),
    );
    (ui, hud, warnings)
}

#[test]
fn the_sections_make_their_elements_in_the_files_order() {
    let (ui, hud, warnings) = load(
        "[HUD]\nshapes = interface\\hud\\hudshapes.ini\nstatic_txm = a.txm\nstatic_blend = 3.9\n[Core]\n[Nothing]\n",
    );
    assert_eq!(
        hud.config,
        HudConfig {
            shapes: Some(b"interface\\hud\\hudshapes.ini".to_vec()),
            static_txm: Some(b"a.txm".to_vec()),
            static_blend: 3,
        }
    );
    assert_eq!(hud.items.len(), 1);
    assert_eq!(hud.items[0].name(), b"Core");
    let window = hud.items[0].window().expect("a window");
    let node = ui.node(window).unwrap();
    assert_eq!(node.name(), b"HUD_Core");
    assert_eq!(node.class_name, Some("HUD_Core"));
    assert_eq!(
        warnings,
        ["Client\\space\\HUD.cpp(310) : *** WARNING: HUD - unknown INI entry 'Nothing'\n"]
    );
    assert_eq!(
        oracle_lines(118, b".\\data\\interface\\hud.ini", &hud),
        [
            r#"{"frame":118,"rec":"hud","path":".\\data\\interface\\hud.ini","static_blend":3,"items":1}"#,
            r#"{"frame":118,"rec":"huditem","i":0,"name":"Core","rtti":null,"tooltip":0}"#,
        ]
    );
}

#[test]
fn a_missing_file_makes_nothing_and_warns() {
    let mut ui = Ui::new(Surface::new(1024, 768));
    let mut warnings = Vec::new();
    let hud = load_hud_elements(&mut ui, b"x.ini", None, FACTORIES, &mut |w| {
        warnings.push(w)
    });
    assert!(hud.items.is_empty());
    assert_eq!(hud.config.static_blend, 2);
    assert_eq!(
        warnings,
        ["Client\\space\\HUD.cpp(317) : *** WARNING: hud file not found 'x.ini'\n"]
    );
}

#[test]
fn the_data_sections_hud_key_gives_the_file_under_the_data_path() {
    let ini = b"[Data]\nfonts = fonts\\fonts.ini\nHUD = interface\\hud.ini\n[Other]\nHUD = x.ini\n";
    assert_eq!(
        hud_ini(ini, b".\\data"),
        Some(HudIni {
            path: b".\\data\\interface\\hud.ini".to_vec(),
            file: b"interface\\hud.ini".to_vec(),
        })
    );
    assert_eq!(hud_ini(b"[Data]\nfonts = f.ini\n", b".\\data"), None);
}
