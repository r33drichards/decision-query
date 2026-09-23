import json
import os
import sqlite3
import subprocess
import sys
import unittest

EXT_PATH = "./dist/debug/decision_query"
MODEL_DIR = os.environ.get("DQ_MODEL_DIR")
DQ_OPTIONS = os.environ.get("DQ_OPTIONS", "{}")
CLI_PATH = os.environ.get("LAYA_CLI", "./build/bin/laya-cli")
CLI_FLAGS = os.environ.get("LAYA_CLI_FLAGS", "--cpu").split()
SMOKE_CASES = "./laya.cpp/benchmarks/cases/smoke.json"
TOLERANCE = 1e-4  # laya.cpp's own acceptance tolerance for public numbers.


def connect(path=":memory:"):
  db = sqlite3.connect(path)
  db.enable_load_extension(True)

  db.execute("create temp table base_functions as select name from pragma_function_list")
  db.execute("create temp table base_modules as select name from pragma_module_list")
  db.load_extension(EXT_PATH)
  db.execute("create temp table loaded_functions as select distinct name from pragma_function_list where name not in (select name from base_functions) order by name")
  db.execute("create temp table loaded_modules as select distinct name from pragma_module_list where name not in (select name from base_modules) order by name")
  return db


db = connect()

# Compared against a sorted query of the registered functions, so keep sorted.
FUNCTIONS = [
  "choice",
  "decide",
  "dq_backend",
  "dq_load",
  "dq_version",
  "noul",
  "score",
]

MODULES = []

BILLING_STATE = {"subject": "Duplicate invoice", "body": "I was charged twice. Please refund the extra charge today."}
BILLING_QUESTIONS = {
  "department": {"type": "choice", "instructions": "Which department should handle this?",
                 "criteria": {"billing": "payments and refunds", "technical": "bugs and outages", "sales": "new contracts"}},
  "urgency": {"type": "score", "instructions": "How urgent is the request?", "criteria": ["not urgent", "soon", "immediate"]},
  "refund": {"type": "noul", "instructions": "Does the customer request a refund?"},
}


def scalar(sql, *args):
  return db.execute(sql, args).fetchone()[0]


