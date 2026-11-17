//! `cargo xtask thn-decompile`: Lua 3.2 source from the precompiled
//! `.thn` scene scripts (design/facts.md F6).
//!
//! The shipped scripts are compiled chunks without debug information, so
//! comments, layout and the spelling of numerals are gone; numbers come
//! back in the text the compiler stored. Every result is compiled again
//! with [`compile::compile`] and must equal the input byte for byte; a
//! file that does not is reported and not written.
//!
//! The output is made from the game data in `outdir/` and must not be
//! committed (AGENTS.md, "Tests and test data").

mod chunk;
mod compile;
mod decompile;
mod number;

use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use chunk::{Chunk, HEADER};

pub const USAGE: &str = "  thn-decompile <file> [<out file>]
  thn-decompile <folder> <out folder>
                 decompile precompiled Lua 3.2 .thn scene scripts to
                 source; a folder is searched for .thn files and mirrored
                 into <out folder>. Each result is recompiled and must
                 equal its input byte for byte, or it is not written
";

/// A decompiled script.
pub struct Decompiled {
    pub source: String,
    /// Number constants whose stored text would not compile back to
    /// itself, with the numeral written instead (stored, written).
    pub respelled: Vec<(String, String)>,
}

/// Decompiles a chunk and proves the source compiles back to it.
pub fn decompile_verified(bytes: &[u8]) -> Result<Decompiled, String> {
    let chunk = Chunk::read(bytes).map_err(|e| e.to_string())?;
    let source = decompile::decompile(&chunk)?;
    let again = compile::compile(source.as_bytes())
        .map_err(|e| format!("the decompiled source does not compile: {e}"))?
        .write();
    if again != bytes {
        let at = again
            .iter()
            .zip(bytes)
            .position(|(a, b)| a != b)
            .unwrap_or(again.len().min(bytes.len()));
        return Err(format!(
            "the decompiled source compiles to different bytes (first difference at byte {at})"
        ));
    }
    let respelled = chunk
        .constants
        .iter()
        .filter_map(|c| match c {
            chunk::Constant::Number(stored) => number::source_numeral(stored)
                .filter(|written| written != stored)
                .map(|written| (stored.clone(), written)),
            chunk::Constant::String(_) => None,
        })
        .collect();
    Ok(Decompiled { source, respelled })
}

fn report_respelled(file: &Path, decompiled: &Decompiled) {
    for (stored, written) in &decompiled.respelled {
        eprintln!(
            "thn-decompile: {}: stored number {stored} written as {written}, which compiles \
             to that text; the engine reads the stored text one unit in the last place apart",
            file.display()
        );
    }
}

/// `.thn` files under `dir`, sorted, as paths relative to it.
fn thn_files(dir: &Path, relative: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<_> = fs::read_dir(dir)
        .map(|d| d.flatten().collect())
        .unwrap_or_default();
    entries.sort_by_key(fs::DirEntry::file_name);
    for entry in entries {
        let path = entry.path();
        let name = relative.join(entry.file_name());
        if path.is_dir() {
            thn_files(&path, &name, out);
        } else if path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("thn"))
        {
            out.push(name);
        }
    }
}

fn read(path: &Path) -> Result<Vec<u8>, String> {
    fs::read(path).map_err(|e| format!("{}: {e}", path.display()))
}

fn write(path: &Path, text: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    fs::write(path, text).map_err(|e| format!("{}: {e}", path.display()))
}

fn run_file(input: &Path, output: Option<&Path>) -> ExitCode {
    let result = read(input).and_then(|bytes| {
        decompile_verified(&bytes).map_err(|e| format!("{}: {e}", input.display()))
    });
    let written = result.and_then(|decompiled| {
        report_respelled(input, &decompiled);
        match output {
            Some(out) => write(out, &decompiled.source),
            None => std::io::stdout()
                .write_all(decompiled.source.as_bytes())
                .map_err(|e| e.to_string()),
        }
    });
    match written {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("thn-decompile: {e}");
            ExitCode::FAILURE
        }
    }
}

