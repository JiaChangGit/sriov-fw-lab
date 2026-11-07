#!/usr/bin/env python3
"""Render the local Traditional Chinese handbook and source listings without CDN assets."""
import html
import importlib.metadata
import json
from pathlib import Path
import re
from urllib.parse import quote

from lab_common import ROOT, artifact_dir, provenance, sha256, write_json
from render_doc_diagrams import diagram_key

DOCS = ROOT / "docs"
CHAPTERS = ["LEARNING_PATH_ZH.md", "BUILD_TUTORIAL_ZH.md", "TOOLS_DEEP_DIVE_ZH.md",
            "SYSTEM_ANALYSIS_ZH.md", "KERNEL_WALKTHROUGH_ZH.md", "TRACE_CODE_ZH.md",
            "TRACE_PRACTICE_ZH.md", "DEBUGGING_ZH.md", "DEMO_ZH.md",
            "SYSTEM_GUIDE_ZH.md", "BUG_LOG.md", "VALIDATION.md"]
CODE = ["fwcore/fsm.c", "fwcore/context.c", "fwcore/admission.c", "fwcore/mailbox.c", "fwcore/codec.c", "fwcore/scheduler.c", "device/toy_pf.c",
        "device/toy_vf.c", "device/toy_hw.c", "guest/pf/toy_pf.c", "guest/pf/toy_control.inc", "guest/admin/labd.c",
        "guest/admin/t06.inc", "guest/admin/recovery.inc", "guest/tenant/vfio.c", "guest/tenant/owner.c",
        "guest/tenant/recovery.c", "guest/tenant/m1_spike.c",
        "guest/tenant/reset.c", "guest/tenant/tenant.c", "guest/common/lab_ring.h",
        "guest/common/lab_wire.c"]
CSS = """
:root{color-scheme:light;--ink:#18293c;--blue:#184d83;--line:#ced9e4;--muted:#526477}
*{box-sizing:border-box}body{margin:0;background:#f2f5f8;color:var(--ink);font:17px/1.8 system-ui,'Microsoft JhengHei',sans-serif}
a{color:var(--blue);overflow-wrap:anywhere}header{background:#102e4d;color:#fff;padding:48px max(24px,calc((100vw - 1100px)/2))}
header p{max-width:850px}header a{color:#b8dcff}header .label{letter-spacing:.15em;font-size:13px;color:#b8dcff}
.layout{max-width:1420px;margin:auto;display:grid;grid-template-columns:230px minmax(0,1fr);gap:32px;padding:30px}
nav{position:sticky;top:20px;align-self:start;max-height:95vh;overflow:auto;font-size:14px}nav a{display:block;margin:12px 0}
main{min-width:0;background:white;padding:36px 44px;border:1px solid var(--line);border-radius:12px}
h1{font-size:2rem;line-height:1.35}h2{font-size:1.5rem;margin-top:2em;border-bottom:1px solid var(--line);padding-bottom:10px}h3{font-size:1.1rem}
p{max-width:95ch}section+section{margin-top:80px;border-top:4px solid var(--blue);padding-top:26px}
pre{overflow:auto;background:#112c45;color:#eaf3fa;padding:20px;border-radius:8px;font-size:14px;line-height:1.6}
code{font-family:Consolas,monospace;font-size:.88em}p code,li code,td code{background:#edf3f8;padding:2px 5px;border-radius:4px;overflow-wrap:anywhere}
.table-wrap{overflow:auto}table{border-collapse:collapse;width:100%;margin:20px 0;font-size:15px}th,td{border:1px solid var(--line);padding:11px 13px;text-align:left;vertical-align:top}th{background:#eaf1f8}
img,svg{max-width:100%;height:auto}.diagram{overflow:auto;margin:20px 0}.diagram svg{max-width:none;display:block}blockquote,.notice{border-left:5px solid #c4841b;background:#fff5df;padding:16px 20px;margin:24px 0}.muted{color:var(--muted)}
.source-line{display:block;white-space:pre}.source-line:target{background:#355b79}.line-no{display:inline-block;width:5em;color:#9db8ce;text-decoration:none;user-select:none}
@media(max-width:850px){.layout{display:block;padding:14px}nav{position:static;max-height:none}main{padding:24px 18px}header{padding:30px 20px}h1{font-size:1.65rem}}
@media print{body{background:white;font-size:11pt}header{color:#18293c;background:white;padding:0}nav{display:none}.layout{display:block;padding:0}main{border:0;padding:0}section{break-before:page}pre{white-space:pre-wrap}a{color:inherit}}
"""


