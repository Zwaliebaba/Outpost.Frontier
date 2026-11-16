//! Comment removal for the policy scan. Comments become spaces so line
//! numbers stay right; string literals are kept, because a library name in
//! a string is as much a dependency as an identifier.

/// Rust source with every comment blanked out.
pub fn strip_rust_comments(src: &str) -> String {
    let c: Vec<char> = src.chars().collect();
    let mut out = String::with_capacity(src.len());
    let mut i = 0;
    while i < c.len() {
        let ch = c[i];
        let next = c.get(i + 1).copied();
        if ch == '/' && next == Some('/') {
            while i < c.len() && c[i] != '\n' {
                out.push(' ');
                i += 1;
            }
        } else if ch == '/' && next == Some('*') {
            let mut depth = 0usize;
            while i < c.len() {
                if c[i] == '/' && c.get(i + 1) == Some(&'*') {
                    depth += 1;
                    out.push_str("  ");
                    i += 2;
                } else if c[i] == '*' && c.get(i + 1) == Some(&'/') {
                    depth -= 1;
                    out.push_str("  ");
                    i += 2;
                    if depth == 0 {
                        break;
                    }
                } else {
                    out.push(blank(c[i]));
                    i += 1;
                }
            }
        } else if let Some(hashes) = raw_string_start(&c, i) {
            let start = i;
            i += hashes + 2;
            while i < c.len() {
                if c[i] == '"' && (0..hashes).all(|k| c.get(i + 1 + k) == Some(&'#')) {
                    i += hashes + 1;
                    break;
                }
                i += 1;
            }
            out.extend(&c[start..i.min(c.len())]);
        } else if ch == '"' {
            let start = i;
            i = quoted_end(&c, i + 1, '"', true, true);
            out.extend(&c[start..i]);
        } else if ch == '\'' && next == Some('\\') {
            // Escaped character literal: '\n', '\'', '\u{..}'.
            let start = i;
            i += 3;
            while i < c.len() && c[i] != '\'' {
                i += 1;
            }
            i = (i + 1).min(c.len());
            out.extend(&c[start..i]);
        } else if ch == '\'' && c.get(i + 2) == Some(&'\'') {
            // One-character literal such as '"'; anything else is a lifetime.
            out.extend(&c[i..i + 3]);
            i += 3;
        } else {
            out.push(ch);
            i += 1;
        }
    }
    out
}

/// TOML source with every comment blanked out.
pub fn strip_toml_comments(src: &str) -> String {
    let c: Vec<char> = src.chars().collect();
    let mut out = String::with_capacity(src.len());
    let mut i = 0;
    while i < c.len() {
        let ch = c[i];
        if (ch == '"' || ch == '\'') && c.get(i + 1) == Some(&ch) && c.get(i + 2) == Some(&ch) {
            let start = i;
            i += 3;
            while i < c.len()
                && !(c[i] == ch && c.get(i + 1) == Some(&ch) && c.get(i + 2) == Some(&ch))
            {
                i += if ch == '"' && c[i] == '\\' { 2 } else { 1 };
            }
            i = (i + 3).min(c.len());
            out.extend(&c[start..i]);
        } else if ch == '"' || ch == '\'' {
            let start = i;
            i = quoted_end(&c, i + 1, ch, ch == '"', false);
            out.extend(&c[start..i]);
        } else if ch == '#' {
            while i < c.len() && c[i] != '\n' {
                out.push(' ');
                i += 1;
            }
        } else {
            out.push(ch);
            i += 1;
        }
    }
    out
}

/// Index just past the closing `quote`, starting inside the literal.
fn quoted_end(c: &[char], mut i: usize, quote: char, escapes: bool, multiline: bool) -> usize {
    while i < c.len() {
        if escapes && c[i] == '\\' {
            i += 2;
            continue;
        }
        if c[i] == quote {
            return i + 1;
        }
        if !multiline && c[i] == '\n' {
            return i;
        }
        i += 1;
    }
    c.len()
}

/// Number of `#` if a raw string (`r"`, `r#"`, `br"`) starts at `i`.
fn raw_string_start(c: &[char], i: usize) -> Option<usize> {
    if c[i] != 'r' {
        return None;
    }
    let prefix_ok = match i.checked_sub(1).map(|p| c[p]) {
        None => true,
        Some('b') => i < 2 || !is_ident(c[i - 2]),
        Some(p) => !is_ident(p),
    };
    if !prefix_ok {
        return None;
    }
    let mut j = i + 1;
    while c.get(j) == Some(&'#') {
        j += 1;
    }
    (c.get(j) == Some(&'"')).then_some(j - i - 1)
}

pub fn is_ident(ch: char) -> bool {
    ch.is_alphanumeric() || ch == '_'
}

fn blank(ch: char) -> char {
    if ch == '\n' { '\n' } else { ' ' }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn line_and_doc_comments_are_removed() {
        let out = strip_rust_comments("let a = 1; // x\n/// y\nlet b = 2;");
        assert_eq!(out.lines().count(), 3);
        assert!(!out.contains('x') && !out.contains('y'));
        assert!(out.contains("let b = 2;"));
    }

    #[test]
    fn nested_block_comments_are_removed() {
        let src = "a /* one /* two */ still */ b";
        let out = strip_rust_comments(src);
        assert_eq!(out.len(), src.len());
        assert_eq!(out.split_whitespace().collect::<Vec<_>>(), ["a", "b"]);
    }

    #[test]
    fn strings_are_kept_and_slashes_in_them_are_not_comments() {
        let out = strip_rust_comments("let u = \"https://x\"; // gone");
        assert!(out.contains("\"https://x\""));
        assert!(!out.contains("gone"));
    }

    #[test]
    fn raw_strings_are_kept() {
        let out = strip_rust_comments("let s = r#\"a \" // b\"#; // gone");
        assert!(out.contains("r#\"a \" // b\"#"));
        assert!(!out.contains("gone"));
    }

    #[test]
    fn quote_character_literals_do_not_open_strings() {
        let out = strip_rust_comments("let q = '\"'; let e = '\\''; // gone");
        assert!(!out.contains("gone"));
    }

    #[test]
    fn lifetimes_do_not_open_character_literals() {
        let out = strip_rust_comments("fn f<'a>(x: &'a str) {} // gone");
        assert!(out.contains("fn f<'a>(x: &'a str) {}"));
        assert!(!out.contains("gone"));
    }

    #[test]
    fn toml_comments_are_removed_but_hashes_in_strings_stay() {
        let out = strip_toml_comments("a = \"#1\" # gone\nb = '''x # y''' # gone");
        assert!(out.contains("\"#1\"") && out.contains("'''x # y'''"));
        assert!(!out.contains("gone"));
    }
}
