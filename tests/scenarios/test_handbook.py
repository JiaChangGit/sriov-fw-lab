"""Reject stale diagrams and broken GitHub links without running build or VM gates."""
from pathlib import Path
import sys
import tempfile
import unittest
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from markdown_it import MarkdownIt
from build_handbook import inline_svg, render_markdown
from check_handbook import Page, check_target, markdown_headings


class HandbookTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "artifacts").mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=ROOT / "artifacts")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.md = MarkdownIt("commonmark", {"html": False}).enable("table")

    def test_changed_diagram_requires_matching_svg(self):
        with self.assertRaisesRegex(RuntimeError, "Missing or stale Mermaid SVG"):
            render_markdown(self.md, "```mermaid\nflowchart TD\nUncachedProofA-->UncachedProofB\n```\n", "chapter-")

    def test_repeated_svg_has_unique_ids_and_scoped_references(self):
        source = self.directory / "source.svg"
        source.write_text('<svg id="chart" aria-labelledby="title description">'
            '<style>#chart .node{fill:white}</style><title id="title">Flow</title>'
            '<desc id="description">Arrow</desc><defs><marker id="arrow"/></defs>'
            '<path marker-end="url(#arrow)"/></svg>')
        page = self.directory / "page.html"
        page.write_text(inline_svg(source, "first-") + inline_svg(source, "second-"))
        parsed = Page(page)
        self.assertEqual(len(parsed.ids), len(set(parsed.ids)))
        self.assertEqual(parsed.svgs, 2)
        for prefix in ("first-", "second-"):
            self.assertIn('aria-labelledby="' + prefix + 'title ' + prefix + 'description"', page.read_text())
            self.assertIn('url(#' + prefix + 'arrow)', page.read_text())
            self.assertIn('#' + prefix + 'chart .node', page.read_text())

    def test_markdown_anchors_ignore_fenced_code_and_handle_duplicates(self):
        path = self.directory / "anchors.md"
        path.write_text("# 重置與隔離\n```python\n# not a heading\n```\n# 重置與隔離\n")
        self.assertEqual(markdown_headings(self.md, path), {"重置與隔離", "重置與隔離-1"})

    def test_encoded_markdown_anchor_resolves(self):
        path = self.directory / "anchors.md"
        path.write_text("# 重置與隔離\n")
        issues = []
        self.assertEqual(check_target(path, "#" + quote("重置與隔離"), {}, self.md, issues), 1)
        self.assertEqual(issues, [])
        check_target(path, "#missing", {}, self.md, issues)
        self.assertEqual(len(issues), 1)

    def test_invalid_source_lines_are_rejected(self):
        path = self.directory / "source.c"
        path.write_text("one\ntwo\nthree\n")
        issues = []
        check_target(path, "#L3", {}, self.md, issues)
        self.assertEqual(issues, [])
        for fragment in ("#L0", "#L4", "#L2-L9"):
            check_target(path, fragment, {}, self.md, issues)
        self.assertEqual(len(issues), 3)


if __name__ == "__main__":
    unittest.main()
