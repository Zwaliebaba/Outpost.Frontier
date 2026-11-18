//! The evaluator: the part of the Lua 3.2 virtual machine (`lvm.c`,
//! `ldo.c`) that the scene scripts reach, and the global state the
//! reference sets up before it loads one (`SceneScriptLua.cpp`).
//!
//! The reference keeps one Lua state for the life of the module and loads
//! every script into it, so globals one script sets are still there when
//! the next one loads. A [`Vm`] is that state.

use std::collections::BTreeMap;
use std::fmt;
use std::rc::Rc;

use super::chunk::{Chunk, ChunkError, Constant, ID_CHUNK, op};
use super::number;
use super::table::{Native, NativeKind, Table, TableError, TableId, Value};

/// `LFIELDS_PER_FLUSH` of `lopcodes.h`.
const FIELDS_PER_FLUSH: u32 = 64;

/// `LUA_VERSION` of the reference's `lua.h`.
const VERSION: &str = "Lua 3.2";

/// `PI` of `lmathlib.c`.
/// The literal rounds to the same double as `std::f64::consts::PI`.
const PI: f64 = std::f64::consts::PI;

/// Globals that hold functions after `lua_open`, `lua_mathlibopen` and
/// `lua_iolibopen` (`lbuiltin.c`, `lmathlib.c`, `liolib.c`), and the
/// error method the scene-script parser installs.
const FUNCTIONS: &[&str] = &[
    "_ALERT",
    "_ERRORMESSAGE",
    "call",
    "collectgarbage",
    "copytagmethods",
    "dofile",
    "dostring",
    "error",
    "getglobal",
    "gettagmethod",
    "newtag",
    "next",
    "nextvar",
    "print",
    "rawgetglobal",
    "rawgettable",
    "rawsetglobal",
    "rawsettable",
    "seterrormethod",
    "setglobal",
    "settag",
    "settagmethod",
    "tag",
    "tonumber",
    "tostring",
    "type",
    "assert",
    "foreach",
    "foreachi",
    "foreachvar",
    "getn",
    "sort",
    "tinsert",
    "tremove",
    "abs",
    "sin",
    "cos",
    "tan",
    "asin",
    "acos",
    "atan",
    "atan2",
    "ceil",
    "floor",
    "mod",
    "frexp",
    "ldexp",
    "sqrt",
    "min",
    "max",
    "log",
    "log10",
    "exp",
    "deg",
    "rad",
    "random",
    "randomseed",
    "clock",
    "date",
    "debug",
    "execute",
    "exit",
    "getenv",
    "remove",
    "rename",
    "setlocale",
    "tmpname",
    "appendto",
    "closefile",
    "flush",
    "openfile",
    "read",
    "readfrom",
    "seek",
    "write",
    "writeto",
];

/// The file handles of `liolib.c`, by global and stream.
const FILES: &[(&str, &str)] = &[
    ("_INPUT", "stdin"),
    ("_OUTPUT", "stdout"),
    ("_STDIN", "stdin"),
    ("_STDOUT", "stdout"),
    ("_STDERR", "stderr"),
];

