#!/usr/bin/env python3
"""Pre-render Markdown Mermaid fences with a pinned library and a local Chromium.

Run only after changing diagram source. Normal handbook builds use the committed
SVG files and need neither a browser nor network access.
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
VERSION = "11.4.1"
LIBRARY_SHA256 = "a43bc1afd446f9c4cc66ac5dd45d02e8d65e26fc5344ec0ef787f88d6ddb6f9e"
FENCES = re.compile(r"^```mermaid[^\S\n]*\n(.*?)^```[^\S\n]*$", re.M | re.S)


def diagram_key(source):
    return hashlib.sha256(source.strip().encode("utf-8")).hexdigest()


def diagram_sources():
    for path in sorted((ROOT / "docs").glob("*.md")):
        for source in FENCES.findall(path.read_text(encoding="utf-8")):
            yield path, source.strip()


def browser_path(explicit):
    candidates = [explicit, os.environ.get("DOCS_BROWSER")]
    candidates += [shutil.which(name) for name in ("chromium", "chromium-browser", "google-chrome")]
    for variable in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"):
        base = os.environ.get(variable)
        if base:
            candidates += [str(Path(base) / name) for name in (
                "Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise SystemExit("Specify a local Chromium browser with --browser or DOCS_BROWSER.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", help="Path to a local Chromium, Chrome or Edge executable")
    args = parser.parse_args()
    browser = browser_path(args.browser)
    cache = ROOT / "artifacts" / "documentation-renderer"
    cache.mkdir(parents=True, exist_ok=True)
    library = cache / ("mermaid-" + VERSION + ".min.js")
    if not library.exists():
        url = "https://cdn.jsdelivr.net/npm/mermaid@" + VERSION + "/dist/mermaid.min.js"
        data = urllib.request.urlopen(url, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != LIBRARY_SHA256:
            raise RuntimeError("Mermaid download hash mismatch")
        library.write_bytes(data)
    if hashlib.sha256(library.read_bytes()).hexdigest() != LIBRARY_SHA256:
        raise RuntimeError("Cached Mermaid hash mismatch")
    sources = {diagram_key(source): source for _, source in diagram_sources()}
    with tempfile.TemporaryDirectory(prefix="sriov-docs-") as temporary:
        scratch = Path(temporary)
        shutil.copyfile(library, scratch / "mermaid.js")
        # One page and one browser process avoid repeated browser startup costs.
        payload = json.dumps(sources, ensure_ascii=True).replace("<", "\\u003c")
        document = """<!doctype html><meta charset="utf-8"><body>
<script src="mermaid.js"></script><script>
mermaid.initialize({startOnLoad:false,securityLevel:'strict',theme:'neutral',htmlLabels:false,
  fontFamily:'Arial, Microsoft JhengHei, Noto Sans CJK TC, sans-serif',
  flowchart:{htmlLabels:false,curve:'linear',useMaxWidth:false},
  sequence:{useMaxWidth:false,wrap:true},state:{useMaxWidth:false}});
async function draw() {
  try {
    for (const [key,source] of Object.entries(SOURCES)) {
      const result=await mermaid.render('diagram-'+key.slice(0,16),source);
      const output=document.createElement('pre');output.dataset.key=key;
      output.textContent=result.svg;document.body.appendChild(output);
    }
    document.body.dataset.complete='true';
  } catch (error) {document.body.dataset.error=String(error);}
}
draw();</script>""".replace("SOURCES", payload)
        page = scratch / "render.html"
        page.write_text(document, encoding="utf-8")
        result = subprocess.run([browser, "--headless", "--disable-gpu", "--no-first-run",
            "--no-default-browser-check", "--disable-extensions", "--disable-background-networking",
            "--user-data-dir=" + str(scratch / "profile"), "--allow-file-access-from-files",
            "--virtual-time-budget=30000", "--dump-dom", page.as_uri()],
            capture_output=True, timeout=60, check=True)
        dom = result.stdout.decode("utf-8")
        if 'data-complete="true"' not in dom:
            raise RuntimeError("Diagram render did not complete: " + dom[-4000:])
        rendered = re.findall(r'<pre data-key="([a-f0-9]+)">(.*?)</pre>', dom, re.S)
        if len(rendered) != len(sources):
            raise RuntimeError("Rendered diagram count mismatch")
        for key, escaped in rendered:
            data = html.unescape(escaped)
            # Mermaid's embedded XHTML may serialize a line break as HTML <br>.
            # A standalone SVG must remain well-formed XML.
            data = re.sub(r"<br\s*>", "<br/>", data)
            ET.fromstring(data)
            data = data.replace("<svg ", '<svg data-source-sha256="' + key +
                '" data-renderer="mermaid-' + VERSION + '" ', 1)
            (ROOT / "docs" / "diagrams" / ("mermaid-" + key[:16] + ".svg")).write_text(
                data + "\n", encoding="utf-8")
    print(json.dumps({"status": "PASS", "diagrams": len(sources), "mermaid": VERSION,
                      "library_sha256": LIBRARY_SHA256}))


if __name__ == "__main__":
    main()
