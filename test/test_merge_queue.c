#include <stdio.h>
#include <string.h>

#include "../components/app_tokens/merge_queue_parse.h"
#include "../components/app_tokens/merge_queue_policy.h"

static int failures;

static void check(const char *what, int condition) {
  if (!condition) {
    printf("FAIL %s\n", what);
    failures++;
  }
}

static void rejected_unchanged(const char *what, const char *json) {
  tk_merge_queue value;
  memset(&value, 0xa5, sizeof value);
  tk_merge_queue before = value;
  if (tk_merge_queue_parse(json, strlen(json), &value)) {
    printf("FAIL %s accepted\n", what);
    failures++;
  }
  check(what, memcmp(&value, &before, sizeof value) == 0);
}

#define SOURCES \
  "\"sources\":[{\"project\":\"kvitt\",\"up\":true,\"paused\":false}," \
  "{\"project\":null,\"up\":false,\"paused\":false}]"

#define VALID \
  "{\"v\":1,\"enabled\":true,\"count\":3,\"incomplete\":false," SOURCES "," \
  "\"prs\":[{\"project\":\"kvitt\",\"number\":134,\"title\":\"feat: x\"}," \
  "{\"project\":\"gaffel\",\"number\":7,\"title\":null}]}"

static tk_merge_queue queue_of(int count, const char *const *projects,
                               const int *numbers, int listed,
                               int incomplete) {
  tk_merge_queue q;
  memset(&q, 0, sizeof q);
  q.enabled = true;
  q.incomplete = incomplete;
  q.count = count;
  q.pr_count = (uint8_t)listed;
  for (int i = 0; i < listed; i++) {
    snprintf(q.prs[i].project, sizeof q.prs[i].project, "%s", projects[i]);
    q.prs[i].number = numbers[i];
  }
  return q;
}

static void test_parse(void) {
  tk_merge_queue value;
  memset(&value, 0, sizeof value);
  check("valid parses", tk_merge_queue_parse(VALID, strlen(VALID), &value));
  check("valid values",
        value.enabled && !value.incomplete && value.count == 3 &&
        value.pr_count == 2 &&
        strcmp(value.prs[0].project, "kvitt") == 0 &&
        value.prs[0].number == 134 && value.prs[0].has_title &&
        strcmp(value.prs[0].title, "feat: x") == 0 &&
        strcmp(value.prs[1].project, "gaffel") == 0 &&
        !value.prs[1].has_title);

  const char *off = "{\"v\":1,\"enabled\":false}";
  memset(&value, 0xa5, sizeof value);
  check("disabled parses", tk_merge_queue_parse(off, strlen(off), &value));
  check("disabled is empty", !value.enabled && value.pr_count == 0);

  const char *empty =
      "{\"v\":1,\"enabled\":true,\"count\":0,\"incomplete\":true,"
      "\"sources\":[],\"prs\":[]}";
  check("empty incomplete parses",
        tk_merge_queue_parse(empty, strlen(empty), &value) &&
        value.enabled && value.incomplete && value.pr_count == 0);

  rejected_unchanged("wrong version",
      "{\"v\":2,\"enabled\":false}");
  rejected_unchanged("disabled with extra key",
      "{\"v\":1,\"enabled\":false,\"count\":0}");
  rejected_unchanged("unknown key",
      "{\"v\":1,\"enabled\":true,\"count\":0,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[],\"extra\":1}");
  rejected_unchanged("duplicate key",
      "{\"v\":1,\"v\":1,\"enabled\":false}");
  rejected_unchanged("count below the list",
      "{\"v\":1,\"enabled\":true,\"count\":0,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"k\",\"number\":1,"
      "\"title\":null}]}");
  rejected_unchanged("number zero",
      "{\"v\":1,\"enabled\":true,\"count\":1,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"k\",\"number\":0,"
      "\"title\":null}]}");
  rejected_unchanged("number as string",
      "{\"v\":1,\"enabled\":true,\"count\":1,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"k\",\"number\":\"1\","
      "\"title\":null}]}");
  rejected_unchanged("project with a slash",
      "{\"v\":1,\"enabled\":true,\"count\":1,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"o/k\",\"number\":1,"
      "\"title\":null}]}");
  rejected_unchanged("missing title key",
      "{\"v\":1,\"enabled\":true,\"count\":1,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"k\",\"number\":1}]}");
  rejected_unchanged("empty title string",
      "{\"v\":1,\"enabled\":true,\"count\":1,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"k\",\"number\":1,"
      "\"title\":\"\"}]}");
  rejected_unchanged("source with unknown key",
      "{\"v\":1,\"enabled\":true,\"count\":0,\"incomplete\":false,"
      "\"sources\":[{\"project\":\"k\",\"up\":true,\"paused\":false,"
      "\"x\":1}],\"prs\":[]}");
  rejected_unchanged("trailing garbage", VALID "x");
  rejected_unchanged("decoded NUL",
      "{\"v\":1,\"enabled\":true,\"count\":1,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[{\"project\":\"k\",\"number\":1,"
      "\"title\":\"a\\u0000b\"}]}");

  char nine[2048];
  int at = snprintf(nine, sizeof nine,
      "{\"v\":1,\"enabled\":true,\"count\":9,\"incomplete\":false,"
      "\"sources\":[],\"prs\":[");
  for (int i = 1; i <= 9; i++) {
    at += snprintf(nine + at, sizeof nine - (size_t)at,
                   "%s{\"project\":\"k\",\"number\":%d,\"title\":null}",
                   i > 1 ? "," : "", i);
  }
  snprintf(nine + at, sizeof nine - (size_t)at, "]}");
  rejected_unchanged("list over capacity", nine);
}

