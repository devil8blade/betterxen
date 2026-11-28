//! Track A (M1-A1): Native ARM64 macOS CLI for BetterXen hybrid execution.

mod drives;
mod stage;
mod supervise;

use std::collections::BTreeMap;
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::time::{Duration, Instant};

use bx_probe::{ShimTrack, classify_dll, probe_pe32_plus};
use bx_telemetry::{ShaderGap, ShaderGapGroup, TraceSummary, TrapCount, summarize_jsonl};
use dxbc_accc::inspect_dxbc;
use supervise::{Outcome, Stop};

const SHIM_DLL_OVERRIDES: &str = "bx_runtime_veh,d3d12_xs,xg_xs,xmem,xcrdapi,XCRDAPI,xgameruntime,acphal,AcpHal,xapu,chad,gameinput,GameInput,\
api-ms-win-core-psm-appnotify-l1-1-0,api-ms-win-core-psm-appnotify-l1-1-1,\
ext-ms-win-rtcore-ntuser-message-l1-1-0,ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0,\
ext-ms-win-rtcore-ntuser-window-l1-1-0,ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0=n,b";

/// Accepted `--backend` values. CrossOver ignores an unknown value silently, so one is refused.
const BACKENDS: [&str; 4] = ["d3dmetal", "dxmt", "dxvk", "wined3d"];

const SAFE_CHILD_PATH: &str = "/usr/bin:/bin:/usr/sbin:/sbin";

const HOST_ENV_ALLOWLIST: &[&str] = &[
    "HOME",
    "USER",
    "LOGNAME",
    "TMPDIR",
    "LANG",
    "LC_ALL",
    "TERM",
    "DISPLAY",
    "XPC_FLAGS",
    "XPC_SERVICE_NAME",
];

fn usage_text() -> &'static str {
    "BetterXen Hybrid CLI (bx-cli)\n\
     \n\
     Usage:\n\
       bx-cli doctor                         Inspect Apple Silicon host, Rosetta 2, CrossOver & mingw-w64\n\
       bx-cli probe <pe32+_path>             Classify PE32+ imports by BetterXen track (A-E)\n\
       bx-cli shader <dxbc_blob_path>        Classify a DXBC shader container (DXIL vs ACCC/FSPX)\n\
       bx-cli trace <log.jsonl>              Summarize bx_runtime_veh JSONL diagnostics\n\
       bx-cli run --exe <path> [--shims <dir>] [--bottle <name> | --prefix <dir>] [--profile lockhart|anaconda] [--log <path>] [--inherit-env] [--timeout <secs>] [--backend d3dmetal|dxmt|dxvk|wined3d (needs --bottle)]\n\
       (shims default to build-shims/bin; a timeout or signal stops the whole bottle or prefix)\n"
}

fn run_capture(program: &str, args: &[&str]) -> Option<String> {
    let out = Command::new(program).args(args).output().ok()?;
    if out.status.success() {
        Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
    } else {
        None
    }
}

fn cmd_doctor() -> ExitCode {
    let arch = run_capture("uname", &["-m"]).unwrap_or_else(|| "unknown".into());
    let macos = run_capture("sw_vers", &["-productVersion"]).unwrap_or_else(|| "unknown".into());
    let cpu = run_capture("sysctl", &["-n", "machdep.cpu.brand_string"])
        .unwrap_or_else(|| "unknown".into());
    let mem_bytes: u64 = run_capture("sysctl", &["-n", "hw.memsize"])
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let mem_gb = mem_bytes / (1024 * 1024 * 1024);
    let default_profile = if mem_gb >= 32 { "anaconda" } else { "lockhart" };

    let crossover_ver = run_capture(
        "defaults",
        &[
            "read",
            "/Applications/CrossOver.app/Contents/Info.plist",
            "CFBundleShortVersionString",
        ],
    );
    let d3dmetal_exists = Path::new(
        "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib64/apple_gptk/external/D3DMetal.framework",
    )
    .exists();
    let mingw = run_capture("which", &["x86_64-w64-mingw32-g++"]);
    let rosetta = Command::new("pgrep")
        .args(["-q", "oahd"])
        .status()
        .is_ok_and(|s| s.success());

    println!("=== BetterXen Host Doctor ===");
    println!("Host arch        : {arch}");
    println!("CPU              : {cpu}");
    println!("macOS            : {macos}");
    println!("Unified memory   : {mem_gb} GiB (recommended profile: {default_profile})");
    println!(
        "Rosetta 2 (oahd) : {}",
        if rosetta { "active" } else { "not detected" }
    );
    println!(
        "CrossOver        : {}",
        crossover_ver.as_deref().unwrap_or("not installed")
    );
    println!(
        "D3DMetal.fwk     : {}",
        if d3dmetal_exists {
            "present"
        } else {
            "not found"
        }
    );
    println!(
        "mingw-w64 g++    : {}",
        mingw
            .as_deref()
            .unwrap_or("missing (brew install mingw-w64)")
    );
    ExitCode::SUCCESS
}

fn cmd_probe(path: &str) -> ExitCode {
    let data = match fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let summary = match probe_pe32_plus(&data) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::FAILURE;
        }
    };
    let total_syms: usize = summary.imports.values().map(Vec::len).sum();
    println!("ImageBase        : 0x{:x}", summary.image_base);
    println!("SizeOfImage      : {} bytes", summary.size_of_image);
    println!("EntryPoint RVA   : 0x{:x}", summary.entry_point_rva);
    println!("Sections         : {}", summary.section_count);
    println!(
        "Static Imports   : {} DLLs, {} symbols",
        summary.imports.len(),
        total_syms
    );

    let mut by_track: BTreeMap<ShimTrack, Vec<(&String, usize)>> = BTreeMap::new();
    for (dll, syms) in &summary.imports {
        by_track
            .entry(classify_dll(dll))
            .or_default()
            .push((dll, syms.len()));
    }
    for (track, dlls) in by_track {
        println!("\n[{track}] ({} DLLs):", dlls.len());
        for (dll, cnt) in dlls {
            println!("  - {dll} ({cnt})");
        }
    }
    ExitCode::SUCCESS
}

fn format_clwb_line(clwb_events: usize, clwb_trap_lower_bound: Option<u64>) -> String {
    if let Some(lower_bound) = clwb_trap_lower_bound {
        format!("clwb VEH traps     : at least {lower_bound} (sampled)")
    } else if clwb_events == 0 {
        "clwb VEH traps     : 0".to_string()
    } else {
        format!("clwb VEH samples   : {clwb_events}")
    }
}

fn format_amd_trap_line(name: &str, count: &TrapCount) -> String {
    let label = format!("{name} VEH traps");
    match count.lower_bound {
        Some(lower_bound) => format!("{label:<18} : at least {lower_bound} (sampled)"),
        None => format!("{label:<18} : {} samples", count.events),
    }
}

/// True when the log carried any shader-gate evidence. A log without it must render exactly as
/// it did before the gate summary existed.
fn has_shader_gate(summary: &TraceSummary) -> bool {
    summary.shader_gate.total > 0
        || !summary.shader_gaps.is_empty()
        || !summary.shader_bridge_missing.is_empty()
}

/// One `label : value` line in the same column as the other trace lines.
fn format_shader_line(label: &str, value: impl std::fmt::Display) -> String {
    format!("{label:<19} : {value}\n")
}

/// A titled block of `{count:5}x  {name}` lines, or nothing when there is nothing to count.
fn format_count_list(title: &str, counts: &BTreeMap<String, usize>) -> String {
    if counts.is_empty() {
        return String::new();
    }
    let mut out = format!("\n{title}:\n");
    for (name, count) in counts {
        let _ = writeln!(out, "  {count:5}x  {name}");
    }
    out
}

/// One `shader_d3dmetal_gap` record: the pipeline, stage, wave width it needs, and the 64-bit
/// atomic feature flags that made it a gap. Missing fields say `unknown` rather than being lost.
fn format_shader_gap_line(gap: &ShaderGap) -> String {
    format!(
        "  pso={} stage={} wave_lanes_min={} atomic64_flags={}\n",
        gap.pso.as_deref().unwrap_or("unknown"),
        gap.stage.as_deref().unwrap_or("unknown"),
        gap.wave_lanes_min.as_deref().unwrap_or("unknown"),
        gap.atomic64_flags.as_deref().unwrap_or("unknown"),
    )
}

/// One `(pso, stage)` group of gap records, with its count and every distinct constraint set
/// (#590). Missing fields say `unknown` rather than being lost.
fn format_shader_gap_group(group: &ShaderGapGroup) -> String {
    let mut out = format!(
        "  {:5}x  pso={} stage={}\n",
        group.count,
        group.pso.as_deref().unwrap_or("unknown"),
        group.stage.as_deref().unwrap_or("unknown"),
    );
    for variant in &group.variants {
        let _ = writeln!(
            out,
            "          wave_lanes_min={} atomic64_flags={} ({}x)",
            variant.wave_lanes_min.as_deref().unwrap_or("unknown"),
            variant.atomic64_flags.as_deref().unwrap_or("unknown"),
            variant.count,
        );
    }
    out
}

