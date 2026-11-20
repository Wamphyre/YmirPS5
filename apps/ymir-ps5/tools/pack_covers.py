#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the offline Saturn cover catalog from Libretro's box art and Redump metadata."""

import argparse
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path
import re
from tempfile import NamedTemporaryFile
import time
from urllib.error import URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

from PIL import Image


ART_REPO = "libretro-thumbnails/Sega_-_Saturn"
DAT_URL = (
    "https://raw.githubusercontent.com/libretro/libretro-database/master/"
    "metadat/redump/Sega%20-%20Saturn.dat"
)


def fetch(url, timeout=30):
    request = Request(url, headers={"User-Agent": "YmirPS5-cover-catalog"})
    for attempt in range(3):
        try:
            with urlopen(request, timeout=timeout) as response:
                return response.read()
        except (URLError, TimeoutError):
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def read_games(data):
    games = {}
    for record in re.finditer(r"^game \(\n(.*?)^\)", data.decode("utf-8-sig"), re.M | re.S):
        name = re.search(r'^\s+name "([^"]+)"', record[1], re.M)
        serial = re.search(r'^\s+serial "([^"]+)"', record[1], re.M)
        if name and serial:
            key = name[1].replace("&", "_").casefold()
            games.setdefault(key, set()).add(serial[1])
    return games


def cover_jpeg(entry, revision, cache):
    name, blob = entry["path"], entry["sha"]
    jpeg = cache / f"{blob}.jpg"
    if not jpeg.is_file():
        url = f"https://raw.githubusercontent.com/{ART_REPO}/{revision}/Named_Boxarts/{quote(name)}"
        body = fetch(url)
        if hashlib.sha1(f"blob {len(body)}\0".encode() + body).hexdigest() != blob:
            raise ValueError(f"Git blob checksum mismatch for {name}")
        with Image.open(io.BytesIO(body)) as original:
            original.thumbnail((256, 256), Image.Resampling.LANCZOS)
            background = Image.new("RGB", original.size, (14, 20, 34))
            art = original.convert("RGBA")
            background.paste(art, mask=art.getchannel("A"))
            with NamedTemporaryFile(dir=cache, delete=False) as temporary:
                path = Path(temporary.name)
                try:
                    background.save(temporary, format="JPEG", quality=85, optimize=True)
                    temporary.close()
                    path.replace(jpeg)
                finally:
                    path.unlink(missing_ok=True)
    return jpeg


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--revision", default="master")
    parser.add_argument("--jobs", type=int, default=12)
    args = parser.parse_args()
    api = f"https://api.github.com/repos/{ART_REPO}"
    commit = json.loads(fetch(f"{api}/commits/{quote(args.revision)}"))
    revision = commit["sha"]
    root = json.loads(fetch(f"{api}/git/trees/{commit['commit']['tree']['sha']}"))
    boxarts = next(entry for entry in root["tree"] if entry["path"] == "Named_Boxarts")
    tree = json.loads(fetch(boxarts["url"]))
    if tree.get("truncated"):
        raise RuntimeError("Libretro's cover list is incomplete")
    games = read_games(fetch(DAT_URL))
    entries = sorted((entry for entry in tree["tree"] if entry["path"].lower().endswith(".png")),
                     key=lambda entry: entry["path"])
    print(f"Preparing {len(entries)} box fronts from {revision}", flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    args.cache.mkdir(parents=True, exist_ok=True)
    index_tmp, pixels_tmp = (args.output / name for name in ("index.tsv.tmp", "art.bin.tmp"))
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as workers:
            futures = {}
            for entry in entries:
                if entry["sha"] not in futures:
                    futures[entry["sha"]] = workers.submit(cover_jpeg, entry, revision, args.cache)
            with index_tmp.open("w", encoding="utf-8", newline="\n") as index, pixels_tmp.open("wb") as pixels:
                index.write("YMIRPS5_COVERS_1\n")
                for number, entry in enumerate(entries, 1):
                    name = entry["path"][:-4]
                    jpeg = futures[entry["sha"]].result().read_bytes()
                    serials = "|".join(sorted(games.get(name.casefold(), ())))
                    index.write(f"{name}\t{pixels.tell()}\t{len(jpeg)}\t{serials}\n")
                    pixels.write(jpeg)
                    if number % 100 == 0:
                        print(f"{number}/{len(entries)}", flush=True)
        pixels_tmp.replace(args.output / "art.bin")
        index_tmp.replace(args.output / "index.tsv")
    finally:
        index_tmp.unlink(missing_ok=True)
        pixels_tmp.unlink(missing_ok=True)
    (args.output / "source.txt").write_text(
        f"Box art from https://github.com/{ART_REPO}/tree/{revision}/Named_Boxarts\n"
        f"Serial metadata from {DAT_URL}\n"
        "Artwork belongs to its respective rights holders.\n",
        encoding="utf-8",
    )
    print(f"Packed {len(entries)} covers in {args.output}", flush=True)


if __name__ == "__main__":
    main()
