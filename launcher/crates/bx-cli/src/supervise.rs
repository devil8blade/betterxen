//! Supervision for `bx-cli run` (#408): the launcher gets its own process group, and a timeout
//! or signal stops that group and then the environment's `wineserver`.
//!
//! The `wineserver` is stopped only after a timeout or signal, never after a normal exit,
//! because it ends every program in that bottle or prefix, not only this run's.

use std::io::IsTerminal;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering};
use std::time::{Duration, Instant};

const POLL: Duration = Duration::from_millis(50);
/// How long the launcher group has to exit after SIGTERM before SIGKILL.
const GRACE: Duration = Duration::from_secs(5);
const SERVER_WAIT: Duration = Duration::from_secs(15);

/// Signals that stop the run: the guest group is terminated and the environment cleaned up.
/// SIGQUIT (Ctrl-\) is included so it cannot kill `bx-cli` and leave the guest running (#461).
const STOP_SIGNALS: [libc::c_int; 4] = [libc::SIGINT, libc::SIGTERM, libc::SIGHUP, libc::SIGQUIT];

static SIGNAL: AtomicI32 = AtomicI32::new(0);
static SUSPEND: AtomicBool = AtomicBool::new(false);
/// Total time this process has spent suspended inside [`supervise`], in nanoseconds.
static SUSPENDED_NANOS: AtomicU64 = AtomicU64::new(0);

extern "C" fn note_signal(signal: libc::c_int) {
    SIGNAL.store(signal, Ordering::SeqCst);
}

extern "C" fn note_suspend(_signal: libc::c_int) {
    SUSPEND.store(true, Ordering::SeqCst);
}

/// Installs `handler` for `signal` unless the disposition this process inherited is `SIG_IGN`.
/// Returns whether the handler was installed.
///
/// A signal the caller chose to ignore (`nohup`, or a `&` job of a non-interactive shell) stays
/// ignored, so it cannot stop the run or the bottle (#461).
fn install_unless_ignored(signal: libc::c_int, handler: extern "C" fn(libc::c_int)) -> bool {
    // SAFETY: `current` is a valid, writable sigaction; a null new action only queries. The
    // handlers passed here only store to an atomic, which is async-signal-safe.
    unsafe {
        let mut current: libc::sigaction = std::mem::zeroed();
        if libc::sigaction(signal, std::ptr::null(), &mut current) == 0
            && current.sa_sigaction == libc::SIG_IGN
        {
            return false;
        }
        libc::signal(signal, handler as libc::sighandler_t) != libc::SIG_ERR
    }
}

/// Records SIGINT, SIGTERM, SIGHUP and SIGQUIT instead of dying, so the run can be cleaned up,
/// and records SIGTSTP (Ctrl-Z) so the guest group can be suspended together with `bx-cli`.
/// Signals that were inherited as ignored are left ignored.
///
/// SIGTSTP is only acted on inside [`supervise`]. A Ctrl-Z typed while nothing is being
/// supervised (for example while shims are copied) has no visible effect until the next
/// supervised phase starts, which is then suspended at once.
pub fn install_signal_handlers() {
    for signal in STOP_SIGNALS {
        install_unless_ignored(signal, note_signal);
    }
    install_unless_ignored(libc::SIGTSTP, note_suspend);
}

pub fn received_signal() -> i32 {
    SIGNAL.load(Ordering::SeqCst)
}

/// Total time spent suspended (Ctrl-Z until continued) across every [`supervise`] call so far.
///
/// A caller that budgets one `--timeout` over several supervised phases subtracts this from its
/// own wall-clock measurement, so a pause during an earlier phase is not charged to a later one.
pub fn suspended_time() -> Duration {
    Duration::from_nanos(SUSPENDED_NANOS.load(Ordering::SeqCst))
}

/// True once per SIGTSTP received since the last call.
fn take_suspend_request() -> bool {
    SUSPEND.swap(false, Ordering::SeqCst)
}

