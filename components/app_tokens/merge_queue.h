#ifndef MERGE_QUEUE_H
#define MERGE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

/* One strict /api/merge-queue payload from the tokenserver: pull requests
 * that local agent-team orchestrators have reviewed and that wait for a
 * person to merge them. The tokenserver lists at most eight; `count` covers
 * them all. `incomplete` = at least one orchestrator did not answer, so the
 * list is a lower bound, never a made-up zero. */
#define TK_MQ_LIST_CAP 8
#define TK_MQ_PROJECT_CAP 101
/* The tokenserver bounds titles to 80 characters; 4 bytes each is the UTF-8
 * worst case, plus the ellipsis it may append. */
#define TK_MQ_TITLE_CAP 324

typedef struct {
  char project[TK_MQ_PROJECT_CAP];
  int32_t number;
  bool has_title;
  char title[TK_MQ_TITLE_CAP];
} tk_mq_pr;

typedef struct {
  bool enabled;
  bool incomplete;
  int32_t count;
  uint8_t pr_count;
  tk_mq_pr prs[TK_MQ_LIST_CAP];
} tk_merge_queue;

#endif
