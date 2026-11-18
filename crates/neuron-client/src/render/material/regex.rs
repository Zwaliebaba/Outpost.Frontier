//! The regular expressions of the material map and batching rules
//! (`FMaterialRegex`, `CppSource/Shading/MaterialRegex.cpp`): the
//! reference's variant of Henry Spencer's compiler and backtracking
//! matcher, ported node for node.
//!
//! A program is a byte string. Byte 0 is the magic `0x9c`; nodes follow
//! from byte 1, each an opcode and a 16-bit big-endian link to the next
//! node (0 for none; the `BACK` opcode's link points backwards), then the
//! operand of `EXACTLY`, `ANYOF` and `ANYBUT`, a NUL-terminated string.
//! Compilation runs twice, once to count the size and once to emit, as the
//! reference does; a program of 0x7fff bytes or more is refused.
//!
//! The dialect is the reference's: `^`, `$`, `.`, `[...]` and `[^...]`
//! classes with ranges and the class-only escapes `\-`, `\\`, `\n`, `\r`,
//! `\t`, `\s` and `\d` (any other escaped letter in a class adds nothing),
//! groups up to nine, `|` and newline alternatives, `*`, `+` and `?` (a
//! doubled quantifier is an error), `\<` and `\>` word boundaries, and `\`
//! before any other character for the character itself. Matching is
//! case-sensitive; "word" characters are ASCII letters, digits and `_`
//! (the "C" locale `isalnum`).
//!
//! The reference reads patterns and subjects as C strings; here they are
//! byte slices that end at their first NUL or at their end, and a read past
//! the end gives 0 (divergence D300).

/// The program's first byte (`0x9c`).
pub const MAGIC: u8 = 0x9c;

/// Programs of this many bytes or more are refused.
pub const MAX_PROGRAM: usize = 0x7fff;

/// Nine groups at most: opening a tenth fails the compilation.
const MAX_GROUPS: i32 = 10;

const END: u8 = 0;
const BOL: u8 = 1;
const EOL: u8 = 2;
const ANY: u8 = 3;
const ANYOF: u8 = 4;
const ANYBUT: u8 = 5;
const BRANCH: u8 = 6;
const BACK: u8 = 7;
const EXACTLY: u8 = 8;
const NOTHING: u8 = 9;
const STAR: u8 = 10;
const PLUS: u8 = 11;
const WORDA: u8 = 12;
const WORDZ: u8 = 13;
const OPEN: u8 = 20;
const CLOSE: u8 = 30;

/// The node flags of the compiler: the piece matches at least one
/// character, it is a single-character atom, it starts with a `*` or `?`.
const HAS_WIDTH: u32 = 1;
const SIMPLE: u32 = 2;
const SP_START: u32 = 4;

/// No node (`GNullNode`).
const NULL: i32 = -1;
/// A node of the counting pass (`GDummyNode`).
const DUMMY: i32 = -2;

/// The "C" locale `isalnum` or `_`.
fn is_word(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_'
}

fn is_quantifier(byte: u8) -> bool {
    matches!(byte, b'*' | b'+' | b'?')
}

/// The node after `node` (`NextRegexNode`).
fn next_node(program: &[u8], node: i32) -> i32 {
    if node == DUMMY {
        return NULL;
    }
    let at = |i: i32| byte_at(program, i);
    let offset = (i32::from(at(node + 1)) << 8) | i32::from(at(node + 2));
    if offset == 0 {
        return NULL;
    }
    if at(node) == BACK {
        node - offset
    } else {
        node + offset
    }
}

/// The program byte at `index`; outside the program (which a compiled
/// program never reaches) 0.
fn byte_at(program: &[u8], index: i32) -> u8 {
    usize::try_from(index)
        .ok()
        .and_then(|i| program.get(i))
        .copied()
        .unwrap_or(0)
}

