/* gstadecswitch.c — audio decoder-switching bin for webOS 25 (LG C5)
 *
 * WHY THIS EXISTS
 * ---------------
 * On the C5, Stremio issues two `selectTrack` calls ~50 ms apart at startup
 * (saved Dolby track, then a revert to track 0) and follows them with a resume
 * seek. decodebin3 reacts to the second select by REMOVING the AC-3 decoder it
 * had just created — see reconfigure_output_stream() in LG's
 * gst-plugins-base/gst/playback/gstdecodebin3.c:5246-5279, which posts
 * `changing-decoder`, unlinks, sets the decoder to NULL and drops it. The
 * removed decoder is `decproxy`, and its DSP set-up is still in flight; the
 * seek's FLUSH-START then lands on a DSP decoderbin that is "not connected
 * yet" and audio DSP0 null-derefs in eform_flush(). Measured on the TV: the
 * DSP dies when the flush arrives within ~10 ms of that teardown, and does not
 * when it arrives ~27 ms later.
 *
 * decodebin3 keeps an existing decoder instead of removing it iff that
 * decoder's sink pad answers accept-caps TRUE for the new stream's caps
 * (gstdecodebin3.c:5194-5205). So this element is a decoder, from decodebin3's
 * point of view, that accepts EVERY family we care about — Dolby, TrueHD/MLP
 * and DTS — and therefore is never removed across an audio-track switch. It
 * swaps its *inner* decoder instead, on its own terms, and holds back the
 * upstream SEEK until a hardware teardown has settled.
 *
 * INNER DECODERS are created by explicit factory name only:
 *   Dolby (x-ac3 / x-eac3 / x-private1-ac3) -> `decproxy`  (stays on the DSP)
 *   TrueHD (x-true-hd) -> `avdec_truehd`,  MLP (x-mlp) -> `avdec_mlp`
 *   DTS (x-dts / x-private1-dts / x-unknown,codec-id=A_DTS) -> `dtsdec`
 * Never by a rank search: this element outranks all of them (320), so a rank
 * search would select itself.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstadecswitch.h"

GST_DEBUG_CATEGORY_STATIC (adecswitch_debug);
#define GST_CAT_DEFAULT adecswitch_debug

/* > avdec_truehd (310) > decproxy (300, set by LG's gstcool.conf), so
 * decodebin3 picks us first for every family in the sink template. */
#define ADECSWITCH_RANK 320

#define DEFAULT_HW_TEARDOWN_GRACE_MS 100
#define DEFAULT_POST_CHANGING_DECODER TRUE

/* Hard ceiling on how long a SEEK may be held, so a teardown that never
 * completes cannot wedge the seeking thread (uMS' main loop) forever. */
#define ADECSWITCH_MAX_SEEK_HOLD_US (500 * G_TIME_SPAN_MILLISECOND)

#define DECPROXY_FACTORY "decproxy"

enum
{
  PROP_0,
  PROP_HW_TEARDOWN_GRACE_MS,
  PROP_POST_CHANGING_DECODER
};

/* Every family we can host, so decodebin3's accept-caps probe on our sink pad
 * says TRUE across a Dolby <-> TrueHD <-> DTS track switch. The DTS entries
 * mirror the patched dtsdec's sink caps (src/gstdtsdec.c:739), including LG's
 * matroskademux retag of raw DTS. */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-ac3; audio/x-eac3; audio/x-private1-ac3; "
        "audio/x-true-hd; audio/x-mlp; "
        "audio/x-dts; audio/x-private1-dts; "
        "audio/x-unknown, codec-id = (string) A_DTS"));

/* audio/x-media is what decproxy emits while its actual decoder is still a
 * puppet (gstdecproxy2.c:2085-2090); audio/x-raw is what our software decoders
 * emit. Both must be in the template or the bin cannot be linked downstream. */
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw; audio/x-media"));

G_DEFINE_TYPE (GstAdecSwitch, gst_adecswitch, GST_TYPE_BIN);

