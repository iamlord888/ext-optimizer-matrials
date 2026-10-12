#!/usr/bin/env python3
"""Real native/runner contracts against a dedicated GaussDB fixture database."""
import csv
import importlib.util
import pathlib
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    database = sys.argv[2]
    root = pathlib.Path(tempfile.mkdtemp(prefix="hapf-integration-"))
    slow = "select count(*) from feedback_source a join feedback_source b on a.bucket=b.bucket"
    fast = "select count(*) from feedback_source where bucket=1"

    def run(name, sql, *options, rc):
        archive = root / name
        result = subprocess.run(
            [str(tool), "-d", database, "-p", "5432", "-w", "--logdir", str(archive),
             "--sampling-blocks=0", "-c", sql, *options],
            capture_output=True, text=True, timeout=40)
        (root / f"{name}.stdout").write_text(result.stdout)
        (root / f"{name}.stderr").write_text(result.stderr)
        check(result.returncode == rc, f"{name}: rc={result.returncode}, stderr={result.stderr}")
        with (archive / "summary.tsv").open(newline="") as stream:
            reader = csv.DictReader(stream, delimiter="\t")
            rows = list(reader)
            check(len(reader.fieldnames) == 18 and len(set(reader.fieldnames)) == 18,
                  "v14 has exactly 18 unique columns")
            check(not {"timeout_iterations", "committed", "conflict", "skipped"} & set(reader.fieldnames),
                  "excluded summary counters are absent")
        row = rows[-1]
        check(None not in row and row["schema_version"] == "14", f"{name}: v14 row alignment")
        return result, row, pathlib.Path(row["archive_dir"])

    result, row, archive = run("partial", slow, "--explain-timeout-ms=150", rc=1)
    check(row["iterations"] == "2" and row["partial_iterations"] == "2", "two PARTIAL counts")
    check(row["explain_timeout_ms"] == "150", "timeout stores configuration")
    check(all(row[field] == "" for field in (
        "sample_tuned_exec_time_ms", "best_execution_time_ms", "best_iteration")),
          "PARTIAL never invents full execution measurements")
    check(result.stdout == "" and row["delivery_status"] != "delivered", "partial-only not delivered")
    log = (archive / "tuning_agent.log").read_text()
    check("iteration=1 outcome=PARTIAL hint_added=true" in log and
          "iteration=2 outcome=PARTIAL hint_added=false" in log, "changed controls next candidate")
    check(log.count("effective_timeout_ms=150 timeout_reason=server_cancel") == 2,
          "per-round outcome, effective timeout and cause")

    result, row, _ = run("complete", fast, "--explain-timeout-ms=-1", rc=0)
    check(row["partial_iterations"] == "0" and row["explain_timeout_ms"] == "-1", "disabled timeout")
    check(float(row["best_execution_time_ms"]) > 0 and int(row["best_iteration"]) > 0,
          "complete has measured best")
    check("begin_outline_data" in result.stdout and row["delivery_status"] == "delivered", "stdout SQL")
    check(row["plantrace_status"] == "ok", "complete final plantrace preserved")
    run("complete", fast, "--explain-timeout-ms=-1", rc=0)
    check((root / "complete" / "summary.tsv").read_text().count("schema_version") == 1,
          "repeat run writes one header")

    result, row, _ = run("no-trusted", "select pg_sleep(1) from feedback_source limit 1",
                         "--enable-extoptimizer=false", "--explain-timeout-ms=150", rc=0)
    check(row["iterations"] == "0" and row["partial_iterations"] == "0", "no trusted not counted")
    check(row["best_iteration"] == "" and row["best_execution_time_ms"] == "", "fallback not measured")
    check(result.stdout and row["delivery_status"] == "delivered", "legacy fallback delivered")

    result, row, _ = run("fallback", slow, "--enable-extoptimizer=false",
                         "--explain-timeout-ms=150", rc=0)
    check(row["partial_iterations"] == "2" and row["best_iteration"] == "", "partial fallback counts")
    check(result.stdout and row["delivery_status"] == "delivered", "legacy partial fallback stdout")

    result, row, _ = run("baseline", slow, "--enable-extoptimizer=false", "--explain-timeout-ms=150",
                         "--baseline-ref-time-ms=1000", rc=1)
    check(result.stdout == "" and row["best_execution_time_ms"] == "1000.000" and
          row["best_iteration"] == "", "baseline preserved and fallback gain gate enforced")
    result, row, _ = run("error", "select invalid_column from feedback_source",
                         "--enable-extoptimizer=false", "--explain-timeout-ms=150", rc=1)
    check(result.stdout == "" and row["iterations"] == "0" and row["partial_iterations"] == "0",
          "ordinary error never delivers or counts feedback")
    _, row, _ = run("attempt-limit", slow, "--explain-timeout-ms=150", "--max-iterations=1", rc=1)
    check(row["iterations"] == "1" and row["partial_iterations"] == "1", "candidate attempt cap")

    best_sql = "select pg_sleep((select case when nextval('feedback_delay_seq')=1 " \
               "then 0.00001 else 0.005 end)) from topology_source a join topology_source b on a.bucket=b.bucket"
    gsql = ["/opt/gaussdb/bin/gsql", "-d", database, "-p", "5432", "-At", "-c"]
    subprocess.run(gsql + ["create sequence if not exists feedback_delay_seq; "
        "select setval('feedback_delay_seq',1,false)"], check=True, capture_output=True)
    verify_sql = "select pg_sleep((select case when nextval('feedback_delay_seq')=1 " \
                 "then 1.5 else 0.001 end)) from topology_source limit 1"
    result, row, _ = run("verify-time-excluded", verify_sql,
                         "--enable-extoptimizer=false", "--verify-initial-plan=true",
                         "--explain-timeout-ms=-1", "--max-iterations=1", rc=0)
    check(float(row["initial_exec_time_ms"]) > 1000,
          "verify timing fixture has a slow initial execution")
    check(float(row["total_tuning_wall_time_ms"]) < float(row["initial_exec_time_ms"]),
          "reported tuning wall time excludes initial verification")

    subprocess.run(gsql + ["select setval('feedback_delay_seq',1,false)"],
                   check=True, capture_output=True)
    result, row, archive = run("best-partial", best_sql, "--enable-extoptimizer=false",
                               "--explain-timeout-ms=3000", rc=0)
    check(row["iterations"] == "2" and row["partial_iterations"] == "1" and row["best_iteration"] == "1",
          "real complete then partial retains measured best")
    check(row["sample_tuned_exec_time_ms"] == row["best_execution_time_ms"] and
          row["plantrace_status"] == "ok", "partial preserves complete measurement and plantrace")
    log = (archive / "tuning_agent.log").read_text()
    final_sql = log.split("CONTEXT PLANTRACE EXPLAIN SQL\n")[-1].split("\n\n", 1)[0]
    check("EXPLAIN " + best_sql in final_sql and "rows(" not in final_sql,
          "final plantrace uses first complete candidate rather than hinted partial")
    subprocess.run(gsql + ["select setval('feedback_delay_seq',1,false)"], check=True, capture_output=True)
    result, row, archive = run("best-global-timeout", best_sql, "--enable-extoptimizer=false",
                               "--explain-timeout-ms=-1", "--search-timeout-ms=1800", rc=0)
    check(row["best_iteration"] == "1" and row["partial_iterations"] == "0" and row["iterations"] == "1",
          "disabled partial preserves complete best after global PQcancel")
    check(row["plantrace_status"] == "ok" and
          (archive / "final_rowshint_plantrace.log").exists(),
          "global PQcancel rebuilds a clean session for final plantrace")

    runner_path = tool.parent / "tuning_agent.py"
    spec = importlib.util.spec_from_file_location("runner", runner_path)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    old_argv = sys.argv
    try:
        sys.argv = [str(runner_path), "-i", str(root), "-o", str(root / "unused-output"),
                    "--explain-timeout-ms=150", "--search-timeout-ms=9000"]
        args = runner.parse_args()
    finally:
        sys.argv = old_argv
    command = runner.build_command(args, root / "input.sql")
    check("--explain-timeout-ms=150" in command, "runner forwards per-round limit")
    check(runner.process_timeout_sec(args) == 9 + runner.RUNNER_TIMEOUT_MARGIN_SEC,
          "runner process timeout remains global")
    inputs = root / "runner-input"
    inputs.mkdir()
    (inputs / "fast.sql").write_text(fast)
    result = subprocess.run([sys.executable, str(runner_path), "-i", str(inputs),
        "-o", str(root / "runner-output"), "-d", database, "-p", "5432", "-w",
        "--explain-timeout-ms=1000", "--logdir", str(root / "runner-log")],
        capture_output=True, text=True, timeout=40)
    check(result.returncode == 0, f"real Python runner failed: {result.stderr}")
    check(any("begin_outline_data" in path.read_text() for path in (root / "runner-output").rglob("*.out")),
          "real runner writes delivered SQL")
    print(f"ok - native v14, delivery and runner integration passed; evidence={root}")


if __name__ == "__main__":
    main()