static void test_policy(void) {
  static const char *const p2[] = {"kvitt", "gaffel"};
  static const int n2[] = {134, 7};
  tk_mq_state s;
  tk_mq_init(&s);
  check("starts hidden", tk_mq_phase_at(&s, 0) == TK_MQ_HIDDEN &&
                         tk_mq_lead(&s) == NULL);

  tk_merge_queue q = queue_of(1, p2, n2, 1, 0);
  check("first PR pulses", tk_mq_apply(&s, &q, 1000));
  check("pulse phase", tk_mq_phase_at(&s, 1000) == TK_MQ_PULSE);
  check("pulse lasts the lease",
        tk_mq_phase_at(&s, 1000 + TK_MQ_PULSE_MS - 1) == TK_MQ_PULSE &&
        tk_mq_phase_at(&s, 1000 + TK_MQ_PULSE_MS) == TK_MQ_STATIC);
  check("lead is the PR", tk_mq_lead(&s) && tk_mq_lead(&s)->number == 134);

  check("same list again does not re-pulse",
        !tk_mq_apply(&s, &q, 50000) &&
        tk_mq_phase_at(&s, 50000) == TK_MQ_STATIC);

  tk_merge_queue two = queue_of(2, p2, n2, 2, 0);
  check("a new PR re-pulses", tk_mq_apply(&s, &two, 60000) &&
                              tk_mq_phase_at(&s, 60000) == TK_MQ_PULSE);

  tk_mq_dismiss(&s);
  check("dismiss hides", tk_mq_phase_at(&s, 60001) == TK_MQ_HIDDEN &&
                         tk_mq_lead(&s) == NULL);
  check("dismissed list stays down", !tk_mq_apply(&s, &two, 70000) &&
                                     tk_mq_phase_at(&s, 70000) ==
                                         TK_MQ_HIDDEN);

  /* An incomplete payload drops gaffel; its return is not news. */
  tk_merge_queue partial = queue_of(1, p2, n2, 1, 1);
  check("incomplete drop", !tk_mq_apply(&s, &partial, 80000));
  check("source back is not news", !tk_mq_apply(&s, &two, 90000) &&
                                   tk_mq_phase_at(&s, 90000) ==
                                       TK_MQ_HIDDEN);

  /* A complete list without gaffel #7: merged. Reopened later = news. */
  tk_merge_queue merged = queue_of(1, p2, n2, 1, 0);
  check("merged PR forgotten", !tk_mq_apply(&s, &merged, 100000));
  check("reopened PR pulses", tk_mq_apply(&s, &two, 110000));
  check("lead is the unseen one",
        tk_mq_lead(&s) && tk_mq_lead(&s)->number == 7);

  tk_merge_queue none = queue_of(0, p2, n2, 0, 0);
  check("empty queue hides", !tk_mq_apply(&s, &none, 120000) &&
                             tk_mq_phase_at(&s, 120000) == TK_MQ_HIDDEN);

  tk_merge_queue off;
  memset(&off, 0, sizeof off);
  tk_mq_init(&s);
  check("disabled never shows", !tk_mq_apply(&s, &off, 1) &&
                                tk_mq_phase_at(&s, 1) == TK_MQ_HIDDEN);

  /* The unseen PR merges while an already seen one stays: still up. */
  tk_mq_init(&s);
  tk_merge_queue first = queue_of(1, p2, n2, 1, 0);
  tk_mq_apply(&s, &first, 0);
  tk_mq_dismiss(&s);
  tk_mq_apply(&s, &two, 10);
  static const char *const only_kvitt[] = {"kvitt"};
  static const int only_134[] = {134};
  tk_merge_queue left = queue_of(1, only_kvitt, only_134, 1, 0);
  tk_mq_apply(&s, &left, 20);
  check("stays up leading with the first", tk_mq_lead(&s) &&
                                           tk_mq_lead(&s)->number == 134);

  tk_mq_pr a = {.number = 1}, b = {.number = 1};
  snprintf(a.project, sizeof a.project, "kvitt");
  snprintf(b.project, sizeof b.project, "gaffel");
  check("key separates projects", tk_mq_key(&a) != tk_mq_key(&b));
}

static void test_title_label(void) {
  char out[64];
  tk_mq_title_label("feat: visa medlemmar", out, sizeof out);
  check("ascii upper", strcmp(out, "FEAT: VISA MEDLEMMAR") == 0);
  tk_mq_title_label("fix: \xc3\xa5\xc3\xa4\xc3\xb6 \xc3\x96", out, sizeof out);
  check("swedish upper", strcmp(out, "FIX: \xc3\x85\xc3\x84\xc3\x96 \xc3\x96") == 0);
  tk_mq_title_label("emoji \xf0\x9f\x9a\x80 \xc3\xa9 x", out, sizeof out);
  check("undrawable becomes ?", strcmp(out, "EMOJI ? ? X") == 0);
  tk_mq_title_label("a\tb", out, sizeof out);
  check("control dropped", strcmp(out, "AB") == 0);
  tk_mq_title_label("a \xe2\x80\x94 b \xe2\x80\x9cq\xe2\x80\x9d it\xe2\x80\x99s\xe2\x80\xa6 \xc2\xb7",
                    out, sizeof out);
  check("typography mapped",
        strcmp(out, "A \xe2\x80\x93 B \"Q\" IT'S... \xc2\xb7") == 0);
  char tiny[4];
  tk_mq_title_label("\xc3\xa5\xc3\xa5", tiny, sizeof tiny);
  check("no split multibyte at cap", strcmp(tiny, "\xc3\x85") == 0);
}

int main(void) {
  test_title_label();
  test_parse();
  test_policy();
  if (failures) {
    printf("%d merge-queue failures\n", failures);
    return 1;
  }
  printf("OK: merge-queue parse and policy\n");
  return 0;
}
