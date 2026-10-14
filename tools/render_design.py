#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
render_design.py — 把 design/ui_draft_glassmorphism.html 渲染成整板图 + 逐画面切图。

为什么不用「猜高度」：Chrome headless 的 --screenshot 只截视口，页面真实高度得问浏览器。
做法是在临时副本里注入一段探针脚本，用 --dump-dom 把 scrollHeight 和每个 .shot 的
getBoundingClientRect 读回来，再按这个尺寸截图 + 用 Pillow 精确裁剪。
（外链图片先等 load，所以必须给 --virtual-time-budget。）

用法：
  python tools/render_design.py                       # 默认渲染 design/ui_draft_glassmorphism.html
  python tools/render_design.py --scale 2 --width 1280
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

PROBE = """
<script>
window.addEventListener('load', function () {
  setTimeout(function () {
    var out = { h: document.documentElement.scrollHeight, shots: [] };
    document.querySelectorAll('.shot').forEach(function (s) {
      var r = s.getBoundingClientRect();
      var lab = s.querySelector('.label');
      out.shots.push({
        top: Math.round(r.top + window.scrollY),
        left: Math.round(r.left + window.scrollX),
        w: Math.round(r.width),
        h: Math.round(r.height),
        name: lab ? lab.textContent.trim() : ''
      });
    });
    var pre = document.createElement('pre');
    pre.id = '__probe';
    pre.textContent = JSON.stringify(out);
    document.body.appendChild(pre);
  }, 400);
});
</script>
"""


def find_chrome():
    for c in CHROME_CANDIDATES:
        if os.path.exists(c):
            return c
    raise SystemExit("找不到 Chrome / Edge 可执行文件")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True)
    return p.returncode, (p.stdout or b"").decode("utf-8", "replace"), (p.stderr or b"").decode("utf-8", "replace")


def measure(chrome, page_url, tmpdir, width, budget):
    """返回 {h, shots:[{top,left,w,h,name}]}"""
    src = os.path.join(tmpdir, "_probe_page.html")
    with open(page_url_to_path(page_url), "r", encoding="utf-8") as f:
        html = f.read()
    html = html.replace("</body>", PROBE + "</body>")
    with open(src, "w", encoding="utf-8") as f:
        f.write(html)

    rc, out, err = run([
        chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
        "--no-first-run", "--no-default-browser-check", "--allow-file-access-from-files",
        "--virtual-time-budget=%d" % budget,
        "--window-size=%d,1200" % width,
        "--dump-dom", "file:///" + src.replace("\\", "/"),
    ])
    m = re.search(r'id="__probe">(.*?)</pre>', out, re.S)
    if not m:
        sys.stderr.write(out[-3000:] + "\n" + err[-3000:] + "\n")
        raise SystemExit("探针没拿到 —— 页面里没生成 __probe 节点")
    return json.loads(m.group(1).replace("&quot;", '"').replace("&amp;", "&"))


def page_url_to_path(url):
    return url.replace("file:///", "").replace("/", os.sep)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", default=os.path.join("design", "ui_draft_glassmorphism.html"))
    ap.add_argument("--outdir", default="design")
    ap.add_argument("--width", type=int, default=1280, help="截图视口宽度（CSS px）")
    ap.add_argument("--scale", type=int, default=2, help="devicePixelRatio，2 = 视网膜")
    ap.add_argument("--budget", type=int, default=20000, help="等待外链图片的虚拟时间预算 ms")
    ap.add_argument("--pad-top", type=int, default=46, help="切图上方留白（放 label）")
    ap.add_argument("--pad-x", type=int, default=14)
    ap.add_argument("--pad-bottom", type=int, default=10)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    html_path = os.path.abspath(os.path.join(root, args.html))
    outdir = os.path.abspath(os.path.join(root, args.outdir))
    os.makedirs(outdir, exist_ok=True)

    chrome = find_chrome()
    url = "file:///" + html_path.replace("\\", "/")

    with tempfile.TemporaryDirectory() as tmp:
        info = measure(chrome, url, tmp, args.width, args.budget)
        page_h = info["h"]
        shots = info["shots"]
        print("页面高度 %d px，%d 个画面" % (page_h, len(shots)))

        full_png = os.path.join(tmp, "_full.png")
        rc, out, err = run([
            chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
            "--no-first-run", "--no-default-browser-check",
            "--force-device-scale-factor=%d" % args.scale,
            "--virtual-time-budget=%d" % args.budget,
            "--window-size=%d,%d" % (args.width, page_h + 2),
            "--screenshot=" + full_png, url,
        ])
        if not os.path.exists(full_png):
            sys.stderr.write(out[-2000:] + "\n" + err[-2000:] + "\n")
            raise SystemExit("整板截图失败")

        from PIL import Image
        img = Image.open(full_png)
        print("整板图 %dx%d" % img.size)
        s = args.scale

        full_out = os.path.join(outdir, "ui_draft_full.png")
        img.crop((0, 0, img.size[0], min(img.size[1], page_h * s))).save(full_out)
        print("  ->", full_out)

        # 每屏切图；文件名沿用既有约定 ui_draft_NN_<slug>.png
        slugs = ["01_main", "02_set", "03_scope", "04_install"]
        for i, sh in enumerate(shots):
            top = max(0, (sh["top"] - args.pad_top) * s)
            bottom = min(img.size[1], (sh["top"] + sh["h"] + args.pad_bottom) * s)
            left = max(0, (sh["left"] - args.pad_x) * s)
            right = min(img.size[0], (sh["left"] + sh["w"] + args.pad_x) * s)
            slug = slugs[i] if i < len(slugs) else "%02d_shot" % (i + 1)
            p = os.path.join(outdir, "ui_draft_%s.png" % slug)
            img.crop((left, top, right, bottom)).save(p)
            print("  -> %s  (%s)" % (p, sh["name"] or slug))


if __name__ == "__main__":
    main()
