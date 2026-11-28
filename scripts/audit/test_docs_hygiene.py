#!/usr/bin/env python3
"""Unit tests for scripts/audit/docs_hygiene.py anchor validation.

Run with:
    python3 -m unittest scripts/audit/test_docs_hygiene.py
"""
import importlib.util
import sys
import unittest
from pathlib import Path

_MODULE_PATH = Path(__file__).resolve().parent / "docs_hygiene.py"
_spec = importlib.util.spec_from_file_location("docs_hygiene", _MODULE_PATH)
docs_hygiene = importlib.util.module_from_spec(_spec)
sys.modules["docs_hygiene"] = docs_hygiene
_spec.loader.exec_module(docs_hygiene)


class SlugTests(unittest.TestCase):
    def test_basic_slug(self):
        slugs = docs_hygiene.markdown_slugs("# Hello World\n")
        self.assertIn("hello-world", slugs)

    def test_strips_punctuation(self):
        slugs = docs_hygiene.markdown_slugs("## What's new? (2024)\n")
        self.assertIn("whats-new-2024", slugs)

    def test_keeps_underscore_and_hyphen(self):
        slugs = docs_hygiene.markdown_slugs("### foo_bar-baz\n")
        self.assertIn("foo_bar-baz", slugs)

    def test_duplicate_headings(self):
        body = "# Setup\n\n# Setup\n\n# Setup\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertIn("setup", slugs)
        self.assertIn("setup-1", slugs)
        self.assertIn("setup-2", slugs)

    def test_suffix_skips_literal_heading_slug(self):
        # Regression: counting identical bases alone lets a generated
        # suffix collide with a literal heading slug, so the third `Foo`
        # also became `foo-1` and a link to the wrong heading validated.
        body = "# Foo\n\n# Foo-1\n\n# Foo\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertEqual(slugs, {"foo", "foo-1", "foo-2"})

    def test_suffix_skips_multiple_literal_heading_slugs(self):
        body = "# Foo\n\n# Foo-1\n\n# Foo-2\n\n# Foo\n\n# Foo\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertEqual(slugs, {"foo", "foo-1", "foo-2", "foo-3", "foo-4"})

    def test_three_identical_headings(self):
        body = "# Foo\n\n# Foo\n\n# Foo\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertEqual(slugs, {"foo", "foo-1", "foo-2"})

    def test_literal_slug_before_duplicates(self):
        body = "# Foo-1\n\n# Foo\n\n# Foo\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertEqual(slugs, {"foo-1", "foo", "foo-2"})

    def test_heading_link_contributes_only_its_text(self):
        # GitHub slugs the rendered heading, where the link's URL is gone.
        # Slugging the raw source appended the URL and made the first real
        # link to the anchor look broken.
        slugs = docs_hygiene.markdown_slugs("## Setup [guide](other.md) notes\n")
        self.assertEqual(slugs, {"setup-guide-notes"})

    def test_heading_link_matches_github_for_the_readme_heading(self):
        # The exact heading at README.md:53; GitHub renders
        # `#active-milestone--m2-first-rendered-frame--shader-format-gate`.
        body = ("### Active Milestone \u2014 [M2: First Rendered Frame & Shader "
                "Format Gate](https://github.com/MacosPorters/betterxen/milestone/18)\n")
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertEqual(
            slugs, {"active-milestone--m2-first-rendered-frame--shader-format-gate"})

    def test_heading_link_duplicate_still_gets_a_suffix(self):
        body = "## See [ADR](a.md) notes\n\n## See [ADR](b.md) notes\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertEqual(slugs, {"see-adr-notes", "see-adr-notes-1"})

    def test_heading_link_inside_a_code_span_stays_literal(self):
        # GitHub keeps `[x](y)` literal inside a code span, so its URL stays in
        # the slug. Substituting links first made this `code-x-span`.
        slugs = docs_hygiene.markdown_slugs("## Code `[x](y)` span\n")
        self.assertEqual(slugs, {"code-xy-span"})

    def test_heading_mixes_a_code_span_and_a_link(self):
        # Only the real link loses its URL; the code span keeps its text.
        slugs = docs_hygiene.markdown_slugs("## See `a` and [b](c.md)\n")
        self.assertEqual(slugs, {"see-a-and-b"})

    def test_heading_double_backtick_code_span(self):
        # GitHub's own renderer gives `#use-a-b-cdmd-here` for this heading:
        # the inner single-backtick run closes the outer span early, so the
        # `[c](d.md)` after it is a real link and loses its URL. Asserting the
        # observed GitHub anchor keeps this honest rather than assuming the
        # "obvious" answer.
        slugs = docs_hygiene.markdown_slugs("## Use ``a `b` [c](d.md)`` here\n")
        self.assertEqual(slugs, {"use-a-b-cdmd-here"})

    def test_heading_image_is_not_treated_as_a_link(self):
        # An image contributes its alt text through GitHub's rendering, but
        # the `(?<!\!)` guard must keep this path unchanged for now.
        slugs = docs_hygiene.markdown_slugs("## ![img](a.png) caption\n")
        self.assertEqual(len(slugs), 1)

    def test_fenced_code_block_ignored(self):
        body = "```\n# Not A Heading\n```\n\n# Real Heading\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertIn("real-heading", slugs)
        self.assertNotIn("not-a-heading", slugs)

    def test_tilde_fence_ignored(self):
        body = "~~~\n# Not A Heading\n~~~\n"
        slugs = docs_hygiene.markdown_slugs(body)
        self.assertNotIn("not-a-heading", slugs)

    def test_atx_levels(self):
        body = "# One\n## Two\n### Three\n#### Four\n##### Five\n###### Six\n"
        slugs = docs_hygiene.markdown_slugs(body)
        for s in ("one", "two", "three", "four", "five", "six"):
            self.assertIn(s, slugs)


