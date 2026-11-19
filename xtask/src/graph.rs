//! The crate dependency rules from AGENTS.md and design/migration-plan.md
//! section 3.

use std::fmt;

use toml::{Table, Value};

use crate::workspace::{Member, Workspace};

/// Workspace crates each member may depend on. A member missing from this
/// table is a policy failure: adding a crate means deciding its place in
/// the graph and recording it in AGENTS.md.
const ALLOWED: &[(&str, &[&str])] = &[
    ("neuron-core", &[]),
    ("lua32-compiler", &["neuron-core"]),
    ("game-sim", &["neuron-core"]),
    ("neuron-client", &["neuron-core"]),
    ("neuron-server", &["neuron-core"]),
    ("game-logic", &["neuron-core", "game-sim", "neuron-server"]),
    ("game-app", &["neuron-core", "game-sim", "neuron-client"]),
    (
        "outpost-frontier",
        &[
            "neuron-core",
            "game-sim",
            "neuron-client",
            "neuron-server",
            "game-logic",
            "game-app",
        ],
    ),
    (
        "outpost-server",
        &["neuron-core", "game-sim", "neuron-server", "game-logic"],
    ),
    ("xtask", &["neuron-core", "lua32-compiler"]),
];

/// Workspace crates a member may use in its tests only: edges allowed in
/// `dev-dependencies` (plain or target-specific) and refused anywhere
/// else.
const DEV_ONLY: &[(&str, &[&str])] = &[("neuron-client", &["lua32-compiler"])];

const DEPENDENCY_TABLES: &[&str] = &["dependencies", "dev-dependencies", "build-dependencies"];

#[derive(Debug, PartialEq, Eq)]
pub struct Finding {
    pub location: String,
    pub message: String,
}

impl fmt::Display for Finding {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.location, self.message)
    }
}

pub fn check(workspace: &Workspace) -> Vec<Finding> {
    let names: Vec<&str> = workspace.members.iter().map(|m| m.name.as_str()).collect();
    let mut findings = Vec::new();
    for member in &workspace.members {
        check_member(member, &names, &mut findings);
    }
    findings
}

fn check_member(member: &Member, workspace_names: &[&str], findings: &mut Vec<Finding>) {
    let location = format!("{}/Cargo.toml", member.dir);

    if !inherits(&member.manifest, &["lints"]) {
        findings.push(Finding {
            location: location.clone(),
            message: "must contain `[lints] workspace = true`".into(),
        });
    }
    if !inherits(&member.manifest, &["package", "rust-version"]) {
        findings.push(Finding {
            location: location.clone(),
            message: "must contain `rust-version.workspace = true`".into(),
        });
    }

    let Some(allowed) = ALLOWED
        .iter()
        .find(|(name, _)| *name == member.name)
        .map(|(_, deps)| *deps)
    else {
        findings.push(Finding {
            location,
            message: format!(
                "crate `{}` has no place in the crate graph; add it to xtask/src/graph.rs and AGENTS.md",
                member.name
            ),
        });
        return;
    };

    let dev_only = DEV_ONLY
        .iter()
        .find(|(name, _)| *name == member.name)
        .map_or(&[][..], |(_, deps)| *deps);

    for (table_name, dependency) in dependencies(&member.manifest) {
        let dependency_name = dependency.as_str();
        let is_dev = table_name.ends_with("dev-dependencies");
        if workspace_names.contains(&dependency_name)
            && !allowed.contains(&dependency_name)
            && !(is_dev && dev_only.contains(&dependency_name))
        {
            findings.push(Finding {
                location: location.clone(),
                message: format!(
                    "`{}` may not depend on `{dependency}` ({table_name})",
                    member.name
                ),
            });
        }
    }
}

/// Whether `[a.b] workspace = true` (or `a.b.workspace = true`) is set.
fn inherits(manifest: &Table, path: &[&str]) -> bool {
    let mut value = None;
    let mut table = manifest;
    for key in path {
        value = table.get(*key);
        match value.and_then(Value::as_table) {
            Some(inner) => table = inner,
            None => return false,
        }
    }
    value
        .and_then(|v| v.get("workspace"))
        .and_then(Value::as_bool)
        .unwrap_or(false)
}

