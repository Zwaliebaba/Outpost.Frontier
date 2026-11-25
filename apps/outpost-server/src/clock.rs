//! The machine's local time for the server's saves (`GetLocalTime`,
//! decision O42 as resolved by the owner on 2026-11-25): `jiff` reads the
//! system's time zone (the registry's zone on Windows with the bundled
//! database, `/etc/localtime` and the zoneinfo files elsewhere) with safe
//! calls that hold in a process with threads, so the executable stays free
//! of unsafe code (T14).

use game_logic::saves::{LocalClock, SystemTime};
use jiff::ZonedDateTime;

/// `GetLocalTime`'s fields of a zoned time: the civil date and time in its
/// zone, the day of the week from Sunday as 0, the milliseconds.
#[must_use]
pub fn system_time(zoned: &ZonedDateTime) -> SystemTime {
    let field = |v: i64| u16::try_from(v).unwrap_or(0);
    SystemTime {
        year: field(i64::from(zoned.year())),
        month: field(i64::from(zoned.month())),
        day_of_week: field(i64::from(zoned.weekday().to_sunday_zero_offset())),
        day: field(i64::from(zoned.day())),
        hour: field(i64::from(zoned.hour())),
        minute: field(i64::from(zoned.minute())),
        second: field(i64::from(zoned.second())),
        milliseconds: field(i64::from(zoned.millisecond())),
    }
}

/// The local clock of the host: the time now in the system's time zone.
/// A machine whose zone cannot be read answers in UTC, as `jiff` does.
#[derive(Clone, Copy, Debug, Default)]
pub struct HostClock;

impl LocalClock for HostClock {
    fn local_time(&mut self) -> SystemTime {
        system_time(&ZonedDateTime::now())
    }
}

#[cfg(test)]
mod tests {
    use jiff::Timestamp;
    use jiff::tz::{Offset, TimeZone};

    use super::*;

    #[test]
    fn the_fields_are_the_zones_civil_time() {
        // 1970-01-01T00:00:00.250Z, a Thursday, two hours east of UTC.
        let at = Timestamp::from_millisecond(250).unwrap();
        let zone = TimeZone::fixed(Offset::from_hours(2).unwrap());
        let t = system_time(&at.to_zoned_datetime(zone));
        assert_eq!(
            (
                t.year,
                t.month,
                t.day,
                t.hour,
                t.minute,
                t.second,
                t.milliseconds
            ),
            (1970, 1, 1, 2, 0, 0, 250)
        );
        assert_eq!(t.day_of_week, 4);
        // West of UTC the date goes back a day: Wednesday 31 December.
        let zone = TimeZone::fixed(Offset::from_hours(-5).unwrap());
        let t = system_time(&at.to_zoned_datetime(zone));
        assert_eq!((t.year, t.month, t.day, t.hour), (1969, 12, 31, 19));
        assert_eq!(t.day_of_week, 3);
    }

    #[test]
    fn the_host_clock_matches_the_utc_clock_up_to_the_offset() {
        // Read from a second thread too: the offset must be had in a
        // process with threads.
        let local = std::thread::spawn(|| HostClock.local_time())
            .join()
            .unwrap();
        let now = ZonedDateTime::now();
        let offset = i64::from(now.offset().seconds());
        let utc = game_logic::saves::UtcClock.local_time();
        let seconds = |t: &SystemTime| {
            let (h, l) = t.file_time();
            ((u64::from(h) << 32 | u64::from(l)) / 10_000_000) as i64
        };
        let difference = seconds(&local) - seconds(&utc) - offset;
        assert!(difference.abs() <= 2, "{local:?} against {utc:?}");
    }
}