class CheckLinksTests(unittest.TestCase):
    """Exercise check_links with a synthetic in-memory file set."""

    def _run(self, files):
        """files: dict mapping repo-relative path -> Markdown body."""
        problems = []
        original_text = docs_hygiene.text

        def fake_text(path):
            return files.get(path)

        docs_hygiene.text = fake_text
        try:
            docs_hygiene.check_links(list(files), problems)
        finally:
            docs_hygiene.text = original_text
        return problems

    def test_existing_heading_passes(self):
        problems = self._run({
            "docs/a.md": "# Hello World\n\n[link](b.md#hello-world)\n",
            "docs/b.md": "# Hello World\n",
        })
        self.assertEqual(problems, [])

    def test_in_page_anchor_passes(self):
        problems = self._run({
            "docs/a.md": "# Hello World\n\n[link](#hello-world)\n",
        })
        self.assertEqual(problems, [])

    def test_nonexistent_heading_fails(self):
        problems = self._run({
            "docs/a.md": "[link](b.md#no-such-heading)\n",
            "docs/b.md": "# Hello World\n",
        })
        self.assertEqual(len(problems), 1)
        self.assertIn("#no-such-heading", problems[0])
        self.assertIn("heading not found", problems[0])

    def test_in_page_nonexistent_heading_fails(self):
        problems = self._run({
            "docs/a.md": "# Real\n\n[link](#this-heading-does-not-exist)\n",
        })
        self.assertEqual(len(problems), 1)
        self.assertIn("#this-heading-does-not-exist", problems[0])

    def test_duplicate_heading_addressable(self):
        problems = self._run({
            "docs/a.md": "[link](b.md#setup-1)\n",
            "docs/b.md": "# Setup\n\n# Setup\n",
        })
        self.assertEqual(problems, [])

    def test_fenced_heading_not_a_slug(self):
        problems = self._run({
            "docs/a.md": "[link](b.md#not-a-heading)\n",
            "docs/b.md": "```\n# Not A Heading\n```\n",
        })
        self.assertEqual(len(problems), 1)
        self.assertIn("#not-a-heading", problems[0])

    def test_external_url_with_fragment_not_checked(self):
        problems = self._run({
            "docs/a.md": "[link](https://example.com/page#nonexistent)\n",
        })
        self.assertEqual(problems, [])

    def test_mailto_not_checked(self):
        problems = self._run({
            "docs/a.md": "[link](mailto:foo@example.com#frag)\n",
        })
        self.assertEqual(problems, [])

    def test_protocol_relative_not_checked(self):
        problems = self._run({
            "docs/a.md": "[link](//example.com/page#frag)\n",
        })
        self.assertEqual(problems, [])

    def test_bare_hash_skipped(self):
        problems = self._run({
            "docs/a.md": "[link](#)\n",
        })
        self.assertEqual(problems, [])

    def test_missing_file_still_reported(self):
        problems = self._run({
            "docs/a.md": "[link](nonexistent.md#foo)\n",
        })
        self.assertEqual(len(problems), 1)
        self.assertIn("broken relative link", problems[0])

    def test_fragment_on_non_markdown_not_checked(self):
        problems = self._run({
            "docs/a.md": "[link](image.png#foo)\n",
            "docs/image.png": None,
        })
        # image.png is binary (text returns None) but it exists in the
        # file list, so the link itself resolves; fragment is not checked.
        self.assertEqual(problems, [])


