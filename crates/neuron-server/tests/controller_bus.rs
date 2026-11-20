//! The `controller_bus` suite (decision record 0006, point 10):
//! `tests/fixtures/oracle/controller_bus.list` run on
//! `neuron_server::controller` with scripted controllers, rendered as
//! `tests/vectors/controller_bus.jsonl`. `Oracle.exe neuron-server` writes
//! the same file from `Server.dll`'s registry
//! (`CppSource/Oracle/oracle_bus.cpp`, which documents the directives).

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::Path;

use neuron_core::id::ObjectIdAllocator;
use neuron_core::rng::CrtRand;
use neuron_server::controller::{
    self, BusHost, ControllerBus, ControllerId, ControllerNode, Priority,
};

const GHOST: ControllerId = ControllerId(0x3FFF_FFF0);
const HEARTBEAT: i32 = 0x20;
const TIMER_EXPIRED: i32 = 0x3E;
const CONTROLLER_DESTROYED: i32 = 0x0C;

#[derive(Clone, Copy, Debug)]
struct Kind(usize);

#[derive(Clone, Copy, Debug)]
enum Msg {
    Test { msg: i32, tag: u32 },
    Heartbeat(f32),
    Timer(u32, u32),
    Destroyed(ControllerId),
}

#[derive(Clone, Debug)]
struct Rule {
    inst: String,
    event: String,
    filter: String,
    limit: u32,
    fired: u32,
    action: Vec<String>,
}

#[derive(Default)]
struct Host {
    bus: ControllerBus<Kind>,
    ids: ObjectIdAllocator,
    rand: CrtRand,
    instances: Vec<String>,
    case: String,
    seq: u32,
    depth: u32,
    rules: Vec<Rule>,
    flids: BTreeMap<String, ControllerId>,
    names: BTreeMap<ControllerId, String>,
    out: Vec<String>,
}

fn parse_uint(text: &str) -> u32 {
    // `strtoul(text, nullptr, 0)` for the decimal and 0x forms the list uses.
    match text.strip_prefix("0x") {
        Some(hex) => u32::from_str_radix(hex, 16).unwrap_or(0),
        None => text
            .parse::<i64>()
            .map(|v| v as u32)
            .unwrap_or_else(|_| panic!("number {text}")),
    }
}

fn parse_float(text: &str) -> f32 {
    // `(float)strtod(text)`.
    text.parse::<f64>()
        .unwrap_or_else(|_| panic!("float {text}")) as f32
}

fn parse_priority(word: &str) -> Priority {
    match word {
        "HIGHEST" => Priority::Highest,
        "ABOVE_NORMAL" => Priority::AboveNormal,
        "NORMAL" => Priority::Normal,
        "BELOW_NORMAL" => Priority::BelowNormal,
        _ => Priority::Lowest,
    }
}

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

impl Host {
    fn head(&mut self) -> String {
        self.seq += 1;
        format!(
            "{{\"case\":\"{}\",\"seq\":{},\"depth\":{}",
            self.case, self.seq, self.depth
        )
    }

    fn line(&mut self, tail: &str) {
        let head = self.head();
        self.out.push(format!("{head}{tail}"));
    }

    fn name_of(&self, id: ControllerId) -> String {
        self.names
            .get(&id)
            .cloned()
            .unwrap_or_else(|| "?".to_owned())
    }

    fn resolve(&self, reference: &str, me: ControllerId) -> ControllerId {
        match reference {
            "self" => me,
            "all" => ControllerId(0),
            "ghost" => GHOST,
            name => self.flids.get(name).copied().unwrap_or(GHOST),
        }
    }