class TestCases(unittest.TestCase):
  """Tests that do not require a checkpoint."""

  def test_funcs(self):
    funcs = [row[0] for row in db.execute("select name from loaded_functions").fetchall()]
    self.assertEqual(funcs, FUNCTIONS)

  def test_modules(self):
    modules = [row[0] for row in db.execute("select name from loaded_modules").fetchall()]
    self.assertEqual(modules, MODULES)

  def test_dq_version(self):
    self.assertEqual(scalar("select dq_version()")[0], "v")

  def test_dq_backend(self):
    # Loading is process wide, so the model-backed tests may already have loaded one.
    backend = scalar("select dq_backend()")
    self.assertTrue(backend is None or isinstance(backend, str))

  def test_dq_load(self):
    with self.assertRaisesRegex(sqlite3.OperationalError, "Not a checkpoint directory"):
      scalar("select dq_load('/nonexistent/checkpoint')")
    with self.assertRaisesRegex(sqlite3.OperationalError, "Unknown option"):
      scalar("select dq_load('/nonexistent/checkpoint', '{\"gpu\": true}')")
    with self.assertRaisesRegex(sqlite3.OperationalError, "Unknown model variant"):
      scalar("select dq_load('/nonexistent/checkpoint', '{\"variant\": \"french\"}')")
    with self.assertRaisesRegex(sqlite3.OperationalError, "must be valid JSON"):
      scalar("select dq_load('/nonexistent/checkpoint', 'nope')")
    with self.assertRaisesRegex(sqlite3.OperationalError, "requires a checkpoint directory"):
      scalar("select dq_load(NULL)")

  def test_noul(self):
    self.assertIsNone(scalar("select noul(NULL, 'question')"))
    self.assertIsNone(scalar("select noul('state', NULL)"))
    self.assertIsNone(scalar("select noul('state', 'question', NULL)"))
    with self.assertRaisesRegex(sqlite3.OperationalError, "noul criteria must be valid JSON"):
      scalar("select noul('state', 'question', 'nope')")

  def test_choice(self):
    self.assertIsNone(scalar("select choice(NULL, 'question', '[\"a\",\"b\"]')"))
    self.assertIsNone(scalar("select choice('state', 'question', NULL)"))
    with self.assertRaisesRegex(sqlite3.OperationalError, "choice criteria must be valid JSON"):
      scalar("select choice('state', 'question', 'nope')")

  def test_score(self):
    self.assertIsNone(scalar("select score(NULL, 'question', '[\"low\",\"high\"]')"))
    with self.assertRaisesRegex(sqlite3.OperationalError, "score criteria must be valid JSON"):
      scalar("select score('state', 'question', '{')")

  def test_decide(self):
    self.assertIsNone(scalar("select decide(NULL, '{}')"))
    self.assertIsNone(scalar("select decide('state', NULL)"))
    with self.assertRaisesRegex(sqlite3.OperationalError, "laya questions must be valid JSON"):
      scalar("select decide('state', 'nope')")
    with self.assertRaisesRegex(sqlite3.OperationalError, "nonempty JSON object"):
      scalar("select decide('state', '{}')")
    with self.assertRaisesRegex(sqlite3.OperationalError, "nonempty JSON object"):
      scalar("select decide('state', '[1]')")

  def test_no_model_error(self):
    # A fresh process with no resident model and no checkpoint at DQ_MODEL_DIR.
    script = (
      "import sqlite3; db = sqlite3.connect(':memory:'); db.enable_load_extension(True); "
      f"db.load_extension({EXT_PATH!r}); db.execute(\"select noul('state', 'question')\")"
    )
    env = dict(os.environ, DQ_MODEL_DIR="/nonexistent/checkpoint")
    env.pop("DQ_OPTIONS", None)
    result = subprocess.run([sys.executable, "-c", script], env=env, capture_output=True, text=True)
    self.assertNotEqual(result.returncode, 0)
    self.assertIn("No decision backend loaded; call dq_load(dir_or_url) or set DQ_MODEL_DIR", result.stderr)


