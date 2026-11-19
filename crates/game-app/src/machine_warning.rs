//! The low-machine warning of `InitializeEngine` (`Client/engine_init.cpp`:
//! `BuildLowMachineWarning` and the dialog `0x82`; work package P4.1).
//!
//! Under 120 MiB of physical memory the warning says so, unless
//! `PerfOptions.ini` skips the machine warnings; else under 400 MiB of
//! free virtual memory it says that, skipped or not. Unless skipped, the
//! renderer's verdict on the video card (device ability `0xf`: none, an
//! unrecognised card, an old driver, an unsupported card) adds a line.
//! Each line is a string of the text catalogs followed by an empty line.
//! A warning with any text is shown in a dialog with Yes and No; No ends
//! the start-up, Yes goes on and sets the skip flag from the dialog's
//! "perform this check every time" box.
//!
//! The port's renderer has no verdict on the card (the reference's GPU
//! quirk database is removed, decision T23), so that line never appears;
//! the dialog is the system's message box, without the box (divergence
//! D534).

use neuron_core::catalog::TextCatalogs;

/// Below this much physical memory (MiB) the warning shows.
pub const MIN_PHYSICAL_MIB: u64 = 0x78;

/// Below this much free virtual memory (MiB) the warning shows.
pub const MIN_VIRTUAL_MIB: u64 = 0x190;

/// "... does not have the minimum memory ...".
pub const IDS_LOW_MEMORY: u32 = 0xcb;
/// "... low on virtual memory ...".
pub const IDS_LOW_VIRTUAL: u32 = 0xd6;
/// The video card's lines by the renderer's verdict 1 to 3.
pub const IDS_DEVICE: [u32; 4] = [0, 0xcd, 0xce, 0xcf];
/// The dialog's caption, "OutpostFrontier Critical Warning".
pub const IDS_CAPTION: u32 = 0xd2;
/// The check box, "Perform this check every time the application runs?".
pub const IDS_CHECK: u32 = 0x86;
/// The question, "Would you like to continue?".
pub const IDS_QUESTION: u32 = 0x87;
/// The buttons, "YES" and "NO".
pub const IDS_YES: u32 = 0x4dc;
pub const IDS_NO: u32 = 0x4dd;

/// The size of the buffers the C++ reads each string into, in characters
/// with the terminator.
const LINE_CHARS: usize = 0x100;

/// The memory figures in MiB: the physical memory installed and the
/// virtual address space free to the process.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MemoryMib {
    pub total_physical: u64,
    pub available_virtual: u64,
}

/// `BuildLowMachineWarning`'s lines (text IDs) in order. `memory` is
/// `None` when the system cannot say (no memory line then);
/// `device_level` is the renderer's verdict on the card, 0 for none (a
/// verdict past 3 adds no line, where the C++ reads past its table).
#[must_use]
pub fn warning_lines(memory: Option<MemoryMib>, skip: bool, device_level: u32) -> Vec<u32> {
    let mut lines = Vec::new();
    if let Some(memory) = memory {
        if memory.total_physical < MIN_PHYSICAL_MIB {
            if skip {
                return lines;
            }
            lines.push(IDS_LOW_MEMORY);
        } else if memory.available_virtual < MIN_VIRTUAL_MIB {
            lines.push(IDS_LOW_VIRTUAL);
        }
    }
    if skip {
        return lines;
    }
    if let Some(&ids) = IDS_DEVICE.get(device_level as usize)
        && ids != 0
    {
        lines.push(ids);
    }
    lines
}

/// One string as the dialog reads it (`GetIdsStringWWithRawFallback` into
/// 256 characters; nothing when it is missing).
#[must_use]
pub fn text(catalogs: &TextCatalogs, ids: u32) -> Vec<u16> {
    let mut buffer = [0u16; LINE_CHARS];
    let count = catalogs.string_with_card_fallback_into(ids, &mut buffer);
    let end = buffer[..count]
        .iter()
        .position(|&u| u == 0)
        .unwrap_or(count);
    buffer[..end].to_vec()
}

/// The warning's text: each line's string, then two line feeds.
#[must_use]
pub fn warning_text(lines: &[u32], catalogs: &TextCatalogs) -> Vec<u16> {
    let mut out = Vec::new();
    for &ids in lines {
        out.extend(text(catalogs, ids));
        out.extend([u16::from(b'\n'), u16::from(b'\n')]);
    }
    out
}

/// The dialog's answer: Yes goes on and sets the skip flag when the check
/// box was cleared (`SetSkipMachineWarnings(Checked == 0)`); No stops the
/// start-up. Returns whether the start-up goes on.
pub fn answer(yes: bool, checked: bool, skip: &mut bool) -> bool {
    if yes {
        *skip = !checked;
    }
    yes
}

#[cfg(test)]
mod tests {
    use super::*;

    fn memory(total_physical: u64, available_virtual: u64) -> Option<MemoryMib> {
        Some(MemoryMib {
            total_physical,
            available_virtual,
        })
    }

    #[test]
    fn the_lines_follow_the_reference_order_and_skip_rules() {
        assert!(warning_lines(memory(16_384, 1 << 20), false, 0).is_empty());
        assert_eq!(warning_lines(memory(64, 100), false, 0), [IDS_LOW_MEMORY]);
        assert!(warning_lines(memory(64, 100), true, 2).is_empty());
        // The virtual-memory line shows even when skipped.
        assert_eq!(warning_lines(memory(512, 399), true, 2), [IDS_LOW_VIRTUAL]);
        assert_eq!(
            warning_lines(memory(512, 399), false, 2),
            [IDS_LOW_VIRTUAL, 0xce]
        );
        assert_eq!(warning_lines(None, false, 3), [0xcf]);
        assert!(warning_lines(None, false, 4).is_empty());
    }

    #[test]
    fn missing_strings_leave_their_line_feeds_and_yes_sets_the_flag() {
        let text = warning_text(&[IDS_LOW_MEMORY, IDS_LOW_VIRTUAL], &TextCatalogs::new());
        assert_eq!(text, [10, 10, 10, 10]);
        let mut skip = false;
        assert!(answer(true, false, &mut skip));
        assert!(skip);
        assert!(!answer(false, true, &mut skip));
        assert!(skip);
        assert!(answer(true, true, &mut skip));
        assert!(!skip);
    }
}
