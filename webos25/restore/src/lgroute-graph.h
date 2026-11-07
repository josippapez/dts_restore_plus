#ifndef LGROUTE_GRAPH_H
#define LGROUTE_GRAPH_H
#include <glib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  guint slot;
  gchar main[64], bypass[64], rts[64], converter[64], lipsync[64];
  gchar signature[65];
} LgRouteOwner;

typedef struct {
  gchar *source, *target;
} LgRouteEdge;

static void lgroute_edge_free (gpointer data)
{
  LgRouteEdge *edge = data;
  g_free (edge->source);
  g_free (edge->target);
  g_free (edge);
}

static gchar *lgroute_endpoint (GRegex *pattern, const gchar *text)
{
  GMatchInfo *match = NULL;
  if (!g_regex_match (pattern, text, 0, &match)) {
    g_match_info_free (match);
    return NULL;
  }
  gchar *implementation = g_match_info_fetch (match, 1);
  gchar *uid = g_match_info_fetch (match, 2);
  gchar *direction = g_match_info_fetch (match, 3);
  gchar *port = g_match_info_fetch (match, 4);
  gchar *node = !strcmp (implementation, "mixerbin")
      ? g_strdup_printf ("%s_%s:%s%s", implementation, uid, direction, port)
      : g_strdup_printf ("%s_%s", implementation, uid);
  g_free (implementation); g_free (uid); g_free (direction); g_free (port);
  g_match_info_free (match);
  return node;
}

static gchar *lgroute_node (GRegex *pattern, const gchar *text)
{
  if (g_str_has_prefix (text, "shared_buf_") && strlen (text) <= 40) {
    for (const gchar *cursor = text + 11; *cursor; cursor++)
      if (!g_ascii_isxdigit (*cursor)) return NULL;
    return text[11] ? g_strdup (text) : NULL;
  }
  return lgroute_endpoint (pattern, text);
}

static GPtrArray *lgroute_edges (const gchar *text)
{
  if (!text || strlen (text) > 1024 * 1024
      || !g_str_has_prefix (text, "digraph {") || !g_str_has_suffix (text, "}\n"))
    return NULL;
  GRegex *endpoint = g_regex_new (
      "^([a-z][a-z0-9_]*)_([0-9a-f]{8})(?:_[0-9a-f]{8,16})?_(in|out)([0-9]*)$", 0, 0, NULL);
  GRegex *edge_pattern = g_regex_new (
      "^\\s*([A-Za-z0-9_]+)\\s*->\\s*([A-Za-z0-9_]+)\\s*(?:\\[[^]]*\\])?;?\\s*$", 0, 0, NULL);
  GPtrArray *edges = g_ptr_array_new_with_free_func (lgroute_edge_free);
  gchar **lines = g_strsplit (text, "\n", -1);
  gboolean ok = TRUE;
  gint braces = 0;
  gboolean quoted = FALSE, started = FALSE;
  for (const gchar *cursor = text; *cursor && ok; cursor++) {
    if (*cursor == '\\' && quoted && cursor[1]) { cursor++; continue; }
    if (*cursor == '"') quoted = !quoted;
    if (quoted) continue;
    if (*cursor == '{') { braces++; started = TRUE; }
    if (*cursor == '}' && --braces < 0) ok = FALSE;
    if (started && !braces && *cursor != '}' && !g_ascii_isspace (*cursor)) ok = FALSE;
  }
  ok = ok && !quoted && braces == 0;
  for (guint index = 0; lines[index] && ok; index++) {
    if (!strstr (lines[index], "->")) continue;
    GMatchInfo *match = NULL;
    if (edges->len >= 8192 || !g_regex_match (edge_pattern, lines[index], 0, &match)) {
      g_match_info_free (match);
      ok = FALSE;
      break;
    }
    gchar *source = g_match_info_fetch (match, 1), *target = g_match_info_fetch (match, 2);
    LgRouteEdge *edge = g_new0 (LgRouteEdge, 1);
    edge->source = lgroute_node (endpoint, source);
    edge->target = lgroute_node (endpoint, target);
    ok = edge->source && edge->target;
    g_ptr_array_add (edges, edge);
    g_free (source); g_free (target);
    g_match_info_free (match);
  }
  g_strfreev (lines);
  g_regex_unref (endpoint);
  g_regex_unref (edge_pattern);
  if (!ok) { g_ptr_array_unref (edges); return NULL; }
  return edges;
}