/// Every dependency of a manifest as (table, package name), including
/// target-specific tables and renamed dependencies.
fn dependencies(manifest: &Table) -> Vec<(String, String)> {
    let mut found = Vec::new();
    collect(manifest, "", &mut found);
    if let Some(targets) = manifest.get("target").and_then(Value::as_table) {
        for (target, table) in targets {
            if let Some(table) = table.as_table() {
                collect(table, &format!("target.{target}."), &mut found);
            }
        }
    }
    found
}

fn collect(table: &Table, prefix: &str, found: &mut Vec<(String, String)>) {
    for kind in DEPENDENCY_TABLES {
        let Some(entries) = table.get(*kind).and_then(Value::as_table) else {
            continue;
        };
        for (key, spec) in entries {
            let package = spec
                .get("package")
                .and_then(Value::as_str)
                .unwrap_or(key)
                .to_owned();
            found.push((format!("{prefix}{kind}"), package));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn member(name: &str, manifest: &str) -> Member {
        Member {
            dir: format!("crates/{name}"),
            name: name.into(),
            manifest: manifest.parse().unwrap(),
        }
    }

    const NAMES: &[&str] = &[
        "neuron-core",
        "neuron-client",
        "game-logic",
        "game-app",
        "lua32-compiler",
    ];

    const HEADER: &str = "[package]\nname = \"x\"\nrust-version.workspace = true\n\
                          [lints]\nworkspace = true\n";

    fn findings(member: &Member) -> Vec<Finding> {
        let mut findings = Vec::new();
        check_member(member, NAMES, &mut findings);
        findings
    }

    #[test]
    fn allowed_edge_passes() {
        let m = member(
            "game-app",
            &format!("{HEADER}[dependencies]\nneuron-client.workspace = true\n"),
        );
        assert!(findings(&m).is_empty());
    }

    #[test]
    fn client_may_not_depend_on_server_rules() {
        let m = member(
            "game-app",
            &format!("{HEADER}[dependencies]\ngame-logic.workspace = true\n"),
        );
        let found = findings(&m);
        assert_eq!(found.len(), 1);
        assert!(found[0].message.contains("may not depend on `game-logic`"));
    }

    #[test]
    fn renamed_and_target_specific_dependencies_are_seen() {
        let m = member(
            "neuron-core",
            &format!(
                "{HEADER}[target.'cfg(windows)'.dev-dependencies]\n\
                 client = {{ package = \"neuron-client\", path = \"../neuron-client\" }}\n"
            ),
        );
        let found = findings(&m);
        assert_eq!(found.len(), 1);
        assert!(
            found[0]
                .message
                .contains("target.cfg(windows).dev-dependencies")
        );
    }

    #[test]
    fn dev_only_edge_passes_in_dev_dependencies_only() {
        for table in ["dev-dependencies", "target.'cfg(windows)'.dev-dependencies"] {
            let m = member(
                "neuron-client",
                &format!("{HEADER}[{table}]\nlua32-compiler.workspace = true\n"),
            );
            assert!(findings(&m).is_empty(), "{table}");
        }
        for table in ["dependencies", "build-dependencies"] {
            let m = member(
                "neuron-client",
                &format!("{HEADER}[{table}]\nlua32-compiler.workspace = true\n"),
            );
            let found = findings(&m);
            assert_eq!(found.len(), 1, "{table}");
            assert!(
                found[0]
                    .message
                    .contains("may not depend on `lua32-compiler`")
            );
        }
        // A dev-only edge of one crate is no edge of another.
        let m = member(
            "game-app",
            &format!("{HEADER}[dev-dependencies]\nlua32-compiler.workspace = true\n"),
        );
        assert_eq!(findings(&m).len(), 1);
    }

    #[test]
    fn external_dependencies_are_not_graph_edges() {
        let m = member(
            "neuron-core",
            &format!("{HEADER}[dependencies]\ntoml = \"0.9\"\n"),
        );
        assert!(findings(&m).is_empty());
    }

    #[test]
    fn missing_lint_inheritance_is_reported() {
        let m = member(
            "neuron-core",
            "[package]\nname = \"x\"\nrust-version.workspace = true\n",
        );
        let found = findings(&m);
        assert_eq!(found.len(), 1);
        assert!(found[0].message.contains("[lints]"));
    }

    #[test]
    fn unknown_crate_is_reported() {
        let m = member("game-extra", HEADER);
        let found = findings(&m);
        assert_eq!(found.len(), 1);
        assert!(found[0].message.contains("no place in the crate graph"));
    }
}
