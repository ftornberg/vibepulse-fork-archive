/* See merge_queue_policy.h. Pure decisions, no LVGL, no clock, no network. */
#include "merge_queue_policy.h"

#include <string.h>

/* FNV-1a over "project#number". A collision would only merge two cards into
 * one dismissal; nothing is ever sent or acted on from this key. */
uint32_t tk_mq_key(const tk_mq_pr *pr) {
  uint32_t hash = 2166136261u;
  for (const char *c = pr->project; *c; c++) {
    hash = (hash ^ (uint8_t)*c) * 16777619u;
  }
  hash = (hash ^ '#') * 16777619u;
  uint32_t number = (uint32_t)pr->number;
  for (int i = 0; i < 4; i++) {
    hash = (hash ^ (uint8_t)(number >> (8 * i))) * 16777619u;
  }
  return hash;
}

static bool contains(const uint32_t *keys, uint8_t count, uint32_t key) {
  for (uint8_t i = 0; i < count; i++) {
    if (keys[i] == key) return true;
  }
  return false;
}

static void ack(tk_mq_state *state, uint32_t key) {
  if (contains(state->acked, state->acked_count, key)) return;
  if (state->acked_count == TK_MQ_ACK_CAP) {
    /* Oldest out: the list holds at most TK_MQ_LIST_CAP live entries. */
    memmove(state->acked, state->acked + 1,
            (TK_MQ_ACK_CAP - 1) * sizeof state->acked[0]);
    state->acked_count--;
  }
  state->acked[state->acked_count++] = key;
}

void tk_mq_init(tk_mq_state *state) {
  if (!state) return;
  memset(state, 0, sizeof *state);
  state->lead = -1;
}

bool tk_mq_apply(tk_mq_state *state, const tk_merge_queue *queue,
                 uint64_t now_ms) {
  if (!state || !queue) return false;
  state->queue = *queue;
  uint8_t listed = queue->enabled ? queue->pr_count : 0;

  uint32_t keys[TK_MQ_LIST_CAP];
  for (uint8_t i = 0; i < listed; i++) keys[i] = tk_mq_key(&queue->prs[i]);

  /* A complete list is the truth: forget dismissals for pull requests that
   * are gone (merged or closed), so a reopened one counts as new. An
   * incomplete list proves nothing about the missing ones. */
  if (queue->enabled && !queue->incomplete) {
    uint8_t kept = 0;
    for (uint8_t i = 0; i < state->acked_count; i++) {
      if (contains(keys, listed, state->acked[i])) {
        state->acked[kept++] = state->acked[i];
      }
    }
    state->acked_count = kept;
  }

  bool pulse = false;
  int8_t lead = -1;
  for (uint8_t i = 0; i < listed; i++) {
    bool unseen = !contains(state->acked, state->acked_count, keys[i]);
    bool arrived = !contains(state->present, state->present_count, keys[i]);
    if (unseen && lead < 0) lead = (int8_t)i;
    if (unseen && arrived) pulse = true;
  }
  memcpy(state->present, keys, listed * sizeof keys[0]);
  state->present_count = listed;

  if (listed == 0) {
    state->showing = false;
    state->lead = -1;
    return false;
  }
  if (pulse) {
    state->showing = true;
    state->pulse_until_ms = now_ms + TK_MQ_PULSE_MS;
  }
  if (state->showing) {
    /* Everything unseen may have gone while the rest stayed: still up,
     * leading with the first listed. */
    state->lead = lead >= 0 ? lead : 0;
  }
  return pulse;
}

void tk_mq_dismiss(tk_mq_state *state) {
  if (!state) return;
  for (uint8_t i = 0; i < state->present_count; i++) {
    ack(state, state->present[i]);
  }
  state->showing = false;
  state->lead = -1;
}

tk_mq_phase tk_mq_phase_at(const tk_mq_state *state, uint64_t now_ms) {
  if (!state || !state->showing || state->present_count == 0) {
    return TK_MQ_HIDDEN;
  }
  return now_ms < state->pulse_until_ms ? TK_MQ_PULSE : TK_MQ_STATIC;
}

const tk_mq_pr *tk_mq_lead(const tk_mq_state *state) {
  if (!state || !state->showing || state->lead < 0 ||
      state->lead >= state->queue.pr_count) {
    return NULL;
  }
  return &state->queue.prs[state->lead];
}

static size_t utf8_length(const unsigned char *s) {
  if (s[0] < 0x80) return 1;
  if ((s[0] & 0xe0) == 0xc0) return 2;
  if ((s[0] & 0xf0) == 0xe0) return 3;
  if ((s[0] & 0xf8) == 0xf0) return 4;
  return 1;
}

void tk_mq_title_label(const char *source, char *destination, size_t cap) {
  if (!destination || cap == 0) return;
  destination[0] = '\0';
  if (!source) return;
  size_t out = 0;
  const unsigned char *c = (const unsigned char *)source;
  while (*c) {
    size_t len = utf8_length(c);
    for (size_t i = 1; i < len; i++) {
      if (c[i] == 0) { len = i; break; } /* truncated sequence: stop here */
    }
    unsigned char glyph[2] = {'?', 0};
    size_t glyph_len = 1;
    if (len == 1) {
      unsigned char b = c[0];
      if (b < 0x20 || b == 0x7f) { c++; continue; }
      if (b >= 'a' && b <= 'z') b = (unsigned char)(b - 32);
      glyph[0] = b < 0x80 ? b : '?';
    } else if (len == 2 && c[0] == 0xC2 && (c[1] == 0xB7 || c[1] == 0xA0)) {
      /* The face has the middle dot; a no-break space is just a space. */
      if (c[1] == 0xB7) { glyph[0] = 0xC2; glyph[1] = 0xB7; glyph_len = 2; }
      else glyph[0] = ' ';
    } else if (len == 3 && c[0] == 0xE2 && c[1] == 0x80 &&
               (c[2] == 0x93 || c[2] == 0x94)) {
      /* En and em dash both as the en dash the face carries (U+2013). */
      if (out + 3 >= cap) break;
      memcpy(destination + out, "\xE2\x80\x93", 3);
      out += 3;
      c += len;
      continue;
    } else if (len == 3 && c[0] == 0xE2 && c[1] == 0x80 &&
               (c[2] == 0x98 || c[2] == 0x99)) {
      glyph[0] = '\'';
    } else if (len == 3 && c[0] == 0xE2 && c[1] == 0x80 &&
               (c[2] == 0x9C || c[2] == 0x9D)) {
      glyph[0] = '"';
    } else if (len == 3 && c[0] == 0xE2 && c[1] == 0x80 && c[2] == 0xA6) {
      if (out + 3 >= cap) break;
      memcpy(destination + out, "...", 3);
      out += 3;
      c += len;
      continue;
    } else if (len == 2 && c[0] == 0xC3 &&
               (c[1] == 0xA4 || c[1] == 0xA5 || c[1] == 0xB6 ||
                c[1] == 0x84 || c[1] == 0x85 || c[1] == 0x96)) {
      glyph[0] = 0xC3;
      glyph[1] = c[1] == 0xA4 ? 0x84 : c[1] == 0xA5 ? 0x85 :
                 c[1] == 0xB6 ? 0x96 : c[1];
      glyph_len = 2;
    }
    if (out + glyph_len >= cap) break;
    memcpy(destination + out, glyph, glyph_len);
    out += glyph_len;
    c += len;
  }
  destination[out] = '\0';
}