/// Stops this process the way the terminal's Ctrl-Z would; returns when it is continued.
fn suspend_self(_guest: libc::pid_t) {
    // SAFETY: raise takes no pointers. SIGSTOP cannot be caught, so this stops the process.
    unsafe {
        libc::raise(libc::SIGSTOP);
    }
}

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Stop {
    TimedOut,
    Signalled(i32),
}

#[derive(Debug)]
pub enum Outcome {
    Exited(ExitStatus),
    Stopped(Stop),
}

impl Outcome {
    /// The guest's own numeric exit code (so a conformance fixture's documented exit-77 skip
    /// stays distinguishable from a failure, #565), 124 for a timeout (as `timeout(1)`),
    /// 128 + N for a signal.
    pub fn exit_code(&self) -> u8 {
        match self {
            // Unix guest codes are already 0-255; a signalled guest has no code, which is a
            // failure. `code.max(1)` at the call sites still turns a failed cleanup into 1.
            Outcome::Exited(status) => status.code().map_or(1, |code| code.clamp(0, 255) as u8),
            Outcome::Stopped(Stop::TimedOut) => 124,
            Outcome::Stopped(Stop::Signalled(signal)) => 128u8.saturating_add(*signal as u8),
        }
    }
}

fn signal_group(child: &Child, signal: libc::c_int) {
    // SAFETY: kill takes no pointers. The child leads its own group and has not been reaped,
    // so the group id cannot have been reused.
    unsafe {
        libc::kill(-(child.id() as libc::pid_t), signal);
    }
}

fn wait_until(child: &mut Child, deadline: Instant) -> std::io::Result<Option<ExitStatus>> {
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
        if Instant::now() >= deadline {
            return Ok(None);
        }
        std::thread::sleep(POLL);
    }
}

/// Runs `command` in a new process group until it exits, `timeout` passes, or `signal`
/// returns non-zero. On a stop the whole group is terminated before this returns.
///
/// The guest is not in the terminal's foreground group, so Ctrl-Z reaches only `bx-cli`. It is
/// passed on: the guest group gets SIGTSTP, `bx-cli` stops itself, and when `bx-cli` is continued
/// the guest group gets SIGCONT. Time spent suspended does not count against `timeout`.
pub fn supervise(
    command: Command,
    timeout: Option<Duration>,
    signal: impl Fn() -> i32,
) -> std::io::Result<Outcome> {
    supervise_with(command, timeout, signal, take_suspend_request, suspend_self)
}

/// [`supervise`] with the suspend request and the self-stop injected, so tests can exercise the
/// job-control path without stopping the test process. `suspend` is called with the guest
/// group's id after that group was sent SIGTSTP and returns when the launcher may continue.
fn supervise_with(
    mut command: Command,
    timeout: Option<Duration>,
    signal: impl Fn() -> i32,
    suspend_requested: impl Fn() -> bool,
    mut suspend: impl FnMut(libc::pid_t),
) -> std::io::Result<Outcome> {
    // A background process group is stopped by SIGTTIN if it reads the terminal, so a terminal
    // is replaced by end-of-file. Piped and redirected input is passed through.
    if std::io::stdin().is_terminal() {
        command.stdin(Stdio::null());
    }
    let mut child = command.process_group(0).spawn()?;
    let started = Instant::now();
    let mut suspended = Duration::ZERO;
    let stop = loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Outcome::Exited(status));
        }
        let received = signal();
        if received != 0 {
            break Stop::Signalled(received);
        }
        if suspend_requested() {
            let paused = Instant::now();
            signal_group(&child, libc::SIGTSTP);
            suspend(child.id() as libc::pid_t);
            signal_group(&child, libc::SIGCONT);
            let pause = paused.elapsed();
            suspended += pause;
            SUSPENDED_NANOS.fetch_add(
                u64::try_from(pause.as_nanos()).unwrap_or(u64::MAX),
                Ordering::SeqCst,
            );
            continue;
        }
        if timeout.is_some_and(|limit| started.elapsed().saturating_sub(suspended) >= limit) {
            break Stop::TimedOut;
        }
        std::thread::sleep(POLL);
    };
    signal_group(&child, libc::SIGTERM);
    // Wait without reaping first, so SIGKILL still reaches members that outlive the leader.
    std::thread::sleep(POLL);
    let deadline = Instant::now() + GRACE;
    while Instant::now() < deadline && group_leader_running(&child) {
        std::thread::sleep(POLL);
    }
    signal_group(&child, libc::SIGKILL);
    wait_until(&mut child, Instant::now() + GRACE)?;
    Ok(Outcome::Stopped(stop))
}

