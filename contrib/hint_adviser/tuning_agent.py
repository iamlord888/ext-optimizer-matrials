#!/usr/bin/env python3
"""Run tuning_agent once per SQL file in an input directory (concurrent)."""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path


RUNNER_TIMEOUT_MARGIN_SEC = 60
DEFAULT_WORKERS = 5
MAX_PASSWORD_BYTES = 4096
ACTIVE_PROCESSES = {}
STOP_REQUESTED = threading.Event()
RUNNER_HELP = """tuning_agent.py runs tuning_agent once per SQL file in an input directory.

Usage:
  tuning_agent.py [connection options] [options] -i INPUT_DIR -o OUTPUT_DIR

Connection options:
  -h, --host=HOST           database server host
  -p, --port=PORT           database server port
  -U, --username=USER       database user name
  -d, --dbname=DBNAME       database name, URI, or keyword connection string
  -s, --schema=SCHEMA       schema to use as the session search_path
  -W, --password=PASSWORD   database user password (visible in ps)
      --password-stdin      read password from stdin
  -w, --no-password         never prompt for password
  -t, --connect-timeout=S   connection timeout in seconds

Options:
  -S, --setting=NAME=VALUE
                           set one external optimizer configuration value;
                           may be specified more than once
      --target-time-ms=MS   enable feedback loop until execution time <= MS
      --search-timeout-ms=MS
      --explain-timeout-ms=MS  per-candidate ANALYZE limit; -1 disables it
                           limit each input SQL search wall-clock time
      --sampling-blocks=N   target sampled blocks; 0 uses catalog statistics
                            (default: 1000)
      --patience-iters=N    stop after N sub-threshold tuning iterations (default: 3)
      --iter-min-improvement=RATIO
                           improvement required over the previous best (default: 0.10)
                           normal-mode baseline: verified plan or first iteration
      --logdir=DIR          archive root for per-SQL logs
                           each per-SQL archive also stores external_optimizer.log
                           (default: $TUNING_AGENT_LOG, then ./tuning_agent_log)
      --verify-initial-plan[=BOOL]
                           EXPLAIN ANALYZE bare SQL before tuning (default: off)
      --enable-extoptimizer[=BOOL]
                           build candidate SQL through the external optimizer (default: on)
  -v, --verbose             mirror tuning_agent INFO logs to stderr (default: off)
      --workers=N           max concurrent tuning_agent processes (default: 5)
  -V, --version             output version information, then exit
  -?, --help                show this help, then exit

Runner options:
  -i INPUT_DIR, --input-dir INPUT_DIR
                        Directory containing .sql files.
  -o OUTPUT_DIR, --output-dir OUTPUT_DIR
                        Directory for optimized SQL in stdout/, stderr/, and results.tsv.

Dump-log command:
  tuning_agent.py dump-log packages a tuning_agent log directory into tar.gz.

Usage:
  tuning_agent.py dump-log -d DBNAME -p PORT [connection options] [--logdir=DIR]

Options:
  -s, --schema=SCHEMA       dump only this schema and its statistics
      --dump-stats[=BOOL]   export schema.sql and DB stats before tar.gz (default: on)
      --logdir=DIR          log directory to package
                           (default: $TUNING_AGENT_LOG, then ./tuning_agent_log)"""


def parse_setting(raw):
    """Split one NAME=VALUE option without owning GUC value semantics."""
    name, separator, value = raw.partition("=")
    if not separator:
        raise argparse.ArgumentTypeError("setting must use NAME=VALUE")

    name = name.strip()
    value = value.strip()
    if not name:
        raise argparse.ArgumentTypeError("setting name must not be empty")
    if not value:
        raise argparse.ArgumentTypeError("setting value must not be empty")
    return name, value


