-- Registers the decision_query loadable functions. Run as a user with the
-- INSERT privilege on the mysql system schema, once decision_query.so is in
-- the server's plugin_dir:  mysql -u root < mysql/install.sql
-- The registrations persist across restarts.
DROP FUNCTION IF EXISTS dq_version;
DROP FUNCTION IF EXISTS dq_backend;
DROP FUNCTION IF EXISTS dq_last_error;
DROP FUNCTION IF EXISTS dq_load;
DROP FUNCTION IF EXISTS noul;
DROP FUNCTION IF EXISTS choice;
DROP FUNCTION IF EXISTS score;
DROP FUNCTION IF EXISTS decide;

CREATE FUNCTION dq_version RETURNS STRING SONAME 'decision_query.so';
CREATE FUNCTION dq_backend RETURNS STRING SONAME 'decision_query.so';
CREATE FUNCTION dq_last_error RETURNS STRING SONAME 'decision_query.so';
CREATE FUNCTION dq_load RETURNS STRING SONAME 'decision_query.so';
CREATE FUNCTION noul RETURNS REAL SONAME 'decision_query.so';
CREATE FUNCTION choice RETURNS STRING SONAME 'decision_query.so';
CREATE FUNCTION score RETURNS REAL SONAME 'decision_query.so';
CREATE FUNCTION decide RETURNS STRING SONAME 'decision_query.so';