/// True while the leader has not exited; does not reap it.
fn group_leader_running(child: &Child) -> bool {
    // SAFETY: `info` is a valid, writable siginfo_t and WNOWAIT leaves the child unreaped.
    unsafe {
        let mut info: libc::siginfo_t = std::mem::zeroed();
        libc::waitid(
            libc::P_PID,
            child.id() as libc::id_t,
            &mut info,
            libc::WEXITED | libc::WNOHANG | libc::WNOWAIT,
        ) == 0
            && info.si_pid() == 0
    }
}

/// `wineserver -k` then `-w` for the environment at `prefix`.
///
/// `-k` exits 1 when no server is running (observed on CrossOver 26.3), so 1 is accepted
/// there; `-w` must still succeed, which a surviving server prevents.
///
/// Each `wineserver` runs in its own process group with no terminal input, so a second Ctrl-C
/// typed while cleanup is running reaches only `bx-cli` (which records it) and cannot kill the
/// cleanup itself (#461).
pub fn stop_server(
    wineserver: &Path,
    prefix: &Path,
    environment: &[(String, String)],
) -> Result<(), String> {
    for (flag, idle_code) in [("-k", Some(1)), ("-w", None)] {
        let mut child = Command::new(wineserver)
            .arg(flag)
            .env_clear()
            .envs(environment.iter().map(|(k, v)| (k, v)))
            .env("WINEPREFIX", prefix)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .process_group(0)
            .spawn()
            .map_err(|e| format!("cannot start wineserver {flag}: {e}"))?;
        match wait_until(&mut child, Instant::now() + SERVER_WAIT) {
            Ok(Some(status)) if status.success() => {}
            Ok(Some(status)) if idle_code.is_some() && status.code() == idle_code => {}
            other => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("wineserver {flag} did not succeed: {other:?}"));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn shell(script: &str) -> Command {
        let mut command = Command::new("/bin/sh");
        command.args(["-c", script]);
        command
    }

    fn scratch(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("bx-cli-{name}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn classifies_exit_timeout_and_signal() {
        let ok = supervise(shell("exit 0"), None, || 0).unwrap();
        assert_eq!(ok.exit_code(), 0);
        let failed = supervise(shell("exit 7"), Some(Duration::from_secs(10)), || 0).unwrap();
        assert_eq!(failed.exit_code(), 7);
        // A conformance fixture's documented skip must stay distinguishable from a failure
        // through `bx-cli run` (#565); the old `!status.success()` collapsed it to 1.
        let skipped = supervise(shell("exit 77"), Some(Duration::from_secs(10)), || 0).unwrap();
        assert_eq!(skipped.exit_code(), 77);
        let slow = supervise(shell("sleep 30"), Some(Duration::from_millis(200)), || 0).unwrap();
        assert!(matches!(slow, Outcome::Stopped(Stop::TimedOut)));
        assert_eq!(slow.exit_code(), 124);
        let interrupted = supervise(shell("sleep 30"), None, || libc::SIGTERM).unwrap();
        assert_eq!(interrupted.exit_code(), 128 + libc::SIGTERM as u8);
    }

    #[test]
    fn a_timeout_stops_children_that_ignore_sigterm() {
        let dir = scratch("group");
        let pid_file = dir.join("child.pid");
        let script = format!(
            "(trap '' TERM; exec sleep 30) & echo $! > '{}'; wait",
            pid_file.display()
        );
        supervise(shell(&script), Some(Duration::from_millis(500)), || 0).unwrap();
        let pid: libc::pid_t = std::fs::read_to_string(&pid_file)
            .unwrap()
            .trim()
            .parse()
            .unwrap();
        let gone = (0..100).any(|_| {
            std::thread::sleep(POLL);
            // SAFETY: signal 0 only checks whether the process exists.
            unsafe { libc::kill(pid, 0) != 0 }
        });
        assert!(gone, "grandchild {pid} survived the timeout");
        std::fs::remove_dir_all(dir).unwrap();
    }

    /// Signal dispositions and the recorded signal are process-wide: tests that touch them run
    /// one at a time and use signals no other test raises.
    static SIGNAL_TESTS: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn set_disposition(signal: libc::c_int, action: libc::sighandler_t) {
        // SAFETY: SIG_DFL and SIG_IGN are valid dispositions for these signals.
        unsafe {
            libc::signal(signal, action);
        }
    }

    fn disposition(signal: libc::c_int) -> libc::sighandler_t {
        // SAFETY: a null new action only queries; `current` is valid and writable.
        unsafe {
            let mut current: libc::sigaction = std::mem::zeroed();
            assert_eq!(libc::sigaction(signal, std::ptr::null(), &mut current), 0);
            current.sa_sigaction
        }
    }

    fn raise(signal: libc::c_int) {
        // SAFETY: raise takes no pointers.
        unsafe {
            libc::raise(signal);
        }
    }

    #[test]
    fn an_inherited_ignored_signal_stays_ignored() {
        let _guard = SIGNAL_TESTS.lock().unwrap_or_else(|e| e.into_inner());
        // As under nohup: the signal arrives already ignored.
        set_disposition(libc::SIGUSR2, libc::SIG_IGN);
        SIGNAL.store(0, Ordering::SeqCst);
        assert!(!install_unless_ignored(libc::SIGUSR2, note_signal));
        assert_eq!(disposition(libc::SIGUSR2), libc::SIG_IGN);
        raise(libc::SIGUSR2);
        assert_eq!(
            received_signal(),
            0,
            "an ignored signal must not stop the run"
        );

        // Control: the same signal with the default disposition is recorded.
        set_disposition(libc::SIGUSR2, libc::SIG_DFL);
        assert!(install_unless_ignored(libc::SIGUSR2, note_signal));
        raise(libc::SIGUSR2);
        assert_eq!(received_signal(), libc::SIGUSR2);
        set_disposition(libc::SIGUSR2, libc::SIG_DFL);
        SIGNAL.store(0, Ordering::SeqCst);
    }

    #[test]
    fn sigquit_is_recorded_and_stops_the_guest_group() {
        let _guard = SIGNAL_TESTS.lock().unwrap_or_else(|e| e.into_inner());
        assert!(STOP_SIGNALS.contains(&libc::SIGQUIT));
        let before = disposition(libc::SIGQUIT);
        set_disposition(libc::SIGQUIT, libc::SIG_DFL);
        SIGNAL.store(0, Ordering::SeqCst);
        assert!(install_unless_ignored(libc::SIGQUIT, note_signal));
        // Without the handler this would kill the test process, as it killed bx-cli.
        raise(libc::SIGQUIT);
        assert_eq!(received_signal(), libc::SIGQUIT);
        set_disposition(libc::SIGQUIT, before);
        SIGNAL.store(0, Ordering::SeqCst);

        let stopped = supervise(shell("sleep 30"), None, || libc::SIGQUIT).unwrap();
        assert!(matches!(
            stopped,
            Outcome::Stopped(Stop::Signalled(libc::SIGQUIT))
        ));
        assert_eq!(stopped.exit_code(), 128 + libc::SIGQUIT as u8);
    }

    /// True once `pid` is reported stopped; leaves the status in place.
    fn is_stopped(pid: libc::pid_t) -> bool {
        (0..100).any(|_| {
            // SAFETY: `info` is valid and writable; WNOWAIT leaves the child's state in place.
            let stopped = unsafe {
                let mut info: libc::siginfo_t = std::mem::zeroed();
                libc::waitid(
                    libc::P_PID,
                    pid as libc::id_t,
                    &mut info,
                    libc::WSTOPPED | libc::WNOHANG | libc::WNOWAIT,
                ) == 0
                    && info.si_pid() == pid
            };
            if !stopped {
                std::thread::sleep(POLL);
            }
            stopped
        })
    }

    #[test]
    fn a_suspend_request_stops_and_resumes_the_guest_without_using_up_the_timeout() {
        let requested = std::cell::Cell::new(false);
        let guest_was_stopped = std::cell::Cell::new(false);
        let suspended_before = suspended_time();
        let outcome = supervise_with(
            shell("sleep 0.4; exit 0"),
            // Shorter than the suspension below: only running time may count.
            Some(Duration::from_millis(1500)),
            || 0,
            || !requested.replace(true),
            |guest| {
                guest_was_stopped.set(is_stopped(guest));
                std::thread::sleep(Duration::from_millis(2000));
            },
        )
        .unwrap();
        assert!(guest_was_stopped.get(), "the guest group was not stopped");
        // The pause is also reported to callers that budget a timeout across several phases.
        let reported = suspended_time() - suspended_before;
        assert!(reported >= Duration::from_millis(2000), "{reported:?}");
        match outcome {
            Outcome::Exited(status) => assert!(status.success(), "{status}"),
            other => panic!("the resumed guest should have finished on its own: {other:?}"),
        }
    }

    #[test]
    fn cleanup_runs_outside_the_callers_process_group() {
        let dir = scratch("cleanup-group");
        let server = dir.join("wineserver");
        let seen = dir.join("pgid");
        std::fs::write(
            &server,
            format!(
                "#!/bin/sh\n/usr/bin/perl -MPOSIX -e 'print getpgrp(), \"\\n\"' >> '{}'\n",
                seen.display()
            ),
        )
        .unwrap();
        std::fs::set_permissions(&server, std::fs::Permissions::from_mode(0o755)).unwrap();
        stop_server(&server, &dir, &[]).unwrap();
        // SAFETY: getpgrp takes no arguments and cannot fail.
        let ours = unsafe { libc::getpgrp() };
        let groups: Vec<libc::pid_t> = std::fs::read_to_string(&seen)
            .unwrap()
            .lines()
            .map(|line| line.trim().parse().unwrap())
            .collect();
        assert_eq!(groups.len(), 2, "-k and -w should both have run");
        for group in groups {
            // A terminal signal goes to the foreground group, which these are not part of.
            assert_ne!(group, ours, "cleanup shares the caller's process group");
        }
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn an_idle_server_is_accepted_only_when_the_wait_succeeds() {
        let dir = scratch("server");
        for (name, kill, wait, ok) in [
            ("running", 0, 0, true),
            ("idle", 1, 0, true),
            ("stuck", 1, 1, false),
            ("broken", 2, 0, false),
        ] {
            let server = dir.join(name);
            std::fs::write(
                &server,
                format!("#!/bin/sh\n[ \"$1\" = -k ] && exit {kill}\nexit {wait}\n"),
            )
            .unwrap();
            std::fs::set_permissions(&server, std::fs::Permissions::from_mode(0o755)).unwrap();
            assert_eq!(stop_server(&server, &dir, &[]).is_ok(), ok, "{name}");
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
