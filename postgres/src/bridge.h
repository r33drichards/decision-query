/* C interface between the PostgreSQL entry points and the C++ laya runtime.
 *
 * No exception crosses this boundary and no PostgreSQL error is raised behind
 * it; failures come back as malloc'd messages that the caller reports. */
#ifndef PGLAYA_BRIDGE_H
#define PGLAYA_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pgdq_request {
  /* Checkpoint directory and JSON options used when no model is resident.
   * Either may be NULL or empty; the environment is consulted next. */
  const char *model_dir;
  const char *options;
  const char *state;
  int state_is_json;
  /* "noul", "choice" or "score" for one question; NULL evaluates `questions`.
   */
  const char *type;
  const char *instructions;
  int instructions_is_json;
  const char *criteria; /* JSON text or NULL. */
  const char
      *questions; /* JSON text of a questions object when type is NULL. */
} pgdq_request;

typedef struct pgdq_response {
  double number; /* noul probability or expected score */
  char *text;    /* choice name or answers JSON */
  char *error;   /* message when the call fails */
} pgdq_response;

/* Returns 0 on success; response strings are malloc'd and freed with
 * pgdq_free. */
int pgdq_evaluate(const pgdq_request *request, pgdq_response *response);

/* Loads a checkpoint; options_json may be NULL. Returns 0 and the backend name
 * on success, otherwise a nonzero value and an error message. */
int pgdq_load(const char *directory, const char *options_json, char **backend,
                char **error);

/* Backend name of the resident model, or NULL when none is loaded. */
char *pgdq_backend(void);

void pgdq_free(char *pointer);

#ifdef __cplusplus
}
#endif

#endif /* PGLAYA_BRIDGE_H */
