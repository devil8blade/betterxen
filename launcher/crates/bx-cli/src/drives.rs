//! Xbox title drive letters, linked in the prefix's `dosdevices` before launch.
//!
//! The project-authorized target title we traced reads its package from `G:` and writes
//! temporary files such as crash logs to `T:`. That is an observation of one title at startup;
//! that `G:` is the package root and `T:` temporary storage for Xbox titles in general is an
//! inference. A Wine prefix has neither drive, so that title's first `G:\...` open fails with
//! `STATUS_OBJECT_PATH_NOT_FOUND`. `bx-cli run` links `g:` to the executable's directory and
//! `t:` to `drive_c/bx_temp`.
//!
//! As with system32 staging (#474), a drive letter the prefix already uses is not ours. A
//! `.bx-drives` manifest in `dosdevices` records each link this tool made, and only a link that
//! is listed and still points where it was pointed is replaced. Any other mapping to a
//! different target is reported and left unchanged.

use std::collections::BTreeMap;
use std::fs;
use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{OpenOptionsExt, symlink};
use std::path::{Path, PathBuf};

const MANIFEST: &str = ".bx-drives";
const LOCK_FILE: &str = ".bx-drive-mapping.lock";

/// The prefix-relative target of `t:`, in the form Wine itself uses for `c:`.
pub const TEMP_TARGET: &str = "../drive_c/bx_temp";

#[derive(Debug)]
pub struct DriveMappingLock {
    _file: fs::File,
}

/// Holds the prefix's drive mappings stable until the returned lock is dropped.
pub fn lock_drive_mappings(prefix: &Path) -> Result<DriveMappingLock, String> {
    fs::create_dir_all(prefix).map_err(|e| {
        format!(
            "cannot prepare prefix {} for drive lock: {e}",
            prefix.display()
        )
    })?;
    let prefix = fs::canonicalize(prefix).map_err(|e| {
        format!(
            "cannot resolve prefix {} for drive lock: {e}",
            prefix.display()
        )
    })?;
    let path = prefix.join(LOCK_FILE);
    let file = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&path)
        .map_err(|e| format!("cannot open drive lock {}: {e}", path.display()))?;

    loop {
        // The open file descriptor stays alive in DriveMappingLock for the full launch.
        let result = unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
        if result == 0 {
            return Ok(DriveMappingLock { _file: file });
        }
        let error = io::Error::last_os_error();
        if error.kind() == io::ErrorKind::Interrupted {
            continue;
        }
        if error.kind() == io::ErrorKind::WouldBlock {
            return Err(format!(
                "another bx-cli run is active for prefix {}; refusing to launch with shared Xbox drive mappings",
                prefix.display()
            ));
        }
        return Err(format!("cannot lock {}: {error}", path.display()));
    }
}

#[derive(Debug)]
pub enum Mapping {
    Created(char, PathBuf),
    /// A link this tool made earlier, pointed at a new target.
    Retargeted(char, PathBuf),
    Unchanged(char, PathBuf),
    /// A mapping this tool did not make, to a different target; left as it is.
    Foreign {
        letter: char,
        existing: String,
        wanted: PathBuf,
    },
}

impl Mapping {
    pub fn describe(&self) -> String {
        match self {
            Mapping::Created(l, t) => {
                format!("mapped {}: to {}", l.to_ascii_uppercase(), t.display())
            }
            Mapping::Retargeted(l, t) => {
                format!(
                    "remapped own {}: to {}",
                    l.to_ascii_uppercase(),
                    t.display()
                )
            }
            Mapping::Unchanged(l, t) => {
                format!(
                    "{}: already maps to {}",
                    l.to_ascii_uppercase(),
                    t.display()
                )
            }
            Mapping::Foreign {
                letter,
                existing,
                wanted,
            } => format!(
                "warning: {}: already maps to {existing}, not {}; left unchanged, so the guest \
                 may not find its files there",
                letter.to_ascii_uppercase(),
                wanted.display()
            ),
        }
    }
}

fn read_manifest(dosdevices: &Path) -> Result<BTreeMap<char, PathBuf>, String> {
    let path = dosdevices.join(MANIFEST);
    let text = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(BTreeMap::new()),
        Err(e) => return Err(format!("cannot read {}: {e}", path.display())),
    };
    let mut owned = BTreeMap::new();
    for line in text.lines().filter(|l| !l.is_empty()) {
        let mut chars = line.chars();
        match (chars.next(), chars.next()) {
            (Some(letter), Some('\t')) if letter.is_ascii_lowercase() => {
                let encoded = chars.as_str();
                let target = match encoded.strip_prefix('@') {
                    Some(hex) => decode_path(hex)
                        .ok_or_else(|| format!("malformed path in {}: {line:?}", path.display()))?,
                    None => PathBuf::from(encoded),
                };
                owned.insert(letter, target);
            }
            _ => return Err(format!("malformed line in {}: {line:?}", path.display())),
        }
    }
    Ok(owned)
}