    /// One action; `false` for `fail`.
    fn act(&mut self, a: &[String], me: ControllerId) -> bool {
        let a: Vec<&str> = a.iter().map(String::as_str).collect();
        match a.as_slice() {
            ["fail"] => {
                self.line(",\"op\":\"fail\"}");
                return false;
            }
            ["send", to, msg, tag] => {
                let (msg, tag) = (parse_uint(msg) as i32, parse_uint(tag));
                self.line(&format!(
                    ",\"op\":\"send\",\"to\":\"{to}\",\"msg\":{msg},\"tag\":{tag}}}"
                ));
                let target = self.resolve(to, me);
                let mut m = Msg::Test { msg, tag };
                let r = match controller::dispatch(self, target, &mut m) {
                    Ok(()) => 0,
                    Err(e) => e.code(),
                };
                self.line(&format!(",\"ret\":\"send\",\"r\":{r}}}"));
            }
            ["broadcast", msg, tag] => {
                let (msg, tag) = (parse_uint(msg) as i32, parse_uint(tag));
                self.line(&format!(
                    ",\"op\":\"broadcast\",\"msg\":{msg},\"tag\":{tag}}}"
                ));
                let mut m = Msg::Test { msg, tag };
                let _ = controller::dispatch(self, ControllerId(0), &mut m);
            }
            ["create", name, prio] => {
                self.line(&format!(
                    ",\"op\":\"create\",\"name\":\"{name}\",\"prio\":\"{prio}\"}}"
                ));
                let instance = format!("Scripted:{name}");
                let id = controller::create(
                    self,
                    b"SimClientStub.dll",
                    instance.as_bytes(),
                    &(),
                    parse_priority(prio),
                );
                if id.0 != 0 {
                    self.flids.insert((*name).to_owned(), id);
                    self.names.insert(id, (*name).to_owned());
                }
                self.line(&format!(",\"ret\":\"create\",\"flid\":{}}}", id.0));
            }
            ["destroy", to] => {
                self.line(&format!(",\"op\":\"destroy\",\"to\":\"{to}\"}}"));
                let target = self.resolve(to, me);
                controller::destroy(self, target);
            }
            ["heartbeat", to, interval] => {
                let interval = parse_float(interval);
                self.line(&format!(
                    ",\"op\":\"heartbeat\",\"to\":\"{to}\",\"interval\":\"{}\"}}",
                    bits(interval)
                ));
                let target = self.resolve(to, me);
                let r = match controller::set_heartbeat(self, target, interval) {
                    Ok(()) => 0,
                    Err(e) => e.code(),
                };
                self.line(&format!(",\"ret\":\"heartbeat\",\"r\":{r}}}"));
            }
            ["timer", to, p1, p2, timeout] => {
                let (p1, p2, timeout) = (parse_uint(p1), parse_uint(p2), parse_float(timeout));
                self.line(&format!(
                    ",\"op\":\"timer\",\"to\":\"{to}\",\"p1\":{p1},\"p2\":{p2},\"timeout\":\"{}\"}}",
                    bits(timeout)
                ));
                let target = self.resolve(to, me);
                let r = match self.bus.set_timer(target, p1, p2, timeout) {
                    Ok(()) => 0,
                    Err(e) => e.code(),
                };
                self.line(&format!(",\"ret\":\"timer\",\"r\":{r}}}"));
            }
            _ => panic!("controller_bus.list: bad directive {}", a.join(" ")),
        }
        true
    }

    fn run_rules(&mut self, me: ControllerId, name: &str, event: &str, filter: &str) -> bool {
        let mut keep = true;
        for index in 0..self.rules.len() {
            let rule = &self.rules[index];
            let matches = rule.inst == name
                && rule.event == event
                && (rule.filter == "*" || rule.filter == filter)
                && (rule.limit == 0 || rule.fired < rule.limit);
            if !matches {
                continue;
            }
            self.rules[index].fired += 1;
            let action = self.rules[index].action.clone();
            if !self.act(&action, me) {
                keep = false;
            }
        }
        keep
    }

    fn state(&mut self) {
        let nodes: Vec<(ControllerNode<Kind>, bool)> = self
            .bus
            .active()
            .iter()
            .map(|n| (n.clone(), false))
            .chain(self.bus.deferred().iter().map(|n| (n.clone(), true)))
            .collect();
        let mut text = String::new();
        for (i, (node, deferred)) in nodes.iter().enumerate() {
            if i > 0 {
                text.push(',');
            }
            let _ = write!(
                text,
                "{{\"name\":\"{}\",\"flid\":{},\"prio\":{},\"interval\":\"{}\",\"phase\":\"{}\",\"deferred\":{}}}",
                self.name_of(node.id),
                node.id.0,
                node.priority.value(),
                bits(node.heartbeat_period),
                bits(node.heartbeat_accum),
                deferred
            );
        }
        self.line(&format!(",\"state\":[{text}]}}"));
    }

    fn top_level(&mut self, a: &[String]) {
        let words: Vec<&str> = a.iter().map(String::as_str).collect();
        match words.as_slice() {
            ["tick", dt] => {
                let dt = parse_float(dt);
                self.line(&format!(",\"op\":\"tick\",\"dt\":\"{}\"}}", bits(dt)));
                controller::tick(self, dt);
                controller::fire_timers(self, dt);
            }
            ["shutdown"] => {
                self.line(",\"op\":\"shutdown\"}");
                controller::shutdown(self);
            }
            ["state"] => self.state(),
            _ => {
                self.act(a, ControllerId(0));
            }
        }
    }
}

impl BusHost for Host {
    type Kind = Kind;
    type Message<'m> = Msg;
    type CreateParams = ();

    fn bus(&mut self) -> &mut ControllerBus<Kind> {
        &mut self.bus
    }

