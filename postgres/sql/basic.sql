-- Behaviour that does not need a checkpoint.
-- Make sure nothing loads lazily during this test.
SET laya.model_dir = '/nonexistent/laya';

SELECT laya_version() LIKE 'v%' AS versioned;
SELECT laya_backend() IS NULL AS unloaded;

-- Loading errors leave the session usable.
SELECT laya_load('/nonexistent/laya');
SELECT laya_load('/nonexistent/laya', '{"gpu": true}');
SELECT laya_load('/nonexistent/laya', '{"variant": "french"}');
SELECT laya_load('/nonexistent/laya', '{"variant": "multilingual"}');
SELECT laya_backend() IS NULL AS still_unloaded;

-- Strict functions return NULL for NULL arguments without running the model.
SELECT laya_noul(NULL, 'question') IS NULL AS noul_null;
SELECT laya_noul('state', NULL) IS NULL AS noul_null_instructions;
SELECT laya_noul('state', 'question', NULL) IS NULL AS noul_null_criteria;
SELECT laya_choice(NULL, 'question', '["a", "b"]') IS NULL AS choice_null;
SELECT laya_score('state', 'question', NULL) IS NULL AS score_null;
SELECT laya('state', NULL) IS NULL AS answers_null;

-- Request validation happens before any model is needed.
SELECT laya('state', '{}');
SELECT laya('state', '[1]');

-- Without a resident model, inference reports how to load one.
SELECT laya_noul('state', 'question');
SELECT laya_choice('{"subject": "hello"}'::jsonb, 'question', '["a", "b"]');
SELECT laya_score('state', 'question', '["low", "high"]');
SELECT laya('state', '{"q": {"type": "noul", "instructions": "question"}}');

-- Settings are validated and reserved under the laya prefix.
SET laya.options = '{"cuda": false}';
SHOW laya.options;