/// The C string at `start` of `bytes`: up to the first NUL or the end.
fn c_string(bytes: &[u8], start: usize) -> &[u8] {
    let tail = bytes.get(start..).unwrap_or_default();
    let end = tail.iter().position(|&b| b == 0).unwrap_or(tail.len());
    &tail[..end]
}

/// `FRegexCompiler`.
struct Compiler<'a> {
    pattern: &'a [u8],
    parse: usize,
    /// `None` in the counting pass.
    program: Option<Vec<u8>>,
    cursor: usize,
    size: usize,
    group_number: i32,
}

impl<'a> Compiler<'a> {
    fn new(pattern: &'a [u8], program: Option<Vec<u8>>) -> Compiler<'a> {
        Compiler {
            pattern,
            parse: 0,
            program,
            cursor: 0,
            size: 0,
            group_number: 1,
        }
    }

    /// The pattern byte at the cursor plus `ahead`; 0 at and past the end.
    fn peek(&self, ahead: isize) -> u8 {
        self.parse
            .checked_add_signed(ahead)
            .and_then(|i| self.pattern.get(i))
            .copied()
            .unwrap_or(0)
    }

    fn cur(&self) -> u8 {
        self.peek(0)
    }

    fn bump(&mut self) -> u8 {
        let byte = self.cur();
        self.parse += 1;
        byte
    }

    fn prog(&self) -> &[u8] {
        self.program.as_deref().unwrap_or_default()
    }

    fn emit_node(&mut self, opcode: u8) -> i32 {
        let Some(program) = self.program.as_mut() else {
            self.size += 3;
            return DUMMY;
        };
        let node = self.cursor;
        program[node] = opcode;
        program[node + 1] = 0;
        program[node + 2] = 0;
        self.cursor += 3;
        node as i32
    }

    fn emit_byte(&mut self, byte: u8) {
        match self.program.as_mut() {
            None => self.size += 1,
            Some(program) => {
                program[self.cursor] = byte;
                self.cursor += 1;
            }
        }
    }

    /// The three-byte node `opcode` inserted before the atom at `node`.
    fn insert_node(&mut self, opcode: u8, node: i32) {
        let cursor = self.cursor;
        let Some(program) = self.program.as_mut() else {
            self.size += 3;
            return;
        };
        let node = node as usize;
        program.copy_within(node..cursor, node + 3);
        self.cursor += 3;
        program[node] = opcode;
        program[node + 1] = 0;
        program[node + 2] = 0;
    }

    /// Points the last node of the chain from `node` at `target`.
    fn patch_tail(&mut self, node: i32, target: i32) {
        if node == DUMMY {
            return;
        }
        let mut node = node;
        let mut next = next_node(self.prog(), node);
        while next != NULL {
            node = next;
            next = next_node(self.prog(), node);
        }
        let Some(program) = self.program.as_mut() else {
            return;
        };
        let offset = if program[node as usize] == BACK {
            node - target
        } else {
            target - node
        };
        program[node as usize + 1] = (offset >> 8) as u8;
        program[node as usize + 2] = offset as u8;
    }

    /// Patches the operand chain of a `BRANCH` node.
    fn patch_operand_tail(&mut self, node: i32, target: i32) {
        if node != NULL && node != DUMMY && byte_at(self.prog(), node) == BRANCH {
            self.patch_tail(node + 3, target);
        }
    }

    /// `CompileExpression`: alternatives, in a group or at the top.
    fn expression(&mut self, parenthesized: bool, flags: &mut u32) -> i32 {
        *flags = HAS_WIDTH;
        let mut group = 0;
        let mut result = NULL;
        if parenthesized {
            if self.group_number >= MAX_GROUPS {
                return NULL;
            }
            group = self.group_number;
            self.group_number += 1;
            result = self.emit_node(OPEN + group as u8);
        }
        let mut branch_flags = 0;
        let mut branch = self.branch(&mut branch_flags);
        if branch == NULL {
            return NULL;
        }
        if result != NULL {
            self.patch_tail(result, branch);
        } else {
            result = branch;
        }
        loop {
            if branch_flags & HAS_WIDTH == 0 {
                *flags &= !HAS_WIDTH;
            }
            *flags |= branch_flags & SP_START;
            if self.cur() != b'|' && self.cur() != b'\n' {
                break;
            }
            self.parse += 1;
            branch = self.branch(&mut branch_flags);
            if branch == NULL {
                return NULL;
            }
            self.patch_tail(result, branch);
        }
        let end = self.emit_node(if parenthesized {
            CLOSE + group as u8
        } else {
            END
        });
        self.patch_tail(result, end);
        let mut node = result;
        while node != NULL {
            self.patch_operand_tail(node, end);
            node = next_node(self.prog(), node);
        }
        if parenthesized {
            if self.bump() != b')' {
                return NULL;
            }
        } else if self.cur() != 0 {
            return NULL;
        }
        result
    }

    /// `CompileBranch`: a concatenation; an empty one holds `NOTHING`.
    fn branch(&mut self, flags: &mut u32) -> i32 {
        *flags = 0;
        let result = self.emit_node(BRANCH);
        let mut previous = NULL;
        while !matches!(self.cur(), 0 | b')' | b'\n' | b'|') {
            let mut piece_flags = 0;
            let piece = self.piece(&mut piece_flags);
            if piece == NULL {
                return NULL;
            }
            *flags |= piece_flags & HAS_WIDTH;
            if previous == NULL {
                *flags |= piece_flags & SP_START;
            } else {
                self.patch_tail(previous, piece);
            }
            previous = piece;
        }
        if previous == NULL {
            self.emit_node(NOTHING);
        }
        result
    }

    /// `CompilePiece`: an atom and its quantifier. Simple atoms repeat with
    /// `STAR` and `PLUS`; others loop through `BRANCH` and `BACK`.
    fn piece(&mut self, flags: &mut u32) -> i32 {
        let mut atom_flags = 0;
        let result = self.atom(&mut atom_flags);
        if result == NULL {
            return NULL;
        }
        let quantifier = self.cur();
        if !is_quantifier(quantifier) {
            *flags = atom_flags;
            return result;
        }
        if atom_flags & HAS_WIDTH == 0 && quantifier != b'?' {
            return NULL;
        }
        *flags = if quantifier == b'+' {
            HAS_WIDTH
        } else {
            SP_START
        };
        match quantifier {
            b'*' => {
                if atom_flags & SIMPLE != 0 {
                    self.insert_node(STAR, result);
                } else {
                    self.insert_node(BRANCH, result);
                    let back = self.emit_node(BACK);
                    self.patch_operand_tail(result, back);
                    self.patch_operand_tail(result, result);
                    let branch = self.emit_node(BRANCH);
                    self.patch_tail(result, branch);
                    let nothing = self.emit_node(NOTHING);
                    self.patch_tail(result, nothing);
                }
            }
            b'+' => {
                if atom_flags & SIMPLE != 0 {
                    self.insert_node(PLUS, result);
                } else {
                    let branch = self.emit_node(BRANCH);
                    self.patch_tail(result, branch);
                    let back = self.emit_node(BACK);
                    self.patch_tail(back, result);
                    let alternate = self.emit_node(BRANCH);
                    self.patch_tail(branch, alternate);
                    let nothing = self.emit_node(NOTHING);
                    self.patch_tail(result, nothing);
                }
            }
            _ => {
                self.insert_node(BRANCH, result);
                let branch = self.emit_node(BRANCH);
                self.patch_tail(result, branch);
                let nothing = self.emit_node(NOTHING);
                self.patch_tail(result, nothing);
                self.patch_operand_tail(result, nothing);
            }
        }
        self.parse += 1;
        if is_quantifier(self.cur()) {
            NULL
        } else {
            result
        }
    }

    /// `CompileAtom`.
    fn atom(&mut self, flags: &mut u32) -> i32 {
        *flags = 0;
        let character = self.bump();
        match character {
            b'^' => return self.emit_node(BOL),
            b'$' => return self.emit_node(EOL),
            b'.' => {
                *flags = HAS_WIDTH | SIMPLE;
                return self.emit_node(ANY);
            }
            b'(' => {
                let mut group_flags = 0;
                let group = self.expression(true, &mut group_flags);
                if group != NULL {
                    *flags |= group_flags & (HAS_WIDTH | SP_START);
                }
                return group;
            }
            b'[' => return self.class(flags),
            0 | b'\n' | b')' | b'*' | b'+' | b'?' | b'|' => return NULL,
            b'\\' => {
                if self.cur() == 0 {
                    self.parse += 1;
                    return NULL;
                }
                if self.cur() == b'<' {
                    self.parse += 1;
                    return self.emit_node(WORDA);
                }
                if self.cur() == b'>' {
                    self.parse += 1;
                    return self.emit_node(WORDZ);
                }
                self.parse += 1;
            }
            _ => {}
        }
        self.parse -= 1;
        self.literal(flags)
    }

    /// The `[` case of `CompileAtom`.
    fn class(&mut self, flags: &mut u32) -> i32 {
        let negated = self.cur() == b'^';
        let result = self.emit_node(if negated { ANYBUT } else { ANYOF });
        if negated {
            self.parse += 1;
        }
        if self.cur() == b']' || self.cur() == b'-' {
            let byte = self.bump();
            self.emit_byte(byte);
        }
        while self.cur() != 0 && self.cur() != b']' {
            let class_character = self.bump();
            if class_character == b'-' {
                if self.cur() == b']' || self.cur() == 0 {
                    self.emit_byte(b'-');
                } else {
                    let mut first = u32::from(self.peek(-2)) + 1;
                    let last = u32::from(self.cur());
                    if first > last + 1 {
                        return NULL;
                    }
                    while first <= last {
                        self.emit_byte(first as u8);
                        first += 1;
                    }
                    self.parse += 1;
                }
            } else if class_character == b'\\' {
                match self.bump() {
                    b'-' => self.emit_byte(b'-'),
                    b'\\' => self.emit_byte(b'\\'),
                    b'n' => self.emit_byte(b'\n'),
                    b'r' => self.emit_byte(b'\r'),
                    b't' => self.emit_byte(b'\t'),
                    b's' => {
                        for byte in *b" \t\n\r" {
                            self.emit_byte(byte);
                        }
                    }
                    b'd' => {
                        for digit in b'0'..=b'9' {
                            self.emit_byte(digit);
                        }
                    }
                    _ => {}
                }
            } else {
                self.emit_byte(class_character);
            }
        }
        self.emit_byte(0);
        if self.cur() != b']' {
            return NULL;
        }
        self.parse += 1;
        *flags = HAS_WIDTH | SIMPLE;
        result
    }

    /// The literal run of `CompileAtom`: characters up to the next special
    /// one; a quantifier leaves the last character to an atom of its own.
    /// `previous` is the reference's pointer, here a pattern index.
    fn literal(&mut self, flags: &mut u32) -> i32 {
        let result = self.emit_node(EXACTLY);
        let mut previous: Option<usize> = None;
        loop {
            let saved_previous = previous;
            let literal = self.bump();
            let next = self.cur();
            if next == b'\\' {
                self.emit_byte(literal);
                if matches!(self.peek(1), 0 | b'<' | b'>') {
                    break;
                }
                previous = Some(self.parse);
                self.parse += 1;
            } else if is_quantifier(next) {
                match saved_previous {
                    Some(at) => self.parse = at,
                    None => self.emit_byte(literal),
                }
                previous = saved_previous;
                break;
            } else if matches!(
                next,
                0 | b'\n' | b'$' | b'(' | b')' | b'.' | b'[' | b'^' | b'|'
            ) {
                self.emit_byte(literal);
                previous = saved_previous;
                break;
            } else {
                self.emit_byte(literal);
                previous = Some(self.parse);
            }
        }
        self.emit_byte(0);
        *flags = if previous.is_none() {
            HAS_WIDTH | SIMPLE
        } else {
            HAS_WIDTH
        };
        result
    }
}

/// A compiled expression (`FMaterialRegex`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialRegex {
    program: Vec<u8>,
    start_character: u8,
    anchored: bool,
    /// The offset and length of the longest literal every match contains.
    must: Option<(usize, usize)>,
}