/// One line per gate record whose decision is not a pass, naming the pipeline,
/// stage, decision and HRESULT. These are the records a reader needs to find a
/// blocker; the counted breakdown alone does not say which pipeline failed
/// (#497 review point 5).
fn format_non_pass_decisions(gate: &bx_telemetry::ShaderGateSummary) -> String {
    let failing: Vec<&bx_telemetry::ShaderGateRecord> = gate
        .records
        .iter()
        .filter(|record| {
            record
                .decision
                .as_deref()
                .is_some_and(|decision| !decision.starts_with("pass_"))
        })
        .collect();
    if failing.is_empty() {
        return String::new();
    }
    let mut out = format!("\nNon-pass pipeline decisions ({}):\n", failing.len());
    for record in failing {
        let _ = writeln!(
            out,
            "  pso={} stage={} decision={} hr={}",
            record.pso.as_deref().unwrap_or("unknown"),
            record.stage.as_deref().unwrap_or("unknown"),
            record.decision.as_deref().unwrap_or("unknown"),
            record.hr.as_deref().unwrap_or("unknown"),
        );
    }
    out
}

/// The shader-gate section, or an empty string for a log with no gate evidence.
fn format_shader_gate_section(summary: &TraceSummary) -> String {
    if !has_shader_gate(summary) {
        return String::new();
    }
    let gate = &summary.shader_gate;
    let mut out = String::from("\nShader gate:\n");
    out.push_str(&format_shader_line("shader_gate records", gate.total));
    if gate.unparsed > 0 {
        out.push_str(&format_shader_line("unparsed details", gate.unparsed));
    }
    if gate.missing_decision > 0 {
        out.push_str(&format_shader_line(
            "records w/o decision",
            gate.missing_decision,
        ));
    }
    out.push_str(&format_count_list("decisions", &gate.decisions));
    out.push_str(&format_count_list("kinds", &gate.kinds));
    out.push_str(&format_count_list("stages", &gate.stages));
    out.push_str(&format_non_pass_decisions(gate));
    if !summary.shader_gaps.is_empty() {
        let _ = writeln!(out, "\nD3DMetal gaps ({}):", summary.shader_gaps.len());
        for gap in &summary.shader_gaps {
            out.push_str(&format_shader_gap_line(gap));
        }
        // #590: the same records grouped by pipeline and stage, so a reader can tell which
        // pipelines are created-but-not-run. Additive: the per-record list above is unchanged.
        let _ = writeln!(
            out,
            "\nD3DMetal gaps by kind/stage ({}):",
            summary.shader_gap_groups.len()
        );
        for group in &summary.shader_gap_groups {
            out.push_str(&format_shader_gap_group(group));
        }
        if summary.shader_gaps_missing_identity > 0 {
            out.push_str(&format_shader_line(
                "gaps w/o pso/stage",
                summary.shader_gaps_missing_identity,
            ));
        }
        if summary.shader_gaps_missing_constraints > 0 {
            out.push_str(&format_shader_line(
                "gaps w/o constraints",
                summary.shader_gaps_missing_constraints,
            ));
        }
    }
    if !summary.shader_bridge_missing.is_empty() {
        let _ = writeln!(
            out,
            "\nShader bridge missing ({}):",
            summary.shader_bridge_missing.len()
        );
        for message in &summary.shader_bridge_missing {
            let _ = writeln!(out, "  {message}");
        }
    }
    out
}

fn cmd_trace(path: &str) -> ExitCode {
    let text = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    print!("{}", render_trace(&text));
    ExitCode::SUCCESS
}

fn render_trace(text: &str) -> String {
    let mut out = String::new();
    let summary = summarize_jsonl(text);
    let _ = writeln!(out, "Trace events       : {}", summary.total_lines);
    let _ = writeln!(
        out,
        "{}",
        format_clwb_line(summary.clwb_events, summary.clwb_trap_lower_bound)
    );
    for (name, count) in &summary.amd_traps {
        let _ = writeln!(out, "{}", format_amd_trap_line(name, count));
    }
    let _ = writeln!(out, "ADX illegal ops    : {}", summary.adx_errors);
    if !summary.unhandled_exceptions.is_empty() {
        let _ = writeln!(
            out,
            "\nUnhandled exceptions ({}):",
            summary.unhandled_exceptions.len()
        );
        for crash in &summary.unhandled_exceptions {
            let _ = writeln!(
                out,
                "  code={} module_base={} + RVA={} RIP={}",
                crash.exception_code.as_deref().unwrap_or("unknown"),
                crash.module_base.as_deref().unwrap_or("unknown"),
                crash.rva.as_deref().unwrap_or("unknown"),
                crash.rip.as_deref().unwrap_or("unknown"),
            );
        }
    }
    if !summary.unimpl_apis.is_empty() {
        let _ = writeln!(out, "\nUnimplemented APIs:");
        for (api, count) in &summary.unimpl_apis {
            let _ = writeln!(out, "  {count:5}x  {api}");
        }
    }
    if !summary.unimpl_vtables.is_empty() {
        let _ = writeln!(out, "\nUnimplemented COM vtable slots:");
        for (slot, count) in &summary.unimpl_vtables {
            let _ = writeln!(out, "  {count:5}x  {slot}");
        }
    }
    out.push_str(&format_shader_gate_section(&summary));
    out
}

fn unix_to_wine_z_path(path: &Path) -> String {
    let s = path.to_string_lossy();
    format!("Z:{}", s.replace('/', "\\"))
}

fn collect_isolated_host_env<F>(mut lookup: F) -> BTreeMap<String, String>
where
    F: FnMut(&str) -> Option<String>,
{
    let mut env_map = BTreeMap::new();
    env_map.insert("PATH".to_string(), SAFE_CHILD_PATH.to_string());
    for &key in HOST_ENV_ALLOWLIST {
        if let Some(val) = lookup(key) {
            env_map.insert(key.to_string(), val);
        }
    }
    env_map
}

#[derive(Debug)]
struct RunArgs {
    exe: PathBuf,
    shims_dir: PathBuf,
    profile: String,
    bottle: Option<String>,
    prefix: Option<PathBuf>,
    log: Option<PathBuf>,
    inherit_env: bool,
    timeout: Option<Duration>,
    backend: Option<String>,
}

fn parse_run_args(args: &[String]) -> Result<RunArgs, String> {
    let mut exe_path: Option<PathBuf> = None;
    let mut shims_dir = PathBuf::from("build-shims/bin");
    let mut profile = String::from("lockhart");
    let mut bottle: Option<String> = None;
    let mut wine_prefix: Option<PathBuf> = None;
    let mut log_file: Option<PathBuf> = None;
    let mut inherit_env = false;
    let mut timeout = None;
    let mut backend: Option<String> = None;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--exe" if i + 1 < args.len() => {
                exe_path = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--shims" if i + 1 < args.len() => {
                shims_dir = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            "--profile" if i + 1 < args.len() => {
                profile = args[i + 1].clone();
                i += 2;
            }
            "--bottle" if i + 1 < args.len() => {
                bottle = Some(args[i + 1].clone());
                i += 2;
            }
            "--prefix" if i + 1 < args.len() => {
                wine_prefix = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--log" if i + 1 < args.len() => {
                log_file = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--timeout" if i + 1 < args.len() => {
                let seconds: u64 = args[i + 1]
                    .parse()
                    .ok()
                    .filter(|s| *s > 0)
                    .ok_or("--timeout needs a whole number of seconds above 0")?;
                timeout = Some(Duration::from_secs(seconds));
                i += 2;
            }
            "--backend" if i + 1 < args.len() => {
                let name = args[i + 1].clone();
                if !BACKENDS.contains(&name.as_str()) {
                    return Err(format!(
                        "unknown backend {name:?}; use one of {}",
                        BACKENDS.join(", ")
                    ));
                }
                backend = Some(name);
                i += 2;
            }
            "--inherit-env" => {
                inherit_env = true;
                i += 1;
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }

    if let Some(name) = &bottle
        && (name.is_empty() || name.starts_with(['-', '.']) || name.contains('/'))
    {
        return Err(format!("invalid bottle name {name:?}"));
    }
    if backend.is_some() && bottle.is_none() {
        // A bare prefix loads Wine's own D3D12 whatever CX_GRAPHICS_BACKEND says (observed on
        // CrossOver 26.3), so accepting it would claim a backend that is not in use.
        return Err("--backend needs --bottle; a bare prefix cannot be made to use it".into());
    }
    Ok(RunArgs {
        exe: exe_path.ok_or("error: --exe <path> is required")?,
        shims_dir,
        profile,
        bottle,
        prefix: wine_prefix,
        log: log_file,
        inherit_env,
        timeout,
        backend,
    })
}

/// Asks CrossOver's installed `CXBottle` module where `--bottle` points, so cleanup targets the
/// prefix the launch uses. `bin/wine` searches `CX_BOTTLE_PATH`, configured paths and managed
/// bottles, so a fixed directory would be wrong for some bottles.
const BOTTLE_PREFIX_QUERY: &str = r#"
use lib "$ENV{CX_ROOT}/lib/perl";
require CXBottle;
CXBottle::get_crossover_config();
my ($scope, $found) = CXBottle::setup_bottle_wineprefix(undef);
die "$@\n" unless defined $scope;
print $ENV{WINEPREFIX};
"#;

