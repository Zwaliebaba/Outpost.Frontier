//! The `[Initial SP DLLs]` and `[Initial MP DLLs]` rows of the server INI
//! (`Server_LoadInitialDlls`, `Server/IServerImpl.cpp:2944-3004`).

use neuron_core::ini::IniReader;

use super::Priority;

/// One `DLL = <dll>, <instance>, <priority>` row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InitialController {
    /// The DLL name, at most 259 bytes.
    pub dll: Vec<u8>,
    /// The instance name, at most 259 bytes.
    pub instance: Vec<u8>,
    /// The priority. A word that names no row keeps the previous row's
    /// priority; before the first match it is `HIGHEST` (the reference
    /// starts its variable at 0, the original at an indeterminate value).
    pub priority: Priority,
}

/// What the section gives `pub::Controller::Create`, in order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct InitialControllers {
    /// The last `path` value (`GDllPath`, which `pub::DLL_LoadLibrary` joins
    /// to a relative DLL name), at most 259 bytes; empty when none was given.
    pub path: Vec<u8>,
    /// The rows in order.
    pub rows: Vec<InitialController>,
}

/// `FBounded::Copy`: at most `size - 1` bytes, cut at a NUL.
fn bounded(value: &[u8], size: usize) -> Vec<u8> {
    let value = neuron_core::hash::c_str(value);
    value[..value.len().min(size - 1)].to_vec()
}

/// Reads the rows of every `[Initial SP DLLs]` section in single player, or
/// every `[Initial MP DLLs]` section otherwise, from an opened reader.
pub fn read_initial_controllers(reader: &mut IniReader, single_player: bool) -> InitialControllers {
    let mut out = InitialControllers::default();
    let mut priority = Priority::Highest;
    while reader.read_header() {
        let wanted = (reader.is_header(b"Initial SP DLLs") && single_player)
            || (reader.is_header(b"Initial MP DLLs") && !single_player);
        if !wanted {
            continue;
        }
        while reader.read_value() {
            if reader.is_value(b"path") {
                out.path = bounded(&reader.value_string(), 0x104);
            } else if reader.is_value(b"DLL") {
                let dll = bounded(&reader.value_string_at(0), 0x104);
                let instance = bounded(&reader.value_string_at(1), 0x104);
                let word = bounded(&reader.value_string_at(2), 0x20);
                if let Some(p) = Priority::from_name(&word) {
                    priority = p;
                }
                out.rows.push(InitialController {
                    dll,
                    instance,
                    priority,
                });
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn read(text: &str, single_player: bool) -> InitialControllers {
        let mut reader = IniReader::new();
        assert!(reader.open_memory(text.as_bytes()));
        read_initial_controllers(&mut reader, single_player)
    }

    #[test]
    fn reads_the_shipped_section() {
        let ini = std::fs::read(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../outdir/OutpostFrontier.ini"),
        )
        .expect("outdir/OutpostFrontier.ini");
        let mut reader = IniReader::new();
        assert!(reader.open_memory(&ini));
        let sp = read_initial_controllers(&mut reader, true);
        assert_eq!(sp.path, b".\\");
        let rows: Vec<(String, &str)> = sp
            .rows
            .iter()
            .map(|r| {
                (
                    String::from_utf8_lossy(&r.instance).into_owned(),
                    r.priority.name(),
                )
            })
            .collect();
        let want = [
            ("GameSupport", "HIGHEST"),
            ("SpaceSupport", "NORMAL"),
            ("BaseSupport", "NORMAL"),
            ("StoryLauncher", "BELOW_NORMAL"),
            ("SpacePop", "LOWEST"),
            ("AISandbox", "BELOW_NORMAL"),
            ("TestAutomation", "BELOW_NORMAL"),
            ("BasePop", "LOWEST"),
        ];
        let want: Vec<(String, &str)> = want.iter().map(|(n, p)| ((*n).to_owned(), *p)).collect();
        assert_eq!(rows, want);
        assert!(sp.rows.iter().all(|r| r.dll == b"Content.dll"));
    }

    #[test]
    fn unknown_words_carry_the_previous_priority() {
        let got = read(
            "[Initial MP DLLs]\nDLL = a.dll, One, bogus\nDLL = a.dll, Two, lowest\nDLL = a.dll, Three, nope\n\
             [Initial SP DLLs]\nDLL = b.dll, Skipped, HIGHEST\n",
            false,
        );
        let p: Vec<Priority> = got.rows.iter().map(|r| r.priority).collect();
        assert_eq!(p, [Priority::Highest, Priority::Lowest, Priority::Lowest]);
        assert!(got.path.is_empty());
    }
}
