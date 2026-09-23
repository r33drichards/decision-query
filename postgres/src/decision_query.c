/* PostgreSQL entry points for the laya extension.
 *
 * Every backend that calls an inference function loads its own copy of the
 * checkpoint, from dq_load() or lazily from the decision_query.model_dir setting. */
#include <postgres.h>
#include <fmgr.h>

#include <catalog/pg_type_d.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/jsonb.h>

#include "bridge.h"

PG_MODULE_MAGIC;

static char *model_dir_setting;
static char *options_setting;
static char *api_key_file_setting;

void _PG_init(void);

void _PG_init(void) {
  DefineCustomStringVariable(
      "decision_query.model_dir", "Checkpoint directory loaded on first use.",
      "Falls back to the DQ_MODEL_DIR environment variable, then "
      "models/laya.",
      &model_dir_setting, "", PGC_USERSET, 0, NULL, NULL, NULL);
  DefineCustomStringVariable(
      "decision_query.options",
      "JSON options applied when a checkpoint loads on first use.",
      "Keys: variant, cuda, bf16, flash, tensor_core. Falls back to "
      "DQ_OPTIONS.",
      &options_setting, "", PGC_USERSET, 0, NULL, NULL, NULL);
  // A path, not the key itself: a GUC holding the secret would be visible in
  // pg_settings and in any config dump, and `SET` would put it in
  // pg_stat_activity and the statement log. SUPERUSER_ONLY because any role
  // that can name the file can make the extension read it.
  DefineCustomStringVariable(
      "decision_query.api_key_file",
      "File holding the bearer token for an HTTP decision endpoint.",
      "Read once per backend when a URL endpoint loads. Prefer this to the "
      "key option, which would place the secret in SQL text. Falls back to "
      "the DQ_API_KEY environment variable, which is cluster-wide and "
      "therefore not per-role.",
      &api_key_file_setting, "", PGC_SUSET, GUC_SUPERUSER_ONLY, NULL, NULL,
      NULL);
#if PG_VERSION_NUM >= 150000
  MarkGUCPrefixReserved("decide");
#else
  EmitWarningsOnPlaceholders("decide");
#endif
}

/* Reports a bridge error, releasing the malloc'd message first. */
static void raise_error(char *message) {
  char *copy = pstrdup(message ? message : "Unknown Laya error");
  pgdq_free(message);
  ereport(ERROR,
          (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), errmsg("%s", copy)));
}

static char *jsonb_argument(FunctionCallInfo fcinfo, int index) {
  Jsonb *value = PG_GETARG_JSONB_P(index);
  return JsonbToCString(NULL, &value->root, VARSIZE(value));
}

/* A jsonb argument is passed as structured JSON; text is passed as is. */
static char *state_argument(FunctionCallInfo fcinfo, int index, int *is_json) {
  if (get_fn_expr_argtype(fcinfo->flinfo, index) == JSONBOID) {
    *is_json = 1;
    return jsonb_argument(fcinfo, index);
  }
  *is_json = 0;
  return text_to_cstring(PG_GETARG_TEXT_PP(index));
}

static text *take_text(char *value) {
  text *result = cstring_to_text(value);
  pgdq_free(value);
  return result;
}

static pgdq_response evaluate(pgdq_request *request) {
  pgdq_response response;
  request->model_dir = model_dir_setting;
  request->options = options_setting;
  if (pgdq_evaluate(request, &response) != 0)
    raise_error(response.error);
  return response;
}

static pgdq_response evaluate_question(FunctionCallInfo fcinfo,
                                         const char *type) {
  pgdq_request request = {0};
  request.type = type;
  request.state = state_argument(fcinfo, 0, &request.state_is_json);
  request.instructions =
      state_argument(fcinfo, 1, &request.instructions_is_json);
  if (PG_NARGS() > 2)
    request.criteria = jsonb_argument(fcinfo, 2);
  return evaluate(&request);
}

PG_FUNCTION_INFO_V1(dq_version);

Datum dq_version(PG_FUNCTION_ARGS) {
  PG_RETURN_TEXT_P(cstring_to_text(PGDQ_VERSION));
}

PG_FUNCTION_INFO_V1(dq_backend);

Datum dq_backend(PG_FUNCTION_ARGS) {
  char *backend = pgdq_backend();
  if (backend == NULL)
    PG_RETURN_NULL();
  PG_RETURN_TEXT_P(take_text(backend));
}

PG_FUNCTION_INFO_V1(dq_load);

Datum dq_load(PG_FUNCTION_ARGS) {
  char *directory = text_to_cstring(PG_GETARG_TEXT_PP(0));
  char *options = PG_NARGS() > 1 ? jsonb_argument(fcinfo, 1) : NULL;
  char *backend = NULL;
  char *error = NULL;
  if (pgdq_load(directory, options, &backend, &error) != 0)
    raise_error(error);
  PG_RETURN_TEXT_P(take_text(backend));
}

PG_FUNCTION_INFO_V1(noul);

Datum noul(PG_FUNCTION_ARGS) {
  pgdq_response response = evaluate_question(fcinfo, "noul");
  PG_RETURN_FLOAT8(response.number);
}

PG_FUNCTION_INFO_V1(score);

Datum score(PG_FUNCTION_ARGS) {
  pgdq_response response = evaluate_question(fcinfo, "score");
  PG_RETURN_FLOAT8(response.number);
}

PG_FUNCTION_INFO_V1(choice);

Datum choice(PG_FUNCTION_ARGS) {
  pgdq_response response = evaluate_question(fcinfo, "choice");
  PG_RETURN_TEXT_P(take_text(response.text));
}

PG_FUNCTION_INFO_V1(decide_answers);

Datum decide_answers(PG_FUNCTION_ARGS) {
  pgdq_request request = {0};
  pgdq_response response;
  char *answers;
  request.state = state_argument(fcinfo, 0, &request.state_is_json);
  request.questions = jsonb_argument(fcinfo, 1);
  response = evaluate(&request);
  answers = pstrdup(response.text);
  pgdq_free(response.text);
  PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(answers)));
}
