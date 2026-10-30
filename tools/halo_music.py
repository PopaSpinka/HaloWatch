"""Музыка для плеера Halo: файлы (ogg, mp3, wav — всё, что читает ffmpeg) → один образ для раздела флеша.

Usage / использование:
  python3 tools/halo_music.py HaloWatch [--flash] [--port PORT]
    reads HaloWatch/assets/music.conf (lines: track  file  "Title"  "Artist"), writes build/music/HaloWatch.bin;
    --flash writes it into the "ffat" partition (0x910000) — the firmware itself is not touched.
    Needs ffmpeg; for --flash — esptool (pip install esptool, or the one from the Arduino ESP32 core).

Звук: ffmpeg → моно 22 050 Гц, срез ниже 120 Гц (маленький динамик их не играет, а запас громкости съедают),
громкость всех треков к одной (loudnorm: −14 LUFS, пики не выше −1,5 дБ) → IMA ADPCM, 4 бита на отсчёт (блоки
по 256 байт: 4 байта заголовка — первый отсчёт и шаг, дальше 504 отсчёта, младшая тетрада первой; так пишет
ffmpeg adpcm_ima_wav). 22 050 Гц × 4 бита = 11 КБ в секунду: 23 МБ раздела — около 35 минут.

Образ: заголовок 4 КБ — "HWM1", версия, число треков, частота, размер блока, отсчётов в блоке; на каждый трек 128
байт: смещение от начала раздела, байты, отсчёты, название (56 байт UTF-8), исполнитель (56). Каждый трек — с
границы 64 КБ (плата отображает его в память страницами по 64 КБ: esp_partition_mmap).
"""
import glob
import os
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE, BLOCK = 22050, 256
SPB = (BLOCK - 4) * 2 + 1  # отсчётов в блоке
PART_OFFSET, PART_SIZE = 0x910000, 0x16E0000
def find_esptool():
    for name in ("esptool", "esptool.py"):
        if shutil.which(name):
            return [shutil.which(name)]
    homes = ["~/Library/Arduino15", "~/.arduino15", os.path.join(os.environ.get("LOCALAPPDATA", ""), "Arduino15")]
    for h in homes:
        hits = sorted(glob.glob(os.path.join(os.path.expanduser(h), "packages/esp32/tools/esptool_py/*/esptool*")))
        if hits:
            return [hits[-1]]
    try:
        import esptool  # noqa: F401
        return [sys.executable, "-m", "esptool"]
    except ImportError:
        sys.exit("esptool not found: pip install esptool")


def find_port():
    if "--port" in sys.argv:
        return sys.argv[sys.argv.index("--port") + 1]
    hits = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not hits:
        sys.exit("board not found — pass --port (e.g. --port COM5 on Windows)")
    return hits[0]


def read_conf(path):
    tracks = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line.startswith("track"):  # пустые строки и комментарии (#)
            continue
        parts = shlex.split(line, comments=True)
        if len(parts) < 4:
            sys.exit(f"music.conf: нужно track файл \"название\" \"исполнитель\": {line}")
        tracks.append((parts[1], parts[2], parts[3]))
    return tracks


def encode(src):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "a.wav")
        af = "highpass=f=120,loudnorm=I=-14:TP=-1.5:LRA=11"
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-ac", "1", "-ar", str(RATE), "-af", af,
                        "-c:a", "adpcm_ima_wav", "-block_size", str(BLOCK), out], check=True)
        data = open(out, "rb").read()
    # WAV: найти fmt (проверить формат) и data
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, pos + 8)
        elif cid == b"data":
            body = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, align, _ = fmt
    if tag != 0x11 or ch != 1 or rate != RATE or align != BLOCK:
        sys.exit(f"{src}: ffmpeg дал не тот формат: {fmt}")
    body = body[: len(body) // BLOCK * BLOCK]  # неполный последний блок — отбросить
    return body, len(body) // BLOCK * SPB


def main():
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and sys.argv[i - 1] != "--port"]
    if not args:
        sys.exit(__doc__)
    sketch = os.path.abspath(args[0])
    name = os.path.basename(sketch.rstrip("/"))
    tracks = read_conf(os.path.join(sketch, "assets", "music.conf"))
    if not tracks:
        sys.exit("в music.conf нет треков")
    head = bytearray(4096)
    struct.pack_into("<4sHHIHH", head, 0, b"HWM1", 1, len(tracks), RATE, BLOCK, SPB)
    blob = bytearray()
    off = 0x10000
    for i, (f, title, artist) in enumerate(tracks):
        body, samples = encode(os.path.join(sketch, f))
        t, a = title.encode("utf-8")[:55], artist.encode("utf-8")[:55]
        struct.pack_into("<III56s56s", head, 16 + i * 128, off, len(body), samples, t, a)
        blob += b"\xff" * (off - 0x10000 - len(blob)) + body
        print(f"  {title} — {artist}: {int(samples / RATE) // 60}:{int(samples / RATE) % 60:02d}, {len(body) / 1024:.0f} КБ")
        off = (0x10000 + len(blob) + 0xFFFF) & ~0xFFFF
    image = bytes(head) + b"\xff" * (0x10000 - len(head)) + bytes(blob)
    if len(image) > PART_SIZE:
        sys.exit(f"образ {len(image) / 1e6:.1f} МБ больше раздела ({PART_SIZE / 1e6:.1f} МБ)")
    os.makedirs(os.path.join(ROOT, "build", "music"), exist_ok=True)
    path = os.path.join(ROOT, "build", "music", name + ".bin")
    open(path, "wb").write(image)
    print(f"{len(tracks)} треков, {len(image) / 1e6:.1f} МБ → {path}")
    if "--flash" in sys.argv:
        subprocess.run(find_esptool() + ["--chip", "esp32s3", "--port", find_port(), "--baud", "921600", "write_flash",
                                         hex(PART_OFFSET), path], check=True)


if __name__ == "__main__":
    main()
