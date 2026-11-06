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

/* THE BYPASS SRCBIN FORMAT. The PCM head is "rts -> srcbin -> lipsync"
 * (renderer.c:1569-1580). The kernel configures only the MAIN head's srcbin
 * (PCMCVT_CMD_SET_ALLFMT to uid_src, renderer.c:457-497) and gives the
 * bypass srcbin nothing but out-channels (renderer.c:1100, :681); Glasshouse's
 * module sent the bypass srcbin the same command itself (c5-route.c:137-149:
 * In 192000/8/16 LE signed, Out 192000/8/32). The decomp has no srcbin, mssrc
 * or pcmcvt source, so what the module does with the ES first event's
 * bits-per-sample=16 is not known; measured on the C5 (DTS-18, 2026-11-06):
 * the rts in front of it took sample-rate=192000 and stride-shift=4 (8 x 16
 * bit) from that event.
 *
 * Default OUT 16: dsp1's eARC writer for an ES first event is cpy_es8, which
 * reads 16-bit IEC words and puts each in bits 8..23 of a 32-bit lane slot
 * (output_arc.c:35-73), with the stride from channels * bits-per-sample / 8
 * of that event (:230). A 32-bit srcbin output would be read as pairs of
 * 16-bit words there. Glasshouse's 32 worked because their first event was
 * PCM (pcm-bypass stayed TRUE): the PCM arc writers cpy_8 / cpy_8_swap_c_lfe
 * copy 32-bit words (:76-100). In = out = 16 is the no-op request.
 *
 * The command goes through /proc/aaudio/instances/<uid>/cmd ("<cmd> <u32>..."
 * -> aimc_send_command, module_list.c send_cmd, root 0220), from the media
 * pipeline process, which runs as root. */
#define LGROUTE_PCMCVT_CMD_SET_ALLFMT 0x50020020u
#define LGROUTE_PROC_INSTANCES "/proc/aaudio/instances"
#define LGROUTE_PROC_MODULE_GRAPH "/proc/aaudio/dot_module_connection"

typedef enum
{
  LGROUTE_FMT_NONE = 0,         /* send nothing */
  LGROUTE_FMT_OUT16 = 16,       /* In 192000/8/16 -> Out 192000/8/16 */
  LGROUTE_FMT_OUT32 = 32        /* In 192000/8/16 -> Out 192000/8/32 (Glasshouse) */
} LgRouteFmt;

#define LGROUTE_FMT_DEFAULT LGROUTE_FMT_OUT16

/* "fmt=none" / "fmt=16" / "fmt=32" anywhere in the marker; the default when
 * absent. Returns 0 for a value it does not know (the caller logs and uses
 * the default). */
static inline int
lgroute_marker_fmt (const char *content, LgRouteFmt * fmt)
{
  const char *p;

  *fmt = LGROUTE_FMT_DEFAULT;
  if (content == NULL || (p = strstr (content, "fmt=")) == NULL)
    return 1;
  p += 4;
  if (strncmp (p, "none", 4) == 0)
    *fmt = LGROUTE_FMT_NONE;
  else if (strncmp (p, "16", 2) == 0)
    *fmt = LGROUTE_FMT_OUT16;
  else if (strncmp (p, "32", 2) == 0)
    *fmt = LGROUTE_FMT_OUT32;
  else
    return 0;
  return 1;
}

/* The line written to the srcbin's cmd file: InFs InCh InFormat InEndian
 * InSigned OutFs OutCh OutFormat, the PcmcvtCmdSetAllfmt order of
 * renderer.c:461-465 (endian 0 = LE, signed 0 = signed). */
static inline int
lgroute_format_command (LgRouteFmt fmt, char *buf, size_t size)
{
  if (fmt == LGROUTE_FMT_NONE)
    return 0;
  return snprintf (buf, size, "0x%x %d %d %d 0 0 %d %d %d\n",
      LGROUTE_PCMCVT_CMD_SET_ALLFMT, LGROUTE_CARRIER_RATE,
      LGROUTE_CARRIER_CHANNELS, 16, LGROUTE_CARRIER_RATE,
      LGROUTE_CARRIER_CHANNELS, (int) fmt) > 0;
}