def esc(text): return html.escape(str(text), quote=True)


def text(x, y, lines, size=17, color="#18344e", anchor="start"):
    return '<text x="%s" y="%s" font-size="%s" fill="%s" text-anchor="%s">%s</text>' % (
        x,y,size,color,anchor,"".join('<tspan x="%s" dy="%s">%s</tspan>' %
        (x,0 if i==0 else size*1.5,esc(s)) for i,s in enumerate(lines)))


def box(x,y,w,h,title,body=(),fill="#edf4fc"):
    return ('<rect x="%s" y="%s" width="%s" height="%s" rx="12" fill="%s" stroke="#7792ae"/>' %
            (x,y,w,h,fill)) + text(x+18,y+31,[title],19) + text(x+18,y+60,body,15)


def arrow(x1,y1,x2,y2,label="",label_x=None,label_y=None):
    return ('<path d="M%s %s L%s %s" stroke="#2b628f" stroke-width="2" fill="none" marker-end="url(#arrow)"/>' %
            (x1,y1,x2,y2)) + (text(label_x if label_x is not None else (x1+x2)/2,
                               label_y if label_y is not None else (y1+y2)/2-10,[label],14,anchor="middle") if label else "")


def svg(name,w,h,title,content):
    out=DOCS/"diagrams"/(name+".svg")
    out.parent.mkdir(parents=True,exist_ok=True)
    data='<svg xmlns="http://www.w3.org/2000/svg" width="%s" viewBox="0 0 %s %s" role="img" aria-labelledby="title">' % (w,w,h)+\
         '<title id="title">'+esc(title)+'</title><defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10z" fill="#2b628f"/></marker></defs>'+\
         '<rect width="100%" height="100%" fill="#fafcfe"/><g font-family="system-ui,Microsoft JhengHei,sans-serif">'+content+'</g></svg>\n'
    data=data.replace('id="title"','id="'+name+'-title"').replace('aria-labelledby="title"','aria-labelledby="'+name+'-title"')
    data=data.replace('id="arrow"','id="'+name+'-arrow"').replace('url(#arrow)','url(#'+name+'-arrow)')
    out.write_text(data,encoding="utf-8")


