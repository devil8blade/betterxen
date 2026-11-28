//! Runs the built `bx-cli` binary to check what `main` prints and returns for the
//! usage paths (#532). The unit tests cover `Dispatch::classify`; these cover the
//! mapping from that classification to the real exit code and output stream.

use std::process::Command;

fn run(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_bx-cli"))
        .args(args)
        .env_clear()
        .output()
        .expect("bx-cli must start")
}

#[test]
fn help_prints_usage_on_stdout_and_succeeds() {
    for flag in ["--help", "-h", "help"] {
        let out = run(&[flag]);
        assert!(out.status.success(), "{flag}: {:?}", out.status);
        let stdout = String::from_utf8_lossy(&out.stdout);
        assert!(
            stdout.contains("bx-cli run --exe"),
            "{flag}: usage on stdout: {stdout}"
        );
        assert!(out.stderr.is_empty(), "{flag}: nothing on stderr");
    }
}

#[test]
fn a_bare_invocation_is_a_usage_error_on_stderr() {
    let out = run(&[]);
    assert_eq!(out.status.code(), Some(1));
    assert!(out.stdout.is_empty(), "nothing on stdout");
    assert!(String::from_utf8_lossy(&out.stderr).contains("bx-cli run --exe"));
}

#[test]
fn an_unknown_subcommand_is_a_usage_error_on_stderr() {
    let out = run(&["frobnicate"]);
    assert_eq!(out.status.code(), Some(1));
    assert!(out.stdout.is_empty(), "nothing on stdout");
    assert!(String::from_utf8_lossy(&out.stderr).contains("bx-cli run --exe"));
}
