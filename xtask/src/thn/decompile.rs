//! Turns the code of a [`Chunk`] back into Lua 3.2 source.
//!
//! Every shipped script is straight-line code that assigns globals from
//! table constructors: no jumps, calls, functions or locals. The decoder
//! runs the code on a stack of expressions instead of values, so each
//! instruction rebuilds the source construct that produced it.

use lua32_compiler::{Chunk, Constant, op, source_numeral};

/// An expression rebuilt from the code.
#[derive(Clone, Debug)]
pub enum Expr {
    /// A numeral, as it is written in the source: the stored text, or
    /// another spelling where that text would not compile back to itself
    /// ([`source_numeral`]).
    Number(String),
    String(Vec<u8>),
    /// A global variable.
    Name(String),
    Add(Box<Expr>, Box<Expr>),
    Table(Vec<Part>),
}

/// A run of a constructor: positional items or `key = value` fields.
#[derive(Clone, Debug)]
pub enum Part {
    List(Vec<Expr>),
    Record(Vec<(Expr, Expr)>),
}

/// One `name = expression` statement.
pub struct Assignment {
    pub name: String,
    pub value: Expr,
}

const KEYWORDS: [&str; 16] = [
    "and", "do", "else", "elseif", "end", "function", "if", "local", "nil", "not", "or", "repeat",
    "return", "then", "until", "while",
];

/// Whether the bytes can be written as a bare name.
pub fn is_name(s: &[u8]) -> bool {
    let Some((&first, rest)) = s.split_first() else {
        return false;
    };
    (first == b'_' || first.is_ascii_alphabetic())
        && rest.iter().all(|&b| b == b'_' || b.is_ascii_alphanumeric())
        && !KEYWORDS.iter().any(|k| k.as_bytes() == s)
}

fn constant(chunk: &Chunk, index: usize) -> Result<&Constant, String> {
    chunk
        .constants
        .get(index)
        .ok_or_else(|| format!("constant {index} does not exist"))
}

fn name(chunk: &Chunk, index: usize) -> Result<String, String> {
    match constant(chunk, index)? {
        Constant::String(s) if is_name(s) => Ok(String::from_utf8_lossy(s).into_owned()),
        other => Err(format!(
            "constant {index} is not a variable name: {other:?}"
        )),
    }
}

/// Decodes the code into its statements.
pub fn decode(chunk: &Chunk) -> Result<Vec<Assignment>, String> {
    let code = &chunk.code;
    if code[1] != 0 {
        return Err("the main function takes parameters".into());
    }
    let mut stack: Vec<Expr> = Vec::new();
    let mut out = Vec::new();
    let mut pc = 2;
    let mut high = 0usize; // LONGARG prefix
    let pop = |stack: &mut Vec<Expr>, n: usize, at: usize| -> Result<Vec<Expr>, String> {
        if stack.len() < n {
            return Err(format!("stack underflow at byte {at}"));
        }
        Ok(stack.split_off(stack.len() - n))
    };
    loop {
        let at = pc;
        let Some(&opcode) = code.get(pc) else {
            return Err("code runs past its end".into());
        };
        pc += 1;
        if opcode == op::ENDCODE {
            break;
        }
        if opcode == op::ADDOP {
            let mut xy = pop(&mut stack, 2, at)?;
            let y = xy.pop().expect("two items");
            let x = xy.pop().expect("two items");
            stack.push(Expr::Add(Box::new(x), Box::new(y)));
            continue;
        }
        // Every other supported opcode has an argument: a byte, or a word
        // for the variant one below it. LONGARG adds the high byte.
        let wide = matches!(
            opcode,
            op::PUSHNUMBERW
                | op::PUSHNUMBERNEGW
                | op::PUSHCONSTANTW
                | op::GETGLOBALW
                | op::CREATEARRAYW
                | op::SETGLOBALW
                | op::SETLISTW
                | op::LONGARGW
        );
        let base = if wide { opcode + 1 } else { opcode };
        let take = if wide { 2 } else { 1 };
        let Some(bytes) = code.get(pc..pc + take) else {
            return Err(format!("truncated instruction at byte {at}"));
        };
        pc += take;
        let low = bytes.iter().fold(0usize, |a, &b| (a << 8) | usize::from(b));
        let arg = (high << 16) | low;
        high = 0;
        match base {
            op::LONGARG => high = arg,
            op::PUSHNUMBER => stack.push(Expr::Number(arg.to_string())),
            op::PUSHNUMBERNEG => stack.push(Expr::Number(format!("-{arg}"))),
            op::PUSHCONSTANT => stack.push(match constant(chunk, arg)? {
                Constant::Number(text) => Expr::Number(
                    source_numeral(text)
                        .ok_or_else(|| format!("constant {arg}: no numeral compiles to {text}"))?,
                ),
                Constant::String(s) => Expr::String(s.clone()),
            }),
            op::GETGLOBAL => stack.push(Expr::Name(name(chunk, arg)?)),
            op::CREATEARRAY => stack.push(Expr::Table(Vec::new())),
            op::SETGLOBAL => {
                let value = pop(&mut stack, 1, at)?.pop().expect("one item");
                if !stack.is_empty() {
                    return Err(format!("assignment at byte {at} leaves values behind"));
                }
                out.push(Assignment {
                    name: name(chunk, arg)?,
                    value,
                });
            }
            op::SETLIST => {
                let Some(&n) = code.get(pc) else {
                    return Err(format!("truncated SETLIST at byte {at}"));
                };
                pc += 1;
                let items = pop(&mut stack, usize::from(n), at)?;
                push_part(&mut stack, at, Part::List(items))?;
            }
            op::SETMAP => {
                let mut kv = pop(&mut stack, 2 * (arg + 1), at)?.into_iter();
                let mut fields = Vec::with_capacity(arg + 1);
                while let (Some(k), Some(v)) = (kv.next(), kv.next()) {
                    fields.push((k, v));
                }
                push_part(&mut stack, at, Part::Record(fields))?;
            }
            _ => {
                return Err(format!(
                    "opcode {opcode} at byte {at} is outside the subset the scene scripts use"
                ));
            }
        }
    }
    if pc != code.len() || !stack.is_empty() {
        return Err("code does not end after its last statement".into());
    }
    Ok(out)
}