/// The environment `bin/wine` starts from, shared by the launch and the bottle lookup.
fn apply_host_env(command: &mut Command, inherit_env: bool) {
    if !inherit_env {
        command.env_clear();
        for (k, v) in collect_isolated_host_env(|k| env::var(k).ok()) {
            command.env(k, v);
        }
    }
    // Inherited values must not point the launcher at a different bottle than cleanup does.
    command.env_remove("WINEPREFIX").env_remove("CX_BOTTLE");
}

fn resolve_bottle_prefix(
    cx_root: &Path,
    mut command: Command,
    name: &str,
) -> Result<PathBuf, String> {
    let output = command
        .env("CX_ROOT", cx_root)
        .env("CX_BOTTLE", name)
        .args(["-e", BOTTLE_PREFIX_QUERY])
        .output()
        .map_err(|e| format!("cannot ask CrossOver where bottle {name:?} is: {e}"))?;
    let path = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());
    if !output.status.success() || !path.is_absolute() || !path.is_dir() {
        return Err(format!(
            "CrossOver cannot locate bottle {name:?}: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(path)
}

fn resolve_bottle_prefix_with_host_env(
    cx_root: &Path,
    name: &str,
    inherit_env: bool,
) -> Result<PathBuf, String> {
    let mut query = Command::new("/usr/bin/perl");
    apply_host_env(&mut query, inherit_env);
    resolve_bottle_prefix(cx_root, query, name)
}

/// Collects the prefixes that cleanup must stop. Re-resolving a named bottle after launch catches
/// the case where a managed-only bottle caused CrossOver's `bin/wine` to create and launch inside
/// a new private stub bottle.
fn cleanup_prefixes_for_stop<F>(
    initial_prefix: &Path,
    bottle: Option<&str>,
    mut re_resolve: F,
) -> Result<Vec<PathBuf>, String>
where
    F: FnMut(&str) -> Result<PathBuf, String>,
{
    let mut prefixes = Vec::new();
    if let Some(name) = bottle {
        let current = re_resolve(name)?;
        prefixes.push(current);
    }
    if !prefixes.iter().any(|p| p == initial_prefix) {
        prefixes.push(initial_prefix.to_path_buf());
    }
    Ok(prefixes)
}

/// The prefix a named bottle resolves to now, if that is no longer `staged_prefix`.
///
/// For a managed-only bottle CrossOver's `bin/wine` may create a private stub bottle and launch
/// the guest there. Shims staged before launch then sit in a prefix the guest never used; this
/// detects that so the run can say so instead of reporting "staged" (#474).
fn staging_prefix_mismatch<F>(
    staged_prefix: &Path,
    bottle: Option<&str>,
    mut re_resolve: F,
) -> Result<Option<PathBuf>, String>
where
    F: FnMut(&str) -> Result<PathBuf, String>,
{
    let Some(name) = bottle else {
        return Ok(None);
    };
    let current = re_resolve(name)?;
    let same = |a: &Path, b: &Path| {
        a == b || matches!((fs::canonicalize(a), fs::canonicalize(b)), (Ok(x), Ok(y)) if x == y)
    };
    Ok((!same(&current, staged_prefix)).then_some(current))
}

/// What is left of `--timeout` for the guest after initialization took `used`. `Ok(None)` means
/// no limit. An error means initialization used all of it: the guest must not be started only to
/// be reported as timed out, as if it had hung (#474).
fn guest_time_left(timeout: Option<Duration>, used: Duration) -> Result<Option<Duration>, String> {
    match timeout {
        None => Ok(None),
        Some(limit) if used < limit => Ok(Some(limit - used)),
        Some(limit) => Err(format!(
            "initialization (prefix setup and shim staging) took {:.1}s and used the whole \
             --timeout of {:.1}s; the guest was not started",
            used.as_secs_f64(),
            limit.as_secs_f64()
        )),
    }
}

/// `bin/wine` deletes an inherited `WINEDLLOVERRIDES` and applies the bottle's
/// `[EnvironmentVariables]` over what it is given, so bottle mode passes both as options.
fn bottle_options(backend: Option<&str>) -> Vec<String> {
    let mut options = vec!["--dll".to_string(), SHIM_DLL_OVERRIDES.to_string()];
    if let Some(backend) = backend {
        options.push("--env".to_string());
        options.push(format!("CX_GRAPHICS_BACKEND={backend}"));
    }
    options
}

/// The CrossOver Wine environment for a bare `--prefix` (bottles get theirs from `bin/wine`).
fn apply_prefix_env(command: &mut Command, cx_root: &Path, prefix: &Path, wineloader: &Path) {
    command
        .env("WINEPREFIX", prefix)
        .env("WINESERVER", cx_root.join("bin/wineserver"))
        .env("WINELOADER", wineloader)
        .env(
            "WINEDLLPATH",
            format!(
                "{}:{}",
                cx_root.join("lib/wine").display(),
                cx_root.join("lib64/wine").display()
            ),
        )
        .env(
            "DYLD_FALLBACK_LIBRARY_PATH",
            format!(
                "{}:{}:/usr/lib",
                cx_root.join("lib64").display(),
                cx_root.join("lib").display()
            ),
        );
}

/// Writes one diagnostic line to stderr. A closed pipe or hung-up terminal is ignored: unlike
/// `eprintln!`, this cannot panic and so cannot cut a run's cleanup short (#461).
fn diag(message: impl std::fmt::Display) {
    let _ = writeln!(std::io::stderr(), "{message}");
}

/// Runs `cleanup` and only then reports the stop and any cleanup failures to `out`, ignoring
/// write errors. True if cleanup reported a failure.
///
/// Cleanup comes first so that nothing about the diagnostic stream (Ctrl-C through `| tee`, a
/// closed terminal) can prevent the environment from being stopped (#461).
fn report_stop(
    stop: Stop,
    cleanup: impl FnOnce() -> Vec<String>,
    out: &mut dyn std::io::Write,
) -> bool {
    let failures = cleanup();
    let _ = match stop {
        Stop::TimedOut => writeln!(out, "timed out; stopped the environment"),
        Stop::Signalled(signal) => {
            writeln!(out, "received signal {signal}; stopped the environment")
        }
    };
    for failure in &failures {
        let _ = writeln!(out, "{failure}");
    }
    !failures.is_empty()
}

/// Stops the environment's `wineserver` after a timeout or signal, then reports it. True if
/// cleanup failed.
fn stop_environment(
    stop: Stop,
    cx_root: &Path,
    environment_dir: &Path,
    bottle: Option<&str>,
    inherit_env: bool,
) -> bool {
    let cleanup = || {
        let server_env: Vec<(String, String)> = collect_isolated_host_env(|k| env::var(k).ok())
            .into_iter()
            .collect();
        let mut failures = Vec::new();
        let prefixes_to_stop = match cleanup_prefixes_for_stop(environment_dir, bottle, |name| {
            resolve_bottle_prefix_with_host_env(cx_root, name, inherit_env)
        }) {
            Ok(prefixes) => prefixes,
            Err(e) => {
                failures.push(format!("cleanup failed to re-resolve bottle prefix: {e}"));
                vec![environment_dir.to_path_buf()]
            }
        };
        for prefix in &prefixes_to_stop {
            if let Err(e) =
                supervise::stop_server(&cx_root.join("bin/wineserver"), prefix, &server_env)
            {
                failures.push(format!(
                    "cleanup failed, programs may still be running: {e}"
                ));
            }
        }
        failures
    };
    report_stop(stop, cleanup, &mut std::io::stderr())
}

/// How many trailing bytes of a log are remembered to recognize it again after the run.
const LOG_MARK_TAIL: u64 = 256;

/// What a trace log was before the run: which file, how long, and how it ended.
#[derive(Debug, Clone, PartialEq, Eq)]
struct LogMark {
    /// Device and inode of the file.
    identity: (u64, u64),
    len: u64,
    /// The last bytes before `len` (up to [`LOG_MARK_TAIL`]).
    tail: Vec<u8>,
}

/// Opens `path` for reading only if it is a regular file.
///
/// `--log` can name anything. Opening a FIFO nobody writes to, or reading a terminal to its end,
/// would block the run before the guest starts or after it has finished, so the open never
/// waits and anything that is not a regular file is treated as having no log to summarize.
fn open_regular_log(path: &Path) -> Option<(fs::File, fs::Metadata)> {
    use std::os::unix::fs::OpenOptionsExt;
    let file = fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .ok()?;
    // Checked on the open handle, so the file cannot be swapped between the check and the read.
    let meta = file.metadata().ok()?;
    meta.is_file().then_some((file, meta))
}

/// The mark of the log at `path`, or `None` if there is no readable regular-file log yet.
fn log_mark(path: &Path) -> Option<LogMark> {
    use std::io::{Read, Seek, SeekFrom};
    use std::os::unix::fs::MetadataExt;
    let (mut file, meta) = open_regular_log(path)?;
    let len = meta.len();
    let tail_len = len.min(LOG_MARK_TAIL);
    file.seek(SeekFrom::Start(len - tail_len)).ok()?;
    let mut tail = Vec::new();
    file.take(tail_len).read_to_end(&mut tail).ok()?;
    Some(LogMark {
        identity: (meta.dev(), meta.ino()),
        len,
        tail,
    })
}

/// A log's identity and contents, read through one open file so the two cannot disagree.
fn read_log(path: &Path) -> Option<((u64, u64), Vec<u8>)> {
    use std::io::Read;
    use std::os::unix::fs::MetadataExt;
    let (mut file, meta) = open_regular_log(path)?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes).ok()?;
    Some(((meta.dev(), meta.ino()), bytes))
}

/// The part of a trace log that this run added, given the log's mark from before the run.
///
/// The old length is used as an offset only when the log is still the same file and still holds
/// the same bytes just before that offset. A log that was replaced or rewritten during the run,
/// whatever its new size, is new in full, as is a log that did not exist before.
fn appended_since<'a>(bytes: &'a [u8], identity: (u64, u64), before: Option<&LogMark>) -> &'a [u8] {
    let Some(mark) = before else {
        return bytes;
    };
    let appended = usize::try_from(mark.len).ok().and_then(|start| {
        let old_end = bytes.get(..start)?;
        (mark.identity == identity && old_end.ends_with(&mark.tail)).then(|| &bytes[start..])
    });
    appended.unwrap_or(bytes)
}

