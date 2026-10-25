#!/usr/bin/env python3
"""Add a verified DMG to the existing v0.2.0 release without replacing app binaries."""
from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from scripts.repair_release_metadata import (
    CONTRACT as ORIGINAL_CONTRACT, MANIFEST_NAME, CHECKSUMS_NAME, asset_map,
    canonical_url, digest, load_json, parse_checksums, validate_package_assets,
    validate_release_identity, verify_file,
)

CONTRACT = dataclasses.replace(
    ORIGINAL_CONTRACT,
    original_manifest_sha256='7748dd79193ac7907a2f55a0dc423ddb2631b7d88acca202678fe8efe5fe1116',
    original_checksums_sha256='70c56c1eb09bc473d52f6127db68daeb3e5270bc66e8ce4981be45faaad3e0a3',
)

def api(suffix, *, method=None):
    args = ['gh', 'api', f'repos/{CONTRACT.repository}/{suffix}']
    if method:
        args += ['--method', method]
    return json.loads(subprocess.check_output(args)) if method != 'DELETE' else subprocess.run(args, check=True)

def download_asset(asset, destination):
    with destination.open('wb') as output:
        subprocess.run(['gh', 'api', f'repos/{CONTRACT.repository}/releases/assets/{asset["id"]}', '-H', 'Accept: application/octet-stream'], stdout=output, check=True)

def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

def download_original(directory):
    directory.mkdir(parents=True, exist_ok=False)
    release = api(f'releases/{CONTRACT.release_id}')
    tag = api(f'git/ref/tags/{CONTRACT.tag}')
    validate_release_identity(release, tag, CONTRACT)
    save_json(directory / 'release.json', release)
    save_json(directory / 'tag.json', tag)
    assets = asset_map(release)
    if set(assets) != set(CONTRACT.packages) | {MANIFEST_NAME, CHECKSUMS_NAME}:
        raise ValueError('Release already changed; refusing a fresh repackaging run')
    for name, expected in {**CONTRACT.packages, MANIFEST_NAME: CONTRACT.original_manifest_sha256, CHECKSUMS_NAME: CONTRACT.original_checksums_sha256}.items():
        if assets[name].get('digest') != 'sha256:' + expected:
            raise ValueError(f'Original GitHub digest changed: {name}')
        download_asset(assets[name], directory / name)
        verify_file(directory / name, expected, name)

def prepare(directory, dmg, report_path, output, contract=CONTRACT):
    release, tag = load_json(directory / 'release.json'), load_json(directory / 'tag.json')
    validate_release_identity(release, tag, contract)
    assets = validate_package_assets(release, directory, contract)
    for name, expected in [(MANIFEST_NAME, contract.original_manifest_sha256), (CHECKSUMS_NAME, contract.original_checksums_sha256)]:
        if assets[name]['digest'] != 'sha256:' + expected:
            raise ValueError('Original metadata changed')
        verify_file(directory / name, expected, name)
    manifest = load_json(directory / MANIFEST_NAME)
    sums = parse_checksums(directory / CHECKSUMS_NAME)
    if sums != {**contract.packages, MANIFEST_NAME: contract.original_manifest_sha256}:
        raise ValueError('Original checksums do not match the reviewed packages')
    if manifest['schemaVersion'] != 1 or manifest['version'] != contract.version or manifest['date'] != contract.published_date:
        raise ValueError('Original manifest identity changed')
    artifacts = manifest['artifacts']
    if {row['name'] for row in artifacts} != set(contract.packages) or len(artifacts) != len(contract.packages):
        raise ValueError('Original manifest package set changed')
    for row in artifacts:
        if row['sha256'] != contract.packages[row['name']] or row['url'] != canonical_url(contract, row['name']):
            raise ValueError('Original manifest package identity changed')
    mac = [row for row in artifacts if row['platform'] == 'macos']
    if len(mac) != 1 or not mac[0]['name'].endswith('.zip'):
        raise ValueError('Expected one original Mac ZIP')
    mac = mac[0]
    expected_name = mac['name'][:-4] + '.dmg'
    report = load_json(report_path)
    if report.get('schemaVersion') != 1 or report.get('sourceRevision') != contract.commit or report.get('architecture') != mac['arch'] or report.get('packageName') != expected_name or report.get('platform') != 'macos' or report.get('version') != contract.version or not re.fullmatch(r'[0-9a-f]{64}', str(report.get('appTreeSha256', ''))):
        raise ValueError('DMG verification report does not match this release')
    if dmg.name != expected_name or report.get('sha256') != digest(dmg):
        raise ValueError('DMG bytes do not match the verified package report')
    mac.update(name=expected_name, url=canonical_url(contract, expected_name), sha256=digest(dmg), notes='Experimental drag-and-drop DMG. Open the disk image, then drag FileFlow to Applications.')
    mac['signing']['details'] = 'Original app-bundle signature retained. Not Developer ID signed or notarized; the DMG does not remove Gatekeeper requirements.'
    output.mkdir(parents=True, exist_ok=True)
    save_json(output / MANIFEST_NAME, manifest)
    sums.update({expected_name: digest(dmg), MANIFEST_NAME: digest(output / MANIFEST_NAME)})
    (output / CHECKSUMS_NAME).write_text(''.join(f'{value}  {name}\n' for name, value in sorted(sums.items())), encoding='utf-8')
    return {name: {key: asset.get(key) for key in ('id', 'name', 'size', 'digest', 'browser_download_url')} for name, asset in assets.items() if name in contract.packages}

