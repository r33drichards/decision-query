-- Behaviour that does not need a checkpoint.
-- Make sure nothing loads lazily during this test.
SET decision_query.model_dir = '/nonexistent/laya';

SELECT dq_version() LIKE 'v%' AS versioned;
SELECT dq_backend() IS NULL AS unloaded;

-- Loading errors leave the session usable.
SELECT dq_load('/nonexistent/laya');
SELECT dq_load('/nonexistent/laya', '{"gpu": true}');
SELECT dq_load('/nonexistent/laya', '{"variant": "french"}');
SELECT dq_load('/nonexistent/laya', '{"variant": "multilingual"}');
SELECT dq_backend() IS NULL AS still_unloaded;

-- Strict functions return NULL for NULL arguments without running the model.
SELECT noul(NULL, 'question') IS NULL AS noul_null;
SELECT noul('state', NULL) IS NULL AS noul_null_instructions;
SELECT noul('state', 'question', NULL) IS NULL AS noul_null_criteria;
SELECT choice(NULL, 'question', '["a", "b"]') IS NULL AS choice_null;
SELECT score('state', 'question', NULL) IS NULL AS score_null;
SELECT laya('state', NULL) IS NULL AS answers_null;

-- Request validation happens before any model is needed.
SELECT laya('state', '{}');
SELECT laya('state', '[1]');

-- Without a resident model, inference reports how to load one.
SELECT noul('state', 'question');
SELECT choice('{"subject": "hello"}'::jsonb, 'question', '["a", "b"]');
SELECT score('state', 'question', '["low", "high"]');
SELECT laya('state', '{"q": {"type": "noul", "instructions": "question"}}');

-- Settings are validated and reserved under the laya prefix.
SET decision_query.options = '{"cuda": false}';
SHOW decision_query.options;