def diagrams():
    c=text(25,34,["執行位置與責任：Windows → WSL / QEMU → guest Linux"],23)
    c+=box(25,65,1030,75,"L0 · Windows 11 / 16 GB",["WSL 管理與實體 RAM / SSD；不載入 toy PF driver"])
    c+=box(25,165,1030,470,"L1 · WSL2 Ubuntu 26.04",["建置工具、Python controller、artifacts；一次一個 QEMU guest"])
    c+=box(55,255,360,140,"QEMU process / toy PCIe device",["PF + 最多 8 VF / vIOMMU","VF requester DMA / fence / MSI-X","fwcore：VF 狀態機與 ledger"])
    c+=box(490,255,530,340,"L2 · 自建 Linux guest",["guest kernel / Intel vIOMMU / VFIO"])
    c+=box(510,330,225,105,"PF driver + labd",["管理交易、lease、pidfd","mailbox / READY / REARM"])
    c+=box(755,330,245,105,"tenant A / tenant B",["獨立 VFIO fd + domain","SQ / CQ / payload / IRQ"])
    c+=arrow(735,390,755,390)
    c+=arrow(490,485,415,485,"PCI MMIO",448,465)+arrow(415,520,490,520,"DMA / IRQ",448,552)
    c+='<path d="M235 395 L235 520 L415 520 M235 485 L415 485" stroke="#2b628f" stroke-width="2" fill="none"/>'
    c+=text(520,490,["CPU VA → guest physical pages","VF RID + IOVA → domain translation","PF 管理入口與 tenant 資料入口分離"],17)
    svg("architecture",1080,660,"系統架構與各層所有權",c)

    c=text(25,36,["資料流：一筆 FILL / CRC32 的完整路徑"],23)
    c+=box(25,85,310,150,"1 · Tenant 發布 SQ",["CPU 寫 32-byte LE descriptor","release → SQ_TAIL → DOORBELL","payload IOVA = 0x400000"])
    c+=box(395,85,310,150,"2 · Device fetch / validate",["ACTIVE + config + BME + gate","保存 descriptor / generation","保留 CQ capacity，最多一筆 delay"])
    c+=box(765,85,310,150,"3 · VF requester + IOMMU",["PCI_DEVICE(對應 VF)","RID 0101 / 0102 → 各自 domain","每次 helper 檢查 MemTxResult"])
    c+=arrow(335,160,395,160)+arrow(705,160,765,160)
    c+=box(765,305,310,155,"4 · Payload DMA",["FILL 寫 / CRC32 讀","每 chunk 核對 gate / generation","跨頁失敗不保證回滾已寫 bytes"])
    c+=box(395,305,310,155,"5 · CQ 發布",["內容與 reserved bytes → phase","phase 最後成功才更新 CQ_TAIL","再檢查 IRQ gate → MSI-X"])
    c+=box(25,305,310,155,"6 · Tenant 消費",["eventfd 提醒 → acquire CQ phase","核 id / epoch / status / payload","完成檢查才更新 CQ_HEAD"])
    c+=arrow(920,235,920,305)+arrow(765,380,705,380)+arrow(395,380,335,380)
    c+=text(35,520,["位址隔離：RID + domain + IOVA　　時間隔離：PF fence　　世代隔離：session / epoch / cancel token"],18)
    svg("data-flow",1100,560,"提交佇列、VF DMA、完成佇列與通知的資料流",c)

    c=text(24,34,["Reset with outstanding work · T07/T08 實作時序"],22)
    xs=[105,385,660,960]; names=["tenant A（owner）","labd + PF driver","device / fwcore","healthy VF B"]
    for x,name in zip(xs,names):
        c+=text(x,80,[name],17,anchor="middle")
        c+='<path d="M%s 96 V970" stroke="#9cadbe" stroke-dasharray="5 5"/>'%x
    for a,b,y,label in [(0,2,145,"SQ 已 fetch；completion 保留，outstanding=1"),
                         (1,0,215,"STOP_REQUEST：停止新增提交"),
                         (1,2,280,"FENCE → cancel / 清 IRQ → 換代；readback"),
                         (0,1,345,"STOPPED：舊請求尚未完成"),
                         (1,2,410,"QUIESCE / RESET → RECOVERING；仍 fenced"),
                         (1,0,475,"RESET_NOTICE：新 epoch，outstanding=0"),
                         (0,1,550,"舊 request ERROR；HELLO，刻意延後 READY"),
                         (1,0,625,"允許配置：exact UNMAP 舊 IOVA → MAP 新 buffer"),
                         (0,2,690,"CONFIG_COMMIT：新 epoch / 新 ready_gen"),
                         (0,1,755,"READY：核 lease / peer / tuple"),
                         (1,2,825,"REARM：解除 fence → RUNNING"),
                         (1,0,900,"RUN：才可使用新 queue")]:
        c+=arrow(xs[a],y,xs[b],y,label,(xs[a]+xs[b])/2,y-13)
    c+=box(840,160,240,590,"B 保持同 epoch",["持續驗證固定 I/O","","A pending","A RECOVERING","A fresh CONFIG fenced","A resumed","","各階段核對進度","與 SQ/CQ/payload guard"])
    c+=text(25,1010,["T08 在 fresh CONFIG fenced 與 REARM 後各執行一次舊 callback：要求只增加 STALE_CPL，無 DMA / IRQ。"],16)
    svg("reset-sequence",1110,1040,"重置時 owner、driver、device 與健康 VF 的時序",c)

    c=text(25,36,["Firmware lifecycle：主要路徑（完整 20 條轉移見下方來源表）"],22)
    states=[(25,85,"FREE"),(365,85,"PROVISIONING"),(725,85,"ACTIVE"),
            (725,285,"QUIESCING"),(365,285,"RESETTING"),(25,285,"RECOVERING"),
            (365,500,"FAILED"),(725,500,"DESTROYING")]
    for x,y,name in states:c+=box(x,y,230,80,name,fill="#fff2e5" if name=="FAILED" else "#edf4fc")
    c+=arrow(255,125,365,125,"CREATE")+arrow(595,125,725,125,"COMMIT")
    c+=arrow(840,165,840,285,"FENCE",895,225)
    c+=arrow(725,325,595,325,"DRAIN_OK")+arrow(365,325,255,325,"RESET_DONE")
    c+='<path d="M140 285 V218 H670 V125 H725" fill="none" stroke="#2b628f" stroke-width="2" marker-end="url(#arrow)"/>'
    c+=text(320,207,["READY_REARM（核對 fresh CONFIG）"],15)
    c+=arrow(480,365,480,500,"EV_TIMEOUT",570,445)
    c+='<path d="M140 365 V540 H365" fill="none" stroke="#2b628f" stroke-width="2" marker-end="url(#arrow)"/>'
    c+=text(185,525,["VERIFY_FAIL"],15)
    c+=arrow(595,540,725,540,"DESTROY")
    c+=text(25,625,["FLR：ACTIVE / QUIESCING / RESETTING / RECOVERING → RECOVERING（queue 清除、fence 保持）",
                        "RELEASE_OK：DESTROYING → FREE；release 失敗 → FAILED。FAILED 保留 quota；KILL 不因 FREE 自動解除。",
                        "ACTIVE 不是 RUNNING 的充分條件：硬體另檢查 config_valid、BME、fence、kill 與 generation。"],16)
    svg("lifecycle",1060,735,"Firmware 主生命週期及 queue gate 的差別",c)

    c=text(25,36,["控制流程：先保留資源，再公開 PCI VF；失敗反向收斂"],22)
    for y,title,body in [(85,"1 · 管理入口與 guard",["labd → sriov_numvfs；admin mutex 序列化","拒絕 live lease / 改變非零 VF 數 / 超過 8 VF"]),
                         (215,"2 · fwcore CREATE / COMMIT",["先驗 slot / reserve / 資源上限","FREE → PROVISIONING → ACTIVE；先建立 ledger"]),
                         (345,"3 · pci_enable_sriov",["Linux 枚舉實際 PCI VF","建立 sysfs / IOMMU group；此時資料 gate 仍 fenced"]),
                         (475,"4 · lease → VFIO owner",["獨立 container、MAP SQ/CQ/payload","latch epoch → CONFIG_COMMIT → READY tuple"]),
                         (605,"5 · PF REARM",["核 lease / session / epoch / ready_gen","確認 RUNNING readback 才允許 tenant 提交"] )]:
        c+=box(25,y,605,100,title,body)
        if y<605:c+=arrow(325,y+100,325,y+130)
    c+=box(705,215,355,145,"失敗 unwind",["移除已公開 VF（如有）","反向 destroy 已 provision slots","release 失敗仍保留 quota"],fill="#fff2e5")
    c+=arrow(630,265,705,265,"FAIL",668,248)+arrow(630,395,705,320)
    c+=box(705,475,355,150,"owner 失敗 cleanup",["先 fence / drain","exact UNMAP → close fds → lease release","短 UNMAP 保留 buffer 到 fd 關閉"],fill="#fff2e5")
    c+=arrow(630,525,705,525,"FAIL",668,507)
    c+=text(35,760,["T02 在真實 probe / provision / PCI 部分枚舉 / owner 階段注入故障，再驗證 cleanup 與 retry。"],16)
    svg("control-flow",1090,800,"管理控制流程及錯誤反向回收",c)

    c=text(25,36,["正常請求時序：資料完成、completion 可見與 IRQ 是不同事件"],22)
    xs=[120,430,760,1040]
    for x,name in zip(xs,["tenant CPU","SQ / CQ memory","QEMU device","VF IOMMU"]):
        c+=text(x,86,[name],17,anchor="middle")
        c+='<path d="M%s 105 V805" stroke="#9cadbe" stroke-dasharray="5 5"/>'%x
    for a,b,y,label in [(0,1,155,"寫 descriptor / payload；release"),(0,2,225,"SQ_TAIL → DOORBELL"),
                         (2,3,295,"以 VF requester fetch SQ / payload"),(3,2,365,"translation + MemTxResult"),
                         (2,1,435,"CQ 內容與 reserved bytes"),(2,1,505,"phase 最後發布 → CQ_TAIL"),
                         (2,0,575,"MSI-X → eventfd 通知"),(0,1,645,"acquire phase → 核 id / epoch / status"),
                         (0,2,715,"更新 CQ_HEAD；允許重用 slot")]:
        c+=arrow(xs[a],y,xs[b],y,label,(xs[a]+xs[b])/2,y-12)
    c+=text(30,840,["eventfd counter 不是 completion 數量；以 CQ phase 判斷可見性，SQ slot 與 CQ slot 也不保證相同。"],16)
    svg("io-sequence",1170,880,"單筆 DMA 與 completion 的發布時序",c)


