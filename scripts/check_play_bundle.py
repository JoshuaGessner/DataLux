#!/usr/bin/env python3
"""Check a review-only Play bundle and native 16 KB alignment; never uploads anything."""
import argparse
import base64
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET

PAGE_SIZE = 16_384
NATIVE = "lib/arm64-v8a/libfileflow.so"
ANDROID = "{http://schemas.android.com/apk/res/android}"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_elf(data):
    require(len(data) >= 64 and data[:6] == b"\x7fELF\x02\x01", "Expected a little-endian ELF64 native library")
    require(struct.unpack_from("<H", data, 18)[0] == 183, "Expected the arm64 native library")
    offset = struct.unpack_from("<Q", data, 32)[0]
    size, count = struct.unpack_from("<HH", data, 54)
    require(size >= 56 and count > 0 and offset + count * size <= len(data), "Invalid ELF program headers")
    loads = []
    relro = []
    for index in range(count):
        kind, flags, file_offset, address, _, file_size, memory_size, alignment = struct.unpack_from("<IIQQQQQQ", data, offset + index * size)
        if kind == 1:  # PT_LOAD
            loads.append((address, address + memory_size, flags))
            require(alignment >= PAGE_SIZE and alignment & (alignment - 1) == 0, "Native LOAD segment is not 16 KB aligned")
            require((address - file_offset) % alignment == 0 and file_offset + file_size <= len(data), "Invalid native LOAD segment")
        if kind == 0x6474E552:  # PT_GNU_RELRO
            relro.append((address, address + memory_size))
    require(loads and relro, "Expected native LOAD and GNU_RELRO segments")
    for start, end in relro:
        if end % PAGE_SIZE:
            # NDK r28+ can put RELRO in its own LOAD. A short final page is safe when
            # protecting it cannot overlap any subsequent writable data. See Google's
            # 16 KB guide: the modulo rule applies to a LOAD shared with writable data.
            own_load = next((load for load in loads if load[:2] == (start, end)), None)
            protected_start = start // PAGE_SIZE * PAGE_SIZE
            protected_end = (end + PAGE_SIZE - 1) // PAGE_SIZE * PAGE_SIZE
            overlap = any(load != own_load and load[2] & 2 and load[0] < protected_end and load[1] > protected_start for load in loads)
            require(own_load is not None and not overlap, "Native RELRO segment overlaps writable data on a 16 KB page")


def check_native(archive, prefix="", uncompressed=False):
    with zipfile.ZipFile(archive) as package:
        names = package.namelist()
        require(len(names) == len(set(names)), "Duplicate archive entries are not accepted")
        libraries = [name for name in names if name.endswith(".so")]
        require(libraries == [prefix + NATIVE], "Expected exactly one arm64 libfileflow.so (ADR-0013)")
        if uncompressed:
            require(package.getinfo(libraries[0]).compress_type == zipfile.ZIP_STORED, "The APK native library must be uncompressed")
        check_elf(package.read(libraries[0]))


def run(command, message, allowed=(0,)):
    result = subprocess.run(command, capture_output=True, text=True)
    require(result.returncode in allowed, message)
    return result.stdout


def check_bundle(bundle, bundletool, certificate, expected_code=None, expected_name=None, jdk_bin=None):
    check_native(bundle, "base/")
    java_tool = lambda name: str(jdk_bin / name) if jdk_bin else name
    # AAB signatures use the JAR format. Strict code 4 covers the self-signed upload
    # certificate's untrusted chain, which is expected; other errors (including unsigned
    # entries, code 16) fail. The Gradle guard checks the upload certificate's validity.
    output = run([java_tool("jarsigner"), "-J-Duser.language=en", "-J-Duser.country=US", "-verify", "-strict", str(bundle)],
                 "The AAB signature could not be verified", allowed=(0, 4))
    require("jar verified" in output.lower(), "The AAB is not signed")
    signer = run([java_tool("keytool"), "-printcert", "-rfc", "-jarfile", str(bundle)], "The AAB signer could not be read")
    pem = re.search(r"-----BEGIN CERTIFICATE-----\s+([A-Za-z0-9+/=\s]+)-----END CERTIFICATE-----", signer)
    require(pem is not None and base64.b64decode(pem[1]) == certificate.read_bytes(), "The AAB signer does not match the configured upload identity")
    command = [java_tool("java"), "-jar", str(bundletool), "dump"]
    config = run(command + ["config", f"--bundle={bundle}"], "Bundle configuration could not be read")
    require("PAGE_ALIGNMENT_16K" in config and "PAGE_ALIGNMENT_4K" not in config, "The AAB must request 16 KB ZIP alignment")
    manifest = ET.fromstring(run(command + ["manifest", f"--bundle={bundle}"], "Bundle manifest could not be read"))
    require(manifest.get("package") == "dev.fileflow", "Unexpected Play application ID")
    application = manifest.find("application")
    require(application is not None and application.get(ANDROID + "debuggable", "false") == "false", "Play AAB must not be debuggable")
    sdk = manifest.find("uses-sdk")
    require(sdk is not None and int(sdk.get(ANDROID + "targetSdkVersion", "0")) >= 36, "Play AAB target SDK must be at least 36")
    code = int(manifest.get(ANDROID + "versionCode", "0"))
    name = manifest.get(ANDROID + "versionName")
    require(code > 0 and (expected_code is None or code == expected_code), "Unexpected AAB version code")
    require(name and (expected_name is None or name == expected_name), "Unexpected AAB version name")
    return {"package": "dev.fileflow", "versionCode": code, "versionName": name,
            "targetSdk": int(sdk.get(ANDROID + "targetSdkVersion")),
            "uploadCertificateSha256": hashlib.sha256(certificate.read_bytes()).hexdigest(),
            "bundleSha256": hashlib.sha256(bundle.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--sdk-root", required=True, type=Path)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--bundletool", type=Path)
    parser.add_argument("--certificate", type=Path)
    parser.add_argument("--license-public-key", type=Path)
    parser.add_argument("--account-origin", default="https://fileflow.anomalousinteractive.com")
    parser.add_argument("--jdk-bin", type=Path, help="Optional JDK bin directory; otherwise use tools on PATH")
    parser.add_argument("--expected-version-code", type=int)
    parser.add_argument("--expected-version-name")
    parser.add_argument("--revision", default="local")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        check_native(args.apk, uncompressed=True)
        zipalign = args.sdk_root / "build-tools/37.0.0/zipalign"
        run([str(zipalign), "-c", "-P", "16", "4", str(args.apk)], "APK ZIP alignment verification failed")
        report = {"native16Kb": True, "apkSha256": hashlib.sha256(args.apk.read_bytes()).hexdigest(), "revision": args.revision}
        if args.bundle:
            require(args.bundletool and args.certificate and args.license_public_key, "Bundle verification requires bundletool and both public identities")
            report.update(check_bundle(args.bundle, args.bundletool, args.certificate, args.expected_version_code, args.expected_version_name, args.jdk_bin))
            report.update({"licenseKeyId": hashlib.sha256(args.license_public_key.read_bytes()).hexdigest()[:16], "accountOrigin": args.account_origin, "storeProfile": True})
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("Play artifact checks passed. No upload was performed." if args.bundle else "APK native and ZIP alignment checks passed.")
    except (ValueError, OSError, zipfile.BadZipFile, ET.ParseError, struct.error) as error:
        print(f"Play artifact check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
