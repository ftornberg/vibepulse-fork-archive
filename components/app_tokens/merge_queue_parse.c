#include "merge_queue_parse.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../../third_party/cjson/cJSON.h"

/* The same lexical and structural guards as github_status_parse.c: cJSON
 * would otherwise accept a decoded NUL, trailing garbage and duplicate keys. */
static bool whitespace(unsigned char byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static bool lexical_guard(const char *json, size_t len) {
  bool in_string = false;
  bool escaped = false;
  for (size_t i = 0; i < len; i++) {
    unsigned char byte = (unsigned char)json[i];
    if (!in_string) {
      if (byte == '"') in_string = true;
      else if (byte < 0x20 && !whitespace(byte)) return false;
      continue;
    }
    if (escaped) {
      escaped = false;
      if (byte == 'u' && i + 4 < len &&
          json[i + 1] == '0' && json[i + 2] == '0' &&
          json[i + 3] == '0' && json[i + 4] == '0') {
        return false;
      }
    } else if (byte == '\\') {
      escaped = true;
    } else if (byte == '"') {
      in_string = false;
    } else if (byte < 0x20) {
      return false;
    }
  }
  return !in_string && !escaped;
}

static bool trailing_whitespace(const char *json, size_t len,
                                const char *parse_end) {
  if (!parse_end || parse_end < json || parse_end > json + len) return false;
  for (const char *cursor = parse_end; cursor < json + len; cursor++) {
    if (!whitespace((unsigned char)*cursor)) return false;
  }
  return true;
}

static bool allowed_name(const char *name, const char *const *allowed) {
  if (!name) return false;
  for (size_t i = 0; allowed[i]; i++) {
    if (strcmp(name, allowed[i]) == 0) return true;
  }
  return false;
}

static bool exact_unique_fields(const cJSON *object,
                                const char *const *allowed) {
  if (!cJSON_IsObject(object)) return false;
  for (const cJSON *item = object->child; item; item = item->next) {
    if (!allowed_name(item->string, allowed)) return false;
    for (const cJSON *other = item->next; other; other = other->next) {
      if (other->string && strcmp(item->string, other->string) == 0) {
        return false;
      }
    }
  }
  return true;
}

static bool exact_int32(const cJSON *item, int32_t *out) {
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
      item->valuedouble < 0 || item->valuedouble > INT32_MAX ||
      floor(item->valuedouble) != item->valuedouble) {
    return false;
  }
  *out = (int32_t)item->valuedouble;
  return true;
}

static bool copy_string(const cJSON *item, char *out, size_t cap) {
  if (!cJSON_IsString(item) || !item->valuestring) return false;
  size_t len = strlen(item->valuestring);
  if (len == 0 || len >= cap) return false;
  memcpy(out, item->valuestring, len + 1);
  return true;
}

/* A project is a repository basename, exactly as GitHub allows it. */
static bool valid_project(const char *value) {
  for (const char *cursor = value; *cursor; cursor++) {
    unsigned char ch = (unsigned char)*cursor;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
      return false;
    }
  }
  return true;
}

static bool parse_pr(const cJSON *item, tk_mq_pr *out) {
  static const char *const allowed[] = {"project", "number", "title", NULL};
  if (!exact_unique_fields(item, allowed)) return false;
  const cJSON *project = cJSON_GetObjectItemCaseSensitive(item, "project");
  const cJSON *number = cJSON_GetObjectItemCaseSensitive(item, "number");
  const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
  if (!copy_string(project, out->project, sizeof out->project) ||
      !valid_project(out->project) ||
      !exact_int32(number, &out->number) || out->number == 0 || !title) {
    return false;
  }
  if (cJSON_IsNull(title)) return true;
  out->has_title = copy_string(title, out->title, sizeof out->title);
  return out->has_title;
}

static bool parse_source(const cJSON *item) {
  static const char *const allowed[] = {"project", "up", "paused", NULL};
  if (!exact_unique_fields(item, allowed)) return false;
  const cJSON *project = cJSON_GetObjectItemCaseSensitive(item, "project");
  return (cJSON_IsNull(project) || cJSON_IsString(project)) &&
         cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(item, "up")) &&
         cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(item, "paused"));
}

bool tk_merge_queue_parse(const char *json, size_t len, tk_merge_queue *out) {
  if (!json || !out || len == 0 || !lexical_guard(json, len)) return false;

  const char *parse_end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts(json, len, &parse_end, false);
  if (!root || !trailing_whitespace(json, len, parse_end)) {
    cJSON_Delete(root);
    return false;
  }

  static const char *const allowed[] = {
      "v", "enabled", "count", "incomplete", "sources", "prs", NULL,
  };
  tk_merge_queue parsed;
  memset(&parsed, 0, sizeof parsed);
  const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
  const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
  int32_t version_value = 0;
  bool ok = exact_unique_fields(root, allowed) &&
            exact_int32(version, &version_value) && version_value == 1 &&
            cJSON_IsBool(enabled);

  if (ok && !cJSON_IsTrue(enabled)) {
    /* Disabled is exactly {"v": 1, "enabled": false}. */
    ok = root->child && root->child->next &&
         root->child->next->next == NULL;
  } else if (ok) {
    parsed.enabled = true;
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "count");
    const cJSON *incomplete =
        cJSON_GetObjectItemCaseSensitive(root, "incomplete");
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(root, "sources");
    const cJSON *prs = cJSON_GetObjectItemCaseSensitive(root, "prs");
    ok = exact_int32(count, &parsed.count) && cJSON_IsBool(incomplete) &&
         cJSON_IsArray(sources) && cJSON_IsArray(prs);
    parsed.incomplete = cJSON_IsTrue(incomplete);
    if (ok) {
      for (const cJSON *source = sources->child; ok && source;
           source = source->next) {
        ok = parse_source(source);
      }
    }
    for (const cJSON *pr = ok ? prs->child : NULL; ok && pr; pr = pr->next) {
      ok = parsed.pr_count < TK_MQ_LIST_CAP &&
           parse_pr(pr, &parsed.prs[parsed.pr_count]);
      if (ok) parsed.pr_count++;
    }
    ok = ok && parsed.count >= parsed.pr_count;
  }

  cJSON_Delete(root);
  if (!ok) return false;
  *out = parsed;
  return true;
}