def verify_current(release, tag, original_packages, dmg, output, contract=CONTRACT):
    validate_release_identity(release, tag, contract)
    assets = asset_map(release)
    if not set(contract.packages).issubset(assets) or set(assets) - (set(contract.packages) | {dmg.name, MANIFEST_NAME, CHECKSUMS_NAME}):
        raise ValueError('Unexpected release asset set')
    for name, original in original_packages.items():
        if {key: assets[name].get(key) for key in original} != original:
            raise ValueError(f'Existing installer changed: {name}')
    if dmg.name in assets and (assets[dmg.name].get('digest') != 'sha256:' + digest(dmg) or assets[dmg.name].get('browser_download_url') != canonical_url(contract, dmg.name)):
        raise ValueError('A different DMG already uses this filename')
    for name, old in [(MANIFEST_NAME, contract.original_manifest_sha256), (CHECKSUMS_NAME, contract.original_checksums_sha256)]:
        if name in assets and assets[name].get('digest') not in {'sha256:' + old, 'sha256:' + digest(output / name)}:
            raise ValueError(f'Unrecognized metadata update: {name}')
    return assets

def publish(directory, dmg, report, output):
    original = prepare(directory, dmg, report, output)
    current = api(f'releases/{CONTRACT.release_id}')
    tag = api(f'git/ref/tags/{CONTRACT.tag}')
    assets = verify_current(current, tag, original, dmg, output)
    if dmg.name not in assets:
        subprocess.run(['gh', 'release', 'upload', CONTRACT.tag, str(dmg), '--repo', CONTRACT.repository], check=True)
    def verify_download(name, expected):
        state = api(f'releases/{CONTRACT.release_id}')
        items = verify_current(state, api(f'git/ref/tags/{CONTRACT.tag}'), original, dmg, output)
        asset = items[name]
        if asset.get('digest') != 'sha256:' + expected:
            raise ValueError(f'Uploaded digest mismatch: {name}')
        destination = output / ('downloaded-' + name)
        download_asset(asset, destination)
        verify_file(destination, expected, name)
        return state, items
    current, assets = verify_download(dmg.name, digest(dmg))
    if all(name in assets and assets[name]['digest'] == 'sha256:' + digest(output / name) for name in (MANIFEST_NAME, CHECKSUMS_NAME)):
        for name in (MANIFEST_NAME, CHECKSUMS_NAME):
            verify_download(name, digest(output / name))
        print('DMG and metadata were already published and verified')
        return
    # Hide only recognized metadata after every source/package guard has passed.
    if MANIFEST_NAME in assets:
        api(f'releases/assets/{assets[MANIFEST_NAME]["id"]}', method='DELETE')
    try:
        if CHECKSUMS_NAME in assets and assets[CHECKSUMS_NAME]['digest'] != 'sha256:' + digest(output / CHECKSUMS_NAME):
            api(f'releases/assets/{assets[CHECKSUMS_NAME]["id"]}', method='DELETE')
        if CHECKSUMS_NAME not in assets or assets[CHECKSUMS_NAME]['digest'] != 'sha256:' + digest(output / CHECKSUMS_NAME):
            subprocess.run(['gh', 'release', 'upload', CONTRACT.tag, str(output / CHECKSUMS_NAME), '--repo', CONTRACT.repository], check=True)
        verify_download(CHECKSUMS_NAME, digest(output / CHECKSUMS_NAME))
        subprocess.run(['gh', 'release', 'upload', CONTRACT.tag, str(output / MANIFEST_NAME), '--repo', CONTRACT.repository], check=True)
        final, _ = verify_download(MANIFEST_NAME, digest(output / MANIFEST_NAME))
        save_json(output / 'release-final.json', final)
    except BaseException:
        # Retried publish jobs reuse the same build artifact and recognize only these exact bytes.
        try:
            item = asset_map(api(f'releases/{CONTRACT.release_id}')).get(MANIFEST_NAME)
            if item and item.get('digest') == 'sha256:' + digest(output / MANIFEST_NAME):
                api(f'releases/assets/{item["id"]}', method='DELETE')
        except Exception:
            pass
        raise
    print('Published verified DMG; all existing installer assets and the source tag are unchanged')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['download', 'prepare', 'publish'])
    parser.add_argument('--directory', type=pathlib.Path, required=True)
    parser.add_argument('--dmg', type=pathlib.Path)
    parser.add_argument('--report', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    if args.command == 'download':
        download_original(args.directory)
    elif not all((args.dmg, args.report, args.output)):
        parser.error('--dmg, --report, and --output are required')
    elif args.command == 'prepare':
        prepare(args.directory, args.dmg, args.report, args.output)
    else:
        publish(args.directory, args.dmg, args.report, args.output)

if __name__ == '__main__':
    main()
