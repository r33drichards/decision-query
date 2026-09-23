-- Laya typed decisions as SQL functions.
\echo Use "CREATE EXTENSION laya" to load this file. \quit

CREATE FUNCTION dq_version()
    RETURNS text
    AS 'MODULE_PATHNAME', 'dq_version'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION dq_backend()
    RETURNS text
    AS 'MODULE_PATHNAME', 'dq_backend'
    LANGUAGE C VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION dq_load(directory text)
    RETURNS text
    AS 'MODULE_PATHNAME', 'dq_load'
    LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;

CREATE FUNCTION dq_load(directory text, options jsonb)
    RETURNS text
    AS 'MODULE_PATHNAME', 'dq_load'
    LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;

-- Inference functions accept the state as text or as jsonb (structured state).
-- They are stable rather than immutable because the resident model can change.
-- The high cost makes the planner evaluate cheaper predicates first.

CREATE FUNCTION noul(state text, instructions text)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION noul(state jsonb, instructions text)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION noul(state text, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION noul(state jsonb, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'noul'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION choice(state text, instructions text, criteria jsonb)
    RETURNS text
    AS 'MODULE_PATHNAME', 'choice'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION choice(state jsonb, instructions text, criteria jsonb)
    RETURNS text
    AS 'MODULE_PATHNAME', 'choice'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION score(state text, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'score'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION score(state jsonb, instructions text, criteria jsonb)
    RETURNS float8
    AS 'MODULE_PATHNAME', 'score'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION decide(state text, questions jsonb)
    RETURNS jsonb
    AS 'MODULE_PATHNAME', 'decide_answers'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;

CREATE FUNCTION decide(state jsonb, questions jsonb)
    RETURNS jsonb
    AS 'MODULE_PATHNAME', 'decide_answers'
    LANGUAGE C STABLE STRICT PARALLEL RESTRICTED COST 10000;