/* ---------------------------------------------------------------------------
 * Our own factory rank while an inner decproxy is alive.
 *
 * decproxy picks its inner decoder from a FRESH rank-sorted DECODER factory
 * list filtered by caps, skipping only its own factory
 * (gstdecproxy2.c:1413-1481). At rank 320 we would be the top match for
 * audio/x-ac3 and decproxy would instantiate an adecswitch inside itself,
 * which would instantiate another decproxy, and so on. So demote our factory
 * to GST_RANK_NONE for as long as any inner decproxy exists.
 *
 * CONFIRMED IN THE 1.24 CORE SOURCE (the monorepo this payload is built
 * against): gst_plugin_feature_set_rank() only writes feature->rank —
 * gstreamer/gst/gstpluginfeature.c:162-169 — and never touches the registry.
 * The registry's feature-list cookie returned by
 * gst_registry_get_feature_list_cookie() (gstreamer/gst/gstregistry.c:2036-2041)
 * is registry->priv->cookie, bumped ONLY when a feature is added or removed
 * (gstregistry.c:524, :607, :638). decodebin3 rebuilds and re-sorts its cached
 * factory list only when that cookie changes
 * (gst_decode_bin_update_factories_list, gstdecodebin3.c:2865-2897), so an
 * already-running decodebin3 keeps the list it built while we were at 320 and
 * still prefers us. A decodebin3 created *while* we are demoted would not see
 * us at all (its list is built with minrank GST_RANK_MARGINAL), which is why
 * the demotion is reference-counted and lifted again as soon as the last inner
 * decproxy reaches NULL, instead of being permanent.
 *
 * Process-global state: the rank lives in the registry, shared by every
 * adecswitch instance in this process. A static GMutex needs no initialiser.
 * ------------------------------------------------------------------------- */
static GMutex adecswitch_rank_lock;
static guint adecswitch_hw_users;

static void
gst_adecswitch_rank_hold (GstAdecSwitch * self)
{
  GstElementFactory *factory = gst_element_get_factory (GST_ELEMENT_CAST (self));

  g_mutex_lock (&adecswitch_rank_lock);
  if (adecswitch_hw_users++ == 0 && factory != NULL) {
    GST_INFO_OBJECT (self, "demoting adecswitch factory to GST_RANK_NONE "
        "while an inner decproxy exists");
    gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), GST_RANK_NONE);
  }
  g_mutex_unlock (&adecswitch_rank_lock);
}

static void
gst_adecswitch_rank_release (GstAdecSwitch * self)
{
  GstElementFactory *factory = gst_element_get_factory (GST_ELEMENT_CAST (self));

  g_mutex_lock (&adecswitch_rank_lock);
  if (--adecswitch_hw_users == 0 && factory != NULL) {
    GST_INFO_OBJECT (self, "restoring adecswitch factory rank %d",
        ADECSWITCH_RANK);
    gst_plugin_feature_set_rank (GST_PLUGIN_FEATURE (factory), ADECSWITCH_RANK);
  }
  g_mutex_unlock (&adecswitch_rank_lock);
}

/* ------------------------------------------------------------------------ */

/* The factory that decodes @caps, or NULL. Returned strings are static, and a
 * family switch is exactly "the factory name changed" — which also makes
 * TrueHD -> MLP a switch (different factory) and AC-3 -> E-AC-3 not one
 * (decproxy swaps its own inner decoder for that). */
static const gchar *
gst_adecswitch_factory_for_caps (GstCaps * caps)
{
  const GstStructure *s;
  const gchar *name;

  if (caps == NULL || gst_caps_is_empty (caps) || gst_caps_is_any (caps))
    return NULL;

  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);

  if (!g_strcmp0 (name, "audio/x-ac3") || !g_strcmp0 (name, "audio/x-eac3")
      || !g_strcmp0 (name, "audio/x-private1-ac3"))
    return DECPROXY_FACTORY;
  if (!g_strcmp0 (name, "audio/x-true-hd"))
    return "avdec_truehd";
  if (!g_strcmp0 (name, "audio/x-mlp"))
    return "avdec_mlp";
  if (!g_strcmp0 (name, "audio/x-dts")
      || !g_strcmp0 (name, "audio/x-private1-dts"))
    return "dtsdec";
  if (!g_strcmp0 (name, "audio/x-unknown")
      && !g_strcmp0 (gst_structure_get_string (s, "codec-id"), "A_DTS"))
    return "dtsdec";

  return NULL;
}

static gboolean
gst_adecswitch_is_decproxy (GstElement * element)
{
  GstElementFactory *factory = gst_element_get_factory (element);

  return factory != NULL
      && !g_strcmp0 (GST_OBJECT_NAME (factory), DECPROXY_FACTORY);
}

