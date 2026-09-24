#!/usr/bin/env python3
"""Tests for the MySQL loadable functions against a throwaway mysqld.

Initializes a temporary data directory, starts mysqld on a private socket with
its plugin_dir pointing at the build tree, registers the functions with
mysql/install.sql and drives them through the mysql client. Nothing is
installed and no running server is touched.

Without --model-dir it checks everything that needs no checkpoint. With one,
the server gets DQ_MODEL_DIR (and DQ_OPTIONS) in its environment and the
model-backed checks run too.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
failures, checks = [], 0


def check(name, ok, detail=""):
    global checks
    checks += 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  -- ' + str(detail) if detail != '' else ''}")
    if not ok:
        failures.append(f"{name}: {detail}")


class Server:
    def __init__(self, mysqld, plugin_dir, env):
        self.tmp = tempfile.mkdtemp(prefix="dq-mysql-")
        self.datadir = os.path.join(self.tmp, "data")
        self.socket = os.path.join(self.tmp, "mysql.sock")
        self.log = os.path.join(self.tmp, "error.log")
        user = ["--user=root"] if os.geteuid() == 0 else []
        subprocess.run([mysqld, "--no-defaults", "--initialize-insecure",
                        f"--datadir={self.datadir}", f"--log-error={self.log}", *user],
                       check=True)
        self.process = subprocess.Popen(
            [mysqld, "--no-defaults", f"--datadir={self.datadir}", f"--socket={self.socket}",
             f"--pid-file={os.path.join(self.tmp, 'mysqld.pid')}", f"--log-error={self.log}",
             f"--plugin-dir={plugin_dir}", "--skip-networking", "--mysqlx=OFF",
             "--secure-file-priv=", *user],
            env=env)
        deadline = time.time() + 120
        while time.time() < deadline:
            if self.process.poll() is not None:
                self.dump_log()
                sys.exit(f"mysqld exited {self.process.returncode} during startup")
            ping = subprocess.run(["mysqladmin", "--no-defaults", "-uroot",
                                   f"--socket={self.socket}", "ping"], capture_output=True)
            if ping.returncode == 0:
                subprocess.run(["mysql", "--no-defaults", "-uroot", f"--socket={self.socket}",
                                "-e", "create database test"], check=True)
                return
            time.sleep(0.5)
        self.dump_log()
        sys.exit("mysqld did not start within two minutes")

    def dump_log(self):
        if os.path.exists(self.log):
            with open(self.log) as f:
                print(f.read(), file=sys.stderr)

    def run(self, sql):
        """Runs SQL in one session; returns (returncode, stdout rows, stderr)."""
        out = subprocess.run(["mysql", "--no-defaults", "-uroot", f"--socket={self.socket}",
                              "--batch", "--raw", "--skip-column-names", "test"],
                             input=sql, text=True, capture_output=True, timeout=900)
        rows = [line for line in out.stdout.splitlines()]
        return out.returncode, rows, out.stderr.strip()

    def query(self, sql):
        """Runs SQL that must succeed and returns its output rows."""
        code, rows, err = self.run(sql)
        if code != 0:
            raise RuntimeError(f"{sql.strip()}\n{err}")
        return rows

    def value(self, sql):
        rows = self.query(sql)
        return rows[0] if rows else None

    def error(self, sql):
        """Runs SQL that must fail and returns the client's error text."""
        code, _, err = self.run(sql)
        return err if code != 0 else None

    def stop(self):
        subprocess.run(["mysqladmin", "--no-defaults", "-uroot", f"--socket={self.socket}",
                        "shutdown"], capture_output=True)
        try:
            self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)


def sql_string(text):
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


