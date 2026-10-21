"""Decode the installed Espressif EAF assets for a browser preview; no firmware edits.

Format and mirror layout follow esp_emote_gfx gfx_eaf_dec.c / gfx_anim.c.
This bounded preview only supports the 8-bit, RLE assets verified below.
"""
import hashlib
import json
import struct
from pathlib import Path
from PIL import Image, ImageOps

SOURCE = Path(__file__).resolve().parents[2] / 'sources/esp_emote_assets'
OUT = Path(__file__).parent
NAMES = {
    'Happy': '开心', 'neutral': '待机眼睛', 'winking': '眨眼',
    'Sad': '难过', 'cry': '哭泣', 'angry': '生气',
    'confused': '疑惑', 'shocked': '惊讶', 'sleep': '困倦', 'listen': '聆听指示',
}


def frames(path):
    data = path.read_bytes()
    assert data[:4] == b'\x89EAF'
    count, checksum, length = struct.unpack_from('<III', data, 4)
    assert length == len(data) - 16 and sum(data[16:]) == checksum
    result = []
    for i in range(count):
        size, offset = struct.unpack_from('<II', data, 16 + i * 8)
        start = 16 + count * 8 + offset
        assert data[start:start + 2] == b'ZZ'
        frame = data[start + 2:start + size]
        assert len(frame) == size - 2
        if frame[:2] == b'_C':
            continue  # EAF control marker, not an image frame.
        assert frame[:2] == b'_S' and frame[9] == 8
        width, height, blocks, block_height = struct.unpack_from('<HHHH', frame, 10)
        lengths = struct.unpack_from('<' + 'I' * blocks, frame, 18)
        pos = 18 + blocks * 4
        palette = frame[pos:pos + 1024]
        pos += 1024
        indices = bytearray()
        for block, length in enumerate(lengths):
            payload = frame[pos:pos + length]
            pos += length
            assert payload[0] == 0 and len(payload[1:]) % 2 == 0
            decoded = bytearray()
            for j in range(1, len(payload), 2):
                decoded.extend(bytes([payload[j + 1]]) * payload[j])
            assert len(decoded) <= width * block_height
            valid = width * min(block_height, height - block * block_height)
            assert len(decoded) >= valid
            indices.extend(decoded[:valid])
        assert len(indices) == width * height
        rgb_palette = []
        for j in range(256):
            b, g, r, alpha = palette[j * 4:j * 4 + 4]
            # Match the device's RGB565 quantization, displayed on black.
            r, g, b = r >> 3, g >> 2, b >> 3
            rgb_palette.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        eye = Image.frombytes('P', (width, height), bytes(indices))
        eye.putpalette(rgb_palette)
        eye = eye.convert('RGB')
        canvas = Image.new('RGB', (320, 240), 'black')
        if path.stem == 'listen':
            canvas.paste(eye, ((320 - width) // 2, 5))
        else:
            y = (240 - height) // 2 + 30
            canvas.paste(eye, (10, y))
            canvas.paste(ImageOps.mirror(eye), (320 - 10 - width, y))
        result.append(canvas)
    assert len(result) > 1
    assert len({hashlib.sha256(x.tobytes()).hexdigest() for x in result}) > 1
    return result


if __name__ == '__main__':
    manifest = []
    for name, label in NAMES.items():
        path = SOURCE / 'emoji_large' / f'{name}.eaf'
        images = frames(path)
        images[0].save(OUT / f'{name}.png')
        images[0].save(OUT / f'{name}.gif', save_all=True, append_images=images[1:],
                       duration=50, loop=0, optimize=False, disposal=2)
        manifest.append({'name': name, 'label': label, 'frames': len(images), 'fps': 20,
                         'source': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    (OUT / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    cards = ''.join(f'<figure><img src="{x["name"]}.gif" alt="{x["label"]} 动画"><figcaption>{x["label"]} <small>{x["frames"]} 帧 · 20 fps</small></figcaption></figure>' for x in manifest)
    html = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>BOX-3 · Emote 原资源预览</title>
<style>*{box-sizing:border-box}body{margin:0;background:#10151c;color:#e7edf6;font:16px/1.6 -apple-system,BlinkMacSystemFont,sans-serif}main{max-width:1120px;margin:auto;padding:32px 24px}h1{font-size:28px;margin:0 0 8px}p{color:#aab8ca;margin:0 0 18px}button{border:1px solid #44556d;border-radius:9px;padding:8px 16px;background:#1c2939;color:#e7edf6;cursor:pointer}section{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:18px;margin-top:22px}figure{margin:0;background:#171f2b;border:1px solid #2b394c;border-radius:12px;overflow:hidden}img{width:100%;aspect-ratio:4/3;object-fit:contain;display:block;background:#000}figcaption{padding:10px 14px}small{float:right;color:#8da0b9;font-size:12px}a{color:#7dd3fc}footer{font-size:13px;color:#aab8ca;margin-top:24px}</style>
<main><h1>Emote 原始动画预览</h1><p>乐鑫维护者发布的 Emote 资源包 · BOX-3 320 × 240 布局 · 原始帧按 20 fps 播放</p><p>这些是当前依赖中的原始 EAF 解码结果；浏览器为方便比较重复播放。预览省略电量、时钟和对白叠层，不是设备实录。</p><button id="toggle" type="button" aria-pressed="false">暂停预览</button><section>''' + cards + '''</section><footer>资源：esp_emote_assets 0.1.2 · commit 29aa5259ac22651b8636683698c021482050b206 · <a href="https://github.com/espressif2022/esp_emote_assets">来源仓库</a><br>小智接入了这套组件；它不同于 21 张 Noto PNG，也不是 BOX-3 旧出厂界面动画。部分固件状态只播一次，这里统一循环便于查看。未修改或烧录固件。</footer></main><script>let paused=false;document.querySelector('#toggle').onclick=function(){paused=!paused;document.querySelectorAll('img').forEach(i=>i.src=i.src.replace(paused?'.gif':'.png',paused?'.png':'.gif'));this.textContent=paused?'继续预览':'暂停预览';this.setAttribute('aria-pressed',String(paused));};</script></html>'''
    (OUT / 'index.html').write_text(html)
    print(json.dumps([{'name': x['name'], 'frames': x['frames']} for x in manifest]))
