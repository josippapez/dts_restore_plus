# meson.build - gst-dtstolpcm
#
# Builds a single shared GStreamer plugin, libgstdtstolpcm.so, containing
# both the "bdlpcmenc" encoder and the "dtstolpcm" decoder bin.
#
# Target GStreamer 1.14.x (LG webOS OLED CX basis). Do NOT bump to APIs
# newer than 1.14 without checking availability on the TV's runtime.
#
# ---------------------------------------------------------------------------
# WebOSBrew / on-TV cross-compile note
# ---------------------------------------------------------------------------
# For the OLED CX you must build against the LG NDK so the plugin matches
# the 1.14.4 "CX" ABI (glibc, gstreamer soname, C++ ABI). Use the
# WebOSBrew starfish / meta-lg-webos-ndk toolchain targeting armv7a-neon
# (hard-float, -mfpu=neon -mfloat-abi=hard), e.g.:
#
#   source /opt/webos-sdk/environment-setup-armv7a-neon-webos-linux-gnueabi
#   meson setup build-arm --cross-file arm-webos.txt \
#       -Dprefix=/usr -Dlibdir=lib
#   ninja -C build-arm
#
# Then install libgstdtstolpcm.so alongside the dts_restore libraries in
# the TV's GStreamer plugin path (see README). The plugin must link against
# the SAME gstreamer-1.0 / gst-plugins-base 1.14.4 that ships on the TV;
# building against a desktop 1.20+ base will produce an incompatible .so.
# ---------------------------------------------------------------------------

project('gst-dtstolpcm', 'c',
  version : '1.0.0',
  meson_version : '>= 0.48',
  default_options : [ 'warning_level=1', 'buildtype=debugoptimized' ])

plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')

cc = meson.get_compiler('c')

gst_req = '>= 1.14.0'

gst_dep       = dependency('gstreamer-1.0',       version : gst_req)
gstbase_dep   = dependency('gstreamer-base-1.0',  version : gst_req)
gstaudio_dep  = dependency('gstreamer-audio-1.0', version : gst_req)

# No config.h is generated; VERSION/PACKAGE are passed directly. The
# sources still guard config.h with #ifdef HAVE_CONFIG_H so adding a
# configure step later is a drop-in change.
plugin_c_args = [
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DPACKAGE="gst-dtstolpcm"',
]

sources = [
  'plugin.c',
  'gstbdlpcmenc.c',
  'gstdtstolpcm.c',
]

gstdtstolpcm = shared_library('gstdtstolpcm',
  sources,
  c_args : plugin_c_args,
  dependencies : [ gst_dep, gstbase_dep, gstaudio_dep ],
  install : true,
  install_dir : plugins_install_dir,
)
