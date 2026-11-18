//! The Lua state after a scene script, in the lines
//! `CppSource/Oracle/oracle_scene_script.cpp` writes: the globals whose
//! value differs from the one they had before any script ran, by name,
//! then every table they reach, numbered in the order the dump first meets
//! them.

use std::collections::HashMap;

use neuron_client::scene_script::{NativeKind, TableId, Value, Vm};

use super::hex;

struct Dumper<'a> {
    vm: &'a Vm,
    ids: HashMap<TableId, usize>,
    queue: Vec<TableId>,
}

impl Dumper<'_> {
    fn encode(&mut self, value: &Value) -> String {
        match value {
            Value::Nil => "nil".into(),
            Value::Number(x) => format!("n{:016x}", x.to_bits()),
            Value::String(s) => format!("s{}", hex(s)),
            Value::Table(id) => {
                let next = self.queue.len();
                let id = *self.ids.entry(*id).or_insert_with(|| {
                    self.queue.push(*id);
                    next
                });
                format!("t{id}")
            }
            Value::Native(n) => match n.kind {
                NativeKind::Function => "function".into(),
                NativeKind::Userdata => "userdata".into(),
            },
        }
    }
}

fn key_class(key: &Value) -> u8 {
    match key {
        Value::Number(_) => 0,
        Value::String(_) => 1,
        _ => 2,
    }
}

/// The dump lines after `head`, the status line.
pub fn render(vm: &Vm, head: String) -> Vec<String> {
    let mut lines = vec![head];
    let mut d = Dumper {
        vm,
        ids: HashMap::new(),
        queue: Vec::new(),
    };
    for (name, value) in vm.changed_globals() {
        let value = d.encode(value);
        lines.push(format!(
            "{{\"global\":\"{}\",\"value\":\"{value}\"}}",
            hex(name)
        ));
    }
    let mut at = 0;
    while at < d.queue.len() {
        let table = d.vm.table(d.queue[at]);
        let mut entries: Vec<(&Value, &Value)> = table.entries().collect();
        if !entries.iter().all(|(k, _)| matches!(k, Value::Number(_))) {
            // A stable sort, as the harness's std::stable_sort.
            entries.sort_by(|(a, _), (b, _)| {
                key_class(a).cmp(&key_class(b)).then_with(|| match (a, b) {
                    (Value::Number(x), Value::Number(y)) => {
                        x.partial_cmp(y).unwrap_or(std::cmp::Ordering::Equal)
                    }
                    (Value::String(x), Value::String(y)) => x.cmp(y),
                    _ => std::cmp::Ordering::Equal,
                })
            });
        }
        let mut text = Vec::with_capacity(entries.len() * 2);
        for (k, v) in entries {
            text.push(d.encode(k));
            text.push(d.encode(v));
        }
        lines.push(format!(
            "{{\"table\":\"{at}\",\"size\":\"{}\",\"entries\":\"{}\"}}",
            table.capacity(),
            text.join(" ")
        ));
        at += 1;
    }
    lines
}

/// `lua_dobuffer`'s result: 0 when the buffer ran to its end, else 1; an
/// empty buffer is 1, as the harness writes it.
pub fn status(vm: &mut Vm, bytes: &[u8]) -> u8 {
    if bytes.is_empty() {
        return 1;
    }
    u8::from(vm.run(bytes).is_err())
}
