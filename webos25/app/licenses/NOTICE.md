# Licensing notice — DTS Enabler / dts_restore_plus (webOS 25)

This package ships several independently licensed components. Each keeps its own license; nothing
here relicenses anything. The full texts are alongside this file: [`LGPL-2.1.txt`](LGPL-2.1.txt) and
[`GPL-2.0.txt`](GPL-2.0.txt).

| Shipped artifact | License | Upstream / how it is built |
|---|---|---|
| App (`index.html`, `js/`, `css/`), JS service, `install.sh`, `init_dts25.sh`, `uninstall.sh` | LGPL-2.1-or-later | this project |
| `libgstisomp4.so` | LGPL-2.1-or-later | gst-plugins-good, `dts_support` default flipped to TRUE; built by `restore/build-demux.sh` |
| `libgstmpegtsdemux.so` | LGPL-2.1-or-later | gst-plugins-bad, same patch; built by `restore/build-demux.sh` |
| `libgstlibav.so` | LGPL-2.1-or-later | gst-libav; built by `restore/build-truehd.sh` |
| `libavcodec.so.58`, `libavformat.so.58`, `libavfilter.so.7`, `libavutil.so.56`, `libswresample.so.3` | LGPL-2.1-or-later | ffmpeg 4.4, configured **without** `--enable-gpl` and **without** `--enable-version3`, with a make-up-gain/DRC patch to `libavcodec/mlpdec.c`; built by `restore/build-truehd.sh` |
| `libgstdtsdec.so` | **GPL-2.0-or-later** | plugin source is LGPL (gst-plugins-bad `ext/dts`), but it links libdca, so the resulting binary is a combined work governed by libdca's GPL |
| `libdca.so.0` | **GPL-2.0-or-later** | libdca (VideoLAN), taken from the Debian `libdca-dev:armel` package during the cross-build |

Because `libgstdtsdec.so` and `libdca.so.0` are GPL-2.0-or-later, this package **as distributed
contains GPL-2.0-or-later code**, and the terms of that license govern its redistribution.

## Written offer for corresponding source

The complete corresponding source for every binary here, together with the exact scripts used to
control compilation and installation, is published at:

  https://github.com/josippapez/dts_restore_plus

Specifically: `webos25/restore/build.sh` (dtsdec + libdca), `webos25/restore/build-truehd.sh`
(gst-libav + ffmpeg), `webos25/restore/build-demux.sh` (isomp4 + mpegtsdemux), and the patched
sources under `webos25/restore/src/`. The builds are containerised and reproducible from that repo
alone.

`libdca.so.0` is built from the Debian `libdca` source package (upstream: VideoLAN,
https://www.videolan.org/developers/libdca.html); the build script pins and fetches it rather than
vendoring a copy. Since libdca is the GPL-2.0-or-later component here, that source is part of this
offer: request it via the repository above and it will be provided, unmodified from the Debian source
package the build used.

This offer is valid for at least three years from the last distribution of this package. If you received this package without access to that repository, request the corresponding
source by opening an issue there, or contact the distributor who gave you the package.

LG's GStreamer sources for webOS are published by LG at https://opensource.lge.com/ and mirrored
under https://github.com/orgs/lgstreamer/repositories; this project builds against those.

## Not endorsed by LG

This is an unofficial community project. It is not affiliated with, endorsed by, or supported by
LG Electronics. Provided "AS IS" without warranty of any kind.