static const gchar *lgroute_peer (GPtrArray *edges, const gchar *node, gboolean upstream)
{
  const gchar *peer = NULL;
  for (guint index = 0; index < edges->len; index++) {
    LgRouteEdge *edge = g_ptr_array_index (edges, index);
    const gchar *key = upstream ? edge->target : edge->source;
    const gchar *value = upstream ? edge->source : edge->target;
    if (strcmp (key, node)) continue;
    if (peer && strcmp (peer, value)) return NULL;
    peer = value;
  }
  return peer;
}

static gboolean lgroute_block_edge (const gchar *text, guint slot)
{
  if (!text || strlen (text) > 1024 * 1024
      || !g_str_has_prefix (text, "digraph {") || !g_str_has_suffix (text, "}\n"))
    return FALSE;
  gchar *pattern = g_strdup_printf (
      "^\\s*renderer_%x_out0\\s*->\\s*mixer_0_in%x\\s*\\[label=\"\"\\]\\s*$", slot, slot);
  gboolean found = g_regex_match_simple (pattern, text, G_REGEX_MULTILINE, 0);
  g_free (pattern);
  return found;
}

static gboolean lgroute_resolve (const gchar *blocks, const gchar *modules,
    guint slot, LgRouteOwner *owner)
{
  memset (owner, 0, sizeof *owner);
  if (slot >= 12 || !lgroute_block_edge (blocks, slot)) return FALSE;
  GPtrArray *edges = lgroute_edges (modules);
  if (!edges) return FALSE;
  gboolean ok = FALSE;
  gchar *suffix = g_strdup_printf (":in%u", slot);
  const gchar *anchor = NULL;
  GString *path = g_string_new (NULL);
  for (guint index = 0; index < edges->len; index++) {
    LgRouteEdge *edge = g_ptr_array_index (edges, index);
    if (!g_str_has_prefix (edge->target, "mixerbin_") || !g_str_has_suffix (edge->target, suffix))
      continue;
    if (anchor && strcmp (anchor, edge->target)) goto done;
    anchor = edge->target;
  }
  if (!anchor) goto done;
  const gchar *node = anchor;
  for (guint depth = 0; depth < 32; depth++) {
    g_string_append_printf (path, "%s\n", node);
    if (g_str_has_prefix (node, "appsrc_")) {
      g_strlcpy (owner->main, node, sizeof owner->main);
      break;
    }
    if (depth && g_str_has_prefix (node, "mixerbin_")) goto done;
    node = lgroute_peer (edges, node, TRUE);
    if (!node) goto done;
  }
  if (!owner->main[0]) goto done;
  guint main_uid = 0;
  if (sscanf (owner->main, "appsrc_%x", &main_uid) != 1) goto done;
  for (guint index = 0; index < edges->len; index++) {
    LgRouteEdge *edge = g_ptr_array_index (edges, index);
    guint uid = 0;
    if (!g_str_has_prefix (edge->source, "appsrc_")
        || sscanf (edge->source, "appsrc_%x", &uid) != 1
        || (uid >> 24) != (main_uid >> 24) || !strcmp (edge->source, owner->main))
      continue;
    if (owner->bypass[0] && strcmp (owner->bypass, edge->source)) goto done;
    g_strlcpy (owner->bypass, edge->source, sizeof owner->bypass);
  }
  if (!owner->bypass[0]) goto done;
  node = owner->bypass;
  for (guint depth = 0; depth < 16; depth++) {
    g_string_append_printf (path, "%s\n", node);
    if (g_str_has_prefix (node, "rts_")) {
      g_strlcpy (owner->rts, node, sizeof owner->rts);
      break;
    }
    if (depth && !g_str_has_prefix (node, "gluepw_")
        && !g_str_has_prefix (node, "shared_buf_") && !g_str_has_prefix (node, "gluepr_"))
      goto done;
    node = lgroute_peer (edges, node, FALSE);
    if (!node) goto done;
  }
  if (!owner->rts[0]) goto done;
  node = lgroute_peer (edges, owner->rts, FALSE);
  if (!node || !g_str_has_prefix (node, "srcbin_")) goto done;
  g_strlcpy (owner->converter, node, sizeof owner->converter);
  g_string_append_printf (path, "%s\n", node);
  node = lgroute_peer (edges, node, FALSE);
  if (!node || !g_str_has_prefix (node, "lipsync_")) goto done;
  g_strlcpy (owner->lipsync, node, sizeof owner->lipsync);
  g_string_append_printf (path, "%s\n", node);
  gchar *signature = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path->str, path->len);
  g_strlcpy (owner->signature, signature, sizeof owner->signature);
  g_free (signature);
  owner->slot = slot;
  ok = TRUE;
done:
  g_free (suffix);
  g_string_free (path, TRUE);
  g_ptr_array_unref (edges);
  if (!ok) memset (owner, 0, sizeof *owner);
  return ok;
}
#endif