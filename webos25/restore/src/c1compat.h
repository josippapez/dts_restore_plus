/* webOS 6 (LG C1) build of gstdtsdec.c, forced in by build-c1.sh with -include.
 * Lives outside gstdtsdec.c so the shipped webOS 25 build stays byte-identical.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef DTSDEC_C1COMPAT_H
#define DTSDEC_C1COMPAT_H
#include <dlfcn.h>
#include <gst/gst.h>
#include <gst/audio/gstaudiodecoder.h>

/* GStreamer 1.14 headers predate the 1.20 element-register macros. */
#define GST_ELEMENT_REGISTER_DECLARE(e) gboolean gst_element_register_##e (GstPlugin * plugin);
#define GST_ELEMENT_REGISTER_DEFINE_CUSTOM(e, f) \
  gboolean gst_element_register_##e (GstPlugin * plugin) { return f (plugin); }
#define GST_ELEMENT_REGISTER(e, p) gst_element_register_##e (p)

/* In the C1's GStreamer 1.16.2 but not the 1.14 it builds against: found at runtime. */
static inline gboolean
c1_set_output_caps (GstAudioDecoder * dec, GstCaps * caps)
{
  gboolean (*set) (GstAudioDecoder *, GstCaps *) =
      (gboolean (*) (GstAudioDecoder *, GstCaps *)) dlsym (RTLD_DEFAULT, "gst_audio_decoder_set_output_caps");
  return set && set (dec, caps);
}
#define gst_audio_decoder_set_output_caps c1_set_output_caps

/* The C1 sink takes the carrier as plain PCM through its bypass: never the ES flag,
 * which gstdtsdec.c sets only through this macro. Its zero lead-in stays. */
#undef GST_BUFFER_FLAG_SET
#define GST_BUFFER_FLAG_SET(buf, flag) \
  (GST_MINI_OBJECT_FLAGS (buf) |= ((flag) == (1u << 21) ? 0 : (flag)))
#endif
