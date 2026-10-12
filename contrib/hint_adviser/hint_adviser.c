/*-------------------------------------------------------------------------
 *
 * hint_adviser.c
 *    Frontend tool for generating optimizer hints.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    contrib/hint_adviser/hint_adviser.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * This tool has five modules on the main path: option, connection, archive,
 * json parsing, and hint generation. main() wires them together;
 * process_one_query() owns the iterative adviser loop for one input SQL.
 *
 * Key entry points:
 * main()                        Parse options, connect, and process one SQL input.
 * process_one_query()           Build candidate SQL, sandbox it, merge new rows hints,
 *                               and stop on the active tuning budget or convergence.
 *
 * Reading guide:
 * Read main() first for input and connection ownership, then process_one_query()
 * for the iteration loop.  The external optimizer slot is inside process_one_query();
 * connection/json/hint modules only provide one clear service each.
 *
 *   argv
 *    |
 *    v
 * [option] parse options
 *    |
 *    v
 * [connection] connect
 *    |
 *    v
 *  original SQL
 *    |
 *    v
 * +---------------- process_one_query() -------------------------------+
 * | select bootstrap/Rows SQL or one external optimizer candidate      |
 * |        |                                                           |
 * |        v                                                           |
 * | [connection] pretty OUTLINE/BLOCKNAME candidate SQL                |
 * |        |                                                           |
 * |        v                                                           |
 * | [connection] outlined EXPLAIN ANALYZE SQL -> PlanNode tree          |
 * |        |                                                           |
 * |        v                                                           |
 * | route the observation to Rows feedback or external feedback        |
 * |        |                                                           |
 * |        v                                                           |
 * | keep the fastest executed candidate and stop on target time, no    |
 * | new hint or insufficient complete-execution-time improvement       |
 * +--------------------------------------------------------------------+
 *    |
 *    v
 *  stdout (original SQL annotated with the fastest outline hints)
 *  archive root summary.tsv (one row per processed SQL)
 */
#include "postgres_fe.h"

#include <sys/time.h>

#include "hint_adviser_log.h"
#include "libpq-fe.h"

#include "hint_adviser_archive.h"
#include "hint_adviser_feedback_round.h"
#include "hint_adviser_feedback_policy.h"
#include "hint_adviser_connection.h"
#include "hint_adviser_extopt.h"
#include "hint_adviser_hint.h"
#include "hint_adviser_option.h"
#include "hint_adviser_plan_tree.h"
#include "hint_adviser_sampling.h"
#include "hint_adviser_sql_log.h"
#include "hint_adviser_string.h"

typedef struct iterationState
{
    int iteration;
    FeedbackLoopState feedback_loop;
    char *candidate_sql;
    bool rows_hint_changed;
} iterationState;

/*
 * Brief: Read the shared monotonic duration clock in microseconds.
 * Return: Current time in microseconds.
 */
static pg_usec_time_t current_time_usec(void)
{
    return connection_monotonic_usec();
}

/*
 * Brief: Calculate elapsed wall time between two already captured timestamps.
 * Inputs: start_time - Start timestamp in microseconds;
 *         end_time - End timestamp in microseconds.
 * Return: Elapsed milliseconds, or 0 if the timestamps are not increasing.
 */
static double elapsed_ms_between(pg_usec_time_t start_time,
                                 pg_usec_time_t end_time)
{
    if (end_time <= start_time) {
        return 0.0;
    }
    return (double) (end_time - start_time) / 1000.0;
}

/*
 * Brief: Print the single final status for one processed SQL.
 * Inputs: options - Tool options; summary - Final summary state;
 *         rc - Process return code.
 */
static void print_query_finish(const HintAdviserOptions *options,
                               HintAdviserRunSummary *summary,
                               int rc)
{
    if (options->verbose) {
        print_output_separator();
        log_message(true, "INFO",
            "Iteration info:\n"
            "  Finish reason: %s\n"
            "  Iterations: %d",
            finish_reason_name(summary->finish_reason),
            summary->iterations);
        if (summary->best_iteration > 0) {
            log_message(true, "INFO",
                "  Best iteration: %d",
                summary->best_iteration);
        }
        log_message(true, "INFO", "%s", "");
    }
    print_output_separator();

    char baseline_execution_time[64];
    if (options->has_initial_exec_time_ms) {
        snprintf(baseline_execution_time, sizeof(baseline_execution_time), "%.3f ms", options->initial_exec_time_ms);
    } else {
        snprintf(baseline_execution_time, sizeof(baseline_execution_time), "unknown");
    }
    if (rc != 0) {
        log_message(true, "INFO",
                "Tuning result: Failed\n"
                "Baseline execution time: %s\n"
                "Total tuning time: %.3f ms\n",
                baseline_execution_time,
                summary->total_tuning_wall_time_ms);
    } else {

        log_message(true, "INFO",
                "Tuning result: Success\n"
                "Baseline execution time: %s\n"
                "Optimized execution time: %.3f ms\n"
                "Total tuning time: %.3f ms\n",
                baseline_execution_time,
                summary->best_execution_time_ms,
                summary->total_tuning_wall_time_ms);
    }
}

