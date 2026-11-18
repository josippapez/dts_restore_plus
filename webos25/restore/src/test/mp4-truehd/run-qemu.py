#!/usr/bin/env python3
"""Check MP4 TrueHD demuxing on the webOS 25 ARM binary under QEMU."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile


RESTORE = Path(__file__).resolve().parents[3]
MIB = 1024 * 1024


def run(command, env=None, cwd=None, expect_error=None):
    command = list(map(str, command))
    result = subprocess.run(command, env=env, cwd=cwd, capture_output=True, text=True,
                            timeout=60)
    output = result.stdout + result.stderr
    if expect_error is None:
        valid = result.returncode == 0
    else:
        valid = result.returncode == 1 and expect_error in result.stderr
    if not valid:
        raise RuntimeError(f"{shlex.join(command)} exited {result.returncode}\n{output}")
    return output


def ffmpeg(*args):
    return run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args])


def reference(sample, track, codec, output):
    ffmpeg("-i", sample, "-map", f"0:a:{track}", "-c:a", "copy", "-f", codec, output)
    data = output.read_bytes()
    if not data:
        raise RuntimeError(f"Empty {codec} reference for {sample}")
    return data


def pad_moov(sample, output):
    # FFmpeg puts moov last, so adding a free child preserves every media offset.
    data = sample.read_bytes()
    offset = 0
    while offset < len(data):
        size, kind = struct.unpack_from(">I4s", data, offset)
        if kind == b"moov" and offset + size == len(data):
            target_size = 33 * MIB
            with output.open("wb") as dest:
                dest.write(data[:offset])
                dest.write(struct.pack(">I4s", target_size, b"moov"))
                dest.write(data[offset + 8:])
                dest.write(struct.pack(">I4s", target_size - size, b"free"))
                dest.truncate(offset + target_size)
            return
        if size < 8:
            break
        offset += size
    raise RuntimeError("Expected a trailing moov box in the generated fixture")


def make_fixtures(work):
    sample = work / "truehd-ac3.mp4"
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
           "-t", "0.4", "-ac", "2", "-map", "0:a", "-map", "0:a",
           "-c:a:0", "truehd", "-c:a:1", "ac3", "-strict", "-2", sample)
    pad_moov(sample, work / "large-moov.mp4")
    # Only the headers are necessary to exercise the bounds before payload reads.
    ftyp = struct.pack(">I4s", 20, b"ftyp") + b"isom\0\0\0\0isom"
    for name, size in ((b"moov", 128 * MIB + 1), (b"uuid", 32 * MIB + 1)):
        (work / f"oversize-{name.decode()}.mp4").write_bytes(
            ftyp + struct.pack(">I4sQ", 1, name, size))
    return {codec: reference(sample, index, codec, work / f"expected.{codec}")
            for index, codec in enumerate(("truehd", "ac3"))}


def mp4_box(kind, data):
    return struct.pack(">I4s", len(data) + 8, kind) + data


def mp4_boxes(data):
    offset = 0
    while offset < len(data):
        size, kind = struct.unpack_from(">I4s", data, offset)
        if size < 8 or size > len(data) - offset:
            raise RuntimeError("Invalid box in generated MP4 fixture")
        yield kind, data[offset + 8:offset + size]
        offset += size


def map_sample_tables(data, table_type, replace):
    result = []
    for kind, payload in mp4_boxes(data):
        if kind in (b"trak", b"mdia", b"minf", b"stbl"):
            payload = map_sample_tables(payload, table_type, replace)
        elif kind == table_type:
            kind, payload = replace(payload)
        result.append(mp4_box(kind, payload))
    return b"".join(result)


def sync_table_fixture(work, name, edit):
    replaced = 0

    def replace(table):
        nonlocal replaced
        count, first, second = struct.unpack_from(">III", table, 4)
        if count < 2 or first != 1 or second <= 1:
            raise RuntimeError("Expected the generated sync table to start at sample 1")
        replaced += 1
        return b"stss", edit(table, count)

    # The fixture's moov follows mdat. Editing the sync table changes neither
    # the media offsets nor any compressed samples.
    source = work / "truehd-ac3.mp4"
    boxes = [mp4_box(kind, map_sample_tables(payload, b"stss", replace)
                     if kind == b"moov" else payload)
             for kind, payload in mp4_boxes(source.read_bytes())]
    if replaced != 1:
        raise RuntimeError("Expected one TrueHD sync table in the generated fixture")
    (work / name).write_bytes(b"".join(boxes))


def delayed_sync_fixture(work):
    sync_table_fixture(work, "delayed-sync.mp4", lambda table, count:
                       table[:4] + struct.pack(">I", count - 1) + table[12:])


def extra_sync_fixture(work):
    # The compact index rejects a sync entry past the last sample. The ordinary
    # index ignores it, so the file must still play.
    sync_table_fixture(work, "extra-sync.mp4", lambda table, count:
                       table[:4] + struct.pack(">I", count + 1) + table[8:]
                       + struct.pack(">I", 0x7fffffff))


def non_interleaved_fixtures(work):
    source = work / "video-truehd.mp4"
    ffmpeg("-f", "lavfi", "-i", "color=size=32x32:rate=25",
           "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
           "-t", "0.4", "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4",
           "-c:a", "truehd", "-strict", "-2", source)
    top = list(mp4_boxes(source.read_bytes()))
    if [kind for kind, _ in top[-2:]] != [b"mdat", b"moov"]:
        raise RuntimeError("Expected mdat followed by moov in generated fixture")
    prefix = b"".join(mp4_box(kind, payload) for kind, payload in top[:-2])
    media, moov = top[-2][1], top[-1][1]
    mdat_start = len(prefix) + 8
    tables = []

    def collect(table):
        tables.append([value for value, in struct.iter_unpack(">I", table[8:])])
        return b"stco", table

    map_sample_tables(moov, b"stco", collect)
    if len(tables) != 2 or any(len(offsets) < 2 for offsets in tables):
        raise RuntimeError("Expected multi-chunk video and audio tracks")
    starts = sorted(tables[0] + tables[1])
    ends = starts[1:] + [mdat_start + len(media)]
    if starts[0] != mdat_start or any(start >= end for start, end in zip(starts, ends)):
        raise RuntimeError("Invalid chunk layout in generated MP4 fixture")
    chunks = {start: media[start - mdat_start:end - mdat_start]
              for start, end in zip(starts, ends)}

    ranges = {}
    for kind, entry_format in (("stco", ">I"), ("co64", ">Q")):
        offsets_by_track = []
        with (work / f"non-interleaved-{kind}.mp4").open("wb") as output:
            output.write(prefix)
            for track, offsets in enumerate(tables):
                if kind == "co64" and track == 1:
                    # Put the audio beyond 4 GiB with a sparse extended-size free box.
                    gap = 1 << 32
                    output.write(struct.pack(">I4sQ", 1, b"free", gap))
                    output.seek(gap - 16, os.SEEK_CUR)
                payload = b"".join(chunks[offset] for offset in offsets)
                pos = output.tell() + 8
                new_offsets = []
                for offset in offsets:
                    new_offsets.append(pos)
                    pos += len(chunks[offset])
                output.write(mp4_box(b"mdat", payload))
                offsets_by_track.append(new_offsets)
            end_of_audio = output.tell()

            new_tables = iter(offsets_by_track)

            def replace(table):
                entries = b"".join(struct.pack(entry_format, pos) for pos in next(new_tables))
                return kind.encode(), table[:8] + entries

            output.write(mp4_box(b"moov", map_sample_tables(moov, b"stco", replace)))
        ranges[kind] = (*offsets_by_track, end_of_audio)

    return reference(source, 0, "truehd", work / "expected-non-interleaved.truehd"), ranges


def check_chunk_ranges(console, kind, video, audio, end_of_audio):
    matches = dict((stream, (int(first), int(last))) for stream, first, last in re.findall(
        r"\(stream (vide|soun)\) first offset (\d+), last offset (\d+)", console))
    # Only TrueHD tracks get the corrected read. Stock qtdemux misreads a co64
    # video table as having no range, which leaves the interleave check off.
    stock_video = (video[0], video[-1]) if kind == "stco" else None
    if matches.get("vide") != stock_video:
        raise RuntimeError(f"Incorrect video chunk range: {matches.get('vide')}")
    # Compaction can split the final audio chunk into several logical chunks.
    first, last = matches.get("soun", (0, 0))
    if first != audio[0] or not audio[-1] <= last < end_of_audio:
        raise RuntimeError(f"Incorrect TrueHD chunk range: {(first, last)}")
    if ("stream not interleaved file" in console) != (kind == "stco"):
        raise RuntimeError(f"Unexpected interleave classification for {kind}")


def check_payload(work, source, expected, codec):
    actual = (work / f"actual.{codec}").read_bytes()
    if actual != expected:
        raise RuntimeError(
            f"{source}: {codec} payload differs\n"
            f"  expected {len(expected)} bytes, SHA256 {hashlib.sha256(expected).hexdigest()}\n"
            f"  got      {len(actual)} bytes, SHA256 {hashlib.sha256(actual).hexdigest()}"
        )


def check_truehd(console, compact):
    caps = [line.split("caps = ", 1)[1] for line in console.splitlines()
            if "GstFileSink:truehd.GstPad:sink: caps = " in line]
    if (not caps or any(not line.startswith("audio/x-true-hd, framed=(boolean)true")
                        or "channels=" in line or "rate=" in line for line in caps)):
        raise RuntimeError(f"Expected framed TrueHD caps without container rate/channels, got {caps}\n{console[-10000:]}")
    counts = re.search(r"TrueHD index: (\d+) access units -> (\d+) contiguous groups", console)
    if compact and (not counts or not 0 < int(counts[2]) < int(counts[1])):
        raise RuntimeError("The fixture did not exercise TrueHD sample index compaction")
    if not compact and counts:
        raise RuntimeError("Expected the sync table fixture to use the ordinary index")
    if "GstFileSink:ac3.GstPad:sink: caps = audio/x-ac3" not in console:
        raise RuntimeError("Expected the second audio track to retain its AC-3 caps")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysroot", type=Path, required=True,
                        help="ARM soft-float SDK sysroot with lib/ld-linux.so.3")
    parser.add_argument("--gst-prefix", type=Path,
                        help="LG GStreamer prefix with ARM tools and libraries (default: sysroot/usr)")
    parser.add_argument("--plugin", type=Path, default=RESTORE / "demux-out/libgstisomp4.so",
                        help="candidate plugin (default: repository's demux-out binary)")
    parser.add_argument("--qemu", default=os.environ.get("QEMU_ARM", "qemu-arm"))
    args = parser.parse_args()
    sysroot = args.sysroot.resolve()
    prefix = args.gst_prefix.resolve() if args.gst_prefix else sysroot / "usr"
    plugin = args.plugin.resolve()
    libdir = prefix / "lib"
    core = libdir / "gstreamer-1.0/libgstcoreelements.so"
    inspect = prefix / "bin/gst-inspect-1.0"
    launch = prefix / "bin/gst-launch-1.0"
    for file in (plugin, core, inspect, launch, sysroot / "lib/ld-linux.so.3"):
        if not file.is_file():
            raise RuntimeError(f"Required file is missing: {file}")
    qemu_executable = shutil.which(args.qemu)
    if qemu_executable is None:
        raise RuntimeError(f"QEMU executable is unavailable: {args.qemu}")

    with tempfile.TemporaryDirectory(prefix="mp4-truehd-") as directory:
        work = Path(directory)
        plugins = work / "plugins"
        plugins.mkdir()
        candidate = plugins / "libgstisomp4.so"
        shutil.copyfile(plugin, candidate)
        (plugins / core.name).symlink_to(core)
        print(f"Plugin: {plugin}", flush=True)
        print(f"SHA256: {hashlib.sha256(candidate.read_bytes()).hexdigest()}", flush=True)

        env = {key: value for key, value in os.environ.items() if not key.startswith("GST_")}
        env.update(
            LC_ALL="C",
            G_DEBUG="fatal-criticals",
            GST_PLUGIN_SYSTEM_PATH_1_0="",
            GST_PLUGIN_PATH_1_0=str(plugins),
            GST_REGISTRY_1_0=str(work / "registry.bin"),
            GST_REGISTRY_FORK="no",
            GST_DEBUG_NO_COLOR="1",
            GST_DEBUG="qtdemux:5",
        )
        library_path = os.pathsep.join(map(str, (libdir, sysroot / "lib", sysroot / "usr/lib")))
        qemu = [Path(qemu_executable).resolve(), "-L", sysroot, "-E",
                f"LD_LIBRARY_PATH={library_path}"]

        def gst(*command, expect_error=None):
            return run([*qemu, *command], env=env, cwd=work, expect_error=expect_error)

        info = gst(inspect, "qtdemux")
        if not any(line.split() == ["Filename", str(candidate)] for line in info.splitlines()):
            raise RuntimeError("qtdemux was not loaded from the candidate plugin")

        expected = make_fixtures(work)
        delayed_sync_fixture(work)
        extra_sync_fixture(work)
        for mode, queue in (("pull", []), ("push", ["!", "queue"])):
            for sample in ("truehd-ac3.mp4", "large-moov.mp4", "delayed-sync.mp4",
                           "extra-sync.mp4"):
                console = gst(launch, "-v", "filesrc", f"location={sample}", *queue,
                              "!", "qtdemux", "name=d",
                              "d.audio_0", "!", "filesink", "name=truehd",
                              "location=actual.truehd", "async=false",
                              "d.audio_1", "!", "filesink", "name=ac3",
                              "location=actual.ac3", "async=false")
                if f"activating {mode}" not in console:
                    raise RuntimeError(f"{sample}: expected {mode} scheduling")
                compact = sample not in ("delayed-sync.mp4", "extra-sync.mp4")
                check_truehd(console, compact)
                for codec, payload in expected.items():
                    check_payload(work, sample, payload, codec)
                index = "compact index" if compact else "ordinary index"
                print(f"PASS {mode} {sample} caps, {index} and exact track payloads")

            detail = ("Moov atom size 134217729 > maximum 134217728" if mode == "pull"
                      else "atom moov has bogus size 134217729")
            gst(launch, "filesrc", "location=oversize-moov.mp4", *queue, "!", "qtdemux",
                expect_error=detail)
            print(f"PASS {mode} rejects moov above 128 MiB")

        gst(launch, "filesrc", "location=oversize-uuid.mp4", "!", "queue", "!", "qtdemux",
            expect_error="atom uuid has bogus size 33554433")
        print("PASS push still rejects other atoms above 32 MiB")

        expected_audio, chunk_ranges = non_interleaved_fixtures(work)
        for kind, ranges in chunk_ranges.items():
            sample = f"non-interleaved-{kind}.mp4"
            console = gst(launch, "filesrc", f"location={sample}", "!", "qtdemux", "name=d",
                          "d.video_0", "!", "fakesink", "async=false",
                          "d.audio_0", "!", "filesink", "location=actual.truehd", "async=false")
            check_chunk_ranges(console, kind, *ranges)
            check_payload(work, sample, expected_audio, "truehd")
            print(f"PASS {kind} non-interleaved chunk ranges and exact TrueHD payload")

        dts_sample = RESTORE / "testfiles/DTS-in-mp4.mp4"
        dts = reference(dts_sample, 0, "dts", work / "expected.dts")
        (work / dts_sample.name).symlink_to(dts_sample)
        gst(launch, "filesrc", f"location={dts_sample.name}", "!", "qtdemux", "name=d",
            "d.audio_0", "!", "filesink", "location=actual.dts", "async=false",
            "d.video_0", "!", "fakesink", "async=false")
        check_payload(work, dts_sample, dts, "dts")
        print("PASS existing DTS-in-MP4 payload")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
