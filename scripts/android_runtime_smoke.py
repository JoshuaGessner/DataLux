#!/usr/bin/env python3
"""Exercise the real debug APK's JNI on a dedicated 16 KB emulator; never publishes."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import uuid
import zipfile


def run(command, timeout=120):
    result = subprocess.run([str(item) for item in command], text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"Command failed: {command[0]}\n{result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--avd-name", default="DataLux_16K_API37")
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = args.output.resolve() / "runtime-report.json"
    report.parent.mkdir(parents=True, exist_ok=True)
    started = datetime.now(timezone.utc).isoformat()
    report.write_text(json.dumps({"status": "RUNNING", "startedUtc": started}, indent=2) + "\n")
    try:
        execute(args, started)
    except Exception as error:
        report.write_text(json.dumps({"status": "FAIL", "startedUtc": started,
                                      "error": str(error)}, indent=2) + "\n")
        raise


def execute(args, started):
    if not re.fullmatch(r"emulator-\d+", args.serial):
        raise ValueError("Only a dedicated emulator serial is accepted; physical phones are never modified")
    root = Path(__file__).resolve().parent.parent
    apk = args.apk.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    adb = [args.sdk / "platform-tools/adb", "-s", args.serial]
    avd = run(adb + ["emu", "avd", "name"]).splitlines()[0]
    if avd != args.avd_name:
        raise RuntimeError(f"Wrong AVD: {avd}")
    if run(adb + ["shell", "getprop", "sys.boot_completed"]) != "1":
        raise RuntimeError("Emulator has not completed boot")
    page_size = int(run(adb + ["shell", "getconf", "PAGE_SIZE"]))
    if page_size != 16384:
        raise RuntimeError(f"Expected a 16 KB kernel, got {page_size}")
    spec = importlib.util.spec_from_file_location("bundle_check", root / "scripts/check_play_bundle.py")
    bundle_check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bundle_check)
    bundle_check.check_native(apk, uncompressed=True)
    with zipfile.ZipFile(apk) as archive:
        native = archive.read(bundle_check.NATIVE)
    library = out / "libfileflow.so"
    library.write_bytes(native)
    classes = out / "classes"
    dex = out / "dex"
    classes.mkdir(exist_ok=True)
    dex.mkdir(exist_ok=True)
    run([args.java_home / "bin/javac", "--release", "17", "-d", classes,
         root / "scripts/android/NativeRuntimeSmoke.java"])
    run([args.java_home / "bin/java", "-cp", args.sdk / "build-tools/37.0.0/lib/d8.jar",
         "com.android.tools.r8.D8", "--min-api", "33", "--lib",
         args.sdk / "platforms/android-37.0/android.jar", "--output", dex,
         *sorted(classes.rglob("*.class"))])
    jar = out / "runtime-smoke.jar"
    with zipfile.ZipFile(jar, "w") as archive:
        archive.write(dex / "classes.dex", "classes.dex")
    remote = "/data/local/tmp/datalux-native-smoke-" + uuid.uuid4().hex[:12]
    run(adb + ["shell", "mkdir", remote])
    compatibility = {}
    try:
        strict = {"bionic.linker.16kb.app_compat.enabled": "fatal", "pm.16kb.app_compat.disabled": "true"}
        for name in strict:
            compatibility[name] = run(adb + ["shell", "getprop", name])
        for name, value in strict.items():
            run(adb + ["shell", "setprop", name, value])
            if run(adb + ["shell", "getprop", name]) != value:
                raise RuntimeError("Could not disable page-size compatibility fallback")
        for source, name in [(apk, "app.apk"), (library, "libfileflow.so"), (jar, "runtime-smoke.jar")]:
            run(adb + ["push", source, remote + "/" + name])
        classpath = remote + "/app.apk:" + remote + "/runtime-smoke.jar"
        command = "CLASSPATH=" + shlex.quote(classpath) + " " + shlex.join([
            "/system/bin/app_process64", "-Djava.library.path=" + remote, "/system/bin",
            "dev.datalux.testing.NativeRuntimeSmoke", remote + "/libfileflow.so"])
        result = run(adb + ["shell", command], timeout=240)
        if "DATALUX_NATIVE_SMOKE_PASS" not in result:
            raise RuntimeError("Native smoke marker missing: " + result)
        run(adb + ["install", "-r", apk])
        launch = run(adb + ["shell", "am", "start", "-W", "-n", "dev.datalux/dev.fileflow.MainActivity"])
        if "Status: ok" not in launch:
            raise RuntimeError("App launch was not successful: " + launch)
        pid = run(adb + ["shell", "pidof", "dev.datalux"])
        if not pid:
            raise RuntimeError("App did not remain running after launch")
        report = {
            "status": "PASS", "startedUtc": started, "build": "debug; not signed Play acceptance",
            "avd": avd, "pageSize": page_size,
            "compatibilityFallback": "disabled; linker=fatal, packageManager=true",
            "android": run(adb + ["shell", "getprop", "ro.build.version.release"]),
            "abi": run(adb + ["shell", "getprop", "ro.product.cpu.abi"]),
            "apkSha256": hashlib.sha256(apk.read_bytes()).hexdigest(),
            "nativeSha256": hashlib.sha256(native).hexdigest(),
            "nativeChecks": result, "launch": launch,
            "limits": "Synthetic JNI processing and activity launch only; no optical recovery, physical camera, Play billing or production trust tested",
        }
        (out / "runtime-report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    finally:
        for name, value in compatibility.items():
            run(adb + ["shell", shlex.join(["setprop", name, value])])
        # Only this run's generated directory and this app on the dedicated test AVD.
        run(adb + ["shell", "rm", "-rf", remote])
        run(adb + ["shell", "am", "force-stop", "dev.datalux"])


if __name__ == "__main__":
    main()
