#!/usr/bin/env python3
"""Replay real HAPF observations against full ANALYZE rows and topology."""
import base64
import json
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    tool = pathlib.Path(sys.argv[1]).resolve()
    database = sys.argv[2]
    root = pathlib.Path(tempfile.mkdtemp(prefix="hapf-topology-"))
    gsql = ["/opt/gaussdb/bin/gsql", "-d", database, "-p", "5432", "-A"]

    def sql(command):
        return subprocess.run(gsql + ["-c", command], capture_output=True, text=True, timeout=40)

    setup = sql("create table if not exists topology_source(id int, bucket int); "
                "truncate topology_source; insert into topology_source "
                "select n,n%10 from generate_series(1,100)n; analyze topology_source;")
    assert setup.returncode == 0, setup.stderr
    cases = {
        "hashjoin": "select pg_sleep(0.005) from topology_source a join topology_source b on a.bucket=b.bucket",
        "lowerbound": "select /*+ hashjoin(a b) rows(a #1) rows(b #1) */ count(*) "
                      "from feedback_source a join feedback_source b on a.bucket=b.bucket",
        "nested-inner": "select /*+ nestloop(a b) leading(a b) */ pg_sleep(0.005) "
                        "from topology_source a join topology_source b on a.id=b.id where a.id<=100",
        "append": "select pg_sleep(0.005) from (select id from topology_source union all "
                  "select id from topology_source) q",
        "subquery": "select pg_sleep(0.005) from (select distinct bucket from topology_source) q "
                    "cross join topology_source b",
        "initplan": "select pg_sleep(0.005) from topology_source "
                    "where id <= (select max(id) from topology_source)",
        "subplan": "select pg_sleep(0.005), "
                   "(select max(b.id) from topology_source b where b.bucket<a.bucket) "
                   "from topology_source a",
    }
    for name, query in cases.items():
        archive = root / name
        run = subprocess.run([str(tool), "-d", database, "-p", "5432", "-w", "-c", query,
            "--enable-extoptimizer=false", "--explain-timeout-ms=150", "--sampling-blocks=0",
            "--max-iterations=1", "--logdir", str(archive)],
            capture_output=True, text=True, timeout=40)
        (root / f"{name}.stderr").write_text(run.stderr)
        logs = list(archive.glob("*/tuning_agent.log"))
        assert logs, name
        log = logs[0].read_text()
        notices = re.findall(r"NOTICE:\s+(HAPF .*)", log)
        messages = [line.split(" ", 4) for line in notices]
        chunks = [base64.b64decode(fields[4]) for fields in messages
                  if len(fields) == 5 and fields[3] == "DATA"]
        assert chunks, f"{name}: no snapshot: {log[-1600:]}"
        payload = b"".join(chunks).decode()
        (root / f"{name}.payload").write_text(payload)
        body = payload.split("\nOBSERVATIONS\n", 1)[1]
        observations = [line.split(",") for line in body.splitlines()]
        pretty = payload.split("PRETTY-BYTES=", 1)[1].split("\n", 1)[1]
        operations = {}
        for line in pretty.splitlines():
            columns = line.split("|")
            if len(columns) >= 9 and columns[0].strip().isdigit():
                operations[int(columns[0])] = columns[1].strip()
        exact = []
        for observation in observations:
            node_id = int(observation[0])
            op = operations[node_id]
            assert not re.search(r"->\s+Hash\s*$", op), f"{name}: Hash observation"
            if observation[1:3] == ["2", "0"]:
                exact.append((op, float(observation[3])))
        outcome = re.search(r"progress=feedback_round .*?outcome=(\w+).*", log)
        assert outcome and outcome[1] in {"PARTIAL", "TIMEOUT_NO_TRUSTED_NODES"}, \
            f"{name}: unsafe mapping or SQL: {log[-1800:]}"
        assert "partial_proof valid=false" not in log, f"{name}: proof mapping rejected"
        print(f"{name}: outcome={outcome[1]} observations={len(observations)} exact={exact}", flush=True)
        if name == "nested-inner":
            inputs = re.findall(r"rows\(([^\n]+)", log)
            assert not any('"b"' in item for item in inputs), "nested inner Rows escaped suppression"
        full_query = query if name != "lowerbound" else (
            "select /*+ hashjoin(a b) rows(a #1) rows(b #1) */ count(*) "
            "from feedback_source a join feedback_source b on a.id=b.id")
        if name == "lowerbound":
            # This large join has exactly 100 million rows by fixture construction.
            # Verify observed bounds against independently known table cardinalities.
            lower_bounds = [o for o in observations
                            if float(o[3]) > float(o[4])
                            and not (int(o[1]) == 2 and int(o[2]) == 0)]
            assert lower_bounds, "real lower-bound case produced none"
            assert all(float(o[3]) <= (100000000 if "Join" in operations[int(o[0])] else 100000)
                       for o in lower_bounds), "invalid real lower bound"
            print("lowerbound: independent fixture cardinalities validate bounds", flush=True)
            continue
        full = sql("set explain_perf_mode=pretty; set statement_timeout=30000; "
                   "explain (analyze, outline on, blockname on) " + full_query)
        assert full.returncode == 0 and "ERROR:" not in full.stderr, f"{name}: complete oracle failed"
        (root / f"{name}.complete").write_text(full.stdout)
        full_rows = {}
        actual_column = None
        for line in full.stdout.splitlines():
            fields = line.split("|")
            if "A-rows" in fields and "id" in fields:
                actual_column = fields.index("A-rows")
            if actual_column is not None and len(fields) > actual_column and fields[0].strip().isdigit():
                try:
                    full_rows[int(fields[0])] = float(fields[actual_column])
                except ValueError:
                    pass
        for observation in observations:
            node_id = int(observation[0])
            state, rescans = map(int, observation[1:3])
            actual, estimated = map(float, observation[3:5])
            terminal = state == 2
            if (terminal and not rescans) or actual > estimated:
                assert node_id in full_rows, f"{name}: oracle node missing"
                if terminal and not rescans:
                    assert actual == full_rows[node_id], \
                        f"{name}: invalid EXACT id={node_id}"
                else:
                    assert actual <= full_rows[node_id], \
                        f"{name}: invalid LOWER_BOUND id={node_id}"
        print(f"{name}: complete oracle verified zero false EXACT/invalid lower bounds", flush=True)
    print(f"ok - real topology cases passed; evidence={root}")


if __name__ == "__main__":
    main()
