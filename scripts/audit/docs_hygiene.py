#!/usr/bin/env python3
"""Fast, deterministic documentation and repository hygiene checks.

Checks only tracked files:
  1. no merge-conflict markers in tracked text files;
  2. every relative Markdown link points at a tracked file or directory;
  3. every fragment (``#heading``) in a Markdown link resolves to a real
     heading in the target Markdown file, including in-page ``](#foo)``
     links validated against the same file's headings;
  4. every workflow file under .github/workflows parses as YAML.

External URLs (http:, https:, mailto:, //) and bare ``#`` links are not
fetched or validated, so the result does not depend on the network.
Exit status is 1 when a problem is found. Pass --require-yaml to fail,
not skip, when PyYAML is missing.
"""
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[2]
MARKER = re.compile(r"^(<{7}( |$)|\|{7}( |$)|={7}$|>{7}( |$))")
LINK = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"^\s*(```|~~~)")
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*#*\s*$")
# GitHub slugs the rendered heading, so an inline link contributes only its
# text: ``## See [ADR](x.md)`` slugs as ``see-adr``, not ``see-adrxmd``.
# Link syntax inside a code span stays literal, so those spans are set aside
# before substituting and put back afterwards.
INLINE_LINK = re.compile(r"(?<!\!)\[([^\]]*)\]\([^)]*\)")
CODE_SPAN = re.compile(r"(`+)(.+?)\1")


def tracked():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True,
                         capture_output=True).stdout.decode("utf-8", "replace")
    return [p for p in out.split("\0") if p and (ROOT / p).is_file()]


def text(path):
    data = (ROOT / path).read_bytes()
    if b"\0" in data:
        return None
    return data.decode("utf-8", "replace")


def _slug(text):
    """GitHub-style slug: lowercase, strip non-alnum/-/_/space, spaces -> -."""
    text = text.lower()
    text = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE)
    return text.replace(" ", "-")


def _heading_text(text):
    """Rendered text of a heading: link text in, code spans left literal."""
    spans = []

    def stash(match):
        spans.append(match.group(0))
        return "\x00%d\x00" % (len(spans) - 1)

    stashed = CODE_SPAN.sub(stash, text)
    substituted = INLINE_LINK.sub(r"\1", stashed)
    for index, span in enumerate(spans):
        substituted = substituted.replace("\x00%d\x00" % index, span)
    return substituted


def markdown_slugs(body):
    """Return the set of GitHub-style heading slugs in a Markdown document.

    Duplicate headings get ``-1``, ``-2``, ... suffixes. Headings inside
    fenced code blocks are ignored.
    """
    slugs = set()
    seen = {}
    in_fence = False
    for line in body.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = HEADING.match(line)
        if not m:
            continue
        base = _slug(_heading_text(m.group(2)))
        if not base:
            continue
        # Advance the numeric suffix until we find a slug that has not
        # already been emitted. Counting only identical bases is not
        # enough: a literal heading such as ``Foo-1`` may already own
        # ``foo-1``, so the next ``Foo`` must become ``foo-2``.
        count = seen.get(base, 0)
        slug = base if count == 0 else f"{base}-{count}"
        while slug in slugs:
            count += 1
            slug = f"{base}-{count}"
        seen[base] = count + 1
        slugs.add(slug)
    return slugs


def check_markers(files, problems):
    for path in files:
        if path.endswith((".patch", ".diff")):
            continue
        body = text(path)
        if body is None:
            continue
        for number, line in enumerate(body.splitlines(), 1):
            if MARKER.match(line):
                problems.append(f"{path}:{number}: merge conflict marker")


def _resolve_link(path, target):
    """Resolve a relative link target to a repo-root-relative POSIX path."""
    base = PurePosixPath(path).parent
    resolved = PurePosixPath(target.lstrip("/")) if target.startswith("/") else base / target
    parts = []
    for part in resolved.parts:
        if part == "..":
            if parts:
                parts.pop()
        elif part != ".":
            parts.append(part)
    return "/".join(parts)


def check_links(files, problems):
    known = set(files)
    dirs = {str(d) for f in files for d in PurePosixPath(f).parents}
    slug_cache = {}

    def slugs_for(md_path):
        if md_path not in slug_cache:
            slug_cache[md_path] = markdown_slugs(text(md_path) or "")
        return slug_cache[md_path]

    for path in files:
        if not path.endswith(".md") or path.startswith(("artifacts/", "third_party/")):
            continue
        in_fence = False
        for number, line in enumerate((text(path) or "").splitlines(), 1):
            if FENCE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            line = re.sub(r"`[^`]*`", "", line)
            for raw_target in LINK.findall(line):
                if re.match(r"^([a-z][a-z0-9+.-]*:|//)", raw_target, re.I):
                    continue
                if raw_target == "#":
                    continue
                fragment = ""
                if "#" in raw_target:
                    fragment = unquote(raw_target.split("#", 1)[1])
                target = unquote(raw_target.split("#", 1)[0].split("?", 1)[0])
                if not target and not fragment:
                    continue
                if target:
                    norm = _resolve_link(path, target)
                    if norm and norm not in known and norm not in dirs:
                        problems.append(f"{path}:{number}: broken relative link '{target}'")
                        continue
                else:
                    norm = path
                if fragment and norm.endswith(".md") and norm in known:
                    if fragment not in slugs_for(norm):
                        shown = raw_target
                        problems.append(
                            f"{path}:{number}: broken fragment '#{fragment}' "
                            f"in link '{shown}' (heading not found)"
                        )


def check_workflows(files, problems, require_yaml):
    try:
        import yaml
    except ImportError:
        if require_yaml:
            problems.append("PyYAML is not installed but --require-yaml was given")
        else:
            print("note: PyYAML not installed; workflow YAML parse check skipped", file=sys.stderr)
        return
    for path in files:
        if path.startswith(".github/workflows/") and path.endswith((".yml", ".yaml")):
            try:
                yaml.safe_load((ROOT / path).read_text(encoding="utf-8"))
            except yaml.YAMLError as err:
                problems.append(f"{path}: invalid YAML: {str(err).splitlines()[0]}")


def main():
    files = tracked()
    problems = []
    check_markers(files, problems)
    check_links(files, problems)
    check_workflows(files, problems, "--require-yaml" in sys.argv[1:])
    for problem in problems:
        print(problem)
    print(f"docs hygiene: {len(files)} tracked files checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