/* CAPS and ACCEPT_CAPS are answered from the static template, NEVER from the
 * current inner decoder. That is the whole mechanism: decodebin3 keeps this
 * bin across a track switch because this query says TRUE for the new family
 * (gstdecodebin3.c:5194-5205). Everything else goes to gst_pad_query_default,
 * which is what a ghost pad uses anyway (gstpad.c gst_pad_init) — ghost pads
 * proxy caps queries through their internal links, and that is exactly the
 * forwarding we must not do here. */
static gboolean
gst_adecswitch_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstAdecSwitch *self = GST_ADECSWITCH (parent);
  GstCaps *tmpl;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *res;

      gst_query_parse_caps (query, &filter);
      tmpl = gst_pad_get_pad_template_caps (pad);
      if (filter != NULL) {
        res = gst_caps_intersect_full (filter, tmpl, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmpl);
      } else {
        res = tmpl;
      }
      gst_query_set_caps_result (query, res);
      gst_caps_unref (res);
      return TRUE;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;
      gboolean ok;

      gst_query_parse_accept_caps (query, &caps);
      tmpl = gst_pad_get_pad_template_caps (pad);
      /* Subset, matching how decodebin3 filters decoder factories in the
       * first place (gst_element_factory_list_filter(..., subsetonly=TRUE),
       * gstdecodebin3.c:5033). */
      ok = gst_caps_is_subset (caps, tmpl);
      gst_caps_unref (tmpl);
      GST_LOG_OBJECT (self, "accept-caps %" GST_PTR_FORMAT " -> %d", caps, ok);
      gst_query_set_accept_caps_result (query, ok);
      return TRUE;
    }
    default:
      return gst_pad_query_default (pad, parent, query);
  }
}

/* Runs on a GStreamer thread-pool thread, never on the streaming thread: a
 * decproxy going to NULL tears down the DSP decoder, which is exactly the
 * operation whose timing we are trying to control. */
static void
gst_adecswitch_teardown_decoder (GstElement * element, gpointer user_data)
{
  GstAdecSwitch *self = GST_ADECSWITCH (element);
  GstElement *decoder = GST_ELEMENT_CAST (user_data);
  gboolean was_hw = gst_adecswitch_is_decproxy (decoder);

  GST_INFO_OBJECT (self, "tearing down %" GST_PTR_FORMAT " (hardware: %d)",
      decoder, was_hw);
  gst_element_set_state (decoder, GST_STATE_NULL);
  gst_bin_remove (GST_BIN_CAST (self), decoder);

  if (was_hw) {
    g_mutex_lock (&self->lock);
    self->hw_teardown_done_at = g_get_monotonic_time ();
    self->hw_teardown_pending--;
    g_cond_broadcast (&self->cond);
    g_mutex_unlock (&self->lock);
    gst_adecswitch_rank_release (self);
  }

  GST_DEBUG_OBJECT (self, "teardown of %s done", GST_OBJECT_NAME (decoder));
}

/* Block until no decproxy teardown is in flight and @grace_us has elapsed
 * since the last one reached NULL, capped at @cap_us in total. */
static void
gst_adecswitch_wait_hw_teardown (GstAdecSwitch * self, gint64 grace_us,
    gint64 cap_us)
{
  gint64 started = g_get_monotonic_time ();
  gint64 cap = started + cap_us;

  g_mutex_lock (&self->lock);
  while (TRUE) {
    gint64 now = g_get_monotonic_time ();
    gint64 until;

    if (self->hw_teardown_pending > 0) {
      until = cap;
    } else if (self->hw_teardown_done_at != 0
        && now < self->hw_teardown_done_at + grace_us) {
      until = MIN (self->hw_teardown_done_at + grace_us, cap);
    } else {
      break;
    }

    if (now >= cap) {
      GST_WARNING_OBJECT (self, "SEEK held for the %" G_GINT64_FORMAT " ms cap "
          "(teardowns pending: %u), proceeding anyway",
          cap_us / G_TIME_SPAN_MILLISECOND, self->hw_teardown_pending);
      break;
    }
    g_cond_wait_until (&self->cond, &self->lock, until);
  }
  g_mutex_unlock (&self->lock);

  GST_DEBUG_OBJECT (self, "held for %" G_GINT64_FORMAT " us",
      g_get_monotonic_time () - started);
}

