#!/usr/bin/env python3
"""Offline validation and transformation for the one-time v0.2.0 metadata repair."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import pathlib
import re
from typing import Any


@dataclasses.dataclass(frozen=True)
class Contract:
    repository: str
    release_id: int
    tag: str
    version: str
    commit: str
    published_date: str
    bad_url_segment: str
    packages: dict[str, str]
    original_manifest_sha256: str
    original_checksums_sha256: str


CONTRACT = Contract(
    repository="JoshuaGessner/FileFlow",
    release_id=423555409,
    tag="v0.2.0",
    version="0.2.0",
    commit="7c9ff1ab58df7becdf8b0f44aa331854d12e8b13",
    published_date="2026-10-25",
    bad_url_segment="untagged-3afdedc772ae32f1bfa0",
    packages={
        "FileFlow-v0.2.0-android-arm64-v8a-debug.apk": "e288b88dbdda17cdbfe915a91a838d573af696efd01ce97fea58b4a45e7bc362",
        "FileFlow-v0.2.0-linux-x86_64.tar.gz": "f7236b9b49a83444b3f5640548ca2806718ea5e1729ee0695e47d3a3ad4d546c",
        "FileFlow-v0.2.0-macos-arm64.zip": "b2a20e7c9d9069c68b384328edb5edaecfd02358664e42d94f2cc39fc330de29",
        "FileFlow-v0.2.0-windows-x86_64.zip": "c8448d51777926cd339dc866148c03f21f737342109b770ffd3ba91407cb55dc",
    },
    original_manifest_sha256="4314c7c694e49fde4adc8868e9b6f8216bdf8b312aaed5a879d3c25818d69956",
    original_checksums_sha256="749cf443d534ef844db1c38f6c8a846a77ce53c96776fd409970c7e6f7098940",
)

MANIFEST_NAME = "fileflow-release-manifest.json"
CHECKSUMS_NAME = "SHA256SUMS"
SIGNING = {"unsigned", "ad-hoc", "signed"}


def digest(path: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def load_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def parse_checksums(path: pathlib.Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_.+-]+)", line)
        if not match or match.group(2) in result:
            raise ValueError(f"invalid checksum line: {line!r}")
        result[match.group(2)] = match.group(1)
    return result


def asset_map(release: dict[str, Any]) -> dict[str, dict[str, Any]]:
    assets: dict[str, dict[str, Any]] = {}
    for asset in release.get("assets", []):
        name = asset.get("name")
        if not isinstance(name, str) or name in assets:
            raise ValueError("release assets have a missing or duplicate name")
        assets[name] = asset
    return assets


def canonical_url(contract: Contract, name: str) -> str:
    return f"https://github.com/{contract.repository}/releases/download/{contract.tag}/{name}"


def validate_release_identity(release: dict[str, Any], tag_ref: dict[str, Any], contract: Contract) -> None:
    expected = {
        "id": contract.release_id,
        "tag_name": contract.tag,
        "target_commitish": contract.commit,
        "draft": False,
        "prerelease": False,
        "immutable": False,
    }
    for field, value in expected.items():
        if release.get(field) != value:
            raise ValueError(f"release {field} changed: expected {value!r}, got {release.get(field)!r}")
    published_at = release.get("published_at")
    if not isinstance(published_at, str) or published_at[:10] != contract.published_date:
        raise ValueError("release publication date changed")
    if tag_ref.get("ref") != f"refs/tags/{contract.tag}":
        raise ValueError("release tag ref changed")
    target = tag_ref.get("object", {})
    if target.get("type") != "commit" or target.get("sha") != contract.commit:
        raise ValueError("release tag no longer resolves directly to the reviewed commit")


def verify_file(path: pathlib.Path, expected: str, label: str) -> None:
    actual = digest(path)
    if actual != expected:
        raise ValueError(f"{label} digest changed: expected {expected}, got {actual}")


def validate_package_assets(
    release: dict[str, Any], asset_dir: pathlib.Path, contract: Contract
) -> dict[str, dict[str, Any]]:
    assets = asset_map(release)
    expected_names = set(contract.packages) | {MANIFEST_NAME, CHECKSUMS_NAME}
    if set(assets) != expected_names:
        raise ValueError(f"release asset set changed: {sorted(assets)}")
    for name, expected_digest in contract.packages.items():
        asset = assets[name]
        if asset.get("digest") != f"sha256:{expected_digest}":
            raise ValueError(f"GitHub digest changed for {name}")
        if asset.get("browser_download_url") != canonical_url(contract, name):
            raise ValueError(f"GitHub download URL changed for {name}")
        verify_file(asset_dir / name, expected_digest, name)
    return assets


def prepare(
    release_path: pathlib.Path,
    tag_ref_path: pathlib.Path,
    original_dir: pathlib.Path,
    output_dir: pathlib.Path,
    contract: Contract = CONTRACT,
) -> None:
    release, tag_ref = load_json(release_path), load_json(tag_ref_path)
    validate_release_identity(release, tag_ref, contract)
    assets = validate_package_assets(release, original_dir, contract)

    metadata_expected = {
        MANIFEST_NAME: contract.original_manifest_sha256,
        CHECKSUMS_NAME: contract.original_checksums_sha256,
    }
    for name, expected_digest in metadata_expected.items():
        if assets[name].get("digest") != f"sha256:{expected_digest}":
            raise ValueError(f"original GitHub digest changed for {name}")
        verify_file(original_dir / name, expected_digest, name)

    original_manifest = load_json(original_dir / MANIFEST_NAME)
    if original_manifest.get("schemaVersion") != 1 or original_manifest.get("version") != contract.version:
        raise ValueError("original manifest schema or version changed")
    if original_manifest.get("date") != contract.published_date:
        raise ValueError("original manifest date changed")
    artifacts = original_manifest.get("artifacts")
    if not isinstance(artifacts, list) or len(artifacts) != len(contract.packages):
        raise ValueError("original manifest package set changed")
    by_name: dict[str, dict[str, Any]] = {}
    for artifact in artifacts:
        if not isinstance(artifact, dict) or not isinstance(artifact.get("name"), str):
            raise ValueError("original manifest contains an invalid artifact")
        name = artifact["name"]
        if name in by_name or name not in contract.packages:
            raise ValueError(f"original manifest has an unexpected artifact: {name}")
        if artifact.get("sha256") != contract.packages[name]:
            raise ValueError(f"original manifest digest changed for {name}")
        signing = artifact.get("signing")
        if not isinstance(signing, dict) or signing.get("status") not in SIGNING or not isinstance(signing.get("details"), str):
            raise ValueError(f"original signing metadata is invalid for {name}")
        if not isinstance(artifact.get("platform"), str) or not isinstance(artifact.get("arch"), str):
            raise ValueError(f"original platform metadata is invalid for {name}")
        known_issues = artifact.get("knownIssues")
        if (not isinstance(artifact.get("notes"), str) or not isinstance(known_issues, list)
                or not all(isinstance(issue, str) for issue in known_issues)):
            raise ValueError(f"original notes are invalid for {name}")
        bad_url = canonical_url(dataclasses.replace(contract, tag=contract.bad_url_segment), name)
        if artifact.get("url") != bad_url:
            raise ValueError(f"original defect no longer matches the reviewed repair for {name}")
        artifact["url"] = assets[name]["browser_download_url"]
        by_name[name] = artifact

    original_sums = parse_checksums(original_dir / CHECKSUMS_NAME)
    expected_sums = {**contract.packages, MANIFEST_NAME: contract.original_manifest_sha256}
    if original_sums != expected_sums:
        raise ValueError("original SHA256SUMS changed")

    output_dir.mkdir(parents=True, exist_ok=True)
    repaired_manifest = {
        "schemaVersion": 1,
        "version": contract.version,
        "date": contract.published_date,
        "artifacts": [by_name[name] for name in sorted(by_name)],
    }
    manifest_path = output_dir / MANIFEST_NAME
    manifest_path.write_text(json.dumps(repaired_manifest, indent=2) + "\n", encoding="utf-8")
    repaired_sums = {**contract.packages, MANIFEST_NAME: digest(manifest_path)}
    (output_dir / CHECKSUMS_NAME).write_text(
        "".join(f"{value}  {name}\n" for name, value in sorted(repaired_sums.items())),
        encoding="utf-8",
    )
    snapshot = {
        name: {key: assets[name].get(key) for key in ("id", "name", "size", "digest", "browser_download_url")}
        for name in sorted(contract.packages)
    }
    (output_dir / "package-snapshot.json").write_text(json.dumps(snapshot, indent=2) + "\n", encoding="utf-8")


def verify_final(
    release_path: pathlib.Path,
    tag_ref_path: pathlib.Path,
    repaired_dir: pathlib.Path,
    snapshot_path: pathlib.Path,
    contract: Contract = CONTRACT,
) -> None:
    release, tag_ref = load_json(release_path), load_json(tag_ref_path)
    validate_release_identity(release, tag_ref, contract)
    assets = asset_map(release)
    expected_names = set(contract.packages) | {MANIFEST_NAME, CHECKSUMS_NAME}
    if set(assets) != expected_names:
        raise ValueError("final release asset set changed")
    snapshot = load_json(snapshot_path)
    current_snapshot = {
        name: {key: assets[name].get(key) for key in ("id", "name", "size", "digest", "browser_download_url")}
        for name in sorted(contract.packages)
    }
    if current_snapshot != snapshot:
        raise ValueError("an installer asset changed during metadata repair")
    for name in (MANIFEST_NAME, CHECKSUMS_NAME):
        expected = digest(repaired_dir / name)
        if assets[name].get("digest") != f"sha256:{expected}":
            raise ValueError(f"final GitHub digest mismatch for {name}")
    manifest = load_json(repaired_dir / MANIFEST_NAME)
    for artifact in manifest["artifacts"]:
        if artifact["url"] != assets[artifact["name"]]["browser_download_url"]:
            raise ValueError(f"final URL mismatch for {artifact['name']}")
    expected_sums = {**contract.packages, MANIFEST_NAME: digest(repaired_dir / MANIFEST_NAME)}
    if parse_checksums(repaired_dir / CHECKSUMS_NAME) != expected_sums:
        raise ValueError("repaired SHA256SUMS is invalid")


def main() -> None:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    verify_parser = subparsers.add_parser("verify-final")
    for child in (prepare_parser, verify_parser):
        child.add_argument("--release", type=pathlib.Path, required=True)
        child.add_argument("--tag-ref", type=pathlib.Path, required=True)
    prepare_parser.add_argument("--original-dir", type=pathlib.Path, required=True)
    prepare_parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    verify_parser.add_argument("--repaired-dir", type=pathlib.Path, required=True)
    verify_parser.add_argument("--snapshot", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(args.release, args.tag_ref, args.original_dir, args.output_dir)
    else:
        verify_final(args.release, args.tag_ref, args.repaired_dir, args.snapshot)


if __name__ == "__main__":
    main()
