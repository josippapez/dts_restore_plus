# Licensing notice — DTS Enabler / dts_restore_plus (webOS 25)

This package ships several independently licensed components. Each keeps its own license; nothing
here relicenses anything. The full texts are alongside this file: [`LGPL-2.1.txt`](LGPL-2.1.txt) and
[`GPL-2.0.txt`](GPL-2.0.txt).

| Shipped artifact | License | Upstream / how it is built |
|---|---|---|
| App (`index.html`, `js/`, `css/`), JS service, `install.sh`, `init_dts25.sh`, `uninstall.sh` | LGPL-2.1-or-later | this project |
| `payload/webos25-demux/libgstisomp4.so` | LGPL-2.1-or-later | gst-plugins-good, `dts_support` default flipped to TRUE; built by `restore/build-demux.sh` |
| `payload/webos25-demux/libgstmpegtsdemux.so` | LGPL-2.1-or-later | gst-plugins-bad, same patch; built by `restore/build-demux.sh` |
| `payload/webos25-truehd/libgstlibav.so` | LGPL-2.1-or-later | gst-libav; built by `restore/build-truehd.sh` |
| `libavcodec.so.58`, `libavformat.so.58`, `libavfilter.so.7`, `libavutil.so.56`, `libswresample.so.3` | LGPL-2.1-or-later | ffmpeg 4.4, configured **without** `--enable-gpl` and **without** `--enable-version3`, with a make-up-gain/DRC patch to `libavcodec/mlpdec.c`; built by `restore/build-truehd.sh` |
| `libgstdtsdec.so` | **GPL-2.0-or-later** | plugin source is LGPL (gst-plugins-bad `ext/dts`), but it links libdca, so the resulting binary is a combined work governed by libdca's GPL |
| `libdca.so.0` | **GPL-2.0-or-later** | libdca (VideoLAN), taken from the Debian `libdca-dev:armel` package during the cross-build |
| `payload/cx/libgstmatroska.so`, `libgstisomp4.so`, `libgstisomp4_1_8.so` | LGPL-2.1-or-later | legacy LG GStreamer 1.14.4 gst-plugins-good payload tracked in root `gst/`; DTS demux restored, with the inherited Matroska Dolby Vision changes |
| `payload/cx/libgstlibav.so` | LGPL-2.1-or-later | legacy LG GStreamer 1.14.4 gst-libav payload tracked in root `gst/`; dca decode with the inherited forced stereo-integer downmix |

Because `libgstdtsdec.so` and `libdca.so.0` are GPL-2.0-or-later, this package **as distributed
contains GPL-2.0-or-later code**, and the terms of that license govern its redistribution.

## Written offer for corresponding source

The complete corresponding source for every binary here, together with the exact scripts used to
control compilation and installation, is published at:

  https://github.com/josippapez/dts_restore_plus

Specifically: `webos25/restore/build.sh` (dtsdec + libdca), `webos25/restore/build-truehd.sh`
(gst-libav + ffmpeg), `webos25/restore/build-demux.sh` (isomp4 + mpegtsdemux), and the patched
sources under `webos25/restore/src/`. Those webOS-25 builds are containerised and reproducible from
that repository alone. The separately packaged legacy `payload/cx/` files are generated unchanged
from the tracked root `gst/` artifacts; their per-file LG GStreamer 1.14.4 provenance and source
repositories are documented in the root `README.md` and `webos25/app/payload/cx/README`.

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