def parse_args():
    parser = argparse.ArgumentParser(
        add_help=False,
        formatter_class=argparse.RawTextHelpFormatter,
        usage=argparse.SUPPRESS,
    )
    parser.add_argument("-?", "--help", action="help", help=argparse.SUPPRESS)
    parser.add_argument(
        "-i", "--input-dir", metavar="INPUT_DIR", help=argparse.SUPPRESS
    )
    parser.add_argument(
        "-o", "--output-dir", metavar="OUTPUT_DIR", help=argparse.SUPPRESS
    )
    parser.add_argument("-h", "--host", metavar="HOST", help=argparse.SUPPRESS)
    parser.add_argument("-p", "--port", metavar="PORT", help=argparse.SUPPRESS)
    parser.add_argument("-U", "--username", metavar="USER", help=argparse.SUPPRESS)
    parser.add_argument("-d", "--dbname", metavar="DBNAME", help=argparse.SUPPRESS)
    parser.add_argument("-s", "--schema", metavar="SCHEMA", help=argparse.SUPPRESS)
    parser.add_argument("-W", "--password", metavar="PASSWORD", help=argparse.SUPPRESS)
    parser.add_argument(
        "--password-stdin", action="store_true", help=argparse.SUPPRESS
    )
    parser.add_argument(
        "-w", "--no-password", action="store_true", help=argparse.SUPPRESS
    )
    parser.add_argument("-t", "--connect-timeout", metavar="S", help=argparse.SUPPRESS)
    parser.add_argument(
        "-S",
        "--setting",
        action="append",
        default=[],
        type=parse_setting,
        metavar="NAME=VALUE",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--target-time-ms", metavar="MS", help=argparse.SUPPRESS)
    parser.add_argument("--explain-timeout-ms", type=int, default=-1,
                        metavar="MS", help=argparse.SUPPRESS)
    parser.add_argument("--sampling-blocks", metavar="N", help=argparse.SUPPRESS)
    parser.add_argument(
        "--patience-iters", type=int, metavar="N", help=argparse.SUPPRESS
    )
    parser.add_argument(
        "--iter-min-improvement",
        type=float,
        metavar="RATIO",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--logdir", metavar="DIR", help=argparse.SUPPRESS)
    parser.add_argument(
        "--verify-initial-plan",
        nargs="?",
        const=True,
        metavar="BOOL",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--enable-extoptimizer",
        nargs="?",
        const=True,
        metavar="BOOL",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--search-timeout-ms", type=int, metavar="MS", help=argparse.SUPPRESS
    )
    parser.add_argument("-v", "--verbose", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument(
        "--workers",
        type=int,
        default=DEFAULT_WORKERS,
        metavar="N",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("-V", "--version", action="store_true", help=argparse.SUPPRESS)
    parser.description = RUNNER_HELP
    args = parser.parse_args()
    if args.search_timeout_ms is not None and args.search_timeout_ms < 0:
        parser.error("--search-timeout-ms must be greater than or equal to 0")
    if args.explain_timeout_ms != -1 and args.explain_timeout_ms <= 0:
        parser.error("--explain-timeout-ms must be -1 or a positive integer")
    if not args.version:
        if args.input_dir is None:
            parser.error("-i/--input-dir is required")
        if args.output_dir is None:
            parser.error("-o/--output-dir is required")
    if args.workers < 1:
        parser.error("--workers must be >= 1")
    if args.patience_iters is not None and args.patience_iters < 1:
        parser.error("--patience-iters must be >= 1")
    if args.iter_min_improvement is not None and not (
        0 < args.iter_min_improvement < 1
    ):
        parser.error("--iter-min-improvement must be greater than 0 and less than 1")
    if args.password_stdin and args.password is not None:
        parser.error("--password-stdin and -W/--password are mutually exclusive")
    if args.password_stdin and args.no_password:
        parser.error("--password-stdin and -w/--no-password are mutually exclusive")
    if args.password is not None and args.no_password:
        parser.error("-W/--password and -w/--no-password are mutually exclusive")
    return args


def list_sql_files(input_dir):
    return sorted(
        path
        for path in input_dir.iterdir()
        if path.is_file() and path.suffix.lower() == ".sql"
    )


def tuning_agent_executable():
    bundled_agent = Path(__file__).resolve().with_name("tuning_agent")

    if bundled_agent.exists():
        return str(bundled_agent)
    return "tuning_agent"


def build_command(args, sql_file, password_via_stdin=False):
    cmd = [tuning_agent_executable()]
    if args.host is not None:
        cmd.extend(["-h", args.host])
    if args.port is not None:
        cmd.extend(["-p", args.port])
    if args.username is not None:
        cmd.extend(["-U", args.username])
    if args.dbname is not None:
        cmd.extend(["-d", args.dbname])
    if args.schema is not None:
        cmd.extend(["-s", args.schema])
    if password_via_stdin:
        cmd.append("--password-stdin")
    elif args.password is not None:
        cmd.extend(["-W", args.password])
    if args.no_password:
        cmd.append("-w")
    if args.connect_timeout is not None:
        cmd.extend(["-t", args.connect_timeout])
    for name, value in args.setting:
        cmd.extend(["-S", f"{name}={value}"])
    if args.target_time_ms is not None:
        cmd.append(f"--target-time-ms={args.target_time_ms}")
    if args.sampling_blocks is not None:
        cmd.append(f"--sampling-blocks={args.sampling_blocks}")
    if args.patience_iters is not None:
        cmd.append(f"--patience-iters={args.patience_iters}")
    if args.iter_min_improvement is not None:
        cmd.append(f"--iter-min-improvement={args.iter_min_improvement}")
    if args.logdir is not None:
        cmd.append(f"--logdir={args.logdir}")
    if args.verify_initial_plan is True:
        cmd.append("--verify-initial-plan")
    elif args.verify_initial_plan is not None:
        cmd.append(f"--verify-initial-plan={args.verify_initial_plan}")
    if args.enable_extoptimizer is True:
        cmd.append("--enable-extoptimizer")
    elif args.enable_extoptimizer is not None:
        cmd.append(f"--enable-extoptimizer={args.enable_extoptimizer}")
    if args.search_timeout_ms is not None and args.search_timeout_ms > 0:
        cmd.append(f"--search-timeout-ms={args.search_timeout_ms}")
    cmd.append(f"--explain-timeout-ms={args.explain_timeout_ms}")
    if args.verbose:
        cmd.append("--verbose")
    if args.version:
        cmd.append("-V")
    cmd.extend(["-f", str(sql_file)])
    return cmd


def process_timeout_sec(args):
    if args.search_timeout_ms is None or args.search_timeout_ms <= 0:
        return None
    return (args.search_timeout_ms / 1000.0) + RUNNER_TIMEOUT_MARGIN_SEC


def terminate_process(process):
    if process.poll() is not None:
        return

    try:
        process.kill()
    except ProcessLookupError:
        return
    process.wait()


def handle_runner_signal(signum, _frame):
    STOP_REQUESTED.set()
    while True:
        try:
            processes = tuple(ACTIVE_PROCESSES.values())
            break
        except RuntimeError:
            continue
    for process in processes:
        terminate_process(process)
    ACTIVE_PROCESSES.clear()
    raise SystemExit(128 + signum)


def run_one_sql(args, sql_file, output_dir, stdin_password=None):
    if STOP_REQUESTED.is_set():
        raise RuntimeError("runner cancellation requested")

    stdout_file = output_dir / "stdout" / f"{sql_file.stem}.out"
    stderr_file = output_dir / "stderr" / f"{sql_file.stem}.err"
    if stdout_file.exists():
        stdout_file.unlink()
    cmd = build_command(
        args, sql_file, password_via_stdin=stdin_password is not None
    )
    env = os.environ.copy()
    if stdin_password is not None:
        env.pop("PGPASSWORD", None)

    start_time = time.monotonic()
    # Java kills the runner process group on timeout, so children must inherit it.
    process = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE if stdin_password is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
    )
    ACTIVE_PROCESSES[process.pid] = process
    try:
        if STOP_REQUESTED.is_set():
            terminate_process(process)
            raise RuntimeError("runner cancellation requested")
        stdout, stderr = process.communicate(
            input=stdin_password, timeout=process_timeout_sec(args)
        )
        elapsed_sec = time.monotonic() - start_time
        stderr_file.write_text(stderr, encoding="utf-8")
        has_output = process.returncode == 0
        if has_output:
            stdout_file.write_text(stdout, encoding="utf-8")
        return sql_file, process.returncode, elapsed_sec, has_output
    except subprocess.TimeoutExpired:
        elapsed_sec = time.monotonic() - start_time
        terminate_process(process)
        stdout, stderr = process.communicate()
        stderr_file.write_text(stderr + "\nTIMEOUT\n", encoding="utf-8")
        return sql_file, 124, elapsed_sec, False
    finally:
        ACTIVE_PROCESSES.pop(process.pid, None)


def read_password_from_stdin():
    try:
        password_bytes = sys.stdin.buffer.read(MAX_PASSWORD_BYTES + 1)
    except OSError as exc:
        raise SystemExit(f"failed to read password from stdin: {exc}") from exc

    if not password_bytes:
        raise SystemExit("password from stdin is empty")
    if len(password_bytes) > MAX_PASSWORD_BYTES:
        raise SystemExit(
            f"password from stdin exceeds {MAX_PASSWORD_BYTES} bytes"
        )
    if b"\0" in password_bytes:
        raise SystemExit("password from stdin contains a null byte")
    try:
        return password_bytes.decode("utf-8").rstrip("\r\n")
    except UnicodeDecodeError as exc:
        raise SystemExit("password from stdin is not valid UTF-8") from exc


def write_result(results, output_dir, sql_file, rc, elapsed_sec, has_output):
    stdout_file = output_dir / "stdout" / f"{sql_file.stem}.out"
    stderr_file = output_dir / "stderr" / f"{sql_file.stem}.err"
    out_path = str(stdout_file.relative_to(output_dir)) if has_output else ""
    results.write(
        f"{sql_file.name}\t{rc}\t{elapsed_sec:.3f}\t"
        f"{out_path}\t"
        f"{stderr_file.relative_to(output_dir)}\n"
    )
    results.flush()
    print(f"[end] {sql_file.name} rc={rc} sec={elapsed_sec:.3f}", flush=True)


def main():
    args = parse_args()
    if args.version:
        return subprocess.call([tuning_agent_executable(), "-V"])

    stdin_password = None
    if args.password_stdin:
        stdin_password = read_password_from_stdin()

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    if not input_dir.is_dir():
        raise SystemExit(f"input directory does not exist: {input_dir}")
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "stdout").mkdir(parents=True, exist_ok=True)
    (output_dir / "stderr").mkdir(parents=True, exist_ok=True)
    signal.signal(signal.SIGINT, handle_runner_signal)
    signal.signal(signal.SIGTERM, handle_runner_signal)

    sql_files = list_sql_files(input_dir)
    workers = min(args.workers, len(sql_files)) if sql_files else 1
    results_path = output_dir / "results.tsv"
    with results_path.open("w", encoding="utf-8") as results:
        results.write("sql_file\trc\telapsed_sec\tout_file\terr_file\n")
        if workers <= 1:
            for sql_file in sql_files:
                print(f"[start] {sql_file.name}", flush=True)
                sql_file_ret, rc, elapsed_sec, has_output = run_one_sql(
                    args, sql_file, output_dir, stdin_password=stdin_password
                )
                write_result(
                    results,
                    output_dir,
                    sql_file_ret,
                    rc,
                    elapsed_sec,
                    has_output,
                )
        else:
            print(
                f"[concurrent] workers={workers} files={len(sql_files)}",
                flush=True,
            )
            with ThreadPoolExecutor(max_workers=workers) as pool:
                future_to_file = {
                    pool.submit(
                        run_one_sql,
                        args,
                        sql_file,
                        output_dir,
                        stdin_password=stdin_password,
                    ): sql_file
                    for sql_file in sql_files
                }
                for future in as_completed(future_to_file):
                    try:
                        sql_file_ret, rc, elapsed_sec, has_output = future.result()
                    except Exception as exc:
                        sql_file_ret = future_to_file[future]
                        rc, elapsed_sec, has_output = 1, 0.0, False
                        print(f"[error] {sql_file_ret.name} {exc}", flush=True)
                    write_result(
                        results,
                        output_dir,
                        sql_file_ret,
                        rc,
                        elapsed_sec,
                        has_output,
                    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
