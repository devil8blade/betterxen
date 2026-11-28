//! Track A / F: Structured JSONL shim trace aggregator.
//!
//! Summarizes `bx_runtime_veh.dll` logs (stub API calls, unimplemented COM vtable
//! slots, `clwb` traps, ADX illegal-instruction tripwires, and unhandled exceptions)
//! so tracks can identify guest failures and missing APIs.

use std::collections::BTreeMap;

#[derive(Debug, Default, PartialEq, Eq)]
pub struct TraceSummary {
    pub total_lines: usize,
    /// Number of `clwb_trapped` records in the log.
    pub clwb_events: usize,
    /// Highest cumulative counter in the sampled CLWB records, if available.
    pub clwb_trap_lower_bound: Option<u64>,
    /// Sampled `<kind>_trapped` records for the other AMD-only instructions the
    /// VEH emulates (`clzero`, `monitorx`, `mwaitx`, `rdpru`, `extrq`, `insertq`).
    pub amd_traps: BTreeMap<String, TrapCount>,
    pub adx_errors: usize,
    pub unhandled_exceptions: Vec<UnhandledException>,
    /// First-chance `breakpoint_exception` records (int3 and assertion failures), which a
    /// guest's own crash reporter would otherwise hide from `unhandled_exception`.
    pub breakpoint_exceptions: Vec<BreakpointException>,
    pub unimpl_apis: BTreeMap<String, usize>,
    pub unimpl_vtables: BTreeMap<String, usize>,
    /// Aggregated `shader_gate` records from `d3d12_xs.dll` (#496).
    pub shader_gate: ShaderGateSummary,
    /// `shader_d3dmetal_gap` records: pipelines D3DMetal creates but does not run (#453).
    pub shader_gaps: Vec<ShaderGap>,
    /// Those same gap records grouped by `(pso, stage)`, in first-seen order (#590).
    pub shader_gap_groups: Vec<ShaderGapGroup>,
    /// Gap records that carried no `pso` or no `stage` (or an empty value for one), so
    /// they are grouped under `unknown`. Counted here so the group counts reconcile
    /// with [`TraceSummary::shader_gaps`].
    pub shader_gaps_missing_identity: usize,
    /// Gap records that carried no `wave_lanes_min` or no `atomic64_flags` (or an empty
    /// value for one). Counted here rather than dropped or assumed.
    pub shader_gaps_missing_constraints: usize,
    /// `shader_bridge_missing` messages, in log order.
    pub shader_bridge_missing: Vec<String>,
}

/// One `(pso, stage)` group of `shader_d3dmetal_gap` records (#590).
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ShaderGapGroup {
    /// The emitted `pso=` value, or `None` when the record carried none (rendered as
    /// `unknown`).
    pub pso: Option<String>,
    /// The emitted `stage=` value, or `None` when the record carried none.
    pub stage: Option<String>,
    /// How many gap records this `(pso, stage)` pair covers.
    pub count: usize,
    /// The distinct `(wave_lanes_min, atomic64_flags)` pairs seen for this pair, in
    /// first-seen order. Two pipelines of the same kind and stage can need different
    /// wave widths or atomic features, so a group keeps each constraint set rather than
    /// reporting only the first and losing the rest.
    pub variants: Vec<ShaderGapVariant>,
}

/// The constraints carried by one or more records of a [`ShaderGapGroup`].
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ShaderGapVariant {
    pub wave_lanes_min: Option<String>,
    pub atomic64_flags: Option<String>,
    /// How many records in the group carried exactly these constraints.
    pub count: usize,
}

/// Aggregated `shader_gate` records. Decisions, kinds and stages are keyed by
/// their emitted string so unknown values are counted rather than dropped.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ShaderGateSummary {
    pub total: usize,
    /// Gate records whose `detail` could not be parsed as `key=value` pairs.
    pub unparsed: usize,
    /// Parsed gate records that carry no `decision` field, so they appear in no
    /// `decisions` bucket. Counted here so the decision counts can be
    /// reconciled against `total` (#497 review). `kind` is legitimately absent
    /// on unclassified records and is not counted here.
    pub missing_decision: usize,
    pub decisions: BTreeMap<String, usize>,
    pub kinds: BTreeMap<String, usize>,
    pub stages: BTreeMap<String, usize>,
    /// Individual gate records, in log order.
    pub records: Vec<ShaderGateRecord>,
}

/// One `shader_gate` record. Fields absent from the emitted `detail` (for
/// example on unclassified records) are `None`; `atomic64` may be `Some("")`.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ShaderGateRecord {
    pub pso: Option<String>,
    pub stage: Option<String>,
    pub decision: Option<String>,
    pub hr: Option<String>,
    pub kind: Option<String>,
    pub shader_stage: Option<String>,
    pub sm: Option<String>,
    pub bytes: Option<String>,
    pub dxil: Option<String>,
    pub accc: Option<String>,
    pub sfi0: Option<String>,
    pub wave_ops: Option<String>,
    pub int64_ops: Option<String>,
    pub atomic64: Option<String>,
    pub wave_lanes: Option<String>,
}

