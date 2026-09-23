#!/usr/bin/env python3
"""Integration checks for a sqlaya decision backend.

Run against a local checkpoint directory or an HTTP endpoint. The structural
checks are identical for both; the behavioural thresholds differ, because the
upstream Laya checkpoint and a hosted System One model are not equally strong
and pretending otherwise would give us a test that only passes by luck.
"""
import argparse, json, os, subprocess, sys

SECRET_Q = (
    "The command text itself contains a literal credential value.",
    {"true": "A credential value is written out verbatim in the command text.",
     "false": "No credential value appears in the text; the command may still "
              "reference, fetch, print or use one."},
)
KEY_CMD = "export ANTHROPIC_API_KEY=sk-ant-api03-Xk9fJ2mQ8vLpZq"
EMIT_CMD = "gh auth refresh -s workflow && gh auth token | pbcopy"
K8S_Q = "Is this command related to Kubernetes?"

failures, checks = [], 0


def sql(extension, backend, options, statements):
    """Runs statements through the sqlite3 CLI with the extension loaded."""
    script = f".load {extension}\n.mode list\n.headers off\n"
    opt = f", '{options}'" if options else ""
    script += f"select laya_load('{backend}'{opt});\n" + statements
    out = subprocess.run(["sqlite3", ":memory:"], input=script, text=True,
                         capture_output=True, timeout=600)
    if out.returncode != 0:
        print(out.stdout); print(out.stderr, file=sys.stderr)
        sys.exit(f"sqlite3 exited {out.returncode}")
    # ggml chatters on stderr; only stdout carries results.
    return [l for l in out.stdout.strip().splitlines() if l.strip()]


def noul(cmd, instructions, criteria=None):
    """One laya_noul call. Criteria are optional; omit rather than pass null."""
    cmd = cmd.replace("'", "''")
    ins = instructions.replace("'", "''")
    if criteria is None:
        return f"select laya_noul('{cmd}', '{ins}');"
    crit = json.dumps(criteria).replace("'", "''")
    return f"select laya_noul('{cmd}', '{ins}', json('{crit}'));"


def check(name, ok, detail=""):
    global checks
    checks += 1
    print(f"{'PASS' if ok else 'FAIL'}  {name}{'  -- ' + detail if detail else ''}")
    if not ok:
        failures.append(f"{name}: {detail}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extension", required=True)
    ap.add_argument("--backend", required=True)
    ap.add_argument("--options", default="")
    ap.add_argument("--profile", choices=["laya", "jev"], required=True)
    a = ap.parse_args()

    # --- structural: identical expectations for every backend ---------------
    rows = sql(a.extension, a.backend, a.options, "\n".join([
        noul(KEY_CMD, *SECRET_Q),
        noul(EMIT_CMD, *SECRET_Q),
        noul("ls -la", *SECRET_Q),
        noul("kubectl get pods -n kube-system", K8S_Q),
        noul("cargo build --release", K8S_Q),
        # Options must be an object of option -> description. A bare JSON array
        # works against a local checkpoint but is rejected by the System One HTTP
        # shape, so the object form is the portable one.
        "select laya_choice('git push origin main', 'Which tool does this command use?', "
        "json('{\"git\":\"the git version control tool\",\"docker\":\"the docker "
        "container tool\",\"kubernetes\":\"the kubernetes orchestrator\",\"other\":"
        "\"something else\"}'));",
    ]))

    if len(rows) < 7:
        sys.exit(f"expected backend name + 6 results, got {len(rows)}: {rows}")
    backend_name, vals, choice = rows[0], rows[1:6], rows[6]
    key_p, emit_p, ls_p, k8s_p, cargo_p = [float(v) for v in vals]

    check("backend reports a name", bool(backend_name.strip()), backend_name)
    check("probabilities are in [0,1]",
          all(0.0 <= v <= 1.0 for v in (key_p, emit_p, ls_p, k8s_p, cargo_p)),
          f"{key_p} {emit_p} {ls_p} {k8s_p} {cargo_p}")
    check("choice returns an allowed option", choice in
          ("git", "docker", "kubernetes", "other"), choice)

    # --- behavioural: ordering holds for any competent backend --------------
    check("literal key outranks a bare listing", key_p > ls_p,
          f"key={key_p} ls={ls_p}")
    check("kubectl outranks cargo on a Kubernetes question", k8s_p > cargo_p,
          f"kubectl={k8s_p} cargo={cargo_p}")

    # --- behavioural: absolute thresholds, only where they are earned -------
    if a.profile == "jev":
        # Measured 0.98 / 0.02 / 0.999 AUC, so these have wide margin.
        check("literal key scores high", key_p >= 0.80, f"{key_p}")
        check("bare listing scores low", ls_p <= 0.20, f"{ls_p}")
        # The discriminating case: this command EMITS a credential without
        # containing one. Aboutness, not containment.
        check("emitting a token is not containing one", emit_p <= 0.30, f"{emit_p}")
        check("kubectl scores high on Kubernetes", k8s_p >= 0.80, f"{k8s_p}")
    else:
        # Upstream Laya is weakly calibrated on the secret question (Brier 0.26
        # measured against hand-verified labels), so asserting absolute values
        # there would be testing noise. Its general judgement is strong, so that
        # is where an absolute threshold is meaningful.
        check("kubectl scores high on Kubernetes", k8s_p >= 0.70, f"{k8s_p}")
        check("cargo scores low on Kubernetes", cargo_p <= 0.30, f"{cargo_p}")

    # --- a bad backend must fail loudly, not silently succeed ---------------
    bad = subprocess.run(["sqlite3", ":memory:"], text=True, capture_output=True,
                         input=f".load {a.extension}\nselect laya_load('/nonexistent/x');\n")
    check("invalid backend raises an error", bad.returncode != 0 or "rror" in bad.stderr,
          f"rc={bad.returncode}")

    print(f"\n{checks - len(failures)}/{checks} checks passed")
    if failures:
        print("\nFAILURES:"); [print(" -", f) for f in failures]
        sys.exit(1)


if __name__ == "__main__":
    main()

