#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Update an installed YmirPS5 title from a release ZIP over FTP."""

import argparse
import hashlib
import io
import json
import sys
from ftplib import FTP, error_perm
from pathlib import Path, PurePosixPath
from uuid import uuid4
from zipfile import ZipFile


TITLE_ID = "PPSA98567"
TITLE_PATH = f"/data/homebrew/{TITLE_ID}"
REGISTRATION = f"/user/app/{TITLE_ID}"
SHELL_MEDIA = {"icon0.png", "param.json", "pic0.dds", "pic1.dds", "snd0.at9"}
GAMES_DIRECTORY = "/data/YmirPS5/games"
GAME_FORMATS = {".ccd", ".chd", ".cue", ".iso", ".mds"}


def package_files(package):
    files = {}
    with ZipFile(package) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            path = PurePosixPath(entry.filename)
            if path.is_absolute() or ".." in path.parts or path.parts[:1] != (TITLE_ID,):
                raise ValueError(f"Unexpected ZIP path {path}")
            name = str(path.relative_to(TITLE_ID))
            if name not in {
                "eboot.bin",
                "LICENSE",
                "lapy.elf",
                "lapy-manifest.json",
                "tools/ymir-ps5-disc.self",
            } and not name.startswith(("sce_sys/", "sce_module/", "covers/", "licenses/")):
                raise ValueError(f"Unexpected title file {name}")
            if name in files:
                raise ValueError(f"Duplicate title file {name}")
            files[name] = archive.read(entry)

    required = {"eboot.bin", "sce_module/libc.prx", "sce_sys/param.json", "sce_sys/icon0.png"}
    if not required.issubset(files):
        raise ValueError("The package is missing required title files")
    if json.loads(files["sce_sys/param.json"])["titleId"] != TITLE_ID:
        raise ValueError("The package's title ID does not match YmirPS5")
    if "tools/ymir-ps5-disc.self" in files and "lapy.elf" not in files:
        raise ValueError("The package has the optical service but is missing the Lapy helper")
    return files


def read_remote(ftp, path):
    file = io.BytesIO()
    ftp.retrbinary(f"RETR {path}", file.write, blocksize=256 * 1024)
    return file.getvalue()


def remote_facts(ftp, path):
    try:
        line = ftp.sendcmd(f"MLST {path}").splitlines()[1].strip()
    except error_perm as error:
        if str(error).startswith("550"):
            return {}
        raise
    return dict(part.split("=", 1) for part in line.split(" ", 1)[0].split(";") if part)


def stage(ftp, path, mode, data, token, staged):
    destination = PurePosixPath(path)
    temp = destination.with_name(f".{destination.name}.{token}.upload").as_posix()
    staged.append(temp)
    ftp.storbinary(f"STOR {temp}", io.BytesIO(data), blocksize=256 * 1024)
    ftp.sendcmd(f"SITE CHMOD {mode} {temp}")
    if hashlib.sha256(read_remote(ftp, temp)).digest() != hashlib.sha256(data).digest():
        raise OSError(f"Staged data failed checksum verification for {path}")
    return temp


def update(ftp, files):
    mount = read_remote(ftp, f"{REGISTRATION}/mount.lnk").decode().rstrip("\x00\r\n")
    if mount != TITLE_PATH:
        raise ValueError(f"Registered title points to {mount}")

    targets = {f"{TITLE_PATH}/{name}": data for name, data in files.items()}
    for name in SHELL_MEDIA:
        path = f"sce_sys/{name}"
        if path in files:
            targets[f"{REGISTRATION}/{path}"] = files[path]
            targets[f"/user/appmeta/{TITLE_ID}/{name}"] = files[path]
    icon_copy = f"{REGISTRATION}/icon0.png"
    if remote_facts(ftp, icon_copy):
        targets[icon_copy] = files["sce_sys/icon0.png"]

    token = uuid4().hex
    staged = []
    changed = {}
    installed = []
    created_dirs = []
    committed = False
    try:
        folders = {PurePosixPath(TITLE_PATH, parent) for name in files for parent in PurePosixPath(name).parents}
        for folder in sorted(folders, key=lambda path: len(path.parts)):
            if not remote_facts(ftp, str(folder)):
                ftp.mkd(str(folder))
                created_dirs.append(str(folder))
                ftp.sendcmd(f"SITE CHMOD 0777 {folder}")
        for path, data in targets.items():
            facts = remote_facts(ftp, path)
            old = read_remote(ftp, path) if facts else None
            if old == data:
                continue
            mode = facts.get("unix.mode", "0777" if path.startswith(TITLE_PATH + "/") else "0644")
            temp = stage(ftp, path, mode, data, token, staged)
            changed[path] = (temp, mode, old)
        # Keep the previous executable in place until the complete update is staged.
        for path in sorted(changed, key=lambda name: name == f"{TITLE_PATH}/eboot.bin"):
            ftp.rename(changed[path][0], path)
            installed.append(path)
            print(f"Updated {path}", flush=True)
        for path in installed:
            if read_remote(ftp, path) != targets[path]:
                raise OSError(f"Installed data differs for {path}")
        committed = True
    except Exception:
        for path in reversed(installed):
            try:
                _, mode, old = changed[path]
                if old is None:
                    ftp.delete(path)
                else:
                    temp = stage(ftp, path, mode, old, token + "-restore", staged)
                    ftp.rename(temp, path)
            except Exception as error:
                print(f"Could not restore {path}: {error}", file=sys.stderr)
        raise
    finally:
        for path in staged:
            try:
                if remote_facts(ftp, path):
                    ftp.delete(path)
            except (OSError, error_perm) as error:
                print(f"Could not clean up {path}: {error}", file=sys.stderr)
        if not committed:
            for folder in reversed(created_dirs):
                try:
                    ftp.rmd(folder)
                except (OSError, error_perm):
                    pass

    print(f"Verified {len(targets)} title files. Updated {len(installed)}.")


def index_games(ftp):
    games = []
    pending = [PurePosixPath(".")]
    while pending:
        directory = pending.pop()
        for name, facts in ftp.mlsd(f"{GAMES_DIRECTORY}/{directory}"):
            if name in (".", "..") or any(c in name for c in "/\r\n"):
                continue
            entry = directory / name
            if facts["type"] == "dir":
                pending.append(entry)
            elif facts["type"] == "file" and entry.suffix.lower() in GAME_FORMATS:
                games.append(entry.as_posix())
    path = "/data/YmirPS5/library.txt"
    data = ("\n".join(["YMIRPS5_GAMES_1", *sorted(games, key=str.casefold)]) + "\n").encode("utf-8")
    staged = []
    try:
        temp = stage(ftp, path, "0644", data, uuid4().hex, staged)
        ftp.rename(temp, path)
    finally:
        for temp in staged:
            if remote_facts(ftp, temp):
                ftp.delete(temp)
    print(f"Indexed {len(games)} games.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, nargs="?")
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--index-games", action="store_true", help="index the games already uploaded to the PS5")
    args = parser.parse_args()
    if args.package is None and not args.index_games:
        parser.error("specify the update package or --index-games")
    files = package_files(args.package) if args.package else None
    with FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=30)
        ftp.login()
        if files:
            update(ftp, files)
        if args.index_games:
            index_games(ftp)


if __name__ == "__main__":
    main()
