#!/usr/bin/env python3
"""Small, dependency-free verification helpers for FileFlow macOS packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import stat


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bundle_digest(root: pathlib.Path) -> str:
    if not root.is_dir() or root.is_symlink():
        raise ValueError(f"not an application directory: {root}")
    digest = hashlib.sha256()
    entries = [root, *sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix())]
    for entry in entries:
        relative = "." if entry == root else entry.relative_to(root).as_posix()
        metadata = entry.lstat()
        if stat.S_ISDIR(metadata.st_mode):
            kind, content = "directory", b""
        elif stat.S_ISLNK(metadata.st_mode):
            kind, content = "symlink", os.readlink(entry).encode("utf-8", "surrogateescape")
        elif stat.S_ISREG(metadata.st_mode):
            kind, content = "file", entry.read_bytes()
        else:
            raise ValueError(f"unsupported bundle entry: {relative}")
        header = f"{kind}\0{relative}\0{stat.S_IMODE(metadata.st_mode):04o}\0{len(content)}\0".encode()
        digest.update(header)
        digest.update(content)
    return digest.hexdigest()


def write_report(args: argparse.Namespace) -> None:
    value = {
        "schemaVersion": 1,
        "packageName": args.dmg.name,
        "sha256": sha256_file(args.dmg),
        "size": args.dmg.stat().st_size,
        "platform": "macos",
        "architecture": args.arch,
        "version": args.version.removeprefix("v"),
        "sourceRevision": args.source_revision,
        "appTreeSha256": args.app_tree_sha256,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    digest_parser = subparsers.add_parser("bundle-digest")
    digest_parser.add_argument("app", type=pathlib.Path)
    report_parser = subparsers.add_parser("write-report")
    report_parser.add_argument("--dmg", type=pathlib.Path, required=True)
    report_parser.add_argument("--output", type=pathlib.Path, required=True)
    report_parser.add_argument("--version", required=True)
    report_parser.add_argument("--arch", choices=("arm64", "x86_64", "universal2"), required=True)
    report_parser.add_argument("--source-revision", required=True)
    report_parser.add_argument("--app-tree-sha256", required=True)
    args = parser.parse_args()
    if args.command == "bundle-digest":
        print(bundle_digest(args.app))
    else:
        write_report(args)


if __name__ == "__main__":
    main()
