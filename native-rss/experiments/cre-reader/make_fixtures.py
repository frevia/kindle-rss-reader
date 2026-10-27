#!/usr/bin/env python3
"""Create deterministic, self-checking HTML and grayscale PNG reading fixtures."""

import binascii
import pathlib
import struct
import sys
import xml.etree.ElementTree as ET
import zlib


def chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF)


def write_png(path: pathlib.Path, width: int, height: int) -> None:
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            # A simple deterministic gradient plus checker pattern distinguishes both fixtures.
            rows.append((32 + (x * 173 // width + y * 47 // height + ((x // 24 + y // 24) % 2) * 36) % 192))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))
    path.write_bytes(png)


BASE = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="UTF-8"/><meta name="viewport" content="width=device-width, initial-scale=1"/>
<title></title><style>
body{{font-size:100%;line-height:162.5%;}}
figure{{margin:0.5em 0;page-break-inside:avoid;}}
img{{display:block;max-width:100%;margin:0 auto;}}
figcaption{{font-size:81.25%;line-height:1.4;text-indent:0;text-align:center;page-break-before:avoid;}}
pre{{white-space:pre-wrap;}}
</style></head>
<body><main>{body}</main></body></html>
"""

PARA = "在信息不断变化的日常里，阅读仍然需要一段安静而连续的时间。我们把观察、记录和复查放在一起，先说明问题出现的背景，再比较不同做法带来的结果。每个结论都应当能够回到具体的事实，也要留下继续修订的空间。好的工具让内容更清楚，却不替读者决定什么值得思考。"


def paragraphs(n: int, prefix: str = "") -> str:
    return "".join(f"<p>{prefix}{PARA}{PARA}</p>" for _ in range(n))


def fixtures() -> dict[str, str]:
    chinese = ("<h1>把阅读留给重要的事情</h1><p>这是一份用于比较排版结果的中文长文，正文由固定段落组成。</p>"
               "<h2>先建立稳定的阅读节奏</h2><p>清晰的层级帮助读者快速找到主题，也让跨页阅读保持连贯。</p>"
               f"<blockquote>{PARA}</blockquote><p>重点是<strong>完整保留文章内容</strong>，并用<em>恰当的留白</em>区分段落。</p>"
               "<ol><li>确认标题与段落层级</li><li>检查换页时的上下文</li><li>比较字体大小和行距</li></ol>" + paragraphs(6))
    mixed = ("<h1>离线阅读与 Small Details</h1><h2>One clear baseline</h2>"
             f"<p>RSS文章会同时出现中文和 English words。{PARA} 阅读地址保留为 <strong>https://example.invalid/notes/readability?mode=offline&amp;v=1</strong>，方便观察 URL 换行。</p>"
             f"<blockquote>{PARA}</blockquote><p><em>Mixed typography</em> tests punctuation, ASCII spacing, and 中文标点 in one flow.</p>"
             "<ol><li>Read the heading</li><li>检查段落和链接</li><li>Keep the same text size</li></ol>" + paragraphs(6, "English note: "))
    code = ("<h1>代码与缩进</h1><h2>空白必须可见</h2>"
            f"<p>代码示例周围仍有普通正文。{PARA}</p><pre><code>function render(item) {{\n  const label = &quot;&lt;article&gt;&quot;;\n  const value = item.text &amp;&amp; item.text.trim();\n  if (value) {{\n    return `&lt;p&gt;${{label}}: ${{value}}&lt;/p&gt;`;\n  }}\n  return &quot;empty&quot;;\n}}</code></pre>"
            f"<blockquote>{PARA}</blockquote><p><strong>特殊字符：</strong> &lt;tag&gt; &amp; &quot;quoted&quot;，缩进使用普通空格。</p>"
            "<ol><li>保留代码块换行</li><li>保留每级缩进</li><li>转义尖括号与与号</li></ol>" + paragraphs(6))
    image_body = ("<h1>图片与图注</h1><h2>图像随正文分页</h2>"
                  f"<p>图片前后的文字构成阅读上下文。{PARA}{PARA}</p><figure><img src=\"fixture-landscape.png\" alt=\"横向灰度样图\"/><figcaption>图一：横向灰度样图，使用固定尺寸和本地数据生成，图注用于检查居中和跨页位置。</figcaption></figure>"
                  + paragraphs(4, "图一之后的说明：") +
                  "<figure><img src=\"fixture-portrait.png\" alt=\"纵向灰度样图\"/><figcaption>图二：纵向灰度样图，检验较高图像与图注共同排版，并确保两张图不会被合并。</figcaption></figure>"
                  + paragraphs(5, "图二之后的说明：") +
                  "<blockquote>图片应按比例缩放，图注紧随对应图片。</blockquote><ol><li>横图</li><li>纵图</li></ol>")
    return {"chinese.html": chinese, "mixed.html": mixed, "code.html": code, "images.html": image_body}


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_fixtures.py OUTDIR")
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    bodies = fixtures()
    for name, body in bodies.items():
        data = BASE.format(title=name.removesuffix(".html"), body=body)
        path = out / name
        path.write_text(data, encoding="utf-8", newline="\n")
        parsed = ET.parse(path).getroot()
        assert parsed.tag == "html" and parsed.attrib["lang"] == "zh-CN"
        assert parsed.find("./head/meta[@charset='UTF-8']") is not None
        assert all(tag in data for tag in ("<h1>", "<h2>", "<p>", "<blockquote>", "<ol>", "<li>"))
        plain = "".join(parsed.itertext())
        assert len(plain) >= 1200, f"{name} must contain at least 1200 source characters"
        if name == "code.html":
            assert "  const" in data and "&lt;article&gt;" in data and "\n" in data[data.index("<pre>"):data.index("</pre>")]
    combined = "".join(BASE.format(title=name, body=body) for name, body in bodies.items())
    assert all(tag in combined for tag in ("<em>", "<strong>", "<pre>", "<code>", "<figure>", "<figcaption>"))
    write_png(out / "fixture-landscape.png", 640, 360)
    write_png(out / "fixture-portrait.png", 360, 640)
    for name, expected in (("fixture-landscape.png", (640, 360)), ("fixture-portrait.png", (360, 640))):
        raw = (out / name).read_bytes()
        assert raw[:8] == b"\x89PNG\r\n\x1a\n" and raw[12:16] == b"IHDR"
        assert struct.unpack(">II", raw[16:24]) == expected
    for src in ("fixture-landscape.png", "fixture-portrait.png"):
        assert (out / src).is_file() and src in bodies["images.html"]
    print("created 4 HTML fixtures and 2 grayscale PNG fixtures; self-checks passed")


if __name__ == "__main__":
    main()