/// Adds a flushed run to the table below it, joining it to the previous
/// run of the same kind (a flush every 64 items or 32 fields).
fn push_part(stack: &mut [Expr], at: usize, part: Part) -> Result<(), String> {
    let Some(Expr::Table(parts)) = stack.last_mut() else {
        return Err(format!("table store at byte {at} has no table"));
    };
    match (parts.last_mut(), part) {
        (Some(Part::List(a)), Part::List(b)) => a.extend(b),
        (Some(Part::Record(a)), Part::Record(b)) => a.extend(b),
        (_, part) => parts.push(part),
    }
    Ok(())
}

fn quote(s: &[u8], out: &mut String) {
    out.push('"');
    for &b in s {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            b'\n' => out.push_str("\\n"),
            b' '..=b'~' => out.push(char::from(b)),
            // Three digits, so a following digit cannot join the escape.
            _ => out.push_str(&format!("\\{b:03}")),
        }
    }
    out.push('"');
}

const INDENT: &str = "  ";
const INLINE_WIDTH: usize = 100;

fn write_expr(e: &Expr, depth: usize, out: &mut String) {
    match e {
        Expr::Number(text) => out.push_str(text),
        Expr::String(s) => quote(s, out),
        Expr::Name(n) => out.push_str(n),
        Expr::Add(x, y) => {
            write_expr(x, depth, out);
            out.push_str(" + ");
            // '+' groups to the left, so a sum on the right needs brackets.
            if matches!(**y, Expr::Add(..)) {
                out.push('(');
                write_expr(y, depth, out);
                out.push(')');
            } else {
                write_expr(y, depth, out);
            }
        }
        Expr::Table(parts) => write_table(parts, depth, out),
    }
}

fn write_table(parts: &[Part], depth: usize, out: &mut String) {
    if parts.is_empty() {
        out.push_str("{}");
        return;
    }
    // A short list of scalars stays on one line: `{ 0, 1, 0 }`.
    if let [Part::List(items)] = parts
        && items.iter().all(|e| !matches!(e, Expr::Table(_)))
    {
        let mut line = String::new();
        for (i, e) in items.iter().enumerate() {
            if i > 0 {
                line.push_str(", ");
            }
            write_expr(e, 0, &mut line);
        }
        if line.len() <= INLINE_WIDTH {
            out.push_str("{ ");
            out.push_str(&line);
            out.push_str(" }");
            return;
        }
    }
    let pad = INDENT.repeat(depth + 1);
    out.push_str("{\n");
    for (p, part) in parts.iter().enumerate() {
        if p > 0 {
            out.push_str(&pad);
            out.push_str(";\n");
        }
        let lines: Vec<String> = match part {
            Part::List(items) => items
                .iter()
                .map(|e| {
                    let mut s = String::new();
                    write_expr(e, depth + 1, &mut s);
                    s
                })
                .collect(),
            Part::Record(fields) => fields
                .iter()
                .map(|(k, v)| {
                    let mut s = String::new();
                    match k {
                        Expr::String(name) if is_name(name) => {
                            s.push_str(&String::from_utf8_lossy(name));
                        }
                        _ => {
                            s.push('[');
                            write_expr(k, depth + 1, &mut s);
                            s.push(']');
                        }
                    }
                    s.push_str(" = ");
                    write_expr(v, depth + 1, &mut s);
                    s
                })
                .collect(),
        };
        let last = lines.len() - 1;
        for (i, line) in lines.iter().enumerate() {
            out.push_str(&pad);
            out.push_str(line);
            out.push_str(if i < last { ",\n" } else { "\n" });
        }
    }
    out.push_str(&INDENT.repeat(depth));
    out.push('}');
}

/// The source text of the chunk.
pub fn decompile(chunk: &Chunk) -> Result<String, String> {
    let mut out = String::new();
    for (i, a) in decode(chunk)?.iter().enumerate() {
        if i > 0 {
            out.push('\n');
        }
        out.push_str(&a.name);
        out.push_str(" = ");
        write_expr(&a.value, 0, &mut out);
        out.push('\n');
    }
    Ok(out)
}
