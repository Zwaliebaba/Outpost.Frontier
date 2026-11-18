//! The shipped sounds (`outdir/data`, decision O15) through the decoder of
//! work package P4.10: every WAV file of the data tree, every WAV a sound
//! table (`DATA/AUDIO/*.ini`, `file = ...`) names, and every voice line
//! stored in the UTF archives decodes. Archetype decoding
//! ([`decode_archetype`]) and streaming ([`StreamSource`]) give the same
//! samples, the decoder accepts every frame, and no sound decodes to more samples than its `fact` chunk
//! allows. Nothing is compared with the oracle: MP3 decoders are not
//! bit-exact (divergence D271).

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use neuron_client::audio::decode::{DecodedSound, StreamSource, decode_archetype};
use neuron_client::audio::wav::{Loader, WAVE_FORMAT_MPEGLAYER3, WavFile};
use neuron_core::utf::{NodeRef, UtfArchive};

fn data_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data")
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            walk(&path, out);
        } else {
            out.push(path);
        }
    }
}

fn has_extension(path: &Path, ext: &str) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case(ext))
}

/// Streams the whole sound in blocks of the refill pass's sizes.
fn stream_all(bytes: &Arc<[u8]>) -> Vec<u8> {
    let mut source = StreamSource::open(bytes.clone(), false).expect("stream opens");
    let mut out = Vec::new();
    let sizes = [21_168u32, 7056, 1, 70_560];
    for size in sizes.iter().cycle() {
        let block = source.read(*size).expect("stream decodes");
        out.extend_from_slice(&block.pcm);
        if block.end {
            break;
        }
        assert!(
            !block.pcm.is_empty(),
            "a stream that is not at its end gives bytes"
        );
    }
    out
}

/// Decodes one sound and checks it; returns an error text on failure.
fn check(name: &str, bytes: Arc<[u8]>, stream: bool) -> Result<(), String> {
    let decoded: DecodedSound =
        decode_archetype(&bytes).map_err(|e| format!("{name}: archetype: {e}"))?;
    if decoded.samples.is_empty() || decoded.format.block_align == 0 {
        return Err(format!("{name}: no samples"));
    }
    if decoded.rejected_frames != 0 {
        return Err(format!(
            "{name}: the decoder rejected {} frames",
            decoded.rejected_frames
        ));
    }
    let wav = WavFile::parse(&bytes, Loader::Archetype).map_err(|e| format!("{name}: {e}"))?;
    if wav.format.tag == WAVE_FORMAT_MPEGLAYER3
        && let Some(fact) = wav.fact_samples
    {
        // The count caps the output; a few files count the trimmed samples
        // too, the music counts both channels, so it is only an upper bound.
        let cap = fact as usize * usize::from(decoded.format.block_align);
        if decoded.samples.len() > cap {
            return Err(format!(
                "{name}: {} bytes decoded, more than the fact chunk's {cap}",
                decoded.samples.len()
            ));
        }
    }
    if stream {
        let streamed = stream_all(&bytes);
        if streamed != decoded.samples {
            return Err(format!(
                "{name}: streamed {} bytes differ from the {} decoded",
                streamed.len(),
                decoded.samples.len()
            ));
        }
    }
    Ok(())
}

/// Runs `check` over `items` on every core.
fn check_all(items: Vec<(String, Arc<[u8]>, bool)>) -> Vec<String> {
    let next = AtomicUsize::new(0);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get());
    let failures = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|scope| {
        for _ in 0..threads {
            scope.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some((name, bytes, stream)) = items.get(i) else {
                        break;
                    };
                    if let Err(e) = check(name, bytes.clone(), *stream) {
                        failures.lock().expect("lock").push(e);
                    }
                }
            });
        }
    });
    failures.into_inner().expect("lock")
}

