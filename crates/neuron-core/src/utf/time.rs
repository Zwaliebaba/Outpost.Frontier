//! File times: the DOS date and time words a full UTF record stores, and
//! the Windows `FILETIME` count they convert to.

use std::time::{SystemTime, UNIX_EPOCH};

/// `FILETIME` ticks (100 ns) per second.
const TICKS_PER_SECOND: u64 = 10_000_000;
/// Days from 1601-01-01 to 1970-01-01.
const DAYS_1601_TO_1970: i64 = 134_774;
/// `FILETIME` of 1970-01-01T00:00:00.
const UNIX_EPOCH_TICKS: u64 = 116_444_736_000_000_000;

fn is_leap(year: i64) -> bool {
    (year % 4 == 0 && year % 100 != 0) || year % 400 == 0
}

fn days_in_month(year: i64, month: u32) -> u32 {
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        _ if is_leap(year) => 29,
        _ => 28,
    }
}

/// Days from 1970-01-01 to the given civil date (proleptic Gregorian).
fn days_from_civil(year: i64, month: u32, day: u32) -> i64 {
    let year = if month <= 2 { year - 1 } else { year };
    let era = year.div_euclid(400);
    let year_of_era = year - era * 400;
    let month = i64::from(month);
    let day_of_year =
        (153 * (if month > 2 { month - 3 } else { month + 9 }) + 2) / 5 + i64::from(day) - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146_097 + day_of_era - 719_468
}

/// `DosDateTimeToFileTime(date, time)`: year 1980 + bits 9..15 of `date`,
/// month bits 5..8, day bits 0..4; hour bits 11..15 of `time`, minute bits
/// 5..10, two-second units bits 0..4.
///
/// `None` where Windows returns `FALSE`: a month outside 1..=12, a day
/// outside the month, an hour above 23, a minute above 59 or a second
/// above 59. The reference ignores that result, so the caller's `FILETIME`
/// keeps whatever it held before.
#[must_use]
pub fn dos_date_time_to_file_time(date: u16, time: u16) -> Option<u64> {
    let year = 1980 + i64::from(date >> 9);
    let month = u32::from((date >> 5) & 0x0F);
    let day = u32::from(date & 0x1F);
    let hour = u64::from(time >> 11);
    let minute = u64::from((time >> 5) & 0x3F);
    let second = u64::from(time & 0x1F) * 2;
    if !(1..=12).contains(&month)
        || day == 0
        || day > days_in_month(year, month)
        || hour > 23
        || minute > 59
        || second > 59
    {
        return None;
    }
    let days = u64::try_from(days_from_civil(year, month, day) + DAYS_1601_TO_1970).ok()?;
    let seconds = days * 86_400 + hour * 3_600 + minute * 60 + second;
    Some(seconds * TICKS_PER_SECOND)
}

/// [`dos_date_time_to_file_time`] of a packed record time: the low word is
/// the date and the high word the time, as `UtfPackedToFileTime` splits it.
#[must_use]
pub fn packed_to_file_time(packed: u32) -> Option<u64> {
    let [d0, d1, t0, t1] = packed.to_le_bytes();
    dos_date_time_to_file_time(u16::from_le_bytes([d0, d1]), u16::from_le_bytes([t0, t1]))
}

/// A host time as a `FILETIME` count; times before 1601 clamp to 0.
#[must_use]
pub fn system_time_to_file_time(time: SystemTime) -> u64 {
    match time.duration_since(UNIX_EPOCH) {
        Ok(after) => UNIX_EPOCH_TICKS
            .saturating_add(after.as_secs().saturating_mul(TICKS_PER_SECOND))
            .saturating_add(u64::from(after.subsec_nanos() / 100)),
        Err(before) => {
            let before = before.duration();
            let ticks = before
                .as_secs()
                .saturating_mul(TICKS_PER_SECOND)
                .saturating_add(u64::from(before.subsec_nanos().div_ceil(100)));
            UNIX_EPOCH_TICKS.saturating_sub(ticks)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dos_date(year: u16, month: u16, day: u16) -> u16 {
        ((year - 1980) << 9) | (month << 5) | day
    }

    fn dos_time(hour: u16, minute: u16, second: u16) -> u16 {
        (hour << 11) | (minute << 5) | (second / 2)
    }

    #[test]
    fn epoch_values() {
        // 1980-01-01 00:00:00 is 119600064000000000 ticks.
        assert_eq!(
            dos_date_time_to_file_time(dos_date(1980, 1, 1), 0),
            Some(119_600_064_000_000_000)
        );
        // 2000-02-29 12:34:56.
        assert_eq!(
            dos_date_time_to_file_time(dos_date(2000, 2, 29), dos_time(12, 34, 56)),
            Some(125_963_012_960_000_000)
        );
    }

    #[test]
    fn invalid_fields() {
        assert_eq!(dos_date_time_to_file_time(0, 0), None);
        assert_eq!(dos_date_time_to_file_time(dos_date(1981, 2, 29), 0), None);
        assert_eq!(dos_date_time_to_file_time(dos_date(1984, 13, 1), 0), None);
        assert_eq!(
            dos_date_time_to_file_time(dos_date(1984, 1, 1), dos_time(24, 0, 0)),
            None
        );
        assert_eq!(
            dos_date_time_to_file_time(dos_date(1984, 1, 1), 0x1F),
            None,
            "62 seconds"
        );
    }

    #[test]
    fn unix_epoch() {
        assert_eq!(system_time_to_file_time(UNIX_EPOCH), UNIX_EPOCH_TICKS);
    }
}