/* Upstream events on the src ghost pad. A SEEK is what produces the FLUSH-START
 * that killed the DSP, so it waits until the decproxy teardown it would race
 * has completed plus the grace period. Everything else — including the
 * CUSTOM_UPSTREAM `acquired-resource`, `set-dts-seamless` and `set-dual-mono`
 * events that decproxy handles in gst_decproxy_src_event()
 * (gstdecproxy2.c:1317-1412) — is forwarded unchanged by the ghost pad. */
static GstPadProbeReturn
gst_adecswitch_src_event_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  GstAdecSwitch *self = GST_ADECSWITCH (user_data);
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  /* uMS grants the audio decoder resource exactly once per pipeline, long
   * before any track switch. decproxy needs it to leave the puppet state and
   * build the real DSP decoder (gstdecproxy2.c:1360-1400), so keep the last
   * one and replay it into every decproxy we plug after this point. */
  if (GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_UPSTREAM
      && gst_event_has_name (event, "acquired-resource")) {
    g_mutex_lock (&self->lock);
    gst_event_replace (&self->resource_event, event);
    g_mutex_unlock (&self->lock);
    GST_INFO_OBJECT (self, "cached acquired-resource for later decproxies");
  }

  if (GST_EVENT_TYPE (event) == GST_EVENT_SEEK)
    gst_adecswitch_wait_hw_teardown (self,
        (gint64) self->grace_ms * G_TIME_SPAN_MILLISECOND,
        ADECSWITCH_MAX_SEEK_HOLD_US);

  return GST_PAD_PROBE_OK;
}

/* Downstream events on the sink ghost pad. A CAPS event naming a different
 * family is the one and only switch trigger; same-family caps changes are
 * forwarded untouched. Runs on the streaming thread with that pad's stream
 * lock held (gstpad.c:5473-5486), so no data can overtake the swap. */