fn write_manifest(dosdevices: &Path, owned: &BTreeMap<char, PathBuf>) -> Result<(), String> {
    let path = dosdevices.join(MANIFEST);
    let mut text = String::new();
    for (letter, target) in owned {
        text.push_str(&format!("{letter}\t@{}\n", encode_path(target)));
    }
    // Written to a temporary file and renamed over the manifest, like `relink`, so an
    // interrupted write can never leave a truncated manifest that later runs refuse to parse.
    let tmp = dosdevices.join(format!("{MANIFEST}.tmp.{}", std::process::id()));
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)
        .map_err(|e| format!("cannot create {}: {e}", tmp.display()))?;
    let written = file.write_all(text.as_bytes()).and_then(|()| {
        drop(file);
        fs::rename(&tmp, &path)
    });
    if written.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    written.map_err(|e| format!("cannot write {}: {e}", path.display()))
}

fn encode_path(path: &Path) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let bytes = path.as_os_str().as_bytes();
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        encoded.push(char::from(HEX[usize::from(byte >> 4)]));
        encoded.push(char::from(HEX[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn decode_path(hex: &str) -> Option<PathBuf> {
    fn nibble(byte: u8) -> Option<u8> {
        match byte {
            b'0'..=b'9' => Some(byte - b'0'),
            b'a'..=b'f' => Some(byte - b'a' + 10),
            b'A'..=b'F' => Some(byte - b'A' + 10),
            _ => None,
        }
    }

    let bytes = hex.as_bytes();
    if bytes.len() % 2 != 0 {
        return None;
    }
    let mut decoded = Vec::with_capacity(bytes.len() / 2);
    for pair in bytes.chunks_exact(2) {
        decoded.push((nibble(pair[0])? << 4) | nibble(pair[1])?);
    }
    Some(PathBuf::from(std::ffi::OsString::from_vec(decoded)))
}

/// Points `link` at `target`, replacing what is there through a rename so the link never
/// disappears in between.
fn relink(dosdevices: &Path, link: &Path, target: &Path) -> io::Result<()> {
    let tmp = dosdevices.join(format!(".bx-drive-tmp.{}", std::process::id()));
    let _ = fs::remove_file(&tmp);
    symlink(target, &tmp)?;
    let result = fs::rename(&tmp, link);
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

/// Links `g:` to `package_dir` and `t:` to [`TEMP_TARGET`] in `prefix`'s `dosdevices`.
pub fn map_xbox_drives(prefix: &Path, package_dir: &Path) -> Result<Vec<Mapping>, String> {
    let dosdevices = prefix.join("dosdevices");
    if !dosdevices.is_dir() {
        return Err(format!(
            "{} does not exist (start the prefix once so Wine creates it)",
            dosdevices.display()
        ));
    }
    let temp_dir = prefix.join("drive_c/bx_temp");
    fs::create_dir_all(&temp_dir)
        .map_err(|e| format!("cannot create {}: {e}", temp_dir.display()))?;

    let mut owned = read_manifest(&dosdevices)?;
    let mut mappings = Vec::new();
    for (letter, target) in [
        ('g', package_dir.to_path_buf()),
        ('t', PathBuf::from(TEMP_TARGET)),
    ] {
        let link = dosdevices.join(format!("{letter}:"));
        let current = match fs::symlink_metadata(&link) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => None,
            Err(e) => return Err(format!("cannot inspect {}: {e}", link.display())),
            Ok(meta) if meta.file_type().is_symlink() => Some(
                fs::read_link(&link).map_err(|e| format!("cannot read {}: {e}", link.display()))?,
            ),
            Ok(_) => {
                mappings.push(Mapping::Foreign {
                    letter,
                    existing: format!("a non-link entry {}", link.display()),
                    wanted: target,
                });
                continue;
            }
        };
        let mapping = match current {
            None => {
                owned.insert(letter, target.clone());
                write_manifest(&dosdevices, &owned)?;
                relink(&dosdevices, &link, &target)
                    .map_err(|e| format!("cannot create {}: {e}", link.display()))?;
                Mapping::Created(letter, target.clone())
            }
            Some(current) if current == target => Mapping::Unchanged(letter, target.clone()),
            Some(current) if owned.get(&letter) == Some(&current) => {
                owned.insert(letter, target.clone());
                write_manifest(&dosdevices, &owned)?;
                if let Err(error) = relink(&dosdevices, &link, &target) {
                    owned.insert(letter, current);
                    let restore = write_manifest(&dosdevices, &owned);
                    let detail = restore
                        .err()
                        .map(|e| format!("; also could not restore the drive manifest: {e}"))
                        .unwrap_or_default();
                    return Err(format!(
                        "cannot replace {}: {error}{detail}",
                        link.display()
                    ));
                }
                Mapping::Retargeted(letter, target.clone())
            }
            Some(current) => {
                mappings.push(Mapping::Foreign {
                    letter,
                    existing: current.display().to_string(),
                    wanted: target,
                });
                continue;
            }
        };
        mappings.push(mapping);
    }
    Ok(mappings)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TempDir(PathBuf);
    impl TempDir {
        fn new(tag: &str) -> Self {
            let p = std::env::temp_dir().join(format!(
                "bx-drives-{tag}-{}-{:?}",
                std::process::id(),
                std::thread::current().id()
            ));
            let _ = fs::remove_dir_all(&p);
            fs::create_dir_all(p.join("prefix/dosdevices")).unwrap();
            fs::create_dir_all(p.join("game")).unwrap();
            TempDir(p)
        }
        fn prefix(&self) -> PathBuf {
            self.0.join("prefix")
        }
        fn game(&self) -> PathBuf {
            self.0.join("game")
        }
        fn link(&self, letter: char) -> PathBuf {
            self.0.join(format!("prefix/dosdevices/{letter}:"))
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn missing_drives_are_created_and_resolve() {
        let t = TempDir::new("create");
        fs::write(t.game().join("package.bin"), b"pkg").unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(
            mappings[..],
            [Mapping::Created('g', _), Mapping::Created('t', _)]
        ));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), t.game());
        assert_eq!(
            fs::read_link(t.link('t')).unwrap(),
            PathBuf::from(TEMP_TARGET)
        );
        // Both links resolve the way Wine resolves them, from inside dosdevices.
        assert_eq!(fs::read(t.link('g').join("package.bin")).unwrap(), b"pkg");
        assert!(t.link('t').is_dir());
    }

    #[test]
    fn a_second_run_changes_nothing() {
        let t = TempDir::new("rerun");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(
            mappings[..],
            [Mapping::Unchanged('g', _), Mapping::Unchanged('t', _)]
        ));
    }

    #[test]
    fn an_own_link_follows_a_new_package_directory() {
        let t = TempDir::new("retarget");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(mappings[0], Mapping::Retargeted('g', _)));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), other);
    }

    #[test]
    fn a_foreign_mapping_is_reported_and_kept() {
        let t = TempDir::new("foreign");
        let elsewhere = t.0.join("elsewhere");
        symlink(&elsewhere, t.link('g')).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(&mappings[0], Mapping::Foreign { letter: 'g', .. }));
        assert!(mappings[0].describe().starts_with("warning: G:"));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), elsewhere);
        // A link this tool did not record is never treated as its own on a later run.
        let again = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(&again[0], Mapping::Foreign { letter: 'g', .. }));
    }

    #[test]
    fn an_own_link_changed_by_someone_else_becomes_foreign() {
        let t = TempDir::new("changed");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        fs::remove_file(t.link('g')).unwrap();
        let elsewhere = t.0.join("elsewhere");
        symlink(&elsewhere, t.link('g')).unwrap();
        let other = t.0.join("other-game");
        let mappings = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(&mappings[0], Mapping::Foreign { letter: 'g', .. }));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), elsewhere);
    }

    #[test]
    fn a_non_link_entry_is_never_touched() {
        let t = TempDir::new("nonlink");
        fs::create_dir_all(t.link('t')).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(&mappings[1], Mapping::Foreign { letter: 't', .. }));
        assert!(t.link('t').is_dir() && !fs::symlink_metadata(t.link('t')).unwrap().is_symlink());
    }

    #[test]
    fn a_prefix_without_dosdevices_is_an_error() {
        let t = TempDir::new("nodos");
        fs::remove_dir_all(t.prefix().join("dosdevices")).unwrap();
        let e = map_xbox_drives(&t.prefix(), &t.game()).unwrap_err();
        assert!(e.contains("dosdevices"), "{e}");
    }

    #[test]
    fn a_malformed_manifest_is_an_error_not_an_ownership_claim() {
        let t = TempDir::new("badmanifest");
        fs::write(t.prefix().join("dosdevices").join(MANIFEST), "g /x\n").unwrap();
        assert!(map_xbox_drives(&t.prefix(), &t.game()).is_err());
    }

    #[test]
    fn a_failed_manifest_write_leaves_no_unrecorded_link() {
        let t = TempDir::new("orphan");
        // A directory where the manifest's temporary file goes makes the write fail.
        let tmp = t
            .prefix()
            .join("dosdevices")
            .join(format!("{MANIFEST}.tmp.{}", std::process::id()));
        fs::create_dir(&tmp).unwrap();
        assert!(map_xbox_drives(&t.prefix(), &t.game()).is_err());
        assert!(
            fs::symlink_metadata(t.link('g')).is_err(),
            "g: must not be created before ownership is recorded"
        );

        fs::remove_dir(&tmp).unwrap();
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();
        let again = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(&again[0], Mapping::Retargeted('g', _)));
    }

    #[test]
    fn an_interrupted_manifest_write_keeps_the_previous_manifest() {
        let t = TempDir::new("atomic");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let manifest = t.prefix().join("dosdevices").join(MANIFEST);
        let before = fs::read(&manifest).unwrap();
        // Fail the next write after the new contents would have been produced: the old
        // manifest must survive whole, never truncated or half written.
        let tmp = t
            .prefix()
            .join("dosdevices")
            .join(format!("{MANIFEST}.tmp.{}", std::process::id()));
        fs::create_dir(&tmp).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();
        assert!(map_xbox_drives(&t.prefix(), &other).is_err());
        assert_eq!(
            fs::read(&manifest).unwrap(),
            before,
            "previous manifest kept intact"
        );
        fs::remove_dir(&tmp).unwrap();
        // The intact manifest still proves ownership, so the retry re-points g:.
        let again = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(&again[0], Mapping::Retargeted('g', _)));
        assert!(
            fs::read_dir(t.prefix().join("dosdevices"))
                .unwrap()
                .all(|e| !e.unwrap().file_name().to_string_lossy().contains(".tmp."))
        );
    }

    #[test]
    fn manifest_temp_does_not_follow_an_existing_symlink() {
        let t = TempDir::new("manifest-symlink");
        let dosdevices = t.prefix().join("dosdevices");
        let target = t.0.join("outside-manifest");
        fs::write(&target, b"unchanged").unwrap();
        let tmp = dosdevices.join(format!("{MANIFEST}.tmp.{}", std::process::id()));
        symlink(&target, &tmp).unwrap();

        assert!(write_manifest(&dosdevices, &BTreeMap::new()).is_err());
        assert_eq!(fs::read(&target).unwrap(), b"unchanged");
        assert_eq!(fs::read_link(&tmp).unwrap(), target);
    }

    #[test]
    fn a_failed_retarget_restores_the_previous_manifest_and_link() {
        let t = TempDir::new("retarget-failure");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();

        let temp_link = t
            .prefix()
            .join("dosdevices")
            .join(format!(".bx-drive-tmp.{}", std::process::id()));
        fs::create_dir(&temp_link).unwrap();
        assert!(map_xbox_drives(&t.prefix(), &other).is_err());
        assert_eq!(fs::read_link(t.link('g')).unwrap(), t.game());
        assert_eq!(
            read_manifest(&t.prefix().join("dosdevices")).unwrap()[&'g'],
            t.game()
        );
    }

    #[test]
    fn a_prefix_lock_rejects_another_launcher_until_released() {
        let t = TempDir::new("lock");
        let lock = lock_drive_mappings(&t.prefix()).unwrap();
        let error = lock_drive_mappings(&t.prefix()).unwrap_err();
        assert!(error.contains("another bx-cli run"), "{error}");

        drop(lock);
        assert!(lock_drive_mappings(&t.prefix()).is_ok());
    }

    #[test]
    fn a_non_utf8_target_round_trips_through_the_manifest() {
        let t = TempDir::new("non-utf8");
        let mut game_bytes = t.0.as_os_str().as_bytes().to_vec();
        game_bytes.extend_from_slice(b"/game-\xff");
        let game = PathBuf::from(std::ffi::OsString::from_vec(game_bytes));
        let owned = BTreeMap::from([('g', game.clone())]);
        write_manifest(&t.prefix().join("dosdevices"), &owned).unwrap();
        assert_eq!(
            read_manifest(&t.prefix().join("dosdevices")).unwrap()[&'g'],
            game
        );
    }

    #[test]
    fn a_legacy_plain_text_target_still_reads() {
        let t = TempDir::new("legacy-manifest");
        let legacy_target = t.0.join("old-game");
        fs::write(
            t.prefix().join("dosdevices").join(MANIFEST),
            format!("g\t{}\n", legacy_target.display()),
        )
        .unwrap();
        assert_eq!(
            read_manifest(&t.prefix().join("dosdevices")).unwrap()[&'g'],
            legacy_target
        );
    }
}