def basic(server):
    """Behaviour that does not need a checkpoint."""
    functions = server.query("select name, ret from mysql.func where dl = 'decision_query.so' "
                             "order by name;")
    check("all functions registered",
          functions == ["choice\t0", "decide\t0", "dq_backend\t0", "dq_last_error\t0",
                        "dq_load\t0", "dq_version\t0", "noul\t1", "score\t1"], functions)

    with open(os.path.join(ROOT, "VERSION")) as f:
        version = "v" + f.read().strip()
    check("dq_version reports VERSION", server.value("select dq_version();") == version,
          server.value("select dq_version();"))
    check("dq_backend is NULL before a load", server.value("select dq_backend();") == "NULL")
    check("dq_last_error is NULL before a failure",
          server.value("select dq_last_error();") == "NULL")

    # Loading errors are reported as SQL errors and leave the server usable.
    err = server.error("select dq_load('/nonexistent/checkpoint');")
    check("missing checkpoint is an error",
          err and "Not a checkpoint directory: /nonexistent/checkpoint" in err, err)
    err = server.error("""select dq_load('/nonexistent/checkpoint', '{"gpu": true}');""")
    check("unknown option is an error", err and "Unknown option: gpu" in err, err)
    err = server.error("""select dq_load('/nonexistent/checkpoint', '{"variant": "french"}');""")
    check("unknown variant is an error", err and "Unknown model variant: french" in err, err)
    err = server.error("""select dq_load('/nonexistent/checkpoint', '{"variant": "multilingual"}');""")
    check("variant is appended to the directory",
          err and "/nonexistent/checkpoint/multilingual" in err, err)
    err = server.error("select dq_load('/nonexistent/checkpoint', 'not json');")
    check("options must be JSON", err and "dq_load options must be valid JSON" in err, err)
    err = server.error("""select dq_load('http://127.0.0.1:9/', '{"key_file": "/etc/passwd"}');""")
    check("key_file is refused from SQL", err and "key_file is not accepted" in err, err)
    err = server.error("select dq_load();")
    check("dq_load arity is checked", err and "dq_load() takes 1 or 2 arguments" in err, err)
    check("dq_backend is still NULL", server.value("select dq_backend();") == "NULL")

    # A non-constant bad directory can only fail per row: NULL plus dq_last_error.
    rows = server.query("create table dirs(d text); insert into dirs values ('/nonexistent/row');"
                        "select dq_load(d) from dirs; select dq_last_error(); drop table dirs;")
    check("row-level load failure returns NULL", rows[:1] == ["NULL"], rows)
    check("row-level failure is kept for dq_last_error",
          len(rows) > 1 and "dq_load: Not a checkpoint directory: /nonexistent/row" in rows[1], rows)

    # Arity and constant JSON arguments are checked up front.
    err = server.error("select noul('state');")
    check("noul arity is checked", err and "noul() takes 2 or 3 arguments" in err, err)
    err = server.error("select choice('state', 'question');")
    check("choice arity is checked", err and "choice() takes 3 arguments" in err, err)
    err = server.error("select choice('state', 'question', '[\"a\",');")
    check("constant criteria must be JSON", err and "choice criteria must be valid JSON" in err, err)
    err = server.error("select decide('state', '{}');")
    check("decide needs a nonempty object",
          err and "questions must be a nonempty JSON object" in err, err)
    err = server.error("select decide('state', '[1]');")
    check("decide rejects an array", err and "questions must be a nonempty JSON object" in err, err)

    # Without a resident model, inference reports how to load one.
    for sql in ("select noul('state', 'question');",
                """select choice(json_object('subject', 'hello'), 'question', '["a", "b"]');""",
                """select score('state', 'question', '["low", "high"]');""",
                """select decide('state', '{"q": {"type": "noul", "instructions": "question"}}');"""):
        err = server.error(sql)
        check(f"no model: {sql[7:40]}...",
              err and "No decision backend loaded; call dq_load(dir_or_url) or set DQ_MODEL_DIR"
              in err, err)

    # Functions stay registered and callable after the errors.
    check("server still answers", server.value("select dq_version();") == version)