/*
 * Brief: Verify that the original SQL can be executed before tuning.
 * Inputs: conn - Database connection; sql - Original SQL.
 * Inputs/Outputs: options - Receives the available initial execution time;
 *                 summary - Receives the final failure state.
 * Return: 0 when verification succeeds; 1 otherwise.
 */
static int verify_initial_plan(PGconn *conn,
                               HintAdviserOptions *options,
                               const char *sql,
                               HintAdviserRunSummary *summary)
{
    HintAdviserOutlineHints outline_hints = {0};
    List *explain_plan_nodes = NIL;
    double execution_time_ms = 0.0;

    if (options->has_initial_exec_time_ms) {
        summary->has_best_execution_time_ms = true;
        summary->best_execution_time_ms = options->initial_exec_time_ms;
        return 0;
    } else if (!options->verify_initial_plan) {
        return 0;
    }

    log_message(options->verbose, "INFO", "progress=verify_initial_plan_start");
    if (execute_explain_analyze(conn,
                                sql,
                                &outline_hints,
                                &explain_plan_nodes,
                                true,
                                &execution_time_ms,
                                options->deadline_end_time_usec,
                                summary)
        != 0) {
        pg_log_warning("stage=verify_initial_plan failed");
        return 1;
    }

    options->has_initial_exec_time_ms = true;
    options->initial_exec_time_ms = execution_time_ms;
    summary->has_best_execution_time_ms = true;
    summary->best_execution_time_ms = execution_time_ms;
    log_message(options->verbose, "INFO",
                "progress=verify_initial_plan_end status=success "
                "execution_time_ms=%.3f",
                execution_time_ms);
    return 0;
}

/*
 * Brief: Build a deliverable outline from the current rows hints.
 * Inputs: options - Tool options; sql - Original SQL; hint_set - Current rows hints.
 * Outputs: summary - Receives the fallback best candidate when it succeeds.
 */
static void make_outline_fallback_sql(const HintAdviserOptions *options,
                                           const char *sql,
                                           const RowsHintSet *hint_set,
                                           HintAdviserRunSummary *summary)
{
    if (options->enable_extoptimizer) {
        return;
    }
    PGconn *fallback_conn = connect_database(&options->connection);
    if (fallback_conn == NULL) {
        pg_log_warning("stage=sampling_outline_fallback failed to connect");
        return;
    }

    int64 deadline_end_time_usec =
        deadline_start(HINT_ADVISER_FALLBACK_TIMEOUT_MS);
    char *candidate_sql = build_hinted_sql(sql, hint_set, NULL, NULL);
    HintAdviserOutlineHints outline_hints = {0};
    List *explain_plan_nodes = NIL;
    if (execute_explain_analyze(fallback_conn,
                                     candidate_sql,
                                     &outline_hints,
                                     &explain_plan_nodes,
                                     false,
                                     NULL,
                                     deadline_end_time_usec,
                                     summary) != 0) {
        pg_log_warning("stage=sampling_outline_fallback EXPLAIN failed");
        goto finish;
    }

    char *outline_sql = build_hinted_sql(sql,
                                         NULL,
                                         NULL,
                                         &outline_hints);
    if (outline_sql == NULL) {
        pg_log_warning("stage=sampling_outline_fallback could not build outline SQL");
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        goto finish;
    }
    (void) update_best_candidate(summary, outline_sql, candidate_sql, 0.0, 0);

finish:
    PQfinish(fallback_conn);
}

/*
 * Description: Process one SQL until target time, convergence, patience,
 *              or timeout.
 * Inputs: conn - database connection; sql - original SQL.
 * Inputs/Outputs: options - Receives a verified initial execution time;
 *                 summary - Final flow state and best candidate.
 */