impl MaterialRegex {
    /// `FMaterialRegex::Compile`: `None` for a pattern the dialect refuses
    /// or a program of [`MAX_PROGRAM`] bytes or more. The pattern ends at
    /// its first NUL.
    #[must_use]
    pub fn compile(pattern: &[u8]) -> Option<MaterialRegex> {
        let pattern = c_string(pattern, 0);
        let mut count = Compiler::new(pattern, None);
        count.emit_byte(MAGIC);
        let mut flags = 0;
        if count.expression(false, &mut flags) == NULL || count.size >= MAX_PROGRAM {
            return None;
        }
        let mut emit = Compiler::new(pattern, Some(vec![0; count.size]));
        emit.emit_byte(MAGIC);
        if emit.expression(false, &mut flags) == NULL {
            return None;
        }
        let program = emit.program.unwrap_or_default();
        let mut regex = MaterialRegex {
            program,
            start_character: 0,
            anchored: false,
            must: None,
        };
        let program = &regex.program;
        if byte_at(program, next_node(program, 1)) == END {
            let first = 4;
            if byte_at(program, first) == EXACTLY {
                regex.start_character = byte_at(program, first + 3);
            } else if byte_at(program, first) == BOL {
                regex.anchored = true;
            }
            if flags & SP_START != 0 {
                let mut node = first;
                let mut must_length = 0;
                while node != NULL {
                    if byte_at(program, node) == EXACTLY {
                        let operand = node as usize + 3;
                        let length = c_string(program, operand).len();
                        if length >= must_length {
                            regex.must = Some((operand, length));
                            must_length = length;
                        }
                    }
                    node = next_node(program, node);
                }
            }
        }
        Some(regex)
    }