class MarkerTests(unittest.TestCase):
    """The conflict-marker check must catch all four markers, including the diff3 one."""

    def _markers(self, text):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "f.md").write_text(text, encoding="utf-8")
            previous = docs_hygiene.ROOT
            docs_hygiene.ROOT = root
            try:
                problems = []
                docs_hygiene.check_markers({"f.md": text}, problems)
                return problems
            finally:
                docs_hygiene.ROOT = previous

    def test_every_marker_kind_is_reported(self):
        for line in ["<<<<<<< HEAD", "||||||| merged common ancestors", "=======", ">>>>>>> topic"]:
            problems = self._markers(line + "\n")
            self.assertEqual(len(problems), 1, f"{line!r} was not reported: {problems}")
            self.assertIn("f.md:1", problems[0])

    def test_diff3_base_marker_is_reported(self):
        base = self._markers("||||||| merged common ancestors\n")
        self.assertEqual(len(base), 1, "the diff3 base marker must be reported")
        self.assertIn("conflict marker", base[0])

    def test_a_diff3_block_reports_all_four_lines(self):
        block = "<<<<<<< HEAD\na\n||||||| merged common ancestors\nb\n=======\nc\n>>>>>>> topic\n"
        problems = self._markers(block)
        self.assertEqual(len(problems), 4, problems)

    def test_ordinary_content_is_not_a_marker(self):
        for text in ["| a | b |\n", "||||||\n", "======\n", "prose with | pipes\n", "---\n"]:
            self.assertEqual(self._markers(text), [], f"{text!r} was wrongly reported")


class WorkflowTests(unittest.TestCase):
    """check_workflows must reject unparseable YAML."""

    def _check(self, text):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / ".github" / "workflows" / "a.yml").write_text(text, encoding="utf-8")
            previous = docs_hygiene.ROOT
            docs_hygiene.ROOT = root
            try:
                problems = []
                docs_hygiene.check_workflows({".github/workflows/a.yml": text}, problems, require_yaml=True)
                return problems
            finally:
                docs_hygiene.ROOT = previous

    def test_parses_a_valid_workflow(self):
        self.assertEqual(self._check("on: push\njobs:\n  b:\n    runs-on: ubuntu-latest\n"), [])

    def test_reports_invalid_yaml(self):
        problems = self._check("on: [push\n")
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("a.yml", problems[0])


if __name__ == "__main__":
    unittest.main()