/// The part of `elapsed` wall-clock time that counts against `--timeout`: everything except the
/// `suspended` time spent stopped by Ctrl-Z, whichever phase of the run that fell in.
fn active_time(elapsed: Duration, suspended: Duration) -> Duration {
    elapsed.saturating_sub(suspended)
}

fn cmd_run(args: &[String]) -> ExitCode {
    let run = match parse_run_args(args) {
        Ok(run) => run,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };
    let RunArgs {
        exe,
        shims_dir,
        profile,
        bottle,
        prefix: wine_prefix,
        log: log_file,
        inherit_env,
        timeout,
        backend,
    } = run;

    let cx_root = Path::new("/Applications/CrossOver.app/Contents/SharedSupport/CrossOver");
    let wine_wrapper = cx_root.join("bin/wine");
    let wineloader = cx_root.join("bin/wineloader");
    if !wineloader.exists() {
        eprintln!("error: CrossOver not found under /Applications/CrossOver.app");
        return ExitCode::FAILURE;
    }

    let abs_shims = fs::canonicalize(&shims_dir).unwrap_or(shims_dir);
    let abs_exe = fs::canonicalize(&exe).unwrap_or(exe);
    let wine_shims_path = unix_to_wine_z_path(&abs_shims);

    let resolved_prefix =
        wine_prefix.unwrap_or_else(|| PathBuf::from("/tmp/betterxen-wine-prefix"));

    // The environment a timeout or signal has to stop.
    let environment_dir = match &bottle {
        Some(name) => match resolve_bottle_prefix_with_host_env(cx_root, name, inherit_env) {
            Ok(path) => path,
            Err(e) => {
                eprintln!("error: {e}");
                return ExitCode::FAILURE;
            }
        },
        None => resolved_prefix.clone(),
    };

    // Handled from here on, so that prefix initialization is cleaned up like the guest is.
    supervise::install_signal_handlers();
    let started = Instant::now();

    // Drive links are prefix-global. Keep them stable until this guest exits so another bx-cli
    // process cannot retarget G: underneath a running title. Taken before system32 staging, so
    // a run refused here has not touched the shared prefix. Runs sharing a prefix are therefore
    // serial: a second launcher on the same prefix is refused while the first is running.
    let _drive_lock = match drives::lock_drive_mappings(&environment_dir) {
        Ok(lock) => lock,
        Err(e) => {
            diag(format_args!("error: {e}"));
            return ExitCode::FAILURE;
        }
    };

    // #457: shims loaded with LOAD_LIBRARY_SEARCH_SYSTEM32 never see WINEPATH, so they are
    // copied into the environment's system32 first. A bare prefix is initialized for that.
    let system32 = stage::system32_dir(&environment_dir);
    if bottle.is_none() && !system32.is_dir() && stage::has_system32_shims(&abs_shims) {
        let _ = fs::create_dir_all(&resolved_prefix);
        let mut boot = Command::new(&wineloader);
        boot.args(["wineboot", "--init"]);
        apply_host_env(&mut boot, inherit_env);
        apply_prefix_env(&mut boot, cx_root, &resolved_prefix, &wineloader);
        boot.env("WINEDEBUG", "-all");
        match supervise::supervise(boot, timeout, supervise::received_signal) {
            Ok(Outcome::Exited(st)) if st.success() => {}
            Ok(Outcome::Exited(st)) => {
                diag(format_args!(
                    "error: wineboot --init failed for {}: {st}",
                    resolved_prefix.display()
                ));
                return ExitCode::FAILURE;
            }
            Ok(outcome @ Outcome::Stopped(stop)) => {
                let failed = stop_environment(stop, cx_root, &environment_dir, None, inherit_env);
                let code = outcome.exit_code();
                return ExitCode::from(if failed { code.max(1) } else { code });
            }
            Err(e) => {
                diag(format_args!(
                    "error: cannot run wineboot for {}: {e}",
                    resolved_prefix.display()
                ));
                return ExitCode::FAILURE;
            }
        }
    }
    let mut staged_names: Vec<String> = Vec::new();
    match stage::stage_system32(&abs_shims, &system32) {
        Ok(staged) => {
            for item in &staged {
                if let stage::Staged::Copied(path)
                | stage::Staged::Replaced(path)
                | stage::Staged::ReplacedWineStub(path)
                | stage::Staged::Unchanged(path) = item
                    && let Some(name) = path.file_name()
                {
                    staged_names.push(name.to_string_lossy().into_owned());
                }
                diag(format_args!("bx-cli: {}", item.describe()));
            }
        }
        Err(e) => {
            let hint = if bottle.is_some() && !system32.is_dir() {
                " (start the bottle once so CrossOver creates it)"
            } else {
                ""
            };
            diag(format_args!(
                "error: cannot stage system32 shims: {e}{hint}"
            ));
            return ExitCode::FAILURE;
        }
    }

    // An Xbox title opens its package on G: and writes temporary files to T:. A missing drive
    // only matters to such a title, so it is reported and the run goes on.
    match abs_exe.parent() {
        Some(package_dir) => match drives::map_xbox_drives(&environment_dir, package_dir) {
            Ok(mappings) => {
                for mapping in &mappings {
                    match mapping {
                        drives::Mapping::Foreign { .. } => diag(mapping.describe()),
                        _ => diag(format_args!("bx-cli: {}", mapping.describe())),
                    }
                }
            }
            Err(e) => diag(format_args!(
                "warning: Xbox drives G: and T: not mapped: {e}"
            )),
        },
        None => diag("warning: Xbox drive G: not mapped: the executable has no parent directory"),
    }

    let mut cmd = if let Some(ref bname) = bottle {
        let mut c = Command::new(&wine_wrapper);
        c.args(["--bottle", bname])
            .args(bottle_options(backend.as_deref()))
            .arg(&abs_exe);
        c
    } else {
        let _ = fs::create_dir_all(&resolved_prefix);
        let mut c = Command::new(&wineloader);
        c.arg(&abs_exe);
        c
    };

    apply_host_env(&mut cmd, inherit_env);

    if bottle.is_none() {
        apply_prefix_env(&mut cmd, cx_root, &resolved_prefix, &wineloader);
    }

    cmd.env("ROSETTA_ADVERTISE_AVX", "1")
        .env("WINEDLLOVERRIDES", SHIM_DLL_OVERRIDES)
        .env("WINEPATH", &wine_shims_path)
        .env("BX_PROFILE", &profile)
        .env("WINEDEBUG", "-all");

    let abs_log = log_file.map(|lf| {
        if lf.is_absolute() {
            lf
        } else {
            env::current_dir().unwrap_or_default().join(lf)
        }
    });
    if let Some(log) = &abs_log {
        cmd.env("BX_LOG_FILE", unix_to_wine_z_path(log));
    }
    let log_before = abs_log.as_deref().and_then(log_mark);

    // `--timeout` bounds the whole run, including any prefix initialization above, but not
    // time the run spent suspended in either phase.
    let active = active_time(started.elapsed(), supervise::suspended_time());
    let remaining = match guest_time_left(timeout, active) {
        Ok(remaining) => remaining,
        Err(e) => {
            diag(format_args!("timed out during initialization: {e}"));
            return ExitCode::from(124);
        }
    };
    let outcome = supervise::supervise(cmd, remaining, supervise::received_signal);
    let code = match &outcome {
        Ok(outcome) => outcome.exit_code(),
        Err(e) => {
            diag(format_args!("failed to run CrossOver wine: {e}"));
            1
        }
    };
    let mut cleanup_failed = false;
    match &outcome {
        Ok(Outcome::Exited(status)) if !status.success() => {
            diag(format_args!("guest exited with status: {status}"));
        }
        Ok(Outcome::Stopped(stop)) => {
            cleanup_failed = stop_environment(
                *stop,
                cx_root,
                &environment_dir,
                bottle.as_deref(),
                inherit_env,
            );
        }
        _ => {}
    }

    // Printed for failed and stopped runs too, which is when it matters most. The log is
    // appended to across runs, so only what this run added is summarized.
    if let Some(log) = &abs_log
        && let Some((identity, bytes)) = read_log(log)
    {
        let fresh = appended_since(&bytes, identity, log_before.as_ref());
        if !fresh.is_empty() {
            // Guest text may not be valid UTF-8; a summary is still worth printing. A closed
            // stdout must not turn into a panic after the run.
            let _ = write!(
                std::io::stdout(),
                "\nTrace summary of this run in {}:\n{}",
                log.display(),
                render_trace(&String::from_utf8_lossy(fresh))
            );
        }
    }
    // #474: the shims were staged into the prefix resolved before launch. If CrossOver launched
    // the bottle from a different prefix that does not expose them, the guest never saw them,
    // and the run must not pass quietly. A different prefix alone is not a failure: a private
    // stub of a managed bottle links its system32 entries back to the managed prefix.
    let mut staged_elsewhere = false;
    if !staged_names.is_empty() {
        match staging_prefix_mismatch(&environment_dir, bottle.as_deref(), |name| {
            resolve_bottle_prefix_with_host_env(cx_root, name, inherit_env)
        }) {
            Ok(None) => {}
            Ok(Some(launched)) => {
                let missing = stage::shims_not_visible_in(
                    &system32,
                    &stage::system32_dir(&launched),
                    &staged_names,
                );
                if missing.is_empty() {
                    diag(format_args!(
                        "bx-cli: bottle launched in {}; the shims staged in {} are visible there",
                        launched.display(),
                        environment_dir.display()
                    ));
                } else {
                    staged_elsewhere = true;
                    diag(format_args!(
                        "error: system32 shims were staged in {} but CrossOver launched the \
                         bottle in {}, where {} cannot be found as staged; the guest did not \
                         see them. Run again: staging will now use the prefix the bottle \
                         launches in",
                        environment_dir.display(),
                        launched.display(),
                        missing.join(", ")
                    ));
                }
            }
            Err(e) => diag(format_args!(
                "warning: cannot confirm which prefix the bottle launched in, so the staged \
                 system32 shims may not have been used: {e}"
            )),
        }
    }
    // The reason for the stop is kept; a failed cleanup or misplaced staging only turns success
    // into failure.
    ExitCode::from(if cleanup_failed || staged_elsewhere {
        code.max(1)
    } else {
        code
    })
}

fn cmd_shader(path: &str) -> ExitCode {
    let data = match fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    match inspect_dxbc(&data) {
        Ok(info) => {
            println!("Kind             : {:?}", info.kind);
            println!(
                "Stage / SM       : {:?} ({}.{})",
                info.stage, info.shader_model_major, info.shader_model_minor
            );
            println!("Container size   : {} bytes", info.container_size);
            println!("DXIL chunk       : {} bytes", info.dxil_size);
            println!(
                "ACCC chunk       : {} bytes (FSPX={})",
                info.accc_size, info.accc_is_fspx
            );
            println!("Root signature   : {}", info.has_root_signature);
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}

/// What a bare invocation of `bx-cli` should do. Split out from `main` so the
/// choice is testable without spawning a process.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Dispatch<'a> {
    /// An explicit `--help` / `-h` / `help`: usage on stdout, succeed.
    Help,
    /// Nothing was asked for: usage on stderr, fail as a usage error.
    NoArgs,
    /// An unrecognised subcommand: usage on stderr, fail.
    Unknown(&'a str),
    /// A real subcommand.
    Sub(&'a str),
}

impl<'a> Dispatch<'a> {
    fn classify(args: &'a [String]) -> Self {
        let Some(sub) = args.first() else {
            return Dispatch::NoArgs;
        };
        match sub.as_str() {
            "--help" | "-h" | "help" => Dispatch::Help,
            "doctor" | "probe" | "shader" | "trace" | "run" => Dispatch::Sub(sub.as_str()),
            _ => Dispatch::Unknown(sub.as_str()),
        }
    }

    /// True when the usage text goes to stdout rather than stderr.
    fn usage_on_stdout(self) -> bool {
        matches!(self, Dispatch::Help)
    }

    /// True when the invocation must not report success.
    fn is_usage_error(self) -> bool {
        matches!(self, Dispatch::NoArgs | Dispatch::Unknown(_))
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    let dispatch = Dispatch::classify(&args);
    if dispatch.usage_on_stdout() {
        print!("{}", usage_text());
    } else if dispatch.is_usage_error() {
        eprint!("{}", usage_text());
    }
    match dispatch {
        Dispatch::Help => ExitCode::SUCCESS,
        Dispatch::NoArgs | Dispatch::Unknown(_) => ExitCode::FAILURE,
        Dispatch::Sub(sub) => match sub {
            "doctor" => cmd_doctor(),
            "probe" if args.len() >= 2 => cmd_probe(&args[1]),
            "shader" if args.len() >= 2 => cmd_shader(&args[1]),
            "trace" if args.len() >= 2 => cmd_trace(&args[1]),
            "run" => cmd_run(&args[1..]),
            // Only reachable when a subcommand that needs an argument got none.
            _ => {
                eprint!("{}", usage_text());
                ExitCode::FAILURE
            }
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A stream whose reader is gone: every write fails, as with EPIPE or EIO.
    struct ClosedStream<'a> {
        cleanup_done: &'a std::cell::Cell<bool>,
        writes_before_cleanup: usize,
        writes: usize,
    }

    impl std::io::Write for ClosedStream<'_> {
        fn write(&mut self, _buf: &[u8]) -> std::io::Result<usize> {
            self.writes += 1;
            if !self.cleanup_done.get() {
                self.writes_before_cleanup += 1;
            }
            Err(std::io::Error::from(std::io::ErrorKind::BrokenPipe))
        }

        fn flush(&mut self) -> std::io::Result<()> {
            Err(std::io::Error::from(std::io::ErrorKind::BrokenPipe))
        }
    }

    #[test]
    fn a_closed_diagnostic_stream_cannot_skip_cleanup() {
        for stop in [Stop::Signalled(libc::SIGINT), Stop::TimedOut] {
            let cleanup_done = std::cell::Cell::new(false);
            let mut closed = ClosedStream {
                cleanup_done: &cleanup_done,
                writes_before_cleanup: 0,
                writes: 0,
            };
            let failed = report_stop(
                stop,
                || {
                    cleanup_done.set(true);
                    vec!["cleanup failed, programs may still be running: stub".to_string()]
                },
                &mut closed,
            );
            assert!(cleanup_done.get(), "cleanup did not run for {stop:?}");
            assert!(failed, "the cleanup failure was lost for {stop:?}");
            assert!(closed.writes > 0, "nothing was reported for {stop:?}");
            assert_eq!(
                closed.writes_before_cleanup, 0,
                "output came before cleanup"
            );
        }
        let mut text = Vec::new();
        assert!(!report_stop(Stop::Signalled(2), Vec::new, &mut text));
        assert_eq!(
            String::from_utf8(text).unwrap(),
            "received signal 2; stopped the environment\n"
        );
    }

    fn mark(identity: (u64, u64), contents: &str) -> LogMark {
        let bytes = contents.as_bytes();
        let tail_start = bytes.len().saturating_sub(LOG_MARK_TAIL as usize);
        LogMark {
            identity,
            len: bytes.len() as u64,
            tail: bytes[tail_start..].to_vec(),
        }
    }

    #[test]
    fn the_trace_summary_covers_only_what_this_run_appended() {
        let file = (1, 100);
        let earlier = "{\"event\":\"unimpl_api\",\"detail\":\"old.dll!Earlier\"}\n";
        let this_run = "{\"event\":\"unimpl_api\",\"detail\":\"new.dll!ThisRun\"}\n";
        let log = format!("{earlier}{this_run}");
        let before = mark(file, earlier);
        let fresh = appended_since(log.as_bytes(), file, Some(&before));
        assert_eq!(fresh, this_run.as_bytes());
        let rendered = render_trace(&String::from_utf8_lossy(fresh));
        assert!(rendered.contains("new.dll!ThisRun"), "{rendered}");
        assert!(!rendered.contains("old.dll!Earlier"), "{rendered}");
        // Nothing appended: nothing to summarize.
        assert!(appended_since(earlier.as_bytes(), file, Some(&before)).is_empty());
        // No log before the run: everything in it is from this run.
        assert_eq!(
            appended_since(this_run.as_bytes(), file, None),
            this_run.as_bytes()
        );
    }

    #[test]
    fn a_replaced_log_is_summarized_in_full_whatever_its_size() {
        let old_file = (1, 100);
        let new_file = (1, 200);
        let old = "{\"event\":\"unimpl_api\",\"detail\":\"old.dll!Earlier\"}\n";
        let before = mark(old_file, old);
        let record = "{\"event\":\"unimpl_api\",\"detail\":\"new.dll!A\"}\n";
        // Shorter, exactly as long, and longer than the old log: the old length must never be
        // used as an offset into a different file.
        let shorter = &record[..record.len() - 1];
        let same_size = format!("{:<width$}\n", record.trim_end(), width = old.len() - 1);
        let longer = format!("{record}{record}{record}");
        assert!(shorter.len() < old.len() && same_size.len() == old.len());
        assert!(longer.len() > old.len());
        for replacement in [shorter, same_size.as_str(), longer.as_str()] {
            let fresh = appended_since(replacement.as_bytes(), new_file, Some(&before));
            assert_eq!(fresh, replacement.as_bytes(), "{} bytes", replacement.len());
        }
        // Rewritten in place (same file, different contents before the old end).
        let fresh = appended_since(longer.as_bytes(), old_file, Some(&before));
        assert_eq!(fresh, longer.as_bytes());
        assert_eq!(
            summarize_jsonl(&String::from_utf8_lossy(fresh)).total_lines,
            3
        );
    }

    #[test]
    fn a_different_file_is_new_in_full_even_when_it_starts_with_the_old_bytes() {
        // Only the file identity can tell these apart: the replacement begins with exactly the
        // old log's bytes and then has more, so the bytes before the old end still match.
        let old = "{\"event\":\"unimpl_api\",\"detail\":\"old.dll!Earlier\"}\n";
        let more = "{\"event\":\"unimpl_api\",\"detail\":\"new.dll!ThisRun\"}\n";
        let replacement = format!("{old}{more}");
        let before = mark((1, 100), old);
        let fresh = appended_since(replacement.as_bytes(), (1, 200), Some(&before));
        assert_eq!(fresh, replacement.as_bytes());
        // Control: the same bytes in the same file are an append, and only the rest is new.
        let appended = appended_since(replacement.as_bytes(), (1, 100), Some(&before));
        assert_eq!(appended, more.as_bytes());
    }

    #[test]
    fn a_log_path_that_is_not_a_regular_file_never_blocks_the_run() {
        let dir = env::temp_dir().join(format!("bx-cli-lognonfile-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let fifo = dir.join("trace.fifo");
        let made = Command::new("/usr/bin/mkfifo").arg(&fifo).status().unwrap();
        assert!(made.success());
        // A FIFO with no writer: a plain open for reading would wait here forever.
        let (tx, rx) = std::sync::mpsc::channel();
        let probe = fifo.clone();
        std::thread::spawn(move || {
            let _ = tx.send((log_mark(&probe), read_log(&probe)));
        });
        let (mark_of_fifo, contents_of_fifo) = rx
            .recv_timeout(Duration::from_secs(5))
            .expect("reading a FIFO log path blocked");
        assert_eq!(mark_of_fifo, None);
        assert_eq!(contents_of_fifo, None);
        // A directory and a missing path are not logs either; a regular file still is.
        assert_eq!(log_mark(&dir), None);
        assert_eq!(read_log(&dir), None);
        assert_eq!(log_mark(&dir.join("absent")), None);
        let real = dir.join("trace.jsonl");
        fs::write(&real, "{}\n").unwrap();
        assert_eq!(log_mark(&real).map(|m| m.len), Some(3));
        assert_eq!(
            read_log(&real).map(|(_, bytes)| bytes),
            Some(b"{}\n".to_vec())
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn log_marks_follow_the_file_on_disk() {
        let dir = env::temp_dir().join(format!("bx-cli-logmark-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let log = dir.join("trace.jsonl");
        assert_eq!(log_mark(&log), None);
        let old = "{\"event\":\"unimpl_api\",\"detail\":\"old.dll!Earlier\"}\n";
        fs::write(&log, old).unwrap();
        let before = log_mark(&log).unwrap();
        assert_eq!(before.len, old.len() as u64);
        assert_eq!(before.tail, old.as_bytes());

        // Appended to, as bx_runtime_veh does: only the new record is this run's.
        let added = "{\"event\":\"unimpl_api\",\"detail\":\"new.dll!ThisRun\"}\n";
        fs::write(&log, format!("{old}{added}")).unwrap();
        let (identity, bytes) = read_log(&log).unwrap();
        assert_eq!(
            appended_since(&bytes, identity, Some(&before)),
            added.as_bytes()
        );

        // Replaced by a new, longer file during the run: all of it is this run's.
        let replacement = dir.join("replacement");
        let longer = format!("{added}{added}{added}");
        fs::write(&replacement, &longer).unwrap();
        fs::rename(&replacement, &log).unwrap();
        let (identity, bytes) = read_log(&log).unwrap();
        assert_ne!(identity, before.identity);
        let fresh = appended_since(&bytes, identity, Some(&before));
        assert_eq!(
            summarize_jsonl(&String::from_utf8_lossy(fresh)).total_lines,
            3
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn a_pause_in_an_earlier_phase_is_not_charged_to_the_guest() {
        let limit = Some(Duration::from_secs(2));
        let ms = Duration::from_millis;
        // As reviewed: paused 2.5 s during wineboot with --timeout 2, 0.3 s actually running.
        assert_eq!(
            guest_time_left(limit, active_time(ms(2800), ms(2500))),
            Ok(Some(ms(1700)))
        );
        // Without a pause the wall clock counts in full.
        assert_eq!(
            guest_time_left(limit, active_time(ms(500), Duration::ZERO)),
            Ok(Some(ms(1500)))
        );
        // Initialization that really ran for the whole timeout still stops the launch.
        assert!(guest_time_left(limit, active_time(ms(5000), ms(2500))).is_err());
        // More suspension than elapsed time cannot go negative.
        assert_eq!(active_time(ms(100), ms(900)), Duration::ZERO);
    }

    #[test]
    fn staging_into_a_prefix_the_bottle_did_not_launch_in_is_reported() {
        let resolved_before = Path::new("/bottles/managed/corp");
        let stub = PathBuf::from("/bottles/private/corp");
        // CrossOver created a private stub during launch: the bottle now resolves there.
        let moved = staging_prefix_mismatch(resolved_before, Some("corp"), |name| {
            assert_eq!(name, "corp");
            Ok(stub.clone())
        });
        assert_eq!(moved, Ok(Some(stub)));
        // The usual case: the bottle still resolves to where the shims went.
        let same = staging_prefix_mismatch(resolved_before, Some("corp"), |_| {
            Ok(resolved_before.to_path_buf())
        });
        assert_eq!(same, Ok(None));
        // A bare --prefix is launched exactly where it was staged; nothing is re-resolved.
        let bare = staging_prefix_mismatch(resolved_before, None, |_| {
            panic!("a bare prefix must not be re-resolved")
        });
        assert_eq!(bare, Ok(None));
        // A resolver failure is passed on, not mistaken for a match.
        let failed = staging_prefix_mismatch(resolved_before, Some("corp"), |_| {
            Err("cannot locate".to_string())
        });
        assert_eq!(failed, Err("cannot locate".to_string()));
    }

    #[test]
    fn the_guest_is_not_started_when_initialization_used_the_whole_timeout() {
        let limit = Some(Duration::from_secs(30));
        assert_eq!(guest_time_left(None, Duration::from_secs(999)), Ok(None));
        assert_eq!(
            guest_time_left(limit, Duration::from_secs(12)),
            Ok(Some(Duration::from_secs(18)))
        );
        for used in [Duration::from_secs(30), Duration::from_secs(45)] {
            let err = guest_time_left(limit, used).unwrap_err();
            assert!(err.contains("initialization"), "{err}");
            assert!(err.contains("guest was not started"), "{err}");
        }
    }

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parses_run_arguments() {
        for bare in [&["--prefix", "/tmp/p"][..], &[]] {
            let mut list = args(&["--exe", "a.exe", "--backend", "d3dmetal"]);
            list.extend(args(bare));
            let error = parse_run_args(&list).unwrap_err();
            assert!(error.contains("--backend needs --bottle"), "{error}");
        }
        let run = parse_run_args(&args(&[
            "--exe",
            "a.exe",
            "--bottle",
            "b",
            "--timeout",
            "30",
            "--backend",
            "d3dmetal",
        ]))
        .unwrap();
        assert_eq!(run.timeout, Some(Duration::from_secs(30)));
        assert_eq!(run.backend.as_deref(), Some("d3dmetal"));
        assert_eq!(run.shims_dir, PathBuf::from("build-shims/bin"));
        assert_eq!(run.profile, "lockhart");
        let plain = parse_run_args(&args(&["--exe", "a.exe"])).unwrap();
        assert!(plain.timeout.is_none() && plain.bottle.is_none() && plain.prefix.is_none());
        for bad in [
            &["--exe", "a.exe", "--timeout", "0"][..],
            &["--exe", "a.exe", "--timeout", "soon"],
            &["--exe", "a.exe", "--backend", "dmxt2"],
            &["--exe", "a.exe", "--bottle", "../x"],
            &["--exe", "a.exe", "--timeout"],
            &["--bottle", "b"],
        ] {
            assert!(parse_run_args(&args(bad)).is_err(), "{bad:?}");
        }
    }

    // A stub CXBottle stands in for CrossOver's: this checks the lookup's environment and that
    // a timeout's cleanup is aimed at the prefix it returns, not at the default directory.
    #[test]
    fn a_timeout_cleans_up_a_bottle_outside_the_default_directory() {
        use std::os::unix::fs::PermissionsExt;
        let root = env::temp_dir().join(format!("bx-cli-altbottle-{}", std::process::id()));
        let bottle = root.join("elsewhere/mine");
        fs::create_dir_all(&bottle).unwrap();
        fs::create_dir_all(root.join("cx/lib/perl")).unwrap();
        fs::write(
            root.join("cx/lib/perl/CXBottle.pm"),
            r#"package CXBottle;
sub get_crossover_config {}
sub setup_bottle_wineprefix {
    my $p = "$ENV{CX_BOTTLE_PATH}/$ENV{CX_BOTTLE}";
    return (undef, undef) unless -d $p;
    $ENV{WINEPREFIX} = $p;
    return ('private', $p);
}
1;
"#,
        )
        .unwrap();
        let query = |name: &str| {
            let mut command = Command::new("/usr/bin/perl");
            command.env("CX_BOTTLE_PATH", root.join("elsewhere"));
            resolve_bottle_prefix(&root.join("cx"), command, name)
        };
        assert!(query("absent").is_err());
        let prefix = query("mine").unwrap();
        assert_eq!(prefix, bottle);

        let server = root.join("wineserver");
        let seen = root.join("seen");
        fs::write(
            &server,
            format!(
                "#!/bin/sh\necho \"$1 $WINEPREFIX\" >> '{}'\n",
                seen.display()
            ),
        )
        .unwrap();
        fs::set_permissions(&server, fs::Permissions::from_mode(0o755)).unwrap();
        let mut sleeper = Command::new("/bin/sh");
        sleeper.args(["-c", "sleep 30"]);
        let outcome =
            supervise::supervise(sleeper, Some(Duration::from_millis(200)), || 0).unwrap();
        assert!(matches!(outcome, Outcome::Stopped(Stop::TimedOut)));
        supervise::stop_server(&server, &prefix, &[]).unwrap();
        let expected = format!("-k {0}\n-w {0}\n", bottle.display());
        assert_eq!(fs::read_to_string(&seen).unwrap(), expected);
        fs::remove_dir_all(root).unwrap();
    }

    // Regression test starting with only a managed bottle and no private stub: the launch creates
    // the private stub, and timeout cleanup re-resolves via CXBottle so wineserver stops the stub.
    #[test]
    fn a_timeout_cleans_up_a_managed_bottle_private_stub_created_on_first_launch() {
        use std::os::unix::fs::PermissionsExt;
        let root = env::temp_dir().join(format!("bx-cli-managed-stub-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let managed = root.join("managed/corp");
        let private_dir = root.join("private");
        let private_stub = private_dir.join("corp");
        fs::create_dir_all(&managed).unwrap();
        fs::create_dir_all(&private_dir).unwrap();
        fs::create_dir_all(root.join("cx/lib/perl")).unwrap();
        fs::write(
            root.join("cx/lib/perl/CXBottle.pm"),
            r#"package CXBottle;
sub get_crossover_config {}
sub setup_bottle_wineprefix {
    my $priv = "$ENV{CX_BOTTLE_PATH}/$ENV{CX_BOTTLE}";
    if (-f "$priv/system.reg") {
        $ENV{WINEPREFIX} = $priv;
        return ('private', $priv);
    }
    my $mgmt = "$ENV{CX_MANAGED_BOTTLE_PATH}/$ENV{CX_BOTTLE}";
    if (-d $mgmt) {
        $ENV{WINEPREFIX} = $mgmt;
        return ('managed', $mgmt);
    }
    return (undef, undef);
}
1;
"#,
        )
        .unwrap();

        let query = |name: &str| {
            let mut command = Command::new("/usr/bin/perl");
            command
                .env("CX_BOTTLE_PATH", &private_dir)
                .env("CX_MANAGED_BOTTLE_PATH", root.join("managed"));
            resolve_bottle_prefix(&root.join("cx"), command, name)
        };

        // Before launch there is no private stub, so CXBottle reports the managed bottle.
        assert!(!private_stub.exists());
        let initial = query("corp").unwrap();
        assert_eq!(initial, managed);

        // Simulated first launch creates the private stub before hanging until timeout.
        let mut launcher = Command::new("/bin/sh");
        launcher.args([
            "-c",
            &format!(
                "mkdir -p '{0}' && touch '{0}/system.reg' && exec sleep 30",
                private_stub.display()
            ),
        ]);
        let outcome =
            supervise::supervise(launcher, Some(Duration::from_millis(300)), || 0).unwrap();
        assert!(matches!(outcome, Outcome::Stopped(Stop::TimedOut)));

        let prefixes = cleanup_prefixes_for_stop(&initial, Some("corp"), query).unwrap();
        assert_eq!(prefixes, vec![private_stub.clone(), managed.clone()]);

        let server = root.join("wineserver");
        let seen = root.join("seen");
        fs::write(
            &server,
            format!(
                "#!/bin/sh\necho \"$1 $WINEPREFIX\" >> '{}'\n",
                seen.display()
            ),
        )
        .unwrap();
        fs::set_permissions(&server, fs::Permissions::from_mode(0o755)).unwrap();
        for prefix in &prefixes {
            supervise::stop_server(&server, prefix, &[]).unwrap();
        }
        let expected = format!(
            "-k {0}\n-w {0}\n-k {1}\n-w {1}\n",
            private_stub.display(),
            managed.display()
        );
        assert_eq!(fs::read_to_string(&seen).unwrap(), expected);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn bottle_mode_passes_overrides_and_backend_as_launcher_options() {
        assert_eq!(bottle_options(None), ["--dll", SHIM_DLL_OVERRIDES]);
        assert_eq!(
            bottle_options(Some("dxvk"))[2..],
            ["--env", "CX_GRAPHICS_BACKEND=dxvk"]
        );
    }

    #[test]
    fn isolated_env_strips_secrets_and_pins_path() {
        let host = BTreeMap::from([
            ("HOME", "/Users/tester"),
            ("USER", "tester"),
            ("PATH", "/custom/dev/bin:/usr/bin"),
            ("GH_TOKEN", "secret_token"),
            ("AWS_SECRET_ACCESS_KEY", "secret_aws"),
            ("CX_GRAPHICS_BACKEND", "wined3d"),
        ]);
        let child = collect_isolated_host_env(|k| host.get(k).map(|s| (*s).to_string()));
        assert_eq!(child.get("PATH").map(String::as_str), Some(SAFE_CHILD_PATH));
        assert_eq!(child.get("HOME").map(String::as_str), Some("/Users/tester"));
        assert_eq!(child.get("USER").map(String::as_str), Some("tester"));
        assert!(!child.contains_key("GH_TOKEN"));
        assert!(!child.contains_key("AWS_SECRET_ACCESS_KEY"));
        assert!(!child.contains_key("CX_GRAPHICS_BACKEND"));
    }

    #[test]
    fn converts_unix_path_to_wine_z_drive() {
        assert_eq!(
            unix_to_wine_z_path(Path::new("/tmp/bx-test.jsonl")),
            "Z:\\tmp\\bx-test.jsonl"
        );
    }

    #[test]
    fn formats_amd_trap_summary_lines() {
        let sampled = TrapCount {
            events: 3,
            lower_bound: Some(4),
        };
        assert_eq!(
            format_amd_trap_line("clzero", &sampled),
            "clzero VEH traps   : at least 4 (sampled)"
        );
        assert_eq!(
            format_amd_trap_line("monitorx", &sampled),
            "monitorx VEH traps : at least 4 (sampled)"
        );
        let unnumbered = TrapCount {
            events: 2,
            lower_bound: None,
        };
        assert_eq!(
            format_amd_trap_line("rdpru", &unnumbered),
            "rdpru VEH traps    : 2 samples"
        );
    }

    #[test]
    fn formats_clwb_summary_lines() {
        assert_eq!(format_clwb_line(0, None), "clwb VEH traps     : 0");
        assert_eq!(format_clwb_line(2, None), "clwb VEH samples   : 2");
        assert_eq!(
            format_clwb_line(3, Some(4)),
            "clwb VEH traps     : at least 4 (sampled)"
        );
    }

    #[test]
    fn a_log_without_shader_gate_records_renders_as_before() {
        let log = r#"{"ts_ms":10,"tid":1,"level":"INFO","sub":"runtime_veh","event":"clwb_trapped","detail":"rip=0x1000 len=4 total_traps=1"}
{"ts_ms":12,"tid":1,"level":"WARN","sub":"stub","event":"unimpl_api","detail":"d3d12_xs.dll!D3D12XboxCreateDevice"}
{"ts_ms":16,"tid":1,"level":"WARN","sub":"com_vtable","event":"unimpl_vtable","detail":"ID3D12Device11[44]::CreatePlacedResourceX"}
"#;
        let rendered = render_trace(log);
        assert!(!rendered.contains("Shader"), "{rendered}");
        assert!(!rendered.contains("shader_gate"), "{rendered}");
        assert_eq!(
            rendered,
            "Trace events       : 3\n\
             clwb VEH traps     : at least 1 (sampled)\n\
             ADX illegal ops    : 0\n\
             \n\
             Unimplemented APIs:\n\
             \x20     1x  d3d12_xs.dll!D3D12XboxCreateDevice\n\
             \n\
             Unimplemented COM vtable slots:\n\
             \x20     1x  ID3D12Device11[44]::CreatePlacedResourceX\n"
        );
        assert_eq!(format_shader_gate_section(&summarize_jsonl(log)), "");
    }

    #[test]
    fn renders_decisions_kinds_stages_gaps_and_missing_bridge() {
        // Realistic records, matching the emitter in shims/d3d12_xs/shader_gate.cpp.
        let log = r#"{"ts_ms":10,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil hr=0x00000000 kind=dxil shader_stage=5 sm=6.6 bytes=1234 dxil=800 accc=0 sfi0=0x4000 wave_ops=1 int64_ops=0 atomic64=typed, wave_lanes=64-64"}
{"ts_ms":12,"tid":1,"level":"WARN","sub":"d3d12_xs","event":"shader_gate","detail":"pso=graphics stage=VS decision=reject_accc_only hr=0x80004001 kind=accc_only shader_stage=0 sm=6.0 bytes=2048 dxil=0 accc=512 sfi0=0x0 wave_ops=0 int64_ops=0 atomic64= wave_lanes=none"}
{"ts_ms":16,"tid":1,"level":"WARN","sub":"d3d12_xs","event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}
{"ts_ms":18,"tid":1,"level":"WARN","sub":"d3d12_xs","event":"shader_bridge_missing","detail":"bx_shader_bridge.dll not found beside d3d12_xs.dll; shaders pass unclassified, ACCC-only refused"}
"#;
        let rendered = render_trace(log);
        assert!(rendered.contains("Shader gate:"), "{rendered}");
        assert!(rendered.contains("shader_gate records : 2"), "{rendered}");
        assert!(rendered.contains("decisions:"), "{rendered}");
        assert!(rendered.contains("    1x  pass_dxil"), "{rendered}");
        assert!(rendered.contains("    1x  reject_accc_only"), "{rendered}");
        assert!(rendered.contains("kinds:"), "{rendered}");
        assert!(rendered.contains("    1x  dxil"), "{rendered}");
        assert!(rendered.contains("    1x  accc_only"), "{rendered}");
        assert!(rendered.contains("stages:"), "{rendered}");
        assert!(rendered.contains("    1x  CS"), "{rendered}");
        assert!(rendered.contains("    1x  VS"), "{rendered}");
        assert!(rendered.contains("D3DMetal gaps (1):"), "{rendered}");
        assert!(
            rendered.contains("  pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000"),
            "{rendered}"
        );
        assert!(
            rendered.contains("Shader bridge missing (1):"),
            "{rendered}"
        );
        assert!(
            rendered.contains("bx_shader_bridge.dll not found beside d3d12_xs.dll"),
            "{rendered}"
        );
        // #497 review point 5: name the rejected pipeline and its HRESULT, not just a count.
        assert!(
            rendered.contains("Non-pass pipeline decisions (1):"),
            "{rendered}"
        );
        assert!(
            rendered.contains("pso=graphics stage=VS decision=reject_accc_only hr=0x80004001"),
            "{rendered}"
        );
        assert!(!rendered.contains("decision=pass_dxil hr="), "{rendered}");
        assert!(!rendered.contains("unparsed"), "{rendered}");
    }

    #[test]
    fn an_unparsable_shader_gate_detail_shows_the_unparsed_count() {
        let log = r#"{"ts_ms":20,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"garbage"}
{"ts_ms":22,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil hr=0x00000000 kind=dxil shader_stage=5 sm=6.6 bytes=100 dxil=64 accc=0 sfi0=0x0 wave_ops=0 int64_ops=0 atomic64= wave_lanes=0-4294967295"}
"#;
        let rendered = render_trace(log);
        assert!(rendered.contains("shader_gate records : 2"), "{rendered}");
        assert!(rendered.contains("unparsed details    : 1"), "{rendered}");
        assert!(rendered.contains("    1x  pass_dxil"), "{rendered}");
    }

    /// The usage paths must not be inverted: an explicit `--help` succeeded,
    /// a bare invocation did nothing, and an unknown subcommand is an error.
    #[test]
    fn help_succeeds_on_stdout_and_a_bare_invocation_is_a_usage_error() {
        for flag in ["--help", "-h", "help"] {
            let argv = args(&[flag]);
            let dispatch = Dispatch::classify(&argv);
            assert_eq!(
                dispatch,
                Dispatch::Help,
                "{flag} is an explicit help request"
            );
            assert!(dispatch.usage_on_stdout(), "{flag} prints usage on stdout");
            assert!(!dispatch.is_usage_error(), "{flag} must exit 0");
        }

        let empty: Vec<String> = Vec::new();
        let none = Dispatch::classify(&empty);
        assert_eq!(none, Dispatch::NoArgs);
        assert!(!none.usage_on_stdout(), "a bare invocation writes stderr");
        assert!(
            none.is_usage_error(),
            "a bare invocation did nothing, so it must not report success"
        );

        let bogus_argv = args(&["definitely-not-a-subcommand"]);
        let bogus = Dispatch::classify(&bogus_argv);
        assert_eq!(bogus, Dispatch::Unknown("definitely-not-a-subcommand"));
        assert!(
            !bogus.usage_on_stdout(),
            "an unknown subcommand writes stderr"
        );
        assert!(bogus.is_usage_error(), "an unknown subcommand is an error");

        for real in ["doctor", "probe", "shader", "trace", "run"] {
            let argv = args(&[real]);
            let dispatch = Dispatch::classify(&argv);
            assert_eq!(dispatch, Dispatch::Sub(real), "{real} is a real subcommand");
            assert!(!dispatch.usage_on_stdout(), "{real} prints no usage");
            assert!(!dispatch.is_usage_error(), "{real} is not a usage error");
        }
    }

    /// The usage text must name every subcommand, so the classifier above is
    /// not testing a table that nothing prints.
    #[test]
    fn the_usage_text_names_every_subcommand() {
        let text = usage_text();
        assert!(
            text.contains("BetterXen Hybrid CLI"),
            "usage carries the banner"
        );
        for sub in ["doctor", "probe", "shader", "trace", "run"] {
            assert!(text.contains(sub), "usage documents {sub}");
        }
    }

    /// #590: the grouped view names each `(pso, stage)` pair with its count, wave width and
    /// 64-bit-atomic flags, in first-seen order, and says when a record lost its identity.
    #[test]
    fn renders_gaps_grouped_by_pipeline_and_stage() {
        let log = r#"{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=VS wave_lanes_min=64 atomic64_flags=0x0: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"garbage"}
"#;
        let rendered = render_trace(log);
        // The #496 per-record list is unchanged.
        assert!(rendered.contains("D3DMetal gaps (4):"), "{rendered}");
        assert!(
            rendered.contains("  pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000"),
            "{rendered}"
        );
        // The additive grouped view.
        assert!(
            rendered.contains("D3DMetal gaps by kind/stage (3):"),
            "{rendered}"
        );
        assert!(
            rendered.contains("      1x  pso=graphics stage=VS"),
            "{rendered}"
        );
        assert!(
            rendered.contains("      2x  pso=compute stage=CS"),
            "{rendered}"
        );
        assert!(
            rendered.contains("          wave_lanes_min=64 atomic64_flags=0x400000 (2x)"),
            "{rendered}"
        );
        assert!(
            rendered.contains("      1x  pso=unknown stage=unknown"),
            "{rendered}"
        );
        assert!(rendered.contains("gaps w/o pso/stage  : 1"), "{rendered}");
        assert!(rendered.contains("gaps w/o constraints : 1"), "{rendered}");
        // First-seen order: the graphics/VS group comes before the compute/CS one.
        let vs = rendered.find("pso=graphics stage=VS").unwrap();
        let cs = rendered.find("      2x  pso=compute stage=CS").unwrap();
        assert!(vs < cs, "{rendered}");
    }

    /// #590: a run with no gap records renders no grouped section at all.
    #[test]
    fn a_run_with_no_gaps_renders_no_grouped_section() {
        let log = r#"{"ts_ms":10,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil hr=0x00000000 kind=dxil shader_stage=5 sm=6.6 bytes=1234 dxil=800 accc=0 sfi0=0x4000 wave_ops=1 int64_ops=0 atomic64=typed, wave_lanes=64-64"}"#;
        let rendered = render_trace(log);
        assert!(rendered.contains("Shader gate:"), "{rendered}");
        assert!(!rendered.contains("D3DMetal gaps"), "{rendered}");
        assert!(!rendered.contains("gaps w/o"), "{rendered}");
    }
}
