#ifndef LGROUTE_GRAPH_H
#define LGROUTE_GRAPH_H
#include <glib.h>
#include <stdio.h>
#include <string.h>

/* "<impl>_<uid 8 hex>_<16 hex>_<port>" -> impl and uid; dotted shared_buf lines have no hash. */
static gboolean lgroute_graph_node (const gchar *token, gchar impl[32], gchar uid[9])
{
  const gchar *port = strrchr (token, '_');
  if (!port || port - token < 1 + 8 + 1 + 16) return FALSE;
  const gchar *hash = port - 16, *id = hash - 1 - 8;
  for (int i = 0; i < 16; i++) if (!g_ascii_isxdigit (hash[i])) return FALSE;
  for (int i = 0; i < 8; i++) if (!g_ascii_isxdigit (id[i])) return FALSE;
  if (hash[-1] != '_' || id == token || id[-1] != '_' || (gsize) (id - 1 - token) >= 32)
    return FALSE;
  memcpy (impl, token, id - 1 - token);
  impl[id - 1 - token] = '\0';
  memcpy (uid, id, 8);
  uid[8] = '\0';
  return TRUE;
}

/* The single srcbin fed by an rts and feeding a lipsync: LG's PCM bypass
 * converter. With lipsync, only the one feeding that lipsync counts. */
static gboolean lgroute_bypass_converter (const gchar *graph, const gchar *lipsync, gchar uid[9])
{
  gchar fed[8][9], feeding[8][9];
  guint n_fed = 0, n_feeding = 0, matches = 0;
  gchar **lines = g_strsplit (graph, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    gchar a[96], b[96], impl_a[32], impl_b[32], uid_a[9], uid_b[9];
    if (sscanf (lines[i], " %95s -> %95[^; ]", a, b) != 2
        || !lgroute_graph_node (a, impl_a, uid_a) || !lgroute_graph_node (b, impl_b, uid_b))
      continue;
    if (!strcmp (impl_a, "rts") && !strcmp (impl_b, "srcbin") && n_fed < 8)
      strcpy (fed[n_fed++], uid_b);
    if (!strcmp (impl_a, "srcbin") && !strcmp (impl_b, "lipsync")
      && (!lipsync || !strcmp (uid_b, lipsync)) && n_feeding < 8)
      strcpy (feeding[n_feeding++], uid_a);
  }
  g_strfreev (lines);
  for (guint i = 0; i < n_fed; i++)
    for (guint j = 0; j < n_feeding; j++)
      if (!strcmp (fed[i], feeding[j]) && matches++ == 0)
        strcpy (uid, fed[i]);
  return matches == 1;
}
#endif