/// The numbers `RegisterSceneScriptLuaConstants` sets, in its order; a
/// later entry replaces an earlier one of the same name (`HARDPOINT` ends
/// up 1, not the entity type 8).
pub const SCENE_CONSTANTS: &[(&str, f64)] = &[
    ("UNKNOWN_ENTITY", 0.0),
    ("COMPOUND", 1.0),
    ("DEFORMABLE", 2.0),
    ("CAMERA", 3.0),
    ("MONITOR", 4.0),
    ("LIGHT", 5.0),
    ("SOUND", 6.0),
    ("MARKER", 7.0),
    ("HARDPOINT", 8.0),
    ("SCENE", 9.0),
    ("SUB_SCENE", 10.0),
    ("MOTION_PATH", 11.0),
    ("DELETED", 12.0),
    ("PSYS", 13.0),
    ("UNDEFINED_EVENT", 0.0),
    ("START_MOTION", 10.0),
    ("START_SOUND", 3.0),
    ("START_IK", 11.0),
    ("START_PATH_ANIMATION", 6.0),
    ("ATTACH_ENTITY", 8.0),
    ("START_LIGHT_PROP_ANIM", 4.0),
    ("START_CAMERA_PROP_ANIM", 5.0),
    ("START_SPATIAL_PROP_ANIM", 7.0),
    ("SET_CAMERA", 2.0),
    ("CONNECT_HARDPOINTS", 9.0),
    ("START_SUB_SCENE", 12.0),
    ("USER_EVENT", 1.0),
    ("START_PSYS", 13.0),
    ("START_PSYS_PROP_ANIM", 14.0),
    ("START_AUDIO_PROP_ANIM", 15.0),
    ("START_FOG_PROP_ANIM", 16.0),
    ("START_REVERB_PROP_ANIM", 17.0),
    ("START_FLR_HEIGHT_ANIM", 18.0),
    ("SUBTITLE", 19.0),
    ("X_AXIS", 0.0),
    ("Y_AXIS", 1.0),
    ("Z_AXIS", 2.0),
    ("NEG_X_AXIS", 3.0),
    ("NEG_Y_AXIS", 4.0),
    ("NEG_Z_AXIS", 5.0),
    ("REFERENCE", 1.0),
    ("HIDDEN", 16.0),
    ("FOG_PROPS_REMOVED", 32.0),
    ("SPATIAL", 2.0),
    ("STREAM", 4.0),
    ("LOOP", 8.0),
    ("LIT_AMBIENT", 2.0),
    ("LIT_DYNAMIC", 4.0),
    ("USE_SCRIPT_DURATION", 1.0),
    ("POSITION", 2.0),
    ("PATH_POSITION", 2.0),
    ("ORIENTATION", 4.0),
    ("LOOK_AT", 8.0),
    ("PARENT_CHILD", 64.0),
    ("ENTITY_RELATIVE", 32.0),
    ("ORIENTATION_RELATIVE", 128.0),
    ("ROOT", 0.0),
    ("PART", 2.0),
    ("HARDPOINT", 1.0),
    ("ADD_PATH", 6.0),
    ("LOOKAT_ENTITY", 8.0),
    ("STOP", 21.0),
    ("STOP_IK", 22.0),
    ("START", 23.0),
    ("PROPERTY_ANIM", 0.0),
    ("F_NONE", 0.0),
    ("F_EXP", 1.0),
    ("F_EXP2", 2.0),
    ("F_LINEAR", 3.0),
    ("L_POINT", 1.0),
    ("L_SPOT", 2.0),
    ("L_DIRECT", 3.0),
    ("Y", 1.0),
    ("N", 0.0),
];

/// Why a buffer did not run to its end. Where the reference reports these
/// through `lua_error`, `lua_dobuffer` returns 1.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ScriptError {
    /// The buffer is a script in source form, which the port does not
    /// load (decision O18, divergence D86).
    Source,
    /// A chunk could not be read.
    Chunk(ChunkError),
    /// An opcode outside the subset of design/facts.md F15 (D86).
    UnsupportedOpcode { opcode: u8, at: usize },
    /// An instruction found fewer values on the stack than it takes.
    StackUnderflow { at: usize },
    /// A table store whose target is not a table.
    NotATable { at: usize },
    /// A global instruction whose constant is not a string, or a constant
    /// index past the table.
    BadConstant { at: usize },
    /// Arithmetic on a value that is not a number and does not convert
    /// to one ("unexpected type at arithmetic operation").
    Arithmetic { at: usize },
    /// A table store with a nil key ("unexpected type to index table").
    NilKey { at: usize },
    /// "table overflow".
    TableOverflow { at: usize },
}

impl fmt::Display for ScriptError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Source => f.write_str("scripts in source form are not supported"),
            Self::Chunk(e) => e.fmt(f),
            Self::UnsupportedOpcode { opcode, at } => write!(
                f,
                "opcode {opcode} at byte {at} is outside the subset the scene scripts use"
            ),
            Self::StackUnderflow { at } => write!(f, "stack underflow at byte {at}"),
            Self::NotATable { at } => write!(f, "table store at byte {at} has no table"),
            Self::BadConstant { at } => write!(f, "bad constant at byte {at}"),
            Self::Arithmetic { at } => {
                write!(f, "unexpected type at arithmetic operation at byte {at}")
            }
            Self::NilKey { at } => write!(f, "table index is nil at byte {at}"),
            Self::TableOverflow { at } => write!(f, "table overflow at byte {at}"),
        }
    }
}

impl std::error::Error for ScriptError {}

impl From<ChunkError> for ScriptError {
    fn from(e: ChunkError) -> Self {
        ScriptError::Chunk(e)
    }
}

/// The scene-script Lua state.
#[derive(Clone, Debug)]
pub struct Vm {
    tables: Vec<Table>,
    globals: BTreeMap<Vec<u8>, Value>,
    /// The globals before any script ran.
    initial: BTreeMap<Vec<u8>, Value>,
}

impl Default for Vm {
    fn default() -> Self {
        Self::new()
    }
}

impl Vm {
    /// The state after `SceneScriptLuaOpen` and the first parser's
    /// `RegisterSceneScriptLuaConstants`.
    #[must_use]
    pub fn new() -> Vm {
        let mut globals = BTreeMap::new();
        for &name in FUNCTIONS {
            globals.insert(
                name.as_bytes().to_vec(),
                Value::Native(Native {
                    kind: NativeKind::Function,
                    name,
                }),
            );
        }
        for &(global, stream) in FILES {
            globals.insert(
                global.as_bytes().to_vec(),
                Value::Native(Native {
                    kind: NativeKind::Userdata,
                    name: stream,
                }),
            );
        }
        globals.insert(b"_VERSION".to_vec(), string(VERSION.as_bytes()));
        globals.insert(b"PI".to_vec(), Value::Number(PI));
        for &(name, value) in SCENE_CONSTANTS {
            globals.insert(name.as_bytes().to_vec(), Value::Number(value));
        }
        Vm {
            tables: Vec::new(),
            initial: globals.clone(),
            globals,
        }
    }

    /// The value of a global; nil when unset.
    #[must_use]
    pub fn global(&self, name: &str) -> &Value {
        self.globals.get(name.as_bytes()).unwrap_or(&Value::Nil)
    }

    /// Every global whose value is not the one it had before any script
    /// ran, by name.
    pub fn changed_globals(&self) -> impl Iterator<Item = (&[u8], &Value)> {
        let mut names: Vec<&Vec<u8>> = self.globals.keys().collect();
        names.extend(
            self.initial
                .keys()
                .filter(|k| !self.globals.contains_key(*k)),
        );
        names.sort();
        names.dedup();
        names.into_iter().filter_map(|name| {
            let now = self.globals.get(name).unwrap_or(&Value::Nil);
            let before = self.initial.get(name).unwrap_or(&Value::Nil);
            (now != before).then_some((name.as_slice(), now))
        })
    }

    /// A table of this state.
    #[must_use]
    pub fn table(&self, id: TableId) -> &Table {
        &self.tables[id.index()]
    }

    /// `lua_dobuffer`: runs the compiled chunks of `bytes` one after the
    /// other. Globals a chunk set before an error stay set.
    pub fn run(&mut self, bytes: &[u8]) -> Result<(), ScriptError> {
        self.collect();
        if bytes.first() != Some(&ID_CHUNK) {
            return Err(ScriptError::Source);
        }
        let mut rest = bytes;
        while !rest.is_empty() {
            let (chunk, used) = Chunk::read(rest)?;
            self.execute(&chunk)?;
            rest = &rest[used..];
        }
        Ok(())
    }

    /// Drops the tables no global reaches; between runs the stack is
    /// empty, so the globals are the only roots.
    fn collect(&mut self) {
        let mut live = vec![false; self.tables.len()];
        let mut work: Vec<TableId> = self.globals.values().filter_map(Value::as_table).collect();
        while let Some(id) = work.pop() {
            if !std::mem::replace(&mut live[id.index()], true) {
                work.extend(self.tables[id.index()].children());
            }
        }
        let mut map = vec![u32::MAX; self.tables.len()];
        let mut next = 0u32;
        for (i, &keep) in live.iter().enumerate() {
            if keep {
                map[i] = next;
                next += 1;
            }
        }
        let mut i = 0;
        self.tables.retain(|_| {
            i += 1;
            live[i - 1]
        });
        for table in &mut self.tables {
            table.remap(&map);
        }
        for value in self.globals.values_mut() {
            value.remap(&map);
        }
    }

    fn new_table(&mut self, n: i64, at: usize) -> Result<Value, ScriptError> {
        let table = Table::new(n).map_err(|_| ScriptError::TableOverflow { at })?;
        let id = TableId(u32::try_from(self.tables.len()).expect("fewer than 2^32 tables"));
        self.tables.push(table);
        Ok(Value::Table(id))
    }

    fn store(
        &mut self,
        target: &Value,
        key: Value,
        value: Value,
        at: usize,
    ) -> Result<(), ScriptError> {
        let id = target.as_table().ok_or(ScriptError::NotATable { at })?;
        self.tables[id.index()]
            .set(key, value)
            .map_err(|e| match e {
                TableError::NilKey => ScriptError::NilKey { at },
                TableError::Overflow => ScriptError::TableOverflow { at },
            })
    }

    /// `luaV_execute` on the main function of a chunk.
    fn execute(&mut self, chunk: &Chunk) -> Result<(), ScriptError> {
        let code = &chunk.code;
        // The reference pads the code with ENDCODE bytes.
        let byte = |pc: usize| code.get(pc).copied().unwrap_or(op::ENDCODE);
        let constant = |index: usize, at: usize| {
            chunk
                .constants
                .get(index)
                .ok_or(ScriptError::BadConstant { at })
        };
        let mut stack: Vec<Value> = Vec::new();
        let mut pc = 2;
        let mut aux: u32 = 0;
        let mut at = pc;
        loop {
            let opcode = byte(pc);
            pc += 1;
            // Word variants add their high byte, then fall through.
            let base = match opcode {
                op::PUSHNUMBERW
                | op::PUSHNUMBERNEGW
                | op::PUSHCONSTANTW
                | op::GETGLOBALW
                | op::CREATEARRAYW
                | op::SETGLOBALW
                | op::SETLISTW
                | op::LONGARGW => {
                    aux = aux.wrapping_add(u32::from(byte(pc)) << 8);
                    pc += 1;
                    opcode + 1
                }
                other => other,
            };
            match base {
                op::ENDCODE => return Ok(()),
                op::LONGARG => {
                    aux = aux.wrapping_add(u32::from(byte(pc)));
                    pc += 1;
                    aux <<= 16;
                    // "do not reset aux"
                    continue;
                }
                op::PUSHNUMBER | op::PUSHNUMBERNEG => {
                    aux = aux.wrapping_add(u32::from(byte(pc)));
                    pc += 1;
                    let x = f64::from(aux.cast_signed());
                    stack.push(Value::Number(if base == op::PUSHNUMBER { x } else { -x }));
                }
                op::PUSHCONSTANT => {
                    aux = aux.wrapping_add(u32::from(byte(pc)));
                    pc += 1;
                    stack.push(match constant(aux as usize, at)? {
                        Constant::Number(x) => Value::Number(*x),
                        Constant::String(s) => string(s),
                    });
                }
                op::GETGLOBAL | op::SETGLOBAL => {
                    aux = aux.wrapping_add(u32::from(byte(pc)));
                    pc += 1;
                    let Constant::String(name) = constant(aux as usize, at)? else {
                        return Err(ScriptError::BadConstant { at });
                    };
                    if base == op::GETGLOBAL {
                        stack.push(self.globals.get(name).cloned().unwrap_or(Value::Nil));
                    } else {
                        let value = stack.pop().ok_or(ScriptError::StackUnderflow { at })?;
                        if value.is_nil() {
                            self.globals.remove(name);
                        } else {
                            self.globals.insert(name.clone(), value);
                        }
                    }
                }
                op::CREATEARRAY => {
                    aux = aux.wrapping_add(u32::from(byte(pc)));
                    pc += 1;
                    let table = self.new_table(i64::from(aux.cast_signed()), at)?;
                    stack.push(table);
                }
                op::SETLIST => {
                    aux = aux.wrapping_add(u32::from(byte(pc)));
                    pc += 1;
                    let n = usize::from(byte(pc));
                    pc += 1;
                    if stack.len() < n + 1 {
                        return Err(ScriptError::StackUnderflow { at });
                    }
                    let first = aux.wrapping_mul(FIELDS_PER_FLUSH);
                    let target = stack[stack.len() - n - 1].clone();
                    for k in (1..=n).rev() {
                        let value = stack.pop().expect("checked above");
                        // `luaH_setint(arr, n + aux, ...)`: an int key.
                        let key = (k as u32).wrapping_add(first).cast_signed();
                        self.store(&target, Value::Number(f64::from(key)), value, at)?;
                    }
                }
                op::SETMAP => {
                    // `aux = *pc++`: a LONGARG prefix is dropped here.
                    let pairs = usize::from(byte(pc)) + 1;
                    pc += 1;
                    if stack.len() < 2 * pairs + 1 {
                        return Err(ScriptError::StackUnderflow { at });
                    }
                    let target = stack[stack.len() - 2 * pairs - 1].clone();
                    for _ in 0..pairs {
                        let value = stack.pop().expect("checked above");
                        let key = stack.pop().expect("checked above");
                        self.store(&target, key, value, at)?;
                    }
                }
                op::ADDOP => {
                    if stack.len() < 2 {
                        return Err(ScriptError::StackUnderflow { at });
                    }
                    let r = stack.pop().expect("checked above");
                    let l = stack.pop().expect("checked above");
                    // `tonumber(r) || tonumber(l)`, right operand first.
                    let (Some(r), Some(l)) = (arith_operand(&r), arith_operand(&l)) else {
                        return Err(ScriptError::Arithmetic { at });
                    };
                    stack.push(Value::Number(l + r));
                }
                _ => return Err(ScriptError::UnsupportedOpcode { opcode, at }),
            }
            aux = 0;
            at = pc;
        }
    }
}

fn string(bytes: &[u8]) -> Value {
    Value::String(Rc::from(bytes))
}

/// An arithmetic operand: a number, or a string that converts to one.
fn arith_operand(v: &Value) -> Option<f64> {
    match v {
        Value::Number(x) => Some(*x),
        Value::String(s) => number::to_number(s),
        _ => None,
    }
}