static void process_one_query(PGconn **connection,
                              HintAdviserOptions *options,
                              const char *sql,
                              const HintAdviserSqlArchive *archive,
                              HintAdviserRunSummary *summary)
{
    PGconn *conn = *connection;

    if (verify_initial_plan(conn, options, sql, summary) != 0) {
        return;
    }

    /* Initial verification is a pre-tuning gate. Keep the existing global
     * deadline, but exclude verification from the reported tuning duration. */
    pg_usec_time_t tuning_start_time = current_time_usec();
    iterationState iteration_state = {
        .iteration = 1
    };

    start_tuning_progress(tuning_start_time, options);

    ExtOptContext *extopt_context = NULL;
    Query *query = NULL;
    FrontendQueryInfo *query_tree_info = NULL;
    if (load_query_info(conn, options, sql, archive->external_optimizer_log_path,
                    &query_tree_info, &extopt_context, &query, summary) != 0) {
        pg_log_warning("stage=load_query_info failed");
        return;
    }

    RowsHintSet hint_set = {0};
    if (get_rowshints_by_dynamic_sampling(conn,
                                         options,
                                         sql,
                                         query_tree_info,
                                         &hint_set,
                                         summary) != 0) {
        pg_log_warning("stage=dynamic_sampling failed");
        /* Dynamic_sampling is not necessary, so just go on if failed. */
    }

    while (true) {
        bool complete_best = summary->best_iteration > 0 && summary->best_outline_sql != NULL;

        if (feedback_effective_timeout(options->explain_timeout_ms,
            options->deadline_end_time_usec, complete_best, summary->best_execution_time_ms) == 0)
        {
            summary->finish_reason = HINT_ADVISER_FINISH_TIME_BUDGET_REACHED;
            break;
        }
        iteration_state.candidate_sql = NULL;
        iteration_state.rows_hint_changed = false;

        if (optimize_sql_with_rows_hint(options, extopt_context, query, sql,
                                &hint_set, summary,
                                &iteration_state.candidate_sql) != 0) {
            pg_log_warning("stage=optimize_sql_with_rows_hint iteration=%d failed",
                           iteration_state.iteration);
            break;
        }
        Assert(iteration_state.candidate_sql != NULL);

        FeedbackResult feedback = feedback_round_execute(
            connection, sql, iteration_state.candidate_sql, query_tree_info,
            options, summary);

        conn = *connection;

        log_message(options->verbose, "INFO",
                    "progress=feedback_round iteration=%d outcome=%s "
                    "effective_timeout_ms=%d timeout_reason=%s",
                    iteration_state.iteration,
                    feedback_outcome_name(feedback.outcome),
                    feedback.effective_timeout_ms, feedback.timeout_reason);
        if (feedback.outcome == FEEDBACK_COMPLETE || feedback.outcome == FEEDBACK_PARTIAL)
        {
            iteration_state.rows_hint_changed = collect_rows_hints(feedback.plan,
                &hint_set, iteration_state.iteration, options->verbose);
        }
        bool stop = feedback_loop_advance(options, &iteration_state.feedback_loop,
            &feedback, iteration_state.candidate_sql, iteration_state.rows_hint_changed, summary);

        summary->total_tuning_wall_time_ms =
            elapsed_ms_between(tuning_start_time, current_time_usec());
        log_message(options->verbose, "INFO",
            "progress=iteration_end iteration=%d outcome=%s hint_added=%s hint_count=%d",
            iteration_state.iteration, feedback_outcome_name(feedback.outcome),
            iteration_state.rows_hint_changed ? "true" : "false", hint_set.count);
        if (stop)
        {
            break;
        }
        iteration_state.iteration++;
    }

    if (summary->best_outline_sql == NULL) {
        HintAdviserFinishReason reason_before_fallback = summary->finish_reason;

        make_outline_fallback_sql(options,
                                  sql,
                                  &hint_set,
                                  summary);
        if (reason_before_fallback == HINT_ADVISER_FINISH_ERROR)
        {
            /* A fallback plan cannot turn a failed candidate into success. */
            summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        }
    }
    capture_plantrace(conn, &options->connection, sql, archive, query_tree_info,
                      HINT_ADVISER_NO_DEADLINE, summary, options->verbose);
    summary->total_tuning_wall_time_ms = elapsed_ms_between(tuning_start_time, current_time_usec());
    ExtOptFreeContext(extopt_context);
    stop_tuning_progress();
}

/*
 * Description: Tool entry point
 * Inputs: argc - number of arguments; argv - argument array
 * Return: 0 on success, non-zero on failure
 */
int main(int argc, char **argv)
{
    HintAdviserOptions options = {0};
    HintAdviserRunSummary summary = {0};

    pg_logging_init(argv[0]);

    parse_options(argc, argv, &options);
    if (options.command_mode == HINT_ADVISER_COMMAND_DUMP_LOG) {
        return dump_log_tar_gz(&options);
    }
    summary.finish_reason = HINT_ADVISER_FINISH_ERROR;
	summary.external_outcome = HINT_ADVISER_EXTERNAL_NOT_ATTEMPTED;
	summary.plantrace_status = HINT_ADVISER_PLANTRACE_NOT_RUN;
    if (!options.has_query) {
        /* Option-only validation is expected to fail silently; real tuning
         * invocations always provide one SQL source. */
        return 1;
    }
    PGconn *conn = connect_database(&options.connection);
    if (conn == NULL) {
        pg_fatal("could not connect to database");
    }

    char *sql = read_query_input(&options.query);
    HintAdviserSqlArchive archive = {0};

    if (begin_sql_archive(&options,
        &options.query,
        &archive,
        &summary) != 0) {
        print_query_finish(&options, &summary, 1);
        PQfinish(conn);
        return 1;
    }
    process_one_query(&conn,
                      &options,
                      sql,
                      &archive,
                      &summary);
	hint_adviser_summary_finalize_delivery(&summary, &options);
	(void) append_run_summary(&archive, &options, &options.query, &summary);
	int rc = summary.delivered ? 0 : 1;
    if (summary.delivered) {
        print_output_separator();
        log_message(true, "INFO", "Optimized sql:\n%s", summary.best_outline_sql);
        printf("%s\n", summary.best_outline_sql);
        fflush(stdout);
    }
    print_query_finish(&options, &summary, rc);
    PQfinish(conn);
    return rc;
}
