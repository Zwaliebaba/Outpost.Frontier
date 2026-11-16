//! Reading the workspace manifests.

use std::fmt;
use std::fs;
use std::path::{Path, PathBuf};

use toml::{Table, Value};

/// The workspace root manifest and its members.
#[derive(Debug)]
pub struct Workspace {
    pub root: PathBuf,
    pub root_manifest: PathBuf,
    pub members: Vec<Member>,
}

/// One workspace member.
#[derive(Debug)]
pub struct Member {
    /// Directory relative to the workspace root, with `/` separators.
    pub dir: String,
    pub name: String,
    pub manifest: Table,
}

#[derive(Debug)]
pub struct LoadError(String);

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl Workspace {
    pub fn load(root: &Path) -> Result<Self, LoadError> {
        let root_manifest = root.join("Cargo.toml");
        let table = read_table(&root_manifest)?;
        let members = table
            .get("workspace")
            .and_then(|w| w.get("members"))
            .and_then(Value::as_array)
            .ok_or_else(|| LoadError("Cargo.toml has no workspace.members".into()))?;

        let mut loaded = Vec::new();
        for member in members {
            let dir = member
                .as_str()
                .ok_or_else(|| LoadError("workspace.members holds a non-string".into()))?;
            if dir.contains(['*', '?', '[']) {
                return Err(LoadError(format!(
                    "workspace member pattern `{dir}` is not supported; list members explicitly"
                )));
            }
            let manifest = read_table(&root.join(dir).join("Cargo.toml"))?;
            let name = manifest
                .get("package")
                .and_then(|p| p.get("name"))
                .and_then(Value::as_str)
                .ok_or_else(|| LoadError(format!("{dir}/Cargo.toml has no package.name")))?
                .to_owned();
            loaded.push(Member {
                dir: dir.replace('\\', "/"),
                name,
                manifest,
            });
        }

        Ok(Self {
            root: root.to_path_buf(),
            root_manifest,
            members: loaded,
        })
    }
}

fn read_table(path: &Path) -> Result<Table, LoadError> {
    let text = fs::read_to_string(path)
        .map_err(|e| LoadError(format!("cannot read {}: {e}", path.display())))?;
    text.parse::<Table>()
        .map_err(|e| LoadError(format!("cannot parse {}: {e}", path.display())))
}