    /// The program bytes, the magic byte first.
    #[must_use]
    pub fn program(&self) -> &[u8] {
        &self.program
    }

    /// The first character of every match when the expression is one
    /// literal-led alternative, else 0.
    #[must_use]
    pub fn start_character(&self) -> u8 {
        self.start_character
    }

    /// The expression is one alternative that starts with `^`.
    #[must_use]
    pub fn anchored(&self) -> bool {
        self.anchored
    }

    /// The program offset and length of the literal every match contains,
    /// found for single-alternative expressions that start with `*` or `?`.
    #[must_use]
    pub fn must(&self) -> Option<(usize, usize)> {
        self.must
    }

    /// `FMaterialRegex::Search` on a compiled expression: whether the
    /// expression matches anywhere in `text`, which ends at its first NUL.
    #[must_use]
    pub fn is_match(&self, text: &[u8]) -> bool {
        if self.program.first() != Some(&MAGIC) {
            return false;
        }
        let text = c_string(text, 0);
        if let Some((offset, length)) = self.must {
            let must = &self.program[offset..offset + length];
            let found = (0..text.len()).any(|at| text[at..].starts_with(must));
            if !found {
                return false;
            }
        }
        let mut executor = Executor::new(&self.program, text);
        if self.anchored {
            return executor.try_at(0);
        }
        if self.start_character != 0 {
            return (0..text.len())
                .filter(|&at| text[at] == self.start_character)
                .any(|at| executor.try_at(at as isize));
        }
        (0..=text.len()).any(|at| executor.try_at(at as isize))
    }
}

