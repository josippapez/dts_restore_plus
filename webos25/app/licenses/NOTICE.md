# Licensing notice — DTS Enabler / dts_restore_plus (webOS 25)

This package ships several independently licensed components. Each keeps its own license; nothing
here relicenses anything. The full texts are alongside this file: [`LGPL-2.1.txt`](LGPL-2.1.txt) and
[`GPL-2.0.txt`](GPL-2.0.txt).

| Shipped artifact | License | Upstream / how it is built |
|---|---|---|
| App (`index.html`, `js/`, `css/`), JS service, `install.sh`, `init_dts25.sh`, `uninstall.sh` | LGPL-2.1-or-later | this project |
| `payload/webos25-demux/libgstisomp4.so` | LGPL-2.1-or-later | gst-plugins-good, `dts_support` default flipped to TRUE; built by `restore/build-demux.sh` |
| `payload/webos25-demux/libgstmpegtsdemux.so` | LGPL-2.1-or-later | gst-plugins-bad, same patch; built by `restore/build-demux.sh` |
| `payload/webos25-demux/libgstmatroska.so` | LGPL-2.1-or-later | gst-plugins-good, Dolby Vision profile 7 patch; links the system libbz2; built by `restore/build-demux.sh` |
| `payload/webos25-truehd/libgstlibav.so` | LGPL-2.1-or-later | gst-libav; built by `restore/build-truehd.sh` |
| `libavcodec.so.58`, `libavformat.so.58`, `libavfilter.so.7`, `libavutil.so.56`, `libswresample.so.3` | LGPL-2.1-or-later | ffmpeg 4.4, configured **without** `--enable-gpl` and **without** `--enable-version3`, with a make-up-gain/DRC patch to `libavcodec/mlpdec.c`; built by `restore/build-truehd.sh` |
| `payload/webos25/libgstdtsdec.so` | LGPL-2.1-or-later | gst-plugins-bad `ext/dts` plugin with ffmpeg n4.4.4's `dca` decoder linked in statically (ffmpeg configured **without** `--enable-gpl`, `--enable-version3` or `--enable-nonfree`); built by `restore/build.sh` |
| `payload/cx/libgstmatroska.so`, `libgstisomp4.so`, `libgstisomp4_1_8.so` | LGPL-2.1-or-later | legacy LG GStreamer 1.14.4 gst-plugins-good payload tracked in root `gst/`; DTS demux restored, with the inherited Matroska Dolby Vision changes |
| `payload/cx/libgstlibav.so` | LGPL-2.1-or-later | legacy LG GStreamer 1.14.4 gst-libav payload tracked in root `gst/`; dca decode with the inherited forced stereo-integer downmix |

`libgstdtsdec.so` contains a statically linked copy of ffmpeg (LGPL-2.1-or-later). As LGPL-2.1
section 6 requires, the complete source of the plugin and of ffmpeg, and the script that builds and
links them, are available under the offer below, so you can modify ffmpeg and relink the plugin.
Releases before webos25-2.43 linked libdca instead, which made that plugin GPL-2.0-or-later.

## Written offer for corresponding source

The complete corresponding source for every binary here, together with the exact scripts used to
control compilation and installation, is published at:

  https://github.com/josippapez/dts_restore_plus

Specifically: `webos25/restore/build.sh` (dtsdec + static ffmpeg dca), `webos25/restore/build-truehd.sh`
(gst-libav + ffmpeg), `webos25/restore/build-demux.sh` (isomp4 + mpegtsdemux + matroska), and the patched
sources under `webos25/restore/src/`. Those webOS-25 builds are containerised and reproducible from
that repository alone. The separately packaged legacy `payload/cx/` files are generated unchanged
from the tracked root `gst/` artifacts; their per-file LG GStreamer 1.14.4 provenance and source
repositories are documented in the root `README.md` and `webos25/app/payload/cx/README`.

ffmpeg is not vendored: `build.sh` and `build-truehd.sh` fetch the pinned `n4.4.4` tag from
https://git.ffmpeg.org/ffmpeg.git (`build.sh` builds it unpatched). That source is part of this
offer: request it via the repository above and it will be provided.

This offer is valid for at least three years from the last distribution of this package. If you received this package without access to that repository, request the corresponding
source by opening an issue there, or contact the distributor who gave you the package.

LG's GStreamer sources for webOS are published by LG at https://opensource.lge.com/ and mirrored
under https://github.com/orgs/lgstreamer/repositories; this project builds against those.

## Not endorsed by LG

This is an unofficial community project. It is not affiliated with, endorsed by, or supported by
LG Electronics. Provided "AS IS" without warranty of any kind.
