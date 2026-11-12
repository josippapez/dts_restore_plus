#!/usr/bin/env python3
"""Check HDMV TrueHD/AC-3 demuxing on the webOS 25 ARM binary under QEMU."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
DEFAULT_PLUGIN = HERE.parents[2] / "demux-out" / "libgstmpegtsdemux.so"
TRUEHD = ("1100", "audio/x-true-hd")
COMPAT_AC3 = ("1100", "audio/x-ac3")
LATER_AC3 = (
    (("1101", "audio/x-ac3"), "expected-1101.ac3"),
    (("1102", "audio/x-ac3"), "expected-1102.ac3"),
)
CASES = {
    "with-ac3": (
        (TRUEHD, "expected.truehd"), (COMPAT_AC3, "expected.ac3"), *LATER_AC3,
    ),
    "no-ac3": (
        (TRUEHD, "expected.truehd"), (COMPAT_AC3, None), *LATER_AC3,
    ),
    "extensionless-truehd": (
        (TRUEHD, "expected.truehd"), (COMPAT_AC3, None), *LATER_AC3,
    ),
    "non-hdmv": LATER_AC3,
}


def run(command, env=None, cwd=None):
    command = [str(arg) for arg in command]
    result = subprocess.run(command, env=env, cwd=cwd, capture_output=True, text=True,
                            timeout=30)
    if result.returncode:
        output = "\n".join(part.strip() for part in (result.stdout, result.stderr) if part)
        raise RuntimeError(f"{shlex.join(command)} exited {result.returncode}\n{output}")
    return result.stdout


def check_ffprobe_order(sample, expected):
    output = run([
        "ffprobe", "-v", "error", "-show_entries", "stream=id,codec_name",
        "-of", "json", sample,
    ])
    codecs = {"audio/x-true-hd": "truehd", "audio/x-ac3": "ac3"}
    actual = [(int(stream["id"], 0), stream.get("codec_name"))
              for stream in json.loads(output)["streams"]]
    order = [(int(pid, 16), codecs[caps]) for pid, caps in expected]
    if actual != order:
        raise RuntimeError(f"FFprobe order {actual} differs from expected {order}")


def check_outputs(console, out, tracks, references):
    expected_order = [track for track, _ in tracks]
    collections = []
    pads = {}
    for line in console.splitlines():
        kind, *fields = line.split("\t")
        if kind == "COLLECTION":
            if len(fields) % 2:
                raise RuntimeError(f"Invalid collection record: {line}")
            collections.append(list(zip(fields[::2], fields[1::2])))
        if kind == "PAD":
            if len(fields) != 3:
                raise RuntimeError(f"Invalid pad record: {line}")
            name, pid, caps = fields
            track = (pid, caps)
            if track in pads:
                raise RuntimeError(f"Duplicate pad for track {track}: {pads[track]}, {name}")
            pads[track] = name

    if not collections or any(order != expected_order for order in collections):
        raise RuntimeError(f"Expected stream collection {expected_order}, got {collections}")
    if set(pads) != set(expected_order) or len(set(pads.values())) != len(pads):
        raise RuntimeError(f"Expected pads for {expected_order}, got {pads}")

    for track, reference in tracks:
        name = pads[track]
        expected = b"" if reference is None else references[reference]
        actual = (out / name).read_bytes()
        if actual != expected:
            raise RuntimeError(
                f"{name} differs from {reference or 'empty output'}\n"
                f"  expected {len(expected)} bytes, SHA256 {hashlib.sha256(expected).hexdigest()}\n"
                f"  got      {len(actual)} bytes, SHA256 {hashlib.sha256(actual).hexdigest()}"
            )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sdk", type=Path,
        default=os.environ.get("SDK_ROOT", Path.home() / "arm-webos-linux-gnueabi_sdk-buildroot"),
        help="webOS 25 armel Buildroot SDK (default: SDK_ROOT or the SDK under HOME)",
    )
    parser.add_argument(
        "--plugin", type=Path, default=DEFAULT_PLUGIN,
        help="libgstmpegtsdemux.so to test (default: the repository's demux-out binary)",
    )
    parser.add_argument(
        "--qemu", default=os.environ.get("QEMU_ARM", "qemu-arm"),
        help="QEMU user-mode ARM executable (default: QEMU_ARM or qemu-arm)",
    )
    parser.add_argument("--sysroot", type=Path, help="QEMU loader sysroot (default: SDK sysroot)")
    parser.add_argument("--cc", help="armel compiler for a separate LG GStreamer cross-build")
    parser.add_argument(
        "--gst-libdir", type=Path,
        help="ARM LG GStreamer 1.24 lib directory with gstreamer-1.0/libgstcoreelements.so "
             "(default: SDK sysroot's usr/lib)",
    )
    args = parser.parse_args()
    if args.cc and (not args.sysroot or not args.gst_libdir):
        parser.error("--cc requires --sysroot and --gst-libdir")
    sdk = args.sdk.resolve()
    sdk_sysroot = sdk / "arm-webos-linux-gnueabi" / "sysroot"
    sysroot = args.sysroot.resolve() if args.sysroot else sdk_sysroot
    gst_libdir = args.gst_libdir.resolve() if args.gst_libdir else sdk_sysroot / "usr" / "lib"
    core_elements = gst_libdir / "gstreamer-1.0" / "libgstcoreelements.so"
    plugin = args.plugin.resolve()
    compiler = args.cc or sdk / "bin" / "arm-linux-gcc"
    qemu_executable = shutil.which(args.qemu)
    if qemu_executable is None:
        raise RuntimeError(f"QEMU executable is unavailable: {args.qemu}")
    for required in (core_elements, plugin, sysroot / "lib" / "ld-linux.so.3"):
        if not required.is_file():
            raise RuntimeError(f"Required file is missing: {required}")

    references = {}
    for tracks in CASES.values():
        for _, reference in tracks:
            if reference is not None and reference not in references:
                references[reference] = (FIXTURES / reference).read_bytes()
                if not references[reference]:
                    raise RuntimeError(f"Reference payload is empty: {reference}")

    pkg_sysroot = sysroot if args.cc else sdk_sysroot
    pkg_dirs = [pkg_sysroot / "usr" / "lib" / "pkgconfig"]
    if args.cc:
        pkg_dirs[:0] = [gst_libdir / "pkgconfig",
                        pkg_sysroot / "usr" / "lib" / "arm-linux-gnueabi" / "pkgconfig"]
        pkg_dirs.append(pkg_sysroot / "usr" / "share" / "pkgconfig")
    pkg_env = dict(
        os.environ,
        PKG_CONFIG_PATH="",
        PKG_CONFIG_SYSROOT_DIR=str(pkg_sysroot),
        PKG_CONFIG_LIBDIR=os.pathsep.join(map(str, pkg_dirs)),
    )
    flags = shlex.split(run(["pkg-config", "--cflags", "--libs", "gstreamer-1.0"], pkg_env))
    with tempfile.TemporaryDirectory(prefix="hdmv-ac3-") as directory:
        work = Path(directory)
        probe = work / "probe"
        candidate = work / "libgstmpegtsdemux.so"
        shutil.copyfile(plugin, candidate)
        print(f"Plugin: {plugin}", flush=True)
        print(f"SHA256: {hashlib.sha256(candidate.read_bytes()).hexdigest()}", flush=True)
        run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2", HERE / "probe.c",
             "-o", probe, *flags])

        gst_env = {key: value for key, value in os.environ.items()
                   if not key.startswith("GST_")}
        gst_env.update(
            G_DEBUG="fatal-criticals",
            GST_PLUGIN_SYSTEM_PATH_1_0="",
            GST_PLUGIN_PATH_1_0="",
            GST_REGISTRY_1_0=str(work / "registry.bin"),
            GST_REGISTRY_FORK="no",
            GST_REGISTRY_UPDATE="no",
        )
        lib_dirs = [gst_libdir, sysroot / "lib"]
        if not args.cc:
            lib_dirs.append(sdk_sysroot / "usr" / "lib")
        lib_dirs.append(sysroot / "usr" / "lib")
        library_path = ":".join(str(path) for path in dict.fromkeys(lib_dirs))
        qemu = [Path(qemu_executable).resolve(), "-L", sysroot, "-E",
                f"LD_LIBRARY_PATH={library_path}", probe]
        for case, tracks in CASES.items():
            sample = FIXTURES / f"{case}.m2ts"
            out = work / case
            out.mkdir()
            try:
                console = run([*qemu, core_elements, candidate, sample, out], gst_env, cwd=out)
                check_outputs(console, out, tracks, references)
                if case != "non-hdmv":
                    check_ffprobe_order(sample, [track for track, _ in tracks])
                if case == "with-ac3":
                    print(console.splitlines()[0], flush=True)
            except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
                raise RuntimeError(f"{case}: {error}") from error
            print(f"PASS {case}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