/// `FMaterialRegex::Search`: false for a missing expression (one that did
/// not compile) or a missing subject.
#[must_use]
pub fn search(expression: Option<&MaterialRegex>, text: Option<&[u8]>) -> bool {
    match (expression, text) {
        (Some(expression), Some(text)) => expression.is_match(text),
        _ => false,
    }
}

/// `FRegexExecutor`. Positions are subject indices; a failed repetition
/// may leave one at -1, as the reference leaves its pointer one before
/// the subject.
struct Executor<'a> {
    program: &'a [u8],
    text: &'a [u8],
    input: isize,
    starts: [Option<isize>; 10],
    ends: [Option<isize>; 10],
}

impl<'a> Executor<'a> {
    fn new(program: &'a [u8], text: &'a [u8]) -> Executor<'a> {
        Executor {
            program,
            text,
            input: 0,
            starts: [None; 10],
            ends: [None; 10],
        }
    }

    /// The subject byte at `at`: 0 at the terminator (and outside).
    fn ch(&self, at: isize) -> u8 {
        usize::try_from(at)
            .ok()
            .and_then(|i| self.text.get(i))
            .copied()
            .unwrap_or(0)
    }

    fn op(&self, node: i32) -> u8 {
        byte_at(self.program, node)
    }

    fn operand(&self, node: i32) -> &'a [u8] {
        c_string(self.program, node as usize + 3)
    }

    /// `Repeat`: the longest run of the simple atom at `node` from the
    /// current position, consumed.
    fn repeat(&mut self, node: i32) -> isize {
        let mut scan = self.input;
        let operand = self.operand(node);
        match self.op(node) {
            ANY => {
                while self.ch(scan) != 0 {
                    scan += 1;
                }
            }
            ANYOF => {
                while self.ch(scan) != 0 && operand.contains(&self.ch(scan)) {
                    scan += 1;
                }
            }
            ANYBUT => {
                while self.ch(scan) != 0 && !operand.contains(&self.ch(scan)) {
                    scan += 1;
                }
            }
            EXACTLY => {
                let first = operand.first().copied().unwrap_or(0);
                while self.ch(scan) == first {
                    scan += 1;
                }
            }
            _ => {}
        }
        let count = scan - self.input;
        self.input = scan;
        count
    }

    /// `Match`: ordered backtracking from `node`.
    fn matches(&mut self, mut node: i32) -> bool {
        while node != NULL {
            let mut next = next_node(self.program, node);
            let saved = self.input;
            let opcode = self.op(node);
            let input = self.ch(self.input);
            match opcode {
                END => return true,
                BOL => {
                    if self.input != 0 {
                        return false;
                    }
                }
                EOL => {
                    if input != 0 {
                        return false;
                    }
                }
                ANY => {
                    if input == 0 {
                        return false;
                    }
                    self.input += 1;
                }
                ANYOF | ANYBUT => {
                    let found = self.operand(node).contains(&input);
                    if input == 0 || found != (opcode == ANYOF) {
                        return false;
                    }
                    self.input += 1;
                }
                BRANCH => {
                    if self.op(next) != BRANCH {
                        next = node + 3;
                    } else {
                        loop {
                            let saved = self.input;
                            if self.matches(node + 3) {
                                return true;
                            }
                            self.input = saved;
                            node = next_node(self.program, node);
                            if node == NULL || self.op(node) != BRANCH {
                                return false;
                            }
                        }
                    }
                }
                BACK | NOTHING => {}
                EXACTLY => {
                    let operand = self.operand(node);
                    if operand.first().copied().unwrap_or(0) != input {
                        return false;
                    }
                    let length = operand.len();
                    if length > 1 {
                        for (i, &byte) in operand.iter().enumerate() {
                            if self.ch(self.input + i as isize) != byte {
                                return false;
                            }
                        }
                    }
                    self.input += length as isize;
                }
                STAR | PLUS => {
                    let next_character = if self.op(next) == EXACTLY {
                        byte_at(self.program, next + 3)
                    } else {
                        0
                    };
                    let mut count = self.repeat(node + 3);
                    let minimum = if opcode == STAR { 0 } else { 1 };
                    while count >= minimum {
                        if (next_character == 0 || self.ch(self.input) == next_character)
                            && self.matches(next)
                        {
                            return true;
                        }
                        self.input = saved + count - 1;
                        count -= 1;
                    }
                    return false;
                }
                WORDA => {
                    if !is_word(input) {
                        return false;
                    }
                    if self.input > 0 && is_word(self.ch(self.input - 1)) {
                        return false;
                    }
                }
                WORDZ => {
                    if is_word(input) {
                        return false;
                    }
                }
                21..=29 => {
                    if !self.matches(next) {
                        return false;
                    }
                    let slot = &mut self.starts[usize::from(opcode - OPEN)];
                    if slot.is_none() {
                        *slot = Some(saved);
                    }
                    return true;
                }
                31..=39 => {
                    if !self.matches(next) {
                        return false;
                    }
                    let slot = &mut self.ends[usize::from(opcode - CLOSE)];
                    if slot.is_none() {
                        *slot = Some(saved);
                    }
                    return true;
                }
                _ => return false,
            }
            node = next;
        }
        false
    }

    /// `Try`: a match starting at `start`, every capture cleared first.
    fn try_at(&mut self, start: isize) -> bool {
        self.input = start;
        self.starts = [None; 10];
        self.ends = [None; 10];
        if !self.matches(1) {
            return false;
        }
        self.starts[0] = Some(start);
        self.ends[0] = Some(self.input);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn matches(pattern: &str, text: &str) -> bool {
        MaterialRegex::compile(pattern.as_bytes()).is_some_and(|r| r.is_match(text.as_bytes()))
    }

    #[test]
    fn literals_and_anchors() {
        assert!(matches("abc", "xxabcxx"));
        assert!(!matches("abd", "xxabcxx"));
        assert!(matches("^alpha_mask.*", "alpha_mask_1"));
        assert!(!matches("^alpha_mask.*", "xalpha_mask"));
        assert!(matches("^tlr_material$", "tlr_material"));
        assert!(!matches("^tlr_material$", "tlr_materials"));
        assert!(matches(".*OcOt", "DcDtEcOcOt"));
        assert!(!matches(".*OcOt", "DcDtEc"));
    }

    #[test]
    fn classes_groups_and_alternatives() {
        assert!(matches("^n-texture.*$", "n-texture01"));
        assert!(matches("[a-c]+x", "zzbbcx"));
        assert!(!matches("[^a-c]x", "ax"));
        assert!(matches("(ab|cd)+e", "xxcdabe"));
        assert!(matches("a?b", "b"));
        assert!(matches("\\<word\\>", "a word here"));
        assert!(!matches("\\<word\\>", "swordfish"));
        assert!(matches("[\\d]+", "x9"));
    }

    #[test]
    fn refused_patterns() {
        for pattern in [
            "",
            "*a",
            "a**",
            "(a",
            "a)",
            "[a",
            "(((((((((((a)))))))))))",
            "a\\",
        ] {
            let compiled = MaterialRegex::compile(pattern.as_bytes());
            if pattern.is_empty() {
                assert!(compiled.is_some(), "the empty pattern compiles");
            } else {
                assert!(compiled.is_none(), "{pattern}");
            }
        }
        assert!(MaterialRegex::compile(b"(a*)*").is_none());
    }

    #[test]
    fn program_layout() {
        // BRANCH, EXACTLY "ab" and END, linked forwards.
        let regex = MaterialRegex::compile(b"ab").unwrap_or_else(|| panic!("compiles"));
        assert_eq!(
            regex.program(),
            &[MAGIC, BRANCH, 0, 9, EXACTLY, 0, 6, b'a', b'b', 0, END, 0, 0]
        );
        assert_eq!(regex.start_character(), b'a');
        assert!(!regex.anchored());
        assert_eq!(regex.must(), None);
        let star = MaterialRegex::compile(b"a*b").unwrap_or_else(|| panic!("compiles"));
        assert_eq!(star.must(), Some((15, 1)));
        assert_eq!(star.start_character(), 0);
    }
}
