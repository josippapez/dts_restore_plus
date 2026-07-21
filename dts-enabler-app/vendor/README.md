# vendored dts_restore payload

The patched GStreamer plugins that actually restore DTS decoding live here, in
`vendor/gst/`. They are **not** part of this repository's own source — they are a
release artifact of the upstream `dts_restore` project and must be kept in sync
with it.

## What must go in `vendor/gst/`

Copy the `.so` files from a dts_restore release (the `gst/` directory of
`dts_restore-<ver>`):

```
vendor/gst/libgstisomp4.so
vendor/gst/libgstisomp4_1_8.so
vendor/gst/libgstmatroska.so
vendor/gst/libgstlibav.so
```

At install/enable time the service copies these to a stable, persistent
location on the TV (`/var/lib/webosbrew/dtsenabler/gst`) and bind-mounts them
over `/usr/lib/gstreamer-1.0/`.

## Keeping in sync with dts_restore

Recommended: add dts_restore as a **git submodule** and symlink or copy its
`gst/` contents into `vendor/gst/` as part of the packaging step, so the app's
version pins an exact dts_restore release:

```sh
git submodule add https://github.com/lgstreamer/dts_restore vendor/dts_restore
cp vendor/dts_restore/gst/*.so vendor/gst/
```

Then bump `appinfo.json` version whenever you re-vendor.

## Provenance & license

These libraries are built from LG's 1.14.4 GStreamer sources with DTS demux/
decoding re-enabled (see the upstream dts_restore README). They are LGPL-2.1+.
Ship the corresponding source offer alongside any distributed `.ipk`.