@unittest.skipUnless(MODEL_DIR, "set DQ_MODEL_DIR to a checkpoint directory to run model-backed tests")
class TestModel(unittest.TestCase):
  @classmethod
  def setUpClass(cls):
    cls.backend = scalar("select dq_load(?, ?)", MODEL_DIR, DQ_OPTIONS)

  def test_dq_load(self):
    self.assertTrue(self.backend)
    self.assertEqual(scalar("select dq_backend()"), self.backend)
    # Reloading the same checkpoint keeps the extension usable.
    self.assertEqual(scalar("select dq_load(?, ?)", MODEL_DIR, DQ_OPTIONS), self.backend)

  def test_noul(self):
    probability = scalar("select noul('Please refund the duplicate charge.', 'Does the customer ask for a refund?')")
    self.assertGreater(probability, 0.5)
    self.assertLessEqual(probability, 1.0)
    negative = scalar("select noul('The service works well. Thank you!', 'Is this a complaint?')")
    self.assertLess(negative, 0.5)
    described = scalar("select noul('Please refund the duplicate charge.', 'Does the customer ask for a refund?', "
                       "json_object('true', 'a refund is requested', 'false', 'no refund is requested'))")
    self.assertGreater(described, 0.5)

  def test_choice(self):
    choice = scalar("select choice(?, 'Which department should handle this?', ?)",
                    BILLING_STATE["body"], json.dumps(BILLING_QUESTIONS["department"]["criteria"]))
    self.assertEqual(choice, "billing")
    # A JSON array of names is accepted as criteria too.
    choice = scalar("select choice(?, 'Which department should handle this?', json_array('billing', 'technical', 'sales'))",
                    BILLING_STATE["body"])
    self.assertEqual(choice, "billing")

  def test_score(self):
    score = scalar("select score(?, 'How urgent is the request?', json_array('not urgent', 'soon', 'immediate'))",
                   BILLING_STATE["body"])
    self.assertGreaterEqual(score, 0.0)
    self.assertLessEqual(score, 2.0)

  def test_decide(self):
    answers = json.loads(scalar("select decide(json(?), ?)", json.dumps(BILLING_STATE), json.dumps(BILLING_QUESTIONS)))
    self.assertEqual(list(answers), ["department", "urgency", "refund"])
    self.assertEqual(answers["department"]["choice"], "billing")
    self.assertEqual(list(answers["department"]["probabilities"]), ["billing", "technical", "sales"])
    self.assertAlmostEqual(sum(answers["department"]["probabilities"].values()), 1.0, delta=1e-3)
    self.assertIn("score", answers["urgency"])
    self.assertIn("noul", answers["refund"])
    # The result carries the JSON subtype, so json functions nest it without re-quoting.
    wrapped = json.loads(scalar("select json_object('answers', decide('Thanks!', ?))",
                                json.dumps({"complaint": {"type": "noul", "instructions": "Is this a complaint?"}})))
    self.assertIn("noul", wrapped["answers"]["complaint"])
    self.assertEqual(scalar("select json_extract(decide(?, ?), '$.department.choice')",
                            BILLING_STATE["body"], json.dumps(BILLING_QUESTIONS)), "billing")

  def test_table_scan(self):
    db.execute("create temp table tickets(id integer primary key, body text)")
    db.executemany("insert into tickets(body) values (?)", [
      ("I was charged twice. Please refund the extra charge today.",),
      ("The service works well. Thank you!",),
      ("The login page returns a 500 error since this morning.",),
    ])
    refunds = db.execute("select id from tickets where noul(body, 'Does the customer request a refund?') > 0.5 order by id").fetchall()
    self.assertEqual(refunds, [(1,)])
    departments = db.execute("select id, choice(body, 'Which department should handle this?', json_array('billing', 'technical', 'sales')) "
                             "from tickets order by id").fetchall()
    self.assertEqual(departments[0], (1, "billing"))
    self.assertEqual(departments[2], (3, "technical"))
    db.execute("drop table tickets")

  @unittest.skipUnless(os.path.exists(CLI_PATH), "build laya-cli (make cli) to run the parity test")
  def test_cli_parity(self):
    with open(SMOKE_CASES) as f:
      cases = json.load(f)
    # One request per line so the CLI groups questions exactly like the extension does.
    stdin = "\n".join(json.dumps({"state": case["state"], "questions": case["questions"]}) for case in cases) + "\n"
    command = [CLI_PATH, "--model", MODEL_DIR] + CLI_FLAGS
    result = subprocess.run(command, input=stdin, capture_output=True, text=True, check=True)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    self.assertEqual(len(lines), len(cases))
    for case, line in zip(cases, lines):
      expected = line["results"][0]["answers"]
      actual = json.loads(scalar("select decide(json(?), ?)", json.dumps(case["state"]), json.dumps(case["questions"])))
      self.assertEqual(list(actual), list(expected))
      for question, answer in expected.items():
        self.assert_close(answer, actual[question], f"{case['id']}.{question}")

  def assert_close(self, expected, actual, path):
    if isinstance(expected, dict):
      self.assertEqual(list(expected), list(actual), path)
      for key in expected:
        self.assert_close(expected[key], actual[key], f"{path}.{key}")
    elif isinstance(expected, (int, float)) and not isinstance(expected, bool):
      self.assertAlmostEqual(expected, actual, delta=TOLERANCE, msg=path)
    else:
      self.assertEqual(expected, actual, path)


class TestCoverage(unittest.TestCase):
  def test_coverage(self):
    test_methods = [method for cls in (TestCases, TestModel) for method in dir(cls) if method.startswith("test_")]
    funcs_with_tests = set(method.replace("test_", "", 1) for method in test_methods)
    for func in FUNCTIONS:
      self.assertTrue(func in funcs_with_tests, f"{func} does not have corresponding test in {funcs_with_tests}")


if __name__ == "__main__":
  unittest.main()