def page(title,body):
    return '<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>'+esc(title)+'</title><style>'+CSS+'</style>'+body+'</html>'


def code_name(name): return name.replace("/","__")+".html"


def source_line(filename, symbol):
    lines = (ROOT / filename).read_text(encoding="utf-8").splitlines()
    found = [i for i, line in enumerate(lines, 1)
             if re.search(r'^\s*(?:[A-Za-z_]\w*\s+\**)+\b' + re.escape(symbol) + r'\s*\(', line)
             and not re.match(r'^\s*(return|if|else|while)\b', line)]
    if not found:
        found = [i for i, line in enumerate(lines, 1)
                 if re.search(r'\b' + re.escape(symbol) + r'\s*\[.*\]\s*=', line)]
    if not found:
        raise RuntimeError("Unknown trace symbol: " + filename + ":" + symbol)
    return found[0]


def inline_svg(path, prefix):
    data = path.read_text(encoding="utf-8")
    # 同一圖可能在多章出現；所有 ID（包括 marker、clipPath、ARIA）按出現位置命名。
    ids = re.findall(r'\sid="([^"]+)"', data)
    for identifier in sorted(set(ids), key=len, reverse=True):
        new = prefix + identifier
        data = data.replace('id="' + identifier + '"', 'id="' + new + '"')
        data = data.replace('url(#' + identifier + ')', 'url(#' + new + ')')
        data = data.replace('href="#' + identifier + '"', 'href="#' + new + '"')
        data = re.sub(r'(aria-(?:labelledby|describedby)=")([^"]+)(")',
                      lambda m: m[1] + ' '.join(new if value == identifier else value
                                for value in m[2].split()) + m[3], data)
    # Mermaid scopes CSS with the root ID; replace selectors after attribute IDs.
    for identifier in sorted(set(ids), key=len, reverse=True):
        data = re.sub(r'#' + re.escape(identifier) + r'(?![\w-])',
                      lambda _: '#' + prefix + identifier, data)
    return '<div class="diagram">' + data + '</div>\n'