/* A node of dot_module_connection is "<impl>_<uid 8 hex>_<16 hex>_<port>"
 * (module_list.c:1127 names instances "<impl>_%08x"); the dotted shared_buf
 * lines have no hash and are skipped. */
static inline int
lgroute_graph_node (const char *tok, char *impl, size_t impl_size, char *uid,
    char *port, size_t port_size)
{
  const char *end = tok + strlen (tok), *p = end, *hash, *u;
  int i;

  while (p > tok && p[-1] != '_')
    p--;
  if (p == tok || (size_t) (end - p) >= port_size)
    return 0;
  strcpy (port, p);
  hash = p - 1;                 /* the '_' before the port */
  if (hash - tok < 16 + 1 + 8 + 1 + 1)
    return 0;
  hash -= 16;
  for (i = 0; i < 16; i++)
    if (!strchr ("0123456789abcdef", hash[i]))
      return 0;
  if (hash[-1] != '_')
    return 0;
  u = hash - 1 - 8;
  for (i = 0; i < 8; i++)
    if (!strchr ("0123456789abcdef", u[i]))
      return 0;
  if (u[-1] != '_' || (size_t) (u - 1 - tok) >= impl_size)
    return 0;
  memcpy (uid, u, 8);
  uid[8] = '\0';
  memcpy (impl, tok, u - 1 - tok);
  impl[u - 1 - tok] = '\0';
  return 1;
}

/* Our bypass srcbin: the kernel builds "rts -> srcbin -> lipsync" only for
 * the PCM bypass head (renderer.c:1567-1580); the main head is "srcbin ->
 * sola -> lipsync", with an rts PREPENDED to it in no-delay mode
 * (renderer.c:1499, :1604-1611), so there "srcbin -> sola" follows, never
 * "srcbin -> lipsync"; the ES bypass head is "queue -> lipsync" (:1577).
 * The head lives only while the renderer is open (measured on the C5: gone
 * from the graph once playback stopped), so a live graph with exactly one
 * such chain names ours. *chains is the number found; the uid is set only
 * when it is 1. */
static inline int
lgroute_find_bypass_srcbin (const char *graph, char uid[9], int *chains)
{
  char rts_in[8][9], ls_out[8][9];
  int n_rts = 0, n_ls = 0, i, j;
  const char *line = graph;

  *chains = 0;
  while (line != NULL && *line != '\0') {
    char a[96], b[96], impl_a[32], impl_b[32], uid_a[9], uid_b[9];
    char port_a[16], port_b[16];
    const char *nl = strchr (line, '\n');
    size_t len = nl ? (size_t) (nl - line) : strlen (line);
    char buf[256];

    if (len < sizeof (buf) && memchr (line, '>', len) != NULL) {
      memcpy (buf, line, len);
      buf[len] = '\0';
      if (sscanf (buf, " %95s -> %95[^; ]", a, b) == 2
          && lgroute_graph_node (a, impl_a, sizeof (impl_a), uid_a, port_a,
              sizeof (port_a))
          && lgroute_graph_node (b, impl_b, sizeof (impl_b), uid_b, port_b,
              sizeof (port_b))) {
        if (!strcmp (impl_a, "rts") && !strcmp (impl_b, "srcbin") && n_rts < 8)
          strcpy (rts_in[n_rts++], uid_b);
        if (!strcmp (impl_a, "srcbin") && !strcmp (impl_b, "lipsync") && n_ls < 8)
          strcpy (ls_out[n_ls++], uid_a);
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  for (i = 0; i < n_rts; i++)
    for (j = 0; j < n_ls; j++)
      if (!strcmp (rts_in[i], ls_out[j])) {
        if ((*chains)++ == 0)
          strcpy (uid, rts_in[i]);
      }
  return *chains == 1;
}

#endif /* __LGROUTE_POLICY_H__ */