static GstPadProbeReturn
gst_adecswitch_sink_event_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data)
{
  GstAdecSwitch *self = GST_ADECSWITCH (user_data);
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  GstElement *old_decoder, *decoder;
  const gchar *factory_name;
  gboolean is_hw;
  GstCaps *caps = NULL;
  GstPad *target;

  if (GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;

  gst_event_parse_caps (event, &caps);
  factory_name = gst_adecswitch_factory_for_caps (caps);
  if (factory_name == NULL) {
    /* Can only happen if our template and this function disagree. */
    GST_ERROR_OBJECT (self, "no inner decoder for %" GST_PTR_FORMAT, caps);
    return GST_PAD_PROBE_OK;
  }
  if (self->decoder != NULL && !g_strcmp0 (self->decoder_factory,
          factory_name)) {
    GST_DEBUG_OBJECT (self, "same family (%s), forwarding %" GST_PTR_FORMAT,
        factory_name, caps);
    return GST_PAD_PROBE_OK;
  }

  old_decoder = self->decoder;
  is_hw = !g_strcmp0 (factory_name, DECPROXY_FACTORY);
  GST_INFO_OBJECT (self, "switching inner decoder %s -> %s for %" GST_PTR_FORMAT,
      self->decoder_factory ? self->decoder_factory : "(none)", factory_name,
      caps);

  if (is_hw)
    gst_adecswitch_rank_hold (self);

  decoder = gst_element_factory_make (factory_name, NULL);
  if (decoder == NULL) {
    if (is_hw)
      gst_adecswitch_rank_release (self);
    GST_ELEMENT_ERROR (self, STREAM, CODEC_NOT_FOUND, (NULL),
        ("no '%s' element to decode %" GST_PTR_FORMAT, factory_name, caps));
    return GST_PAD_PROBE_DROP;
  }

  if (is_hw
      && g_object_class_find_property (G_OBJECT_GET_CLASS (decoder),
          "propagate-sticky-event")) {
    /* decodebin3 sets this on every decproxy it creates itself
     * (gstdecodebin3.c:5300-5305); keep the decproxy configured exactly as it
     * is on the TV today, since it is now our child rather than its. */
    g_object_set (decoder, "propagate-sticky-event", FALSE, NULL);
  }

  /* uMS re-grants decoder resources on this message, which is how it reacts to
   * decodebin3's own decoder removals (gstdecodebin3.c:5249-5255). */
  if (old_decoder != NULL && self->post_changing_decoder)
    gst_element_post_message (GST_ELEMENT_CAST (self),
        gst_message_new_element (GST_OBJECT_CAST (self),
            gst_structure_new ("changing-decoder", "caps", GST_TYPE_CAPS, caps,
                NULL)));

  if (old_decoder != NULL) {
    /* Count the teardown before anything is detached, so a SEEK arriving
     * during the swap already waits instead of slipping past. */
    if (gst_adecswitch_is_decproxy (old_decoder)) {
      g_mutex_lock (&self->lock);
      self->hw_teardown_pending++;
      g_mutex_unlock (&self->lock);
    }
    gst_element_set_locked_state (old_decoder, TRUE);
  }

  gst_bin_add (GST_BIN_CAST (self), decoder);

  /* Retargeting unlinks the old decoder and links the new one. gst_pad_link()
   * calls schedule_events() (gstreamer/gst/gstpad.c:2574), which marks the
   * ghost pad's sticky events unreceived; the CAPS event we are letting
   * through then re-pushes stream-start ahead of itself
   * (gstpad.c:5573-5581) and the first buffer flushes the rest
   * (check_sticky, gstpad.c:4138). So the new decoder gets stream-start,
   * caps, segment and tags in the right order without us replaying them. */
  target = gst_element_get_static_pad (decoder, "sink");
  gst_ghost_pad_set_target (GST_GHOST_PAD (self->sinkpad), target);
  gst_object_unref (target);

  target = gst_element_get_static_pad (decoder, "src");
  gst_ghost_pad_set_target (GST_GHOST_PAD (self->srcpad), target);
  gst_object_unref (target);

  self->decoder = decoder;
  self->decoder_factory = factory_name;

  if (!gst_element_sync_state_with_parent (decoder))
    GST_WARNING_OBJECT (self, "could not sync %s state with parent",
        factory_name);

  /* Replay the cached grant, otherwise this decproxy keeps its fakeadec puppet,
   * produces no buffers and stalls the whole pipeline. Observed on the C5:
   * switching TrueHD -> Dolby a second time froze video and killed audio. */
  if (is_hw) {
    GstEvent *resource;

    g_mutex_lock (&self->lock);
    resource = self->resource_event ? gst_event_ref (self->resource_event) : NULL;
    g_mutex_unlock (&self->lock);

    if (resource != NULL) {
      GST_INFO_OBJECT (self, "replaying acquired-resource into %s",
          GST_OBJECT_NAME (decoder));
      if (!gst_element_send_event (decoder, resource))
        GST_WARNING_OBJECT (self, "%s did not handle acquired-resource",
            GST_OBJECT_NAME (decoder));
    } else {
      GST_WARNING_OBJECT (self,
          "no acquired-resource seen yet; %s may stay a puppet",
          GST_OBJECT_NAME (decoder));
    }
  }

  if (old_decoder != NULL)
    gst_element_call_async (GST_ELEMENT_CAST (self),
        gst_adecswitch_teardown_decoder, gst_object_ref (old_decoder),
        (GDestroyNotify) gst_object_unref);

  return GST_PAD_PROBE_OK;
}

static GstStateChangeReturn
gst_adecswitch_change_state (GstElement * element, GstStateChange transition)
{
  GstAdecSwitch *self = GST_ADECSWITCH (element);

  /* Let a pending async teardown finish before we leave PAUSED, so the
   * decproxy has released the DSP before this bin stops. This is not a race
   * with GstBin: gst_element_call_async() holds a ref on us
   * (gstelement.c:3871-3875), GstBin only drops children in gst_bin_dispose,
   * and the old decoder is locked-state so gst_bin_element_set_state() skips
   * it (gstbin.c:2489-2501). Nothing downstream can be seeking here, so only
   * the pending teardown matters, not the grace period. */
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
    gst_adecswitch_wait_hw_teardown (self, 0, 2 * G_TIME_SPAN_SECOND);

  return GST_ELEMENT_CLASS (gst_adecswitch_parent_class)->change_state (element,
      transition);
}

static void
gst_adecswitch_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAdecSwitch *self = GST_ADECSWITCH (object);

  switch (prop_id) {
    case PROP_HW_TEARDOWN_GRACE_MS:
      self->grace_ms = g_value_get_uint (value);
      break;
    case PROP_POST_CHANGING_DECODER:
      self->post_changing_decoder = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_adecswitch_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstAdecSwitch *self = GST_ADECSWITCH (object);

  switch (prop_id) {
    case PROP_HW_TEARDOWN_GRACE_MS:
      g_value_set_uint (value, self->grace_ms);
      break;
    case PROP_POST_CHANGING_DECODER:
      g_value_set_boolean (value, self->post_changing_decoder);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_adecswitch_finalize (GObject * object)
{
  GstAdecSwitch *self = GST_ADECSWITCH (object);

  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);

  G_OBJECT_CLASS (gst_adecswitch_parent_class)->finalize (object);
}

static void
gst_adecswitch_dispose (GObject * object)
{
  GstAdecSwitch *self = GST_ADECSWITCH (object);

  /* The current inner decoder is never torn down by
   * gst_adecswitch_teardown_decoder() — that only runs on a family switch —
   * so a decproxy still installed when the bin goes away would leave our
   * factory demoted to GST_RANK_NONE for the rest of the process, and
   * decodebin3 builds its factory list with minrank GST_RANK_MARGINAL
   * (gstdecodebin3.c:2879-2881), so we would never be plugged again. */
  if (self->decoder != NULL && gst_adecswitch_is_decproxy (self->decoder))
    gst_adecswitch_rank_release (self);
  self->decoder = NULL;
  self->decoder_factory = NULL;
  gst_event_replace (&self->resource_event, NULL);

  G_OBJECT_CLASS (gst_adecswitch_parent_class)->dispose (object);
}

static void
gst_adecswitch_class_init (GstAdecSwitchClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_adecswitch_set_property;
  gobject_class->get_property = gst_adecswitch_get_property;
  gobject_class->dispose = gst_adecswitch_dispose;
  gobject_class->finalize = gst_adecswitch_finalize;

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_adecswitch_change_state);

  g_object_class_install_property (gobject_class, PROP_HW_TEARDOWN_GRACE_MS,
      g_param_spec_uint ("hw-teardown-grace-ms", "HW teardown grace (ms)",
          "How long after an inner decproxy reaches NULL an upstream SEEK is held "
          "back, so its flush cannot reach a half-torn-down audio DSP. The total hold "
          "is capped at 500 ms.",
          0, 500, DEFAULT_HW_TEARDOWN_GRACE_MS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_POST_CHANGING_DECODER,
      g_param_spec_boolean ("post-changing-decoder", "Post changing-decoder",
          "Post a 'changing-decoder' element message when the inner decoder "
          "family changes, as decodebin3 does when it removes a decoder",
          DEFAULT_POST_CHANGING_DECODER,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  gst_element_class_set_static_metadata (element_class,
      "Audio decoder switch", "Codec/Decoder/Audio",
      "Hosts decproxy for Dolby and avdec_truehd/avdec_mlp/dtsdec otherwise, "
      "so decodebin3 never removes a decoder across an audio-track switch",
      "dts_restore_plus");
}

static void
gst_adecswitch_init (GstAdecSwitch * self)
{
  GstPadTemplate *tmpl;

  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  self->grace_ms = DEFAULT_HW_TEARDOWN_GRACE_MS;
  self->post_changing_decoder = DEFAULT_POST_CHANGING_DECODER;

  /* Both ghost pads start without a target: the inner decoder cannot be chosen
   * before the first CAPS event names a family. decodebin3 only queries
   * accept-caps and links these pads before then, and the query function below
   * answers from the template, so no target is needed yet. */
  tmpl = gst_static_pad_template_get (&sink_template);
  self->sinkpad = gst_ghost_pad_new_no_target_from_template ("sink", tmpl);
  gst_object_unref (tmpl);
  gst_pad_set_query_function (self->sinkpad, gst_adecswitch_sink_query);
  gst_pad_add_probe (self->sinkpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      gst_adecswitch_sink_event_probe, self, NULL);
  gst_element_add_pad (GST_ELEMENT_CAST (self), self->sinkpad);

  tmpl = gst_static_pad_template_get (&src_template);
  self->srcpad = gst_ghost_pad_new_no_target_from_template ("src", tmpl);
  gst_object_unref (tmpl);
  gst_pad_add_probe (self->srcpad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      gst_adecswitch_src_event_probe, self, NULL);
  gst_element_add_pad (GST_ELEMENT_CAST (self), self->srcpad);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (adecswitch_debug, "adecswitch", 0,
      "audio decoder-switching bin");

  return gst_element_register (plugin, "adecswitch", ADECSWITCH_RANK,
      GST_TYPE_ADECSWITCH);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    adecswitch,
    "Audio decoder-switching bin (keeps Dolby on the DSP across track switches)",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