/// One `shader_d3dmetal_gap` record.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ShaderGap {
    pub pso: Option<String>,
    pub stage: Option<String>,
    pub wave_lanes_min: Option<String>,
    pub atomic64_flags: Option<String>,
}

/// Tokenize a `detail` string of space-separated `key=value` pairs.
///
/// Only the `shader_d3dmetal_gap` record appends free text after its pairs
/// (`... atomic64_flags=0x400000: D3DMetal ...`), so `stop_at_free_text`
/// selects how a token without `=` is treated:
///
/// - `true` (gap records): the token ends the key=value section.
/// - `false` (`shader_gate` records): a stray token is skipped and parsing
///   continues, so one unexpected word cannot silently drop every later
///   field (#497 review).
///
/// Values never contain spaces, so `atomic64=` followed by a space yields an
/// empty value.
/// Splits a log detail string into `key=value` pairs.
///
/// `stop_at_free_text` exists for records that end with prose after their
/// fields (`shader_d3dmetal_gap` ends in a sentence), where the first token
/// without `=` marks the end of the structured part.
///
/// For `shader_gate` the whole string is structured, so a token without `=`
/// is skipped rather than treated as the end. Stopping there would silently
/// drop every later field while the record still counted as parsed: with
/// `atomic64=typed, raw, wave_lanes=64-64`, a `break` loses `wave_lanes`,
/// whereas a `continue` keeps it. The emitter writes no spaces inside a value
/// today, so this is robustness against a future value containing one, not a
/// fix for current output.
fn parse_key_values(detail: &str, stop_at_free_text: bool) -> Vec<(&str, &str)> {
    let mut pairs = Vec::new();
    for token in detail.split_ascii_whitespace() {
        match token.split_once('=') {
            Some((key, value)) if !key.is_empty() => pairs.push((key, value)),
            _ if stop_at_free_text => break,
            _ => continue,
        }
    }
    pairs
}

/// Sampled trap records of one instruction kind.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct TrapCount {
    pub events: usize,
    /// Highest cumulative counter in the sampled records, if available.
    pub lower_bound: Option<u64>,
}

const AMD_TRAP_EVENTS: [(&str, &str); 6] = [
    ("clzero_trapped", "clzero"),
    ("monitorx_trapped", "monitorx"),
    ("mwaitx_trapped", "mwaitx"),
    ("rdpru_trapped", "rdpru"),
    ("extrq_trapped", "extrq"),
    ("insertq_trapped", "insertq"),
];

fn sampled_counter(detail: &str) -> Option<u64> {
    detail
        .split_ascii_whitespace()
        .find_map(|field| field.strip_prefix("total_traps="))
        .and_then(|count| count.parse::<u64>().ok())
}

#[derive(Debug, PartialEq, Eq)]
pub struct UnhandledException {
    pub exception_code: Option<String>,
    pub module_base: Option<String>,
    pub rva: Option<String>,
    pub rip: Option<String>,
}

#[derive(Debug, PartialEq, Eq)]
pub struct BreakpointException {
    pub exception_code: Option<String>,
    pub module: Option<String>,
    pub module_base: Option<String>,
    pub rva: Option<String>,
    /// 1-based index of this record and the per-process cap, so a reader can tell a run
    /// that hit the cap (later breakpoints not recorded) from one that stopped short of it.
    pub record: Option<u32>,
    pub limit: Option<u32>,
}

fn extract_json_u32(line: &str, key: &str) -> Option<u32> {
    let needle = format!("\"{key}\":");
    let start = line.find(&needle)? + needle.len();
    let digits: &str = &line[start..];
    let end = digits
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(digits.len());
    digits[..end].parse().ok()
}

fn extract_json_str<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let needle = format!("\"{key}\":\"");
    let start = line.find(&needle)? + needle.len();
    let rest = &line[start..];
    let end = rest.find('"')?;
    Some(&rest[..end])
}

fn get_field<'a>(pairs: &[(&'a str, &'a str)], key: &str) -> Option<String> {
    pairs
        .iter()
        .find(|(k, _)| *k == key)
        .map(|(_, v)| (*v).to_owned())
}

fn record_shader_gate(summary: &mut TraceSummary, detail: &str) {
    let gate = &mut summary.shader_gate;
    gate.total += 1;
    let pairs = parse_key_values(detail, false);
    if pairs.is_empty() {
        gate.unparsed += 1;
        return;
    }
    let record = ShaderGateRecord {
        pso: get_field(&pairs, "pso"),
        stage: get_field(&pairs, "stage"),
        decision: get_field(&pairs, "decision"),
        hr: get_field(&pairs, "hr"),
        kind: get_field(&pairs, "kind"),
        shader_stage: get_field(&pairs, "shader_stage"),
        sm: get_field(&pairs, "sm"),
        bytes: get_field(&pairs, "bytes"),
        dxil: get_field(&pairs, "dxil"),
        accc: get_field(&pairs, "accc"),
        sfi0: get_field(&pairs, "sfi0"),
        wave_ops: get_field(&pairs, "wave_ops"),
        int64_ops: get_field(&pairs, "int64_ops"),
        atomic64: get_field(&pairs, "atomic64"),
        wave_lanes: get_field(&pairs, "wave_lanes"),
    };
    match &record.decision {
        Some(decision) => *gate.decisions.entry(decision.clone()).or_insert(0) += 1,
        None => gate.missing_decision += 1,
    }
    if let Some(kind) = &record.kind {
        *gate.kinds.entry(kind.clone()).or_insert(0) += 1;
    }
    if let Some(stage) = &record.stage {
        *gate.stages.entry(stage.clone()).or_insert(0) += 1;
    }
    gate.records.push(record);
}