def with_model(server, model_dir, options):
    """Model-backed checks; the server has DQ_MODEL_DIR in its environment."""
    body = "I was charged twice. Please refund the extra charge today."
    departments = json.dumps({"billing": "payments and refunds",
                              "technical": "bugs and outages", "sales": "new contracts"})

    # Every statement below runs in one session so the lazy load is observed.
    rows = server.query(
        "select dq_backend() is null;"
        "select noul('Please refund the duplicate charge.', 'Does the customer ask for a refund?');"
        "select dq_backend() is not null;")
    check("unloaded before the first call", rows[0] == "1", rows)
    check("first inference loads lazily from DQ_MODEL_DIR", float(rows[1]) > 0.5, rows[1])
    check("backend reported after the lazy load", rows[2] == "1", rows)

    backend = server.value("select dq_backend();")
    loaded = server.value(f"select dq_load({sql_string(model_dir)}, {sql_string(options)});")
    check("dq_load returns the backend name", loaded == backend, f"{loaded} vs {backend}")

    complaint = float(server.value(
        "select noul('The service works well. Thank you!', 'Is this a complaint?');"))
    check("praise is not a complaint", complaint < 0.5, complaint)
    p = float(server.value(
        f"select noul({sql_string(body)}, 'Does the customer request a refund?', "
        """'{"true": "a refund is requested", "false": "no refund is requested"}');"""))
    check("noul with criteria", p > 0.5, p)
    check("probabilities are in [0,1]", 0.0 <= p <= 1.0, p)

    c = server.value(f"select choice({sql_string(body)}, 'Which department should handle this?', "
                     f"{sql_string(departments)});")
    check("choice with an object of descriptions", c == "billing", c)
    c = server.value(f"select choice({sql_string(body)}, 'Which department should handle this?', "
                     """'["billing", "technical", "sales"]');""")
    check("choice with an array of names", c == "billing", c)
    c = server.value("select choice(json_object('subject', 'Duplicate invoice', 'body', "
                     f"{sql_string(body)}), 'Which department should handle this?', "
                     f"{sql_string(departments)});")
    check("choice with a JSON_OBJECT state", c == "billing", c)

    s = float(server.value(f"select score({sql_string(body)}, 'How urgent is the request?', "
                           """json_array('not urgent', 'soon', 'immediate'));"""))
    check("score stays inside the rubric", 0.0 <= s <= 2.0, s)
    destructive = float(server.value(
        "select score('rm -rf --no-preserve-root /', 'How destructive is this command?', "
        "json_array('harmless, reads only','changes local state','destroys data irreversibly'));"))
    harmless = float(server.value(
        "select score('ls -la', 'How destructive is this command?', "
        "json_array('harmless, reads only','changes local state','destroys data irreversibly'));"))
    check("rm -rf outranks ls on a destructiveness rubric", destructive > harmless,
          f"{destructive} vs {harmless}")

    questions = json.dumps({
        "department": {"type": "choice", "instructions": "Which department should handle this?",
                       "criteria": json.loads(departments)},
        "urgency": {"type": "score", "instructions": "How urgent is the request?",
                    "criteria": ["not urgent", "soon", "immediate"]},
        "refund": {"type": "noul", "instructions": "Does the customer request a refund?"},
    })
    # The result is utf8mb4 text, so MySQL's JSON functions take it directly.
    rows = server.query(
        f"set @a = decide(json_object('subject', 'Duplicate invoice', 'body', {sql_string(body)}), "
        f"{sql_string(questions)});"
        "select json_valid(@a);"
        "select json_unquote(json_extract(@a, '$.department.choice'));"
        "select json_extract(@a, '$.department.probabilities.billing');"
        "select json_extract(@a, '$.refund.noul');"
        "select json_extract(@a, '$.urgency.score');")
    check("decide returns valid JSON", rows[0] == "1", rows)
    check("decide: department", rows[1] == "billing", rows[1])
    check("decide: billing probability", float(rows[2]) > 0.5, rows[2])
    check("decide: refund", float(rows[3]) > 0.5, rows[3])
    check("decide: urgency in range", 0.0 <= float(rows[4]) <= 2.0, rows[4])

    rows = server.query(
        "create table tickets(id int primary key, body text);"
        "insert into tickets values "
        "(1, 'I was charged twice. Please refund the extra charge today.'),"
        "(2, 'The service works well. Thank you!'),"
        "(3, 'The login page returns a 500 error since this morning.'),"
        "(4, NULL);"
        "select id from tickets where noul(body, 'Does the customer request a refund?') > 0.5 "
        "order by id;"
        "select '--';"
        "select id, choice(body, 'Which department should handle this?', "
        """'["billing", "technical", "sales"]') from tickets where id in (1, 3, 4) order by id;"""
        "select '--';"
        "select id, json_contains_path(decide(body, '{\"refund\": {\"type\": \"noul\", "
        "\"instructions\": \"Does the customer request a refund?\"}}'), 'one', '$.refund.noul') "
        "from tickets where id < 4 order by id;"
        "drop table tickets;")
    split = rows.index("--")
    check("filter a table with noul", rows[:split] == ["1"], rows[:split])
    rest = rows[split + 1:]
    split2 = rest.index("--")
    check("route a table with choice; NULL state yields NULL",
          rest[:split2] == ["1\tbilling", "3\ttechnical", "4\tNULL"], rest[:split2])
    check("decide per row", rest[split2 + 1:] == ["1\t1", "2\t1", "3\t1"], rest[split2 + 1:])

    # A criteria column that is not JSON fails per row: NULL plus dq_last_error.
    rows = server.query(
        "create table crit(c text); insert into crit values ('not json');"
        "select choice('state', 'question', c) from crit; select dq_last_error(); drop table crit;")
    check("bad criteria column yields NULL", rows[:1] == ["NULL"], rows)
    check("and is kept for dq_last_error",
          len(rows) > 1 and "choice: choice criteria must be valid JSON" in rows[1], rows)

    # Non-ASCII text survives the trip through a latin1 column.
    c = server.value(
        "create table l1(body text character set latin1);"
        "insert into l1 values ('Je veux être remboursé, j''ai été débité deux fois.');"
        "select choice(body, 'Which department should handle this?', "
        f"{sql_string(departments)}) from l1; drop table l1;")
    check("latin1 column is converted to UTF-8", c == "billing", c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-dir", default=os.path.join(ROOT, "build", "mysql"),
                    help="directory holding decision_query.so")
    ap.add_argument("--mysqld", default=shutil.which("mysqld") or "/usr/sbin/mysqld")
    ap.add_argument("--model-dir", default=os.environ.get("DQ_MODEL_DIR", ""))
    ap.add_argument("--options", default=os.environ.get("DQ_OPTIONS", '{"cuda": false}'))
    a = ap.parse_args()

    plugin_dir = os.path.abspath(a.plugin_dir)
    if not os.path.exists(os.path.join(plugin_dir, "decision_query.so")):
        sys.exit(f"{plugin_dir}/decision_query.so not found; build it with `make mysql`")

    env = {k: v for k, v in os.environ.items()
           if k not in ("DQ_MODEL_DIR", "DQ_OPTIONS", "DQ_API_KEY", "DQ_API_KEY_FILE")}
    model_dir = os.path.abspath(a.model_dir) if a.model_dir else ""

    # The basic checks need an empty environment so nothing loads lazily.
    server = Server(a.mysqld, plugin_dir, {**env, "DQ_MODEL_DIR": "/nonexistent/checkpoint"})
    try:
        with open(os.path.join(ROOT, "mysql", "install.sql")) as f:
            server.query(f.read())
        basic(server)
    finally:
        server.stop()

    if model_dir:
        server = Server(a.mysqld, plugin_dir,
                        {**env, "DQ_MODEL_DIR": model_dir, "DQ_OPTIONS": a.options})
        try:
            with open(os.path.join(ROOT, "mysql", "install.sql")) as f:
                server.query(f.read())
            with_model(server, model_dir, a.options)
        except Exception:
            server.dump_log()
            raise
        finally:
            server.stop()
    else:
        print("\nDQ_MODEL_DIR not set; skipped the model-backed checks")

    print(f"\n{checks - len(failures)}/{checks} checks passed")
    if failures:
        print("\nFAILURES:")
        for f in failures:
            print(" -", f)
        sys.exit(1)


if __name__ == "__main__":
    main()
