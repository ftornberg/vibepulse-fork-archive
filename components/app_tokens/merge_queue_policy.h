#ifndef MERGE_QUEUE_POLICY_H
#define MERGE_QUEUE_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "merge_queue.h"

/* When the "ready to merge" card is up, and when it pulses.
 *
 * Kept out of the LVGL layer like the completion and Needs You policies:
 * pure, host-tested, no clock of its own. A pull request is known by its
 * project and number. The card comes up when one the person has not yet
 * dismissed appears, pulses (and so borrows the glass) for TK_MQ_PULSE_MS —
 * the same as a waiting card — and then sits static inside VibePulse until
 * it is dismissed or the queue empties. A new pull request while the card is
 * up starts a new pulse: new information earns a breath. */
#define TK_MQ_PULSE_MS 45000u
/* Dismissed pull requests remembered; more than the list can hold, so a
 * source that drops out and comes back does not resurrect the card. */
#define TK_MQ_ACK_CAP 16

typedef enum {
  TK_MQ_HIDDEN,
  TK_MQ_PULSE,
  TK_MQ_STATIC,
} tk_mq_phase;

typedef struct {
  tk_merge_queue queue;
  uint32_t acked[TK_MQ_ACK_CAP];
  uint8_t acked_count;
  uint32_t present[TK_MQ_LIST_CAP]; /* keys in the last applied list */
  uint8_t present_count;
  bool showing;
  uint64_t pulse_until_ms;
  int8_t lead; /* index in queue.prs of the pull request to show, or -1 */
} tk_mq_state;

void tk_mq_init(tk_mq_state *state);

/* Fold in the latest payload. Returns true when the card should pulse anew. */
bool tk_mq_apply(tk_mq_state *state, const tk_merge_queue *queue,
                 uint64_t now_ms);

/* The person tapped the card away: everything listed now counts as seen. */
void tk_mq_dismiss(tk_mq_state *state);

tk_mq_phase tk_mq_phase_at(const tk_mq_state *state, uint64_t now_ms);

/* The pull request the card leads with (the newest unseen one, else the
 * first listed), or NULL while the card is hidden. */
const tk_mq_pr *tk_mq_lead(const tk_mq_state *state);

uint32_t tk_mq_key(const tk_mq_pr *pr);

/* A pull request title in the card's detail face (plex_ui_14): ASCII in
 * upper case, å/ä/ö as Å/Ä/Ö, dashes, curly quotes and the ellipsis as what
 * the face carries, anything else as '?', control characters dropped. Never
 * a missing-glyph box on the glass. */
void tk_mq_title_label(const char *source, char *destination, size_t cap);

#endif