/// A field value, treating an empty value (`pso= stage=CS`) as absent. The emitter
/// always writes a value, so an empty one means the record is malformed, not that the
/// pipeline is named "".
fn non_empty(value: Option<String>) -> Option<String> {
    value.filter(|v| !v.is_empty())
}

/// Adds one gap record to the per-`(pso, stage)` grouping, keeping first-seen order.
///
/// Nothing here panics: a record missing `pso`, `stage`, `wave_lanes_min` or
/// `atomic64_flags` is grouped under `unknown` (and counted), never dropped or guessed.
fn record_shader_gap_group(groups: &mut TraceSummary, gap: &ShaderGap) {
    let pso = non_empty(gap.pso.clone());
    let stage = non_empty(gap.stage.clone());
    let wave_lanes_min = non_empty(gap.wave_lanes_min.clone());
    let atomic64_flags = non_empty(gap.atomic64_flags.clone());
    if pso.is_none() || stage.is_none() {
        groups.shader_gaps_missing_identity += 1;
    }
    if wave_lanes_min.is_none() || atomic64_flags.is_none() {
        groups.shader_gaps_missing_constraints += 1;
    }

    // The group list is small (one entry per affected pipeline stage), so a linear scan
    // keeps first-seen order without a second index to keep in sync.
    let group_index = match groups
        .shader_gap_groups
        .iter()
        .position(|g| g.pso == pso && g.stage == stage)
    {
        Some(index) => index,
        None => {
            groups.shader_gap_groups.push(ShaderGapGroup {
                pso,
                stage,
                count: 0,
                variants: Vec::new(),
            });
            groups.shader_gap_groups.len() - 1
        }
    };
    let group = &mut groups.shader_gap_groups[group_index];
    group.count += 1;
    match group
        .variants
        .iter()
        .position(|v| v.wave_lanes_min == wave_lanes_min && v.atomic64_flags == atomic64_flags)
    {
        Some(index) => group.variants[index].count += 1,
        None => group.variants.push(ShaderGapVariant {
            wave_lanes_min,
            atomic64_flags,
            count: 1,
        }),
    }
}

fn record_shader_gap(summary: &mut TraceSummary, detail: &str) {
    let pairs = parse_key_values(detail, true);
    // The emitter appends `: D3DMetal creates such pipelines ...` directly
    // after the flags value, so strip the trailing colon.
    let atomic64_flags = get_field(&pairs, "atomic64_flags")
        .map(|v| v.strip_suffix(':').map(str::to_owned).unwrap_or(v));
    let gap = ShaderGap {
        pso: get_field(&pairs, "pso"),
        stage: get_field(&pairs, "stage"),
        wave_lanes_min: get_field(&pairs, "wave_lanes_min"),
        atomic64_flags,
    };
    record_shader_gap_group(summary, &gap);
    summary.shader_gaps.push(gap);
}

