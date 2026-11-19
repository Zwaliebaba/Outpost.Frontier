//! The faces that stand in for ones a Windows machine does not have
//! (decision O29).
//!
//! `fonts.ini` and `rich_fonts.ini` name `Agency FB` and `Arial Unicode MS`
//! (Microsoft Office faces), `Vibrocentric` (shipped with nothing) and
//! `FixedSys` (a raster face DirectWrite cannot draw). The C++ client drew
//! whatever GDI's font mapper picked for them. The port keeps the names in
//! the data and, where neither the game's own `DATA/FONTS` nor the system
//! has a face, asks for the face this table gives; Arial for any other.

/// The face of last resort.
pub const LAST_RESORT: &[u8] = b"Arial";

/// The fixed substitutes, by face name (compared ignoring ASCII case).
pub const TABLE: [(&[u8], &[u8]); 4] = [
    (b"Agency FB", b"Bahnschrift SemiCondensed"),
    (b"Arial Unicode MS", b"Arial"),
    (b"Vibrocentric", b"Arial"),
    (b"FixedSys", b"Consolas"),
];

/// The face to ask for in place of `face`, which the font sources lack.
#[must_use]
pub fn substitute(face: &[u8]) -> &'static [u8] {
    TABLE
        .iter()
        .find(|(name, _)| name.eq_ignore_ascii_case(face))
        .map_or(LAST_RESORT, |&(_, sub)| sub)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_table_and_the_last_resort() {
        assert_eq!(substitute(b"Agency FB"), b"Bahnschrift SemiCondensed");
        assert_eq!(substitute(b"agency fb"), b"Bahnschrift SemiCondensed");
        assert_eq!(substitute(b"Arial Unicode MS"), b"Arial");
        assert_eq!(substitute(b"VIBROCENTRIC"), b"Arial");
        assert_eq!(substitute(b"FixedSys"), b"Consolas");
        assert_eq!(substitute(b"No Such Face"), b"Arial");
        assert_eq!(substitute(b""), b"Arial");
    }
}
