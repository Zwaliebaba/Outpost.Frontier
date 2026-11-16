//! Workspace tooling. Run with `cargo xtask <command>`.
#![forbid(unsafe_code)]

mod graph;
mod ini;
mod lexer;
mod scan;
mod workspace;

use std::path::Path;
use std::process::ExitCode;

const USAGE: &str = "\
usage: cargo xtask <command>

commands:
  check-policy   check the crate graph, lint settings, legacy API use and
                 the placement of unsafe code
";

fn usage() -> String {
    format!("{USAGE}{}", ini::USAGE)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("check-policy") if args.len() == 1 => check_policy(),
        Some("ini") => ini::run(&args[1..]),
        Some("help" | "--help" | "-h") => {
            println!("{}", usage());
            ExitCode::SUCCESS
        }
        _ => {
            eprintln!("{}", usage());
            ExitCode::from(2)
        }
    }
}

fn check_policy() -> ExitCode {
    let root = workspace_root();
    let workspace = match workspace::Workspace::load(root) {
        Ok(workspace) => workspace,
        Err(error) => {
            eprintln!("check-policy: {error}");
            return ExitCode::FAILURE;
        }
    };

    let mut findings = graph::check(&workspace);
    let scan = scan::scan_workspace(&workspace);
    findings.extend(scan.findings);

    if findings.is_empty() {
        println!(
            "check-policy: ok ({} crates, {} files scanned)",
            workspace.members.len(),
            scan.files
        );
        ExitCode::SUCCESS
    } else {
        for finding in &findings {
            eprintln!("{finding}");
        }
        eprintln!("check-policy: {} problem(s)", findings.len());
        ExitCode::FAILURE
    }
}

fn workspace_root() -> &'static Path {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives one level below the workspace root")
}