    fn deliver(&mut self, id: ControllerId, kind: Kind, message: &mut Msg) {
        self.depth += 1;
        let name = self.instances[kind.0].clone();
        match *message {
            Msg::Heartbeat(elapsed) => {
                self.line(&format!(
                    ",\"to\":\"{name}\",\"flid\":{},\"msg\":{HEARTBEAT},\"elapsed\":\"{}\"}}",
                    id.0,
                    bits(elapsed)
                ));
                self.run_rules(id, &name, "heartbeat", "*");
            }
            Msg::Timer(p1, p2) => {
                self.line(&format!(
                    ",\"to\":\"{name}\",\"flid\":{},\"msg\":{TIMER_EXPIRED},\"p1\":{p1},\"p2\":{p2}}}",
                    id.0
                ));
                self.run_rules(id, &name, "timer", &p1.to_string());
            }
            Msg::Destroyed(destroyed) => {
                self.line(&format!(
                    ",\"to\":\"{name}\",\"flid\":{},\"msg\":{CONTROLLER_DESTROYED},\"id\":{}}}",
                    id.0, destroyed.0
                ));
                let filter = self.name_of(destroyed);
                self.run_rules(id, &name, "destroyed", &filter);
            }
            Msg::Test { msg, tag } => {
                self.line(&format!(
                    ",\"to\":\"{name}\",\"flid\":{},\"msg\":{msg},\"tag\":{tag}}}",
                    id.0
                ));
                self.run_rules(id, &name, &msg.to_string(), &tag.to_string());
            }
        }
        self.depth -= 1;
    }

    fn instantiate(
        &mut self,
        id: ControllerId,
        _dll: &[u8],
        name: &[u8],
        _params: &(),
    ) -> Option<Kind> {
        let name = String::from_utf8_lossy(name).into_owned();
        let short = name
            .strip_prefix("Scripted:")
            .expect("scripted instance")
            .to_owned();
        self.depth += 1;
        self.flids.insert(short.clone(), id);
        self.names.insert(id, short.clone());
        self.line(&format!(",\"ctor\":\"{short}\",\"flid\":{}}}", id.0));
        let keep = self.run_rules(id, &short, "ctor", "*");
        self.depth -= 1;
        keep.then(|| {
            self.instances.push(short);
            Kind(self.instances.len() - 1)
        })
    }

    fn destroy_instance(&mut self, id: ControllerId, kind: Kind) {
        self.depth += 1;
        let name = self.instances[kind.0].clone();
        self.line(&format!(",\"dtor\":\"{name}\",\"flid\":{}}}", id.0));
        self.run_rules(id, &name, "dtor", "*");
        self.depth -= 1;
    }

    fn allocate_id(&mut self) -> u32 {
        self.ids.allocate()
    }

    fn rand(&mut self) -> i32 {
        self.rand.rand()
    }

    fn heartbeat_message(elapsed: f32) -> Msg {
        Msg::Heartbeat(elapsed)
    }

    fn timer_expired_message(param1: u32, param2: u32) -> Msg {
        Msg::Timer(param1, param2)
    }

    fn controller_destroyed_message(id: ControllerId) -> Msg {
        Msg::Destroyed(id)
    }
}

fn run(list: &str) -> Vec<String> {
    let mut host = Host::default();
    for line in list.lines() {
        if line.starts_with('#') {
            continue;
        }
        let a: Vec<String> = line.split_whitespace().map(str::to_owned).collect();
        if a.is_empty() {
            continue;
        }
        if a[0] == "case" && (a.len() == 2 || a.len() == 3) {
            host.case = a[1].clone();
            host.seq = 0;
            host.rules.clear();
            host.flids.clear();
            host.names.clear();
            host.rand.seed(a.get(2).map_or(1, |s| parse_uint(s)));
        } else if a[0] == "on" && a.len() >= 6 {
            host.rules.push(Rule {
                inst: a[1].clone(),
                event: a[2].clone(),
                filter: a[3].clone(),
                limit: parse_uint(&a[4]),
                fired: 0,
                action: a[5..].to_vec(),
            });
        } else {
            host.top_level(&a);
        }
    }
    host.out
}

#[test]
fn controller_bus_vectors() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests");
    let list = fs::read_to_string(root.join("fixtures/oracle/controller_bus.list"))
        .expect("controller_bus.list");
    let lines = run(&list);
    let path = root.join("vectors/controller_bus.jsonl");
    let rendered: String = lines.iter().map(|l| format!("{l}\n")).collect();
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, rendered).expect("write controller_bus.jsonl");
        return;
    }
    let committed = fs::read_to_string(&path).expect("controller_bus.jsonl");
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(&lines).enumerate() {
        assert_eq!(*want, got, "controller_bus.jsonl line {}", index + 1);
    }
    assert_eq!(
        committed.len(),
        lines.len(),
        "controller_bus.jsonl: line count"
    );
}
