#!/usr/bin/env python3
"""Check the generated handbook's local references, anchors and recorded source hashes."""
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit
from lab_common import ROOT, artifact_dir, provenance, sha256, write_json
from build_handbook import CHAPTERS
from render_doc_diagrams import FENCES


def markdown_headings(md, path):
    tokens = md.parse(path.read_text(encoding="utf-8"))
    counts = {}
    anchors = set()
    for index, token in enumerate(tokens):
        if token.type != "heading_open":
            continue
        slug = re.sub(r"[^\w\- ]", "", tokens[index + 1].content.lower()).replace(" ", "-")
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        anchors.add(slug + ("-" + str(count) if count else ""))
    return anchors


def check_target(path, href, pages, md, issues):
    u = urlsplit(href)
    if u.scheme or u.netloc:
        return 0
    target = (path.parent / unquote(u.path)).resolve() if u.path else path
    if not target.exists():
        issues.append("Missing target: " + href + " in " + str(path.relative_to(ROOT)))
        return 0
    fragment = unquote(u.fragment)
    if fragment and target.suffix == ".html":
        destination = pages.get(target) or Page(target)
        if fragment not in destination.ids:
            issues.append("Missing anchor: " + href)
    elif fragment and target.suffix == ".md":
        if fragment not in markdown_headings(md, target):
            issues.append("Missing Markdown anchor: " + href)
    elif fragment and re.fullmatch(r"L\d+(?:-L\d+)?", fragment):
        lines = [int(value) for value in re.findall(r"\d+", fragment)]
        if min(lines) < 1 or max(lines) > len(target.read_text(encoding="utf-8").splitlines()):
            issues.append("Invalid source line: " + href)
    return 1


class Page(HTMLParser):
    def __init__(self,path):
        super().__init__()
        self.ids=[];self.links=[];self.assets=[];self.svgs=0
        self.feed(path.read_text())
    def handle_starttag(self,tag,attrs):
        attrs=dict(attrs)
        if "id" in attrs:self.ids.append(attrs["id"])
        if tag=="a" and "href" in attrs:self.links.append(attrs["href"])
        if tag in ("script","img","iframe") and "src" in attrs:self.assets.append(attrs["src"])
        if tag=="link" and attrs.get("rel")=="stylesheet":self.assets.append(attrs.get("href",""))
        if tag=="svg":self.svgs+=1


def main():
    from markdown_it import MarkdownIt
    md = MarkdownIt("commonmark", {"html": False}).enable("table")
    out=artifact_dir("handbook-check");issues=[]
    manifest=json.loads((ROOT/"docs/handbook-manifest.json").read_text())
    for kind in ("inputs","outputs"):
        for name,digest in manifest[kind].items():
            if not (ROOT/name).is_file() or sha256(ROOT/name)!=digest:issues.append("Changed "+kind+": "+name)
    paths=[ROOT/name for name in manifest["outputs"] if name.endswith(".html")]
    pages={p:Page(p) for p in paths}
    links=0
    for path,page in pages.items():
        if len(page.ids)!=len(set(page.ids)):issues.append("Duplicate ID: "+str(path))
        if any(urlsplit(a).scheme or a.startswith("//") for a in page.assets):issues.append("Network asset: "+str(path))
        for href in page.links:
            links += check_target(path, href, pages, md, issues)
    markdown_paths = [ROOT / "README.md", *(ROOT / "docs" / name for name in CHAPTERS),
                      ROOT / "docs/README_ZH.md", ROOT / "docs/README_EN.md", ROOT / "docs/DIAGRAMS.md"]
    markdown_links = 0
    expected_diagrams = 0
    for path in markdown_paths:
        source = path.read_text(encoding="utf-8")
        if path.parent == ROOT / "docs" and path.name in CHAPTERS:
            expected_diagrams += len(FENCES.findall(source))
        for token in md.parse(source):
            for child in token.children or []:
                if child.type in ("link_open", "image"):
                    href = child.attrGet("href" if child.type == "link_open" else "src")
                    markdown_links += check_target(path, href, pages, md, issues)
                    if child.type == "image" and path.name in CHAPTERS and href.endswith(".svg"):
                        expected_diagrams += 1
    actual_diagrams = pages[ROOT / "docs/handbook.html"].svgs
    if actual_diagrams != expected_diagrams or actual_diagrams < 6:
        issues.append("Inline SVG count mismatch: expected %d, found %d" % (expected_diagrams, actual_diagrams))
    result={**provenance(),"status":"FAIL" if issues else "PASS","scope":"document_integrity_and_local_links",
            "pages":len(pages),"local_links":links,"markdown_links":markdown_links,
            "inline_diagrams":actual_diagrams,"issues":issues,"visual_review":"separate_browser_review",
            "manifest_sha256":sha256(ROOT/"docs/handbook-manifest.json")}
    write_json(out/"result.json",result)
    print(json.dumps({"status":result["status"],"pages":len(pages),"local_links":links,
                      "markdown_links":markdown_links,"inline_diagrams":actual_diagrams,
                      "issues":issues,"evidence":str(out.relative_to(ROOT))}))
    return bool(issues)


if __name__=="__main__":raise SystemExit(main())