fn run_folder(input: &Path, output: &Path) -> ExitCode {
    let mut files = Vec::new();
    thn_files(input, Path::new(""), &mut files);
    let (mut done, mut text, mut failed) = (0, 0, 0);
    for relative in &files {
        let source = input.join(relative);
        let result = read(&source).and_then(|bytes| {
            if !bytes.starts_with(HEADER) {
                text += 1;
                return Ok(());
            }
            let decompiled =
                decompile_verified(&bytes).map_err(|e| format!("{}: {e}", relative.display()))?;
            report_respelled(relative, &decompiled);
            write(&output.join(relative), &decompiled.source)?;
            done += 1;
            Ok(())
        });
        if let Err(e) = result {
            eprintln!("thn-decompile: {e}");
            failed += 1;
        }
    }
    println!(
        "thn-decompile: {done} decompiled and verified, {text} already source (skipped), \
         {failed} failed, into {}",
        output.display()
    );
    if failed == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

pub fn run(args: &[String]) -> ExitCode {
    let input = args.first().map(Path::new);
    match (input, args.get(1).map(Path::new), args.len()) {
        (Some(input), Some(output), 2) if input.is_dir() => run_folder(input, output),
        (Some(input), output, 1 | 2) if input.is_file() => run_file(input, output),
        (Some(input), _, 1 | 2) if !input.exists() => {
            eprintln!("thn-decompile: {} does not exist", input.display());
            ExitCode::FAILURE
        }
        _ => {
            eprintln!("usage:\n{USAGE}");
            ExitCode::from(2)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::chunk::{Chunk, Constant, op};
    use super::compile::compile;
    use super::decompile::decompile;
    use super::*;

    fn round_trip(src: &str) -> (Chunk, String) {
        let chunk = compile(src.as_bytes()).unwrap_or_else(|e| panic!("{e}\n{src}"));
        let bytes = chunk.write();
        assert_eq!(Chunk::read(&bytes).as_ref(), Ok(&chunk));
        let text = decompile_verified(&bytes).unwrap_or_else(|e| panic!("{e}\n{src}"));
        (chunk, text.source)
    }

    /// Code worked out by hand from `lparser.c` for a small script.
    #[test]
    fn compiles_as_the_reference_parser_does() {
        let chunk = compile(b"x = 1\ny = { -2, 0.5; a = Z + 3 }\n").unwrap();
        assert_eq!(
            chunk.constants,
            [
                Constant::String(b"x".to_vec()),
                Constant::String(b"y".to_vec()),
                Constant::Number("0.5".into()),
                Constant::String(b"a".to_vec()),
                Constant::String(b"Z".to_vec()),
            ]
        );
        #[rustfmt::skip]
        let code = [
            4, 0,                           // stack size 4, no parameters
            op::PUSHNUMBER, 1,
            op::SETGLOBAL, 0,               // x = 1
            op::CREATEARRAY, 3,             // { ... } with 3 items in all
            op::PUSHNUMBERNEG, 2,
            op::PUSHCONSTANT, 2,
            op::SETLIST, 0, 2,              // t[1], t[2]
            op::PUSHCONSTANT, 3,            // "a"
            op::GETGLOBAL, 4,
            op::PUSHNUMBER, 3,
            op::ADDOP,
            op::SETMAP, 0,                  // t.a
            op::SETGLOBAL, 1,               // y = t
            op::ENDCODE,
        ];
        assert_eq!(chunk.code, code);
        let mut header = HEADER.to_vec();
        header.extend([0, 0, 0, 0, 0, 0, 0, 0, 1, 0]);
        assert!(chunk.write().starts_with(&header));
    }

    #[test]
    fn wide_arguments_and_flushes() {
        // 300 items: CREATEARRAYW, SETLIST flushes at 64, constants past
        // index 255 (PUSHCONSTANTW), and integers past a byte (PUSHNUMBERW).
        let items: Vec<String> = (0..300).map(|i| format!("{i}.5")).collect();
        let fields: Vec<String> = (0..70).map(|i| format!("f{i} = {}", i * 1000)).collect();
        let src = format!(
            "list = {{ {} }}\nrec = {{ {} }}\n",
            items.join(", "),
            fields.join(", ")
        );
        let (chunk, text) = round_trip(&src);
        assert_eq!(chunk.code[2..5], [op::CREATEARRAYW, 1, 44]);
        assert!(chunk.code.windows(3).any(|w| w == [op::SETLIST, 3, 64]));
        assert!(chunk.code.windows(3).any(|w| w == [op::SETLIST, 4, 44]));
        assert!(chunk.code.windows(2).any(|w| w == [op::SETMAP, 31]));
        assert!(chunk.code.contains(&op::PUSHCONSTANTW));
        assert!(chunk.code.contains(&op::PUSHNUMBERW));
        assert!(text.contains("f69 = 69000"));
    }

    #[test]
    fn numbers_are_stored_once() {
        let names: Vec<String> = (0..25).map(|i| format!("n{i} = 1.5")).collect();
        let (chunk, _) = round_trip(&names.join("\n"));
        let numbers = chunk
            .constants
            .iter()
            .filter(|c| matches!(c, Constant::Number(_)))
            .count();
        assert_eq!(numbers, 1, "1.5 is reused from 25 constants back");
    }

    #[test]
    fn source_constructs_round_trip() {
        let src = r#"
            -- comments and layout are not kept
            duration = 29.875;
            entities = {
              { entity_name = "a\"b\\c\n\001\255", type = SCENE, flags = A + B + (C + D) },
              { [1] = "x", ["end"] = 2, ["two words"] = -0.003906, [K] = 9e-006 },
              { 1, 2; x = 3 },
              { y = 1; 4, 5 },
              {},
              { -65535, 65536, -70000.25, 1e+020 },
            }
        "#;
        let (_, text) = round_trip(src);
        assert!(text.contains(r#""a\"b\\c\n\001\255""#), "{text}");
        assert!(text.contains("A + B + (C + D)"), "{text}");
        assert!(text.contains(r#"["end"] = 2"#), "{text}");
        assert!(
            text.contains("{ y = 1; 4, 5 }") || text.contains(";\n"),
            "{text}"
        );
    }

    #[test]
    fn rejects_what_the_subset_does_not_cover() {
        for src in [
            "x = f(1)",
            "x = a.b",
            "local x = 1",
            "x = -y",
            "x = 1 - 2",
            "x, y = 1, 2",
            "x = [[long]]",
            "$debug\nx = 1",
            "x = 1 == 2",
        ] {
            assert!(compile(src.as_bytes()).is_err(), "{src}");
        }
    }

    #[test]
    fn rejects_other_chunks() {
        assert!(decompile_verified(b"duration = 1\n").is_err());
        let mut bytes = compile(b"x = 1").unwrap().write();
        bytes.push(0);
        assert!(decompile_verified(&bytes).is_err());
        // A chunk the compiler would not make from any source: the stack
        // size byte is wrong.
        let mut chunk = compile(b"x = 1").unwrap();
        chunk.code[0] = 9;
        let text = decompile(&chunk).unwrap();
        assert_eq!(text, "x = 1\n");
        assert!(decompile_verified(&chunk.write()).is_err());
    }

    /// Every `.thn` file of the game data committed in `outdir/data`
    /// (design/decisions.md O15) decompiles, and the source compiles back
    /// to its bytes. Nothing is written.
    #[test]
    fn original_data_round_trips() {
        let root = crate::workspace_root().join("outdir/data");
        let mut files = Vec::new();
        thn_files(&root, Path::new(""), &mut files);
        assert!(!files.is_empty(), "no .thn files under {}", root.display());
        let mut failures = Vec::new();
        let mut respelled = 0;
        let mut compiled = 0;
        for relative in &files {
            let bytes = fs::read(root.join(relative)).expect("readable .thn file");
            if !bytes.starts_with(HEADER) {
                continue;
            }
            compiled += 1;
            match decompile_verified(&bytes) {
                Ok(d) => respelled += d.respelled.len(),
                Err(e) => failures.push(format!("{}: {e}", relative.display())),
            }
        }
        println!(
            "{compiled} compiled .thn files of {}, {} failed, {respelled} numbers respelled",
            files.len(),
            failures.len()
        );
        assert!(
            failures.is_empty(),
            "{} of {compiled} did not round-trip:\n{}",
            failures.len(),
            failures.join("\n")
        );
    }
}
