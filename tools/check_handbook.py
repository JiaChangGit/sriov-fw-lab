#!/usr/bin/env python3
"""Check the generated handbook's local references, anchors and recorded source hashes."""
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit
from lab_common import ROOT, artifact_dir, provenance, sha256, write_json


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
            u=urlsplit(href)
            if u.scheme or u.netloc:continue
            target=(path.parent/unquote(u.path)).resolve() if u.path else path
            if not target.exists():issues.append("Missing target: "+href+" in "+path.name);continue
            links+=1
            if u.fragment and target.suffix==".html":
                dest=pages.get(target) or Page(target)
                if unquote(u.fragment) not in dest.ids:issues.append("Missing anchor: "+href)
            if u.fragment and target.suffix==".md":
                headings=[re.sub(r"[^\w\- ]","",line.lstrip("# ").lower()).replace(" ","-")
                          for line in target.read_text().splitlines() if line.startswith("#")]
                if unquote(u.fragment) not in headings:issues.append("Missing Markdown anchor: "+href)
    if pages[ROOT/"docs/handbook.html"].svgs!=6:issues.append("Six inline SVG diagrams required")
    result={**provenance(),"status":"FAIL" if issues else "PASS","scope":"document_integrity_and_local_links",
            "pages":len(pages),"local_links":links,"issues":issues,"visual_review":"separate_browser_review",
            "manifest_sha256":sha256(ROOT/"docs/handbook-manifest.json")}
    write_json(out/"result.json",result)
    print(json.dumps({"status":result["status"],"pages":len(pages),"local_links":links,"issues":issues,"evidence":str(out.relative_to(ROOT))}))
    return bool(issues)


if __name__=="__main__":raise SystemExit(main())
