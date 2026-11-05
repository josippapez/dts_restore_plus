/* lgroute-policy.h — the decisions behind LG's own eARC bitstream route
 * (approach A), kept free of GStreamer so run-tests.sh can exercise them.
 *
 * Decomp references are maxdewald/webos25-audio-decomp @ acf7f02.
 *
 * THE FLIP. lgaudiosink's "pcm-multi-ch" only caches a flag in lgao
 * (lgao.c:1690-1701). Everything that reads it happens at two moments:
 *   - the CAPS event handler opens, configures and connects the renderer in
 *     one go (render_element.c:570-573): lgao_config sends the flag to the
 *     kernel in AREN_SET_PCM_INFO (lgao.c:1472-1474) and
 *     _lgao_connect_renderer asks for the bypass head (lgao.c:345), which the
 *     kernel builds as "rts -> srcbin -> lipsync" when the stored flag is set
 *     (aapi_media.c:1296-1299, renderer.c:1567-1574);
 *   - the first fed buffer sends the first events (lgao.c:2050-2051): with
 *     both flags clear the bypass appsrc gets es=true, codec, data-rate
 *     (lgao.c:1953-1970, logged as "Event BYPASSS"), else a PCM event
 *     (lgao.c:1980-1995, logged "pcm_bypass or pcm_multi_ch").
 * So the flag is TRUE while the carrier CAPS pass through the sink and FALSE
 * again before its first buffer. Both probes sit on lgaudiosink's own sink
 * pad: an event probe there runs before the sink's event function
 * (gstpad.c:5950-5982, 1.24) and a buffer probe before its chain function
 * (gstpad.c:4471-4494), under the same stream lock, so no queue or thread in
 * between can reorder them.
 *
 * WHY pcm-multi-ch AND NOT pcm-bypass. Both build the same PCM head
 * (aapi_media.c:1296) and both make renderer_get_pcm_bypass() true for LG's
 * "Cloud Game" route (alsa_sndout.c:2276-2290). pcm_bypass additionally sets
 * "ch-mapping-lfe-c" on the PCM first event (lgao.c:1991-1994), which never
 * fires here because the flag is FALSE by then; pcm_multi_ch additionally
 * sets "out-channels" on the main srcbin at aren_start (aapi_media.c:1658-
 * 1663), which carries no data here (ES goes to the bypass appsrc only,
 * lgao.c:2070-2073). pcm-multi-ch is the one already measured on the C5
 * (DTS-9: lgao logged "pcm_bypass or pcm_multi_ch", PCM head built).
 *
 * THE CARRIER CAPS. render_element.c:362-393 take rate / channels / format
 * into the PCM info the kernel's srcbin is configured from, :497-515 take
 * data-rate / data-channels / data-bps / codec-str into the ES first event,
 * :399-413 take acmod. acmod must be given and 0: a channel-mask alone makes
 * it 7 (:408), a changed acmod re-sends the first events from inside the
 * CAPS handler (:586-593) while the flag is still TRUE, and GstAudioDecoder
 * refuses 8-channel caps without a channel-mask (audio-info.c:254, 1.24).
 * codec-str is "mat" for DTS too: dsp1 maps dts* to its codes 5..9
 * (output_arc.c:161-174) but the kernel turns anything above MAT (4) into
 * PCM and puts the eARC link in L-PCM mode (output.c:689-695, 703-710);
 * MAT keeps codec 4 on both sides and the receiver reads the real type from
 * the burst preamble. data-rate 768000 selects HBR (output_arc.c:228,
 * hal_arc.c:78-81); data-channels 8 selects the 4-lane writer
 * (output_arc.c:222).
 */
#ifndef __LGROUTE_POLICY_H__
#define __LGROUTE_POLICY_H__

#include <stdio.h>
#include <string.h>

#define LGROUTE_CARRIER_RATE      192000
#define LGROUTE_CARRIER_CHANNELS  8
#define LGROUTE_CARRIER_MASK      0xc3f   /* 7.1; only acmod matters, see above */
#define LGROUTE_CARRIER_ACMOD     0
#define LGROUTE_CARRIER_CODEC_STR "mat"
#define LGROUTE_CARRIER_DATA_RATE 768000
#define LGROUTE_CARRIER_DATA_CH   8
#define LGROUTE_CARRIER_DATA_BPS  16

/* lgaudiosink buffer flag selecting LGAO_DATA_ES: bit 21,
 * GST_BUFFER_FLAG_LAST << 1 (render_element.c:790, :916-917). */
#define LGROUTE_BUFFER_FLAG_ES (1u << 21)

typedef enum
{
  LGROUTE_IN_CARRIER_CAPS,      /* our carrier CAPS reached the sink pad */
  LGROUTE_IN_OTHER_CAPS,        /* someone else's CAPS reached it */
  LGROUTE_IN_BUFFER             /* a buffer reached it */
} LgRouteInput;

typedef enum
{
  LGROUTE_ACT_NONE,
  LGROUTE_ACT_SET_TRUE,         /* pcm-multi-ch := TRUE */
  LGROUTE_ACT_SET_FALSE         /* pcm-multi-ch := FALSE */
} LgRouteAction;

typedef struct
{
  int flag;                     /* what we last set pcm-multi-ch to */
  int flips;                    /* TRUE -> FALSE flips done before a buffer */
} LgRouteFlip;

static inline void
lgroute_flip_init (LgRouteFlip * f)
{
  f->flag = 0;
  f->flips = 0;
}

static inline LgRouteAction
lgroute_flip_step (LgRouteFlip * f, LgRouteInput in)
{
  switch (in) {
    case LGROUTE_IN_CARRIER_CAPS:
      if (f->flag)
        return LGROUTE_ACT_NONE;
      f->flag = 1;
      return LGROUTE_ACT_SET_TRUE;
    case LGROUTE_IN_OTHER_CAPS:
      if (!f->flag)
        return LGROUTE_ACT_NONE;
      f->flag = 0;
      return LGROUTE_ACT_SET_FALSE;
    case LGROUTE_IN_BUFFER:
      if (!f->flag)
        return LGROUTE_ACT_NONE;
      f->flag = 0;
      f->flips++;
      return LGROUTE_ACT_SET_FALSE;
  }
  return LGROUTE_ACT_NONE;
}

/* The boot hook's watcher writes "<pid> <soundOutput> <soundOutputDigital>"
 * (init_dts25.sh); dtsdec's passthrough reads it the same way
 * (gstdtsdec.c:1598-1615). Returns 1 when the text names an ARC/eARC
 * receiver with Pass Through or Auto and stores the watcher pid, which the
 * caller checks for liveness. */
static inline int
lgroute_parse_sound_output (const char *text, int *pid)
{
  char out[64] = "", digital[64] = "";
  int p = 0;

  if (text == NULL || sscanf (text, "%d %63s %63s", &p, out, digital) != 3
      || p <= 0)
    return 0;
  *pid = p;
  return strcmp (out, "external_arc") == 0
      && (strcmp (digital, "passThrough") == 0 || strcmp (digital, "auto") == 0);
}

/* The opt-in marker's content: the word "force" skips the receiver check,
 * for a TV without one (the C5 test plan). */
static inline int
lgroute_marker_forces (const char *content)
{
  return content != NULL && strstr (content, "force") != NULL;
}

#endif /* __LGROUTE_POLICY_H__ */