#[test]
fn every_wav_file_decodes_and_streams() {
    let root = data_root();
    let mut paths = Vec::new();
    walk(&root, &mut paths);
    paths.retain(|p| has_extension(p, "wav"));
    assert!(!paths.is_empty(), "outdir/data has no WAV files");
    let items: Vec<_> = paths
        .iter()
        .map(|p| {
            let bytes: Arc<[u8]> = fs::read(p).expect("read").into();
            let name = p.strip_prefix(&root).unwrap_or(p).display().to_string();
            (name, bytes, true)
        })
        .collect();
    let count = items.len();
    let failures = check_all(items);
    assert!(
        failures.is_empty(),
        "{} of {count} failed:\n{}",
        failures.len(),
        failures.join("\n")
    );
    println!("{count} WAV files decoded and streamed");
}

#[test]
fn every_sound_table_entry_names_a_file() {
    let root = data_root();
    let mut paths = Vec::new();
    walk(&root, &mut paths);
    let by_name: HashMap<String, &PathBuf> = paths
        .iter()
        .map(|p| {
            let rel = p.strip_prefix(&root).unwrap_or(p);
            (
                rel.to_string_lossy()
                    .replace('\\', "/")
                    .to_ascii_lowercase(),
                p,
            )
        })
        .collect();
    let mut tables = Vec::new();
    walk(&root.join("AUDIO"), &mut tables);
    tables.retain(|p| has_extension(p, "ini"));
    let mut named = 0;
    let mut missing = Vec::new();
    for table in &tables {
        let text = fs::read(table).expect("read");
        for line in String::from_utf8_lossy(&text).lines() {
            let Some((key, value)) = line.split_once('=') else {
                continue;
            };
            if !key.trim().eq_ignore_ascii_case("file") {
                continue;
            }
            let value = value.trim().split(',').next().unwrap_or("").trim();
            if !value.to_ascii_lowercase().ends_with(".wav") {
                continue;
            }
            named += 1;
            let rel = value.replace('\\', "/").to_ascii_lowercase();
            let rel = rel.strip_prefix("data/").unwrap_or(&rel).to_string();
            if !by_name.contains_key(&rel) {
                missing.push(format!("{}: {value}", table.display()));
            }
        }
    }
    assert!(named > 0, "no sound table names a WAV file");
    // Every named file exists, so the decode test above covers it.
    assert!(
        missing.is_empty(),
        "missing sound files:\n{}",
        missing.join("\n")
    );
    println!("{named} sound table entries name existing WAV files");
}

fn utf_sounds(archive: &UtfArchive, node: NodeRef, path: &str, out: &mut Vec<(String, Vec<u8>)>) {
    for child in archive.children(node) {
        let Ok(child) = child else { continue };
        let name = archive
            .name(child)
            .map(|n| String::from_utf8_lossy(n).into_owned())
            .unwrap_or_default();
        let path = format!("{path}/{name}");
        if archive.is_dir(child).unwrap_or(false) {
            utf_sounds(archive, child, &path, out);
        } else if let Ok(data) = archive.data(child)
            && data.starts_with(b"RIFF")
        {
            out.push((path, data.to_vec()));
        }
    }
}

#[test]
fn every_voice_line_in_the_archives_decodes() {
    let root = data_root();
    let mut paths = Vec::new();
    walk(&root, &mut paths);
    paths.retain(|p| has_extension(p, "utf"));
    let mut items = Vec::new();
    for path in &paths {
        let Ok(archive) = UtfArchive::parse(fs::read(path).expect("read")) else {
            continue;
        };
        let mut sounds = Vec::new();
        let name = path
            .strip_prefix(&root)
            .unwrap_or(path)
            .display()
            .to_string();
        utf_sounds(&archive, archive.root(), &name, &mut sounds);
        items.extend(
            sounds
                .into_iter()
                .map(|(n, bytes)| (n, Arc::<[u8]>::from(bytes), false)),
        );
    }
    assert!(!items.is_empty(), "no voice lines in outdir/data");
    let count = items.len();
    let failures = check_all(items);
    assert!(
        failures.is_empty(),
        "{} of {count} failed:\n{}",
        failures.len(),
        failures.join("\n")
    );
    println!("{count} voice lines decoded");
}