pub fn summarize_jsonl(input: &str) -> TraceSummary {
    let mut summary = TraceSummary::default();
    for line in input.lines() {
        let trimmed = line.trim();
        if !trimmed.starts_with('{') {
            continue;
        }
        summary.total_lines += 1;
        let event = extract_json_str(trimmed, "event").unwrap_or("");
        let detail = extract_json_str(trimmed, "detail").unwrap_or("");
        match event {
            "clwb_trapped" => {
                summary.clwb_events += 1;
                summary.clwb_trap_lower_bound =
                    summary.clwb_trap_lower_bound.max(sampled_counter(detail));
            }
            _ if AMD_TRAP_EVENTS.iter().any(|(e, _)| *e == event) => {
                let name = AMD_TRAP_EVENTS.iter().find(|(e, _)| *e == event).unwrap().1;
                let entry = summary.amd_traps.entry(name.to_string()).or_default();
                entry.events += 1;
                entry.lower_bound = entry.lower_bound.max(sampled_counter(detail));
            }
            "shader_gate" => record_shader_gate(&mut summary, detail),
            "shader_d3dmetal_gap" => record_shader_gap(&mut summary, detail),
            "shader_bridge_missing" => {
                summary.shader_bridge_missing.push(detail.to_owned());
            }
            "adx_illegal_instruction" => summary.adx_errors += 1,
            "unhandled_exception" => {
                summary.unhandled_exceptions.push(UnhandledException {
                    exception_code: extract_json_str(trimmed, "exception_code").map(str::to_owned),
                    module_base: extract_json_str(trimmed, "module_base").map(str::to_owned),
                    rva: extract_json_str(trimmed, "rva").map(str::to_owned),
                    rip: extract_json_str(trimmed, "rip").map(str::to_owned),
                });
            }
            "breakpoint_exception" => {
                summary.breakpoint_exceptions.push(BreakpointException {
                    exception_code: extract_json_str(trimmed, "exception_code").map(str::to_owned),
                    module: extract_json_str(trimmed, "module").map(str::to_owned),
                    module_base: extract_json_str(trimmed, "module_base").map(str::to_owned),
                    rva: extract_json_str(trimmed, "rva").map(str::to_owned),
                    record: extract_json_u32(trimmed, "record"),
                    limit: extract_json_u32(trimmed, "limit"),
                });
            }
            "unimpl_api" => *summary.unimpl_apis.entry(detail.to_string()).or_insert(0) += 1,
            "unimpl_vtable" => {
                *summary
                    .unimpl_vtables
                    .entry(detail.to_string())
                    .or_insert(0) += 1
            }
            _ => {}
        }
    }
    summary
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aggregates_stub_and_veh_events() {
        let sample = r#"
{"ts_ms":10,"tid":1,"level":"INFO","sub":"runtime_veh","event":"clwb_trapped","detail":"rip=0x1000 len=4 total_traps=1"}
{"ts_ms":12,"tid":1,"level":"WARN","sub":"stub","event":"unimpl_api","detail":"d3d12_xs.dll!D3D12XboxCreateDevice"}
{"ts_ms":14,"tid":1,"level":"WARN","sub":"stub","event":"unimpl_api","detail":"d3d12_xs.dll!D3D12XboxCreateDevice"}
{"ts_ms":16,"tid":1,"level":"WARN","sub":"com_vtable","event":"unimpl_vtable","detail":"ID3D12Device11[44]::CreatePlacedResourceX"}
"#;
        let s = summarize_jsonl(sample);
        assert_eq!(s.total_lines, 4);
        assert_eq!(s.clwb_events, 1);
        assert_eq!(s.clwb_trap_lower_bound, Some(1));
        assert_eq!(s.adx_errors, 0);
        assert!(s.unhandled_exceptions.is_empty());
        assert_eq!(
            s.unimpl_apis.get("d3d12_xs.dll!D3D12XboxCreateDevice"),
            Some(&2)
        );
        assert_eq!(
            s.unimpl_vtables
                .get("ID3D12Device11[44]::CreatePlacedResourceX"),
            Some(&1)
        );
    }

    #[test]
    fn uses_highest_counter_from_sampled_clwb_records() {
        for (actual_traps, records, expected_bound) in [
            (1, &[1][..], 1),
            (6, &[1, 4, 2], 4),
            (100, &[1, 2, 4, 8, 16, 32, 64], 64),
        ] {
            let mut log = String::new();
            for counter in records {
                log.push_str(&format!(
                    "{{\"event\":\"clwb_trapped\",\"detail\":\"rip=0x1000 len=4 total_traps={counter}\"}}\n"
                ));
            }
            let summary = summarize_jsonl(&log);
            assert_eq!(summary.clwb_events, records.len(), "{actual_traps} traps");
            assert_eq!(
                summary.clwb_trap_lower_bound,
                Some(expected_bound),
                "{actual_traps} traps"
            );
        }
    }

    #[test]
    fn preserves_clwb_record_count_without_valid_counters() {
        let summary = summarize_jsonl(
            r#"{"event":"clwb_trapped","detail":"rip=0x1000 len=4"}
{"event":"clwb_trapped","detail":"rip=0x1000 len=4 total_traps=invalid"}
{"event":"unimpl_api","detail":"total_traps=100"}"#,
        );
        assert_eq!(summary.clwb_events, 2);
        assert_eq!(summary.clwb_trap_lower_bound, None);
    }

    #[test]
    fn collects_breakpoint_exceptions_in_log_order() {
        let log = r#"
{"ts_ms":5,"tid":7,"level":"WARN","sub":"runtime_veh","event":"breakpoint_exception","exception_code":"0x80000003","module":"title.exe","module_base":"0x0000000140000000","rva":"0x0000000002236178","address":"0x0000000142236178","record":1,"limit":16}
{"ts_ms":6,"tid":7,"level":"WARN","sub":"runtime_veh","event":"breakpoint_exception","exception_code":"0xc0000420","module":"?","module_base":"0x0000000000000000","rva":"0x0000000000000000","address":"0x0000000000410000","record":2,"limit":16}
"#;
        let summary = summarize_jsonl(log);
        assert!(summary.unhandled_exceptions.is_empty());
        assert_eq!(
            summary.breakpoint_exceptions,
            [
                BreakpointException {
                    exception_code: Some("0x80000003".into()),
                    module: Some("title.exe".into()),
                    module_base: Some("0x0000000140000000".into()),
                    rva: Some("0x0000000002236178".into()),
                    record: Some(1),
                    limit: Some(16),
                },
                BreakpointException {
                    exception_code: Some("0xc0000420".into()),
                    module: Some("?".into()),
                    module_base: Some("0x0000000000000000".into()),
                    rva: Some("0x0000000000000000".into()),
                    record: Some(2),
                    limit: Some(16),
                },
            ]
        );
    }

    #[test]
    fn collects_unhandled_exceptions_in_log_order() {
        let log = r#"
{"ts_ms":16,"tid":1,"level":"ERROR","sub":"runtime_veh","event":"unhandled_exception","exception_code":"0xc0000005","module_base":"0x0000000140000000","rva":"0x0000000000001234","rip":"0x0000000140001234","rsp":"0x000000000021fd80","registers":{"rax":"0x0123456789abcdef","rbx":"0x0000000000000000","rcx":"0x0000000000000000","rdx":"0x0000000000000000","rsi":"0x0000000000000000","rdi":"0x0000000000000000","rbp":"0x0000000000000000","r8":"0x0000000000000000","r9":"0x0000000000000000","r10":"0x0000000000000000","r11":"0x0000000000000000","r12":"0x0000000000000000","r13":"0x0000000000000000","r14":"0x0000000000000000","r15":"0x0000000000000000"}}
{"ts_ms":20,"tid":1,"level":"ERROR","sub":"runtime_veh","event":"unhandled_exception","exception_code":"0xc000001d","module_base":"0x0000000000000000","rva":"0x0000000000000000","rip":"0x0000000000410000","rsp":"0x000000000021fd80","registers":{}}
"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.total_lines, 2);
        assert_eq!(
            summary.unhandled_exceptions,
            [
                UnhandledException {
                    exception_code: Some("0xc0000005".into()),
                    module_base: Some("0x0000000140000000".into()),
                    rva: Some("0x0000000000001234".into()),
                    rip: Some("0x0000000140001234".into()),
                },
                UnhandledException {
                    exception_code: Some("0xc000001d".into()),
                    module_base: Some("0x0000000000000000".into()),
                    rva: Some("0x0000000000000000".into()),
                    rip: Some("0x0000000000410000".into()),
                },
            ]
        );
    }

    #[test]
    fn counts_amd_only_traps_per_kind() {
        let summary = summarize_jsonl(
            r#"{"event":"clzero_trapped","detail":"rip=0x1000 len=3 total_traps=1"}
{"event":"clzero_trapped","detail":"rip=0x1000 len=3 total_traps=2"}
{"event":"clzero_trapped","detail":"rip=0x1000 len=3 total_traps=4"}
{"event":"extrq_trapped","detail":"rip=0x2000 len=5 total_traps=1"}
{"event":"rdpru_trapped","detail":"rip=0x3000 len=3"}"#,
        );
        assert_eq!(
            summary.amd_traps["clzero"],
            TrapCount {
                events: 3,
                lower_bound: Some(4)
            }
        );
        assert_eq!(
            summary.amd_traps["extrq"],
            TrapCount {
                events: 1,
                lower_bound: Some(1)
            }
        );
        assert_eq!(
            summary.amd_traps["rdpru"],
            TrapCount {
                events: 1,
                lower_bound: None
            }
        );
        assert!(!summary.amd_traps.contains_key("mwaitx"));
        assert_eq!(summary.clwb_events, 0);
    }

    #[test]
    fn summarizes_full_pass_dxil_gate_record() {
        let log = r#"{"ts_ms":10,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil hr=0x00000000 kind=dxil shader_stage=5 sm=6.6 bytes=1234 dxil=800 accc=0 sfi0=0x4000 wave_ops=1 int64_ops=0 atomic64=typed, wave_lanes=64-64"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gate.total, 1);
        assert_eq!(summary.shader_gate.unparsed, 0);
        assert_eq!(summary.shader_gate.decisions["pass_dxil"], 1);
        assert_eq!(summary.shader_gate.kinds["dxil"], 1);
        assert_eq!(summary.shader_gate.stages["CS"], 1);
        assert_eq!(summary.shader_gate.records.len(), 1);
        let r = &summary.shader_gate.records[0];
        assert_eq!(r.pso.as_deref(), Some("compute"));
        assert_eq!(r.stage.as_deref(), Some("CS"));
        assert_eq!(r.decision.as_deref(), Some("pass_dxil"));
        assert_eq!(r.hr.as_deref(), Some("0x00000000"));
        assert_eq!(r.kind.as_deref(), Some("dxil"));
        assert_eq!(r.shader_stage.as_deref(), Some("5"));
        assert_eq!(r.sm.as_deref(), Some("6.6"));
        assert_eq!(r.bytes.as_deref(), Some("1234"));
        assert_eq!(r.dxil.as_deref(), Some("800"));
        assert_eq!(r.accc.as_deref(), Some("0"));
        assert_eq!(r.sfi0.as_deref(), Some("0x4000"));
        assert_eq!(r.wave_ops.as_deref(), Some("1"));
        assert_eq!(r.int64_ops.as_deref(), Some("0"));
        assert_eq!(r.atomic64.as_deref(), Some("typed,"));
        assert_eq!(r.wave_lanes.as_deref(), Some("64-64"));
    }

    #[test]
    fn summarizes_reject_accc_only_gate_record() {
        let log = r#"{"ts_ms":12,"tid":1,"level":"WARN","sub":"d3d12_xs","event":"shader_gate","detail":"pso=graphics stage=VS decision=reject_accc_only hr=0x80004001 kind=accc_only shader_stage=0 sm=6.0 bytes=2048 dxil=0 accc=512 sfi0=0x0 wave_ops=0 int64_ops=0 atomic64= wave_lanes=none"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gate.total, 1);
        assert_eq!(summary.shader_gate.decisions["reject_accc_only"], 1);
        assert_eq!(summary.shader_gate.kinds["accc_only"], 1);
        assert_eq!(summary.shader_gate.stages["VS"], 1);
        let r = &summary.shader_gate.records[0];
        assert_eq!(r.hr.as_deref(), Some("0x80004001"));
        assert_eq!(r.sm.as_deref(), Some("6.0"));
        assert_eq!(r.accc.as_deref(), Some("512"));
        assert_eq!(r.atomic64.as_deref(), Some(""));
        assert_eq!(r.wave_lanes.as_deref(), Some("none"));
    }

    #[test]
    fn summarizes_unclassified_gate_record_without_kind() {
        let log = r#"{"ts_ms":14,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_unclassified hr=0x00000000 bytes=4096"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gate.total, 1);
        assert_eq!(summary.shader_gate.decisions["pass_unclassified"], 1);
        assert_eq!(summary.shader_gate.stages["CS"], 1);
        assert!(summary.shader_gate.kinds.is_empty());
        let r = &summary.shader_gate.records[0];
        assert_eq!(r.bytes.as_deref(), Some("4096"));
        assert_eq!(r.kind, None);
        assert_eq!(r.sm, None);
        assert_eq!(r.dxil, None);
        assert_eq!(r.atomic64, None);
        assert_eq!(r.wave_lanes, None);
    }

    #[test]
    fn summarizes_d3dmetal_gap_with_free_text_suffix() {
        let log = r#"{"ts_ms":16,"tid":1,"level":"WARN","sub":"d3d12_xs","event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gaps.len(), 1);
        let gap = &summary.shader_gaps[0];
        assert_eq!(gap.pso.as_deref(), Some("compute"));
        assert_eq!(gap.stage.as_deref(), Some("CS"));
        assert_eq!(gap.wave_lanes_min.as_deref(), Some("64"));
        assert_eq!(gap.atomic64_flags.as_deref(), Some("0x400000"));
    }

    #[test]
    fn collects_shader_bridge_missing_messages() {
        let log = r#"{"ts_ms":18,"tid":1,"level":"WARN","sub":"d3d12_xs","event":"shader_bridge_missing","detail":"bx_shader_bridge.dll not found beside d3d12_xs.dll; shaders pass unclassified, ACCC-only refused"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_bridge_missing.len(), 1);
        assert!(summary.shader_bridge_missing[0].contains("not found beside d3d12_xs.dll"));
    }

    #[test]
    fn malformed_shader_gate_detail_is_counted_not_dropped() {
        for detail in ["", "garbage", "=value", "   "] {
            let log = format!(
                "{{\"ts_ms\":20,\"tid\":1,\"level\":\"INFO\",\"sub\":\"d3d12_xs\",\"event\":\"shader_gate\",\"detail\":\"{detail}\"}}"
            );
            let summary = summarize_jsonl(&log);
            assert_eq!(summary.shader_gate.total, 1, "detail={detail:?}");
            assert_eq!(summary.shader_gate.unparsed, 1, "detail={detail:?}");
            assert!(summary.shader_gate.records.is_empty(), "detail={detail:?}");
        }
    }

    #[test]
    fn empty_atomic64_value_is_preserved() {
        let log = r#"{"ts_ms":22,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil hr=0x00000000 kind=dxil shader_stage=5 sm=6.6 bytes=100 dxil=64 accc=0 sfi0=0x0 wave_ops=0 int64_ops=0 atomic64= wave_lanes=0-4294967295"}"#;
        let summary = summarize_jsonl(log);
        let r = &summary.shader_gate.records[0];
        assert_eq!(r.atomic64.as_deref(), Some(""));
        assert_eq!(r.wave_lanes.as_deref(), Some("0-4294967295"));
    }

    #[test]
    fn a_stray_token_does_not_drop_later_gate_fields() {
        // #497 review point 1: one value containing a space must not turn every
        // later field into None.
        let log = r#"{"event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil atomic64=typed, raw wave_lanes=64-64"}"#;
        let summary = summarize_jsonl(log);
        let r = &summary.shader_gate.records[0];
        assert_eq!(r.decision.as_deref(), Some("pass_dxil"));
        assert_eq!(r.wave_lanes.as_deref(), Some("64-64"));
        assert_eq!(summary.shader_gate.unparsed, 0);
    }

    #[test]
    fn two_atomic64_flags_are_parsed() {
        // #497 review point 1: the emitter concatenates flags with no separator,
        // but two flags must round-trip if they ever are spaced.
        let log = r#"{"event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil kind=dxil atomic64=typed, groupshared, wave_lanes=64-64"}"#;
        let summary = summarize_jsonl(log);
        let r = &summary.shader_gate.records[0];
        assert_eq!(r.atomic64.as_deref(), Some("typed,"));
        assert_eq!(r.wave_lanes.as_deref(), Some("64-64"));
    }

    #[test]
    fn a_gate_record_without_a_decision_is_counted_as_missing() {
        // #497 review point 2: decision counts must reconcile against total.
        let log = r#"{"event":"shader_gate","detail":"pso=compute stage=CS"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gate.total, 1);
        assert_eq!(summary.shader_gate.missing_decision, 1);
        assert_eq!(summary.shader_gate.unparsed, 0);
        assert!(summary.shader_gate.decisions.is_empty());
        assert_eq!(summary.shader_gate.stages["CS"], 1);
    }

    #[test]
    fn decision_buckets_plus_missing_reconcile_with_total() {
        let log = r#"{"event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil bytes=1"}
{"event":"shader_gate","detail":"pso=compute stage=CS"}
{"event":"shader_gate","detail":"garbage"}"#;
        let summary = summarize_jsonl(log);
        let gate = &summary.shader_gate;
        let bucketed: usize = gate.decisions.values().sum();
        assert_eq!(gate.total, 3);
        assert_eq!(gate.unparsed, 1);
        assert_eq!(gate.missing_decision, 1);
        assert_eq!(bucketed + gate.unparsed + gate.missing_decision, gate.total);
    }

    #[test]
    fn unknown_decisions_and_kinds_are_counted() {
        let log = r#"{"event":"shader_gate","detail":"pso=compute stage=CS decision=some_future_decision hr=0x00000000 kind=some_future_kind bytes=1"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gate.decisions["some_future_decision"], 1);
        assert_eq!(summary.shader_gate.kinds["some_future_kind"], 1);
    }

    /// A stray token must not end the structured part of a `shader_gate`
    /// record: if it did, every later field would silently become `None`
    /// while the record still counted as parsed.
    #[test]
    fn a_record_without_a_decision_is_counted_as_missing() {
        let mut summary = TraceSummary::default();
        record_shader_gate(&mut summary, "pso=graphics stage=pixel");
        let gate = &summary.shader_gate;
        assert_eq!(gate.total, 1);
        assert_eq!(gate.unparsed, 0);
        assert_eq!(gate.missing_decision, 1);
        assert!(gate.decisions.is_empty());
    }

    /// The free-text path is the opposite: `shader_d3dmetal_gap` ends in a
    /// sentence, so the first token without `=` ends the structured part.
    #[test]
    fn free_text_ends_the_structured_part_of_a_gap_record() {
        let pairs = parse_key_values("pso=graphics stage=pixel and then some prose", true);
        assert_eq!(pairs.len(), 2);
        assert_eq!(pairs[1], ("stage", "pixel"));
    }

    /// #590: two gaps on different stages must stay two groups, in first-seen order, each
    /// with its own wave width and 64-bit-atomic flags.
    #[test]
    fn groups_gaps_on_different_stages_separately_in_first_seen_order() {
        // Same pso, different stages: grouping on pso alone would collapse these, so the
        // test discriminates the (pso, stage) key rather than only the pso.
        let log = r#"{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=VS wave_lanes_min=64 atomic64_flags=0x0: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=PS wave_lanes_min=64 atomic64_flags=0x0: D3DMetal creates such pipelines but does not run them (#453)"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gaps.len(), 3);
        assert_eq!(summary.shader_gaps_missing_identity, 0);
        assert_eq!(summary.shader_gaps_missing_constraints, 0);
        assert_eq!(summary.shader_gap_groups.len(), 3);

        let first = &summary.shader_gap_groups[0];
        assert_eq!(first.pso.as_deref(), Some("graphics"));
        assert_eq!(first.stage.as_deref(), Some("VS"));
        assert_eq!(first.count, 1);
        assert_eq!(
            first.variants,
            [ShaderGapVariant {
                wave_lanes_min: Some("64".into()),
                atomic64_flags: Some("0x0".into()),
                count: 1,
            }]
        );

        let second = &summary.shader_gap_groups[1];
        assert_eq!(second.pso.as_deref(), Some("compute"));
        assert_eq!(second.stage.as_deref(), Some("CS"));
        assert_eq!(second.count, 1);

        // A second stage of the *same* pso stays its own group, which is what separates a
        // (pso, stage) key from a pso-only key.
        let third = &summary.shader_gap_groups[2];
        assert_eq!(third.pso.as_deref(), Some("graphics"));
        assert_eq!(third.stage.as_deref(), Some("PS"));
        assert_eq!(third.count, 1);
    }

    /// #590: the same `(pso, stage)` twice is one group with a count of two, even when the
    /// identical records are not adjacent.
    #[test]
    fn groups_identical_gaps_together() {
        let record = r#"{"event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}"#;
        let other = r#"{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=PS wave_lanes_min=32 atomic64_flags=0x0: D3DMetal creates such pipelines but does not run them (#453)"}"#;
        let log = format!("{record}\n{other}\n{record}\n");
        let summary = summarize_jsonl(&log);
        assert_eq!(summary.shader_gaps.len(), 3);
        assert_eq!(summary.shader_gap_groups.len(), 2);
        let compute = &summary.shader_gap_groups[0];
        assert_eq!(compute.stage.as_deref(), Some("CS"));
        assert_eq!(compute.count, 2);
        assert_eq!(
            compute.variants,
            [ShaderGapVariant {
                wave_lanes_min: Some("64".into()),
                atomic64_flags: Some("0x400000".into()),
                count: 2,
            }]
        );
        assert_eq!(summary.shader_gap_groups[1].count, 1);
        // Every record is in exactly one group.
        let grouped: usize = summary.shader_gap_groups.iter().map(|g| g.count).sum();
        assert_eq!(grouped, summary.shader_gaps.len());
    }

    /// #590: one pipeline kind and stage can need different wave widths or atomic
    /// features, so a group keeps every distinct constraint set, not only the first.
    #[test]
    fn one_group_keeps_each_distinct_constraint_set() {
        let log = r#"{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=PS wave_lanes_min=64 atomic64_flags=0x0: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=PS wave_lanes_min=64 atomic64_flags=0x400000: D3DMetal creates such pipelines but does not run them (#453)"}
{"event":"shader_d3dmetal_gap","detail":"pso=graphics stage=PS wave_lanes_min=64 atomic64_flags=0x0: D3DMetal creates such pipelines but does not run them (#453)"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gap_groups.len(), 1);
        let group = &summary.shader_gap_groups[0];
        assert_eq!(group.count, 3);
        assert_eq!(
            group.variants,
            [
                ShaderGapVariant {
                    wave_lanes_min: Some("64".into()),
                    atomic64_flags: Some("0x0".into()),
                    count: 2,
                },
                ShaderGapVariant {
                    wave_lanes_min: Some("64".into()),
                    atomic64_flags: Some("0x400000".into()),
                    count: 1,
                },
            ]
        );
        assert_eq!(summary.shader_gaps_missing_constraints, 0);
    }

    /// #590: a gap record with a missing or empty field must not panic. It is grouped
    /// under `unknown` and counted, never dropped or guessed.
    #[test]
    fn a_malformed_gap_record_is_grouped_under_unknown_and_counted() {
        for (detail, missing_identity, missing_constraints) in [
            ("", 1, 1),
            ("garbage", 1, 1),
            ("pso=compute", 1, 1),
            ("stage=CS", 1, 1),
            (
                "pso= stage=CS wave_lanes_min=64 atomic64_flags=0x400000",
                1,
                0,
            ),
            ("pso=compute stage=CS", 0, 1),
            (
                "pso=compute stage=CS wave_lanes_min=64 atomic64_flags=",
                0,
                1,
            ),
        ] {
            let log = format!(r#"{{"event":"shader_d3dmetal_gap","detail":"{detail}"}}"#);
            let summary = summarize_jsonl(&log);
            assert_eq!(summary.shader_gaps.len(), 1, "detail={detail:?}");
            assert_eq!(
                summary.shader_gap_groups.len(),
                1,
                "detail={detail:?} lost its record"
            );
            assert_eq!(
                summary.shader_gaps_missing_identity, missing_identity,
                "detail={detail:?}"
            );
            assert_eq!(
                summary.shader_gaps_missing_constraints, missing_constraints,
                "detail={detail:?}"
            );
            let group = &summary.shader_gap_groups[0];
            assert_eq!(group.count, 1, "detail={detail:?}");
            let recorded: usize = group.variants.iter().map(|v| v.count).sum();
            assert_eq!(recorded, 1, "detail={detail:?}");
        }
    }

    /// #590: the fields a malformed record does carry are still reported.
    #[test]
    fn a_partly_malformed_gap_record_keeps_the_fields_it_has() {
        let log =
            r#"{"event":"shader_d3dmetal_gap","detail":"pso=compute stage=CS wave_lanes_min=64"}"#;
        let summary = summarize_jsonl(log);
        let group = &summary.shader_gap_groups[0];
        assert_eq!(group.pso.as_deref(), Some("compute"));
        assert_eq!(group.stage.as_deref(), Some("CS"));
        assert_eq!(
            group.variants,
            [ShaderGapVariant {
                wave_lanes_min: Some("64".into()),
                atomic64_flags: None,
                count: 1,
            }]
        );
        assert_eq!(summary.shader_gaps_missing_identity, 0);
        assert_eq!(summary.shader_gaps_missing_constraints, 1);
    }

    /// #590: a run with no gap records produces no groups and no missing-field counts, so
    /// the trace renders as it did before the grouping existed.
    #[test]
    fn a_run_with_no_gaps_has_no_groups() {
        let log = r#"{"ts_ms":10,"tid":1,"level":"INFO","sub":"d3d12_xs","event":"shader_gate","detail":"pso=compute stage=CS decision=pass_dxil hr=0x00000000 kind=dxil shader_stage=5 sm=6.6 bytes=1234 dxil=800 accc=0 sfi0=0x4000 wave_ops=1 int64_ops=0 atomic64=typed, wave_lanes=64-64"}"#;
        let summary = summarize_jsonl(log);
        assert_eq!(summary.shader_gate.total, 1);
        assert!(summary.shader_gaps.is_empty());
        assert!(summary.shader_gap_groups.is_empty());
        assert_eq!(summary.shader_gaps_missing_identity, 0);
        assert_eq!(summary.shader_gaps_missing_constraints, 0);
    }
}
