-- Laya typed decisions as SQL functions.
\echo Use "CREATE EXTENSION laya" to load this file. \quit

CREATE FUNCTION laya_version()
    RETURNS text
    AS 'MODULE_PATHNAME', 'laya_version'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION laya_backend()
    RETURNS text
    AS 'MODULE_PATHNAME', 'laya_backend'
    LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION laya_load(directory text)
    RETURNS text
    AS 'MODULE_PATHNAME', 'laya_load'
    LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;

CREATE FUNCTION laya_load(directory text, options jsonb)
    RETURNS text
    AS 'MODULE_PATHNAME', 'laya_load'
    LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;

-- Inference functions accept the state as text or as jsonb (structured state).
-- They are stable rather than immutable because the resident model can change.
-- The high cost makes the planner evaluate cheaper predicates first.

CREATE FUNCTION laya_noul(state text, instructions text)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'laya_noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_noul(state jsonb, instructions text)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'laya_noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_noul(state text, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'laya_noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_noul(state jsonb, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'laya_noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_choice(state text, instructions text, criteria jsonb)
    RETURNS text
    AS 'MODULE_PATHNAME', 'laya_choice'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_choice(state jsonb, instructions text, criteria jsonb)
    RETURNS text
    AS 'MODULE_PATHNAME', 'laya_choice'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_score(state text, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'laya_score'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya_score(state jsonb, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'laya_score'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya(state text, questions jsonb)
    RETURNS jsonb
    AS 'MODULE_PATHNAME', 'laya_answers'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION laya(state jsonb, questions jsonb)
    RETURNS jsonb
    AS 'MODULE_PATHNAME', 'laya_answers'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;