def render_markdown(md,source,prefix):
    tokens=md.parse(source)
    counts={}
    for i,t in enumerate(tokens):
        if t.type == "fence" and t.info.strip() == "mermaid":
            key = diagram_key(t.content)
            image = DOCS / "diagrams" / ("mermaid-" + key[:16] + ".svg")
            if not image.is_file() or 'data-source-sha256="' + key + '"' not in image.read_text():
                raise RuntimeError("Missing or stale Mermaid SVG; run tools/render_doc_diagrams.py: " + key)
            t.type = "html_block"
            t.content = inline_svg(image, prefix + "diagram-" + str(i) + "-")
            continue
        if t.type!="heading_open": continue
        slug=re.sub(r"[^\w\- ]","",tokens[i+1].content.lower()).replace(" ","-")
        count=counts.get(slug,0);counts[slug]=count+1
        t.attrSet("id",prefix+slug+("-"+str(count) if count else ""))
    return md.renderer.render(tokens,md.options,{})


def main():
    from markdown_it import MarkdownIt
    out=artifact_dir("handbook")
    diagrams()
    source_dir=DOCS/"code";source_dir.mkdir(exist_ok=True)
    inputs={}
    for name in CODE:
        p=ROOT/name
        if not p.is_file(): continue
        inputs[name]=sha256(p)
        lines='\n'.join('<span class="source-line" id="L%d"><a class="line-no" href="#L%d">%d</a>%s</span>' %
                        (i,i,i,esc(line)) for i,line in enumerate(p.read_text().splitlines(),1))
        (source_dir/code_name(name)).write_text(page(name,'<main><a href="../handbook.html">回教材</a><h1>'+esc(name)+
            '</h1><p>SHA256 <code>'+inputs[name]+'</code></p><pre>'+lines+'</pre></main>'),encoding="utf-8")
    md=MarkdownIt("commonmark",{"html":False}).enable("table")
    toc=[];chapters=[]
    for index,name in enumerate(CHAPTERS):
        src=DOCS/name;inputs[str(src.relative_to(ROOT))]=sha256(src)
        title=src.read_text().splitlines()[0].lstrip("# ")
        body=render_markdown(md,src.read_text(),"chapter-%d-"%index)
        body=body.replace('<table>','<div class="table-wrap"><table>').replace('</table>','</table></div>')
        # 依原始碼位置建立可以離線點擊的 trace anchor，函式不存在就拒絕猜行號。
        pattern=r'(<a\b[^>]*>.*?</a>)|<code>((?:fwcore|device|guest)/[A-Za-z0-9_./]+):([A-Za-z0-9_]+)(?:\(\))?</code>'
        def link(match):
            if match.group(1): return match.group(0)
            filename,symbol=match.group(2),match.group(3)
            if filename not in inputs: return match.group(0)
            return '<a href="code/'+quote(code_name(filename))+'#L'+str(source_line(filename,symbol))+'">'+match.group(0)+'</a>'
        body=re.sub(pattern,link,body)
        def source_link(match):
            filename,fragment=match.group(1),match.group(2) or ""
            return 'href="code/'+quote(code_name(filename))+fragment+'"' if filename in CODE else match.group(0)
        body=re.sub(r'href="\.\./((?:fwcore|device|guest)/[A-Za-z0-9_./]+)(#[^"]+)?"',source_link,body)
        # artifacts 不隨版本庫發布；保留文字路徑，避免乾淨 checkout 出現失效連結。
        # 本地 SVG 直接嵌入；斷網及僅複製 HTML 時圖仍可見。
        for image in (DOCS/"diagrams").glob("*.svg"):
            body=re.sub(r'<img src="diagrams/'+re.escape(image.name)+r'" alt="[^"]*"\s*/?>','<div class="diagram">'+image.read_text()+'</div>',body)
        # Markdown 章節交叉引用留在同份 HTML；其他原始 Markdown 仍保留為來源連結。
        for target_index,target in enumerate(CHAPTERS):
            body=re.sub(r'href="'+re.escape(target)+r'(?:#([^"]+))?"',
                        lambda m:'href="#chapter-'+str(target_index)+('-'+m.group(1) if m.group(1) else '')+'"',body)
        toc.append('<a href="#chapter-%d">%02d · %s</a>'%(index,index+1,esc(title)))
        chapters.append('<section id="chapter-%d">%s</section>'%(index,body))
    fsm=(ROOT/"fwcore/fsm.c").read_text()
    edges=re.findall(r'\{FW_(\w+),FW_(\w+),FW_(\w+)\}',fsm)
    table='<h2>來源中的完整 FSM 轉移表</h2><p>由當前 fwcore/fsm.c 抽取；前置條件仍須閱讀函式內的 guard。</p><table><thead><tr><th>From</th><th>Event</th><th>To</th></tr></thead><tbody>'
    table+=''.join('<tr>'+''.join('<td><code>'+esc(c)+'</code></td>' for c in row)+'</tr>' for row in edges)+'</tbody></table>'
    fsm_chapter=CHAPTERS.index("SYSTEM_ANALYSIS_ZH.md")
    chapters[fsm_chapter]=chapters[fsm_chapter].replace('</section>',table+'</section>')
    header='<header><div class="label">SR-IOV FIRMWARE & PLATFORM LAB</div><h1>從一筆 DMA，看懂隔離與生命週期</h1><p>Linux driver／虛擬化實作教材 · 繁體中文 · 流程、資料流與狀態圖</p><p>16 GB 筆電 · WSL2 · QEMU TCG · 交付範圍 M0–M3／T01–T10。實測結果與限制請見驗證紀錄。</p></header>'
    (DOCS/"handbook.html").write_text(page("SR-IOV 系統分析與建置教材",header+'<div class="layout"><nav>'+''.join(toc)+
        '<a href="handbook-manifest.json">版本與檔案雜湊</a></nav><main>'+''.join(chapters)+'</main></div>'),encoding="utf-8")
    inputs["tools/build_handbook.py"]=sha256(Path(__file__))
    inputs["tools/render_doc_diagrams.py"]=sha256(ROOT/"tools/render_doc_diagrams.py")
    outputs=[DOCS/"handbook.html",*(DOCS/"diagrams").glob("*.svg"),*(DOCS/"code").glob("*.html")]
    manifest={**provenance(),"status":"PASS","scope":"document_generation_only","project_scope":"M0-M3 / T01-T10",
              "visual_review":"NOT_RUN","renderer":{"markdown-it-py":importlib.metadata.version("markdown-it-py")},
              "inputs":inputs,"outputs":{str(p.relative_to(ROOT)):sha256(p) for p in outputs},"fsm_edges":len(edges)}
    write_json(DOCS/"handbook-manifest.json",manifest);write_json(out/"result.json",manifest)
    print(json.dumps({"status":"PASS","scope":manifest["scope"],"evidence":str(out.relative_to(ROOT)),"html":"docs/handbook.html"}))


if __name__=="__main__": main()
