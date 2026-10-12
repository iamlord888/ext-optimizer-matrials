/* One candidate's execution, cancellation, recovery and verified feedback. */
#include "postgres_fe.h"

#include <limits.h>
#include <math.h>

#include "hint_adviser_feedback_round.h"
#include "hint_adviser_partial_proof.h"
#include "hint_adviser_sql_log.h"
#include "hint_adviser_string.h"

#define FEEDBACK_FINALIZE_RESERVE_MS 8000
#define FEEDBACK_REPLAN_TIMEOUT_MS 5000
#define FEEDBACK_CANCEL_DELIVERY_GRACE_MS 500
#define FEEDBACK_CANCEL_GRACE_MS \
    (HAPF_SNAPSHOT_TIMEOUT_MS + FEEDBACK_CANCEL_DELIVERY_GRACE_MS)

/* Keep PQcancel behind the server timeout without overflowing the local budget. */
static int feedback_watchdog_timeout(int effective_timeout_ms)
{
    Assert(effective_timeout_ms > 0);
    if (effective_timeout_ms > INT_MAX - FEEDBACK_CANCEL_GRACE_MS)
    {
        return INT_MAX;
    }
    return effective_timeout_ms + FEEDBACK_CANCEL_GRACE_MS;
}

/* Return the stable log spelling of a round outcome. */
const char *feedback_outcome_name(FeedbackOutcome outcome)
{
    switch (outcome)
    {
        case FEEDBACK_COMPLETE:
            return "COMPLETE";
        case FEEDBACK_PARTIAL:
            return "PARTIAL";
        case FEEDBACK_TIMEOUT_NO_TRUSTED_NODES:
            return "TIMEOUT_NO_TRUSTED_NODES";
        case FEEDBACK_ERROR:
            return "ERROR";
    }
    return "ERROR";
}

/*
 * Central budget policy: keep eight seconds for cancellation/snapshot/replan.
 * Only a completed candidate may supply the adaptive 1.25x execution limit.
 * Return zero when no new round fits, or -1 for the legacy no-timeout path.
 */
int feedback_effective_timeout(int configured_ms, int64 search_deadline,
    bool has_complete_best, double best_time_ms)
{
    if (configured_ms == -1)
    {
        return search_deadline >= 0 && connection_monotonic_usec() >= search_deadline ? 0 : -1;
    }
    int64 available = search_deadline < 0 ? INT_MAX :
        (search_deadline - connection_monotonic_usec()) / 1000 - FEEDBACK_FINALIZE_RESERVE_MS;

    if (configured_ms <= 0 || available <= 0)
    {
        return 0;
    }
    int timeout = Min((int64) configured_ms, available);

    if (has_complete_best && isfinite(best_time_ms) && best_time_ms >= 0)
    {
        double adaptive = Max(1.0, floor(best_time_ms * 1.25));

        if (adaptive < timeout)
        {
            timeout = (int) adaptive;
        }
    }
    return timeout;
}

/* Copy a scalar result without permitting missing/extra rows or columns. */
static int feedback_scalar(PGresult *res, void *data)
{
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1 ||
        PQnfields(res) != 1 || PQgetisnull(res, 0, 0))
    {
        return 1;
    }
    *(char **) data = pstrdup(PQgetvalue(res, 0, 0));
    return 0;
}

/* Accept the single boolean success result returned while arming capture. */
static int feedback_true(PGresult *res, void *data)
{
    (void) data;
    return PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1 &&
        PQnfields(res) == 1 && !PQgetisnull(res, 0, 0) &&
        strcmp(PQgetvalue(res, 0, 0), "t") == 0 ? 0 : 1;
}

/* Run bounded setup/recovery SQL; these statements never become candidates. */
static bool feedback_command(PGconn *conn, const char *sql, int64 deadline)
{
    return exec_async(conn, sql, "feedback session", deadline, NULL, NULL, true) == 0;
}

/*
 * A cancelled query may have drained to an idle, healthy connection at the
 * exact global deadline.  There is no budget left for another SQL command;
 * treating the skipped session restore as a connection failure would discard
 * an already measured complete best.  The dirty session is closed before the
 * round returns, and the normal restore path remains required while there is
 * still global budget.
 */
static bool feedback_restore_session(PGconn *conn, const char *sql,
    int64 global_deadline, int64 phase_deadline, bool allow_expired_skip)
{
    if (allow_expired_skip && global_deadline >= 0 &&
        connection_monotonic_usec() >= global_deadline)
    {
        return true;
    }
    return feedback_command(conn, sql, phase_deadline);
}

/* Arm one execution-scoped capture. */
static bool feedback_arm(PGconn *conn, uint64_t id, int64 deadline)
{
    char *sql = psprintf("select extopt.partial_capability(%llu)",
                         (unsigned long long) id);
    bool valid = exec_async(conn, sql, "partial capability", deadline,
        feedback_true, NULL, true) == 0;

    pfree(sql);
    return valid;
}

/* Publish a measured complete result only after the plan and delivery SQL are ready. */
static FeedbackResult feedback_publish_complete(FeedbackResult result,
    PlanNode *plan, char *outline_sql, double elapsed)
{
    if (plan != NULL && outline_sql != NULL)
    {
        result.outcome = FEEDBACK_COMPLETE;
        result.plan = plan;
        result.outline_sql = outline_sql;
        result.execution_time_ms = elapsed;
    }
    return result;
}

/* The plain path retains legacy full execution behavior and does not require HAPF. */
static FeedbackResult feedback_complete(PGconn **connection, const char *original_sql,
    const char *candidate_sql, const FrontendQueryInfo *query_info,
    const HintAdviserOptions *options, HintAdviserRunSummary *summary)
{
    FeedbackResult result = {.outcome = FEEDBACK_ERROR, .effective_timeout_ms = -1,
                             .timeout_reason = "disabled"};
    HintAdviserOutlineHints outline = {0};
    List *pretty = NIL;
    double elapsed = 0.0;

    PGconn *conn = *connection;

    if (!feedback_command(conn, "SET explain_perf_mode=pretty", options->deadline_end_time_usec))
    {
        return result;
    }
    ConnectionExecution execution = connection_feedback_analyze(conn, candidate_sql,
        options->deadline_end_time_usec, &outline, &pretty, &elapsed);

    result.attempted = execution.sent;
    result.timeout_reason = execution.failure_reason;
    int64 recovery_deadline = connection_phase_deadline(options->deadline_end_time_usec, 3000);
    bool discard_connection = execution.query_cancelled && execution.healthy &&
        options->deadline_end_time_usec >= 0 &&
        connection_monotonic_usec() >= options->deadline_end_time_usec;

    if (!connection_recover_until(connection, &options->connection, !execution.healthy,
                                recovery_deadline) ||
        !feedback_restore_session(*connection, "SET explain_perf_mode=normal",
                                options->deadline_end_time_usec, recovery_deadline,
                                discard_connection))
    {
        result.timeout_reason = "connection_recovery_failed";
        return result;
    }
    conn = *connection;
    if (!execution.success)
    {
        if (execution.query_cancelled && execution.healthy && !execution.handler_failed)
        {
            /* With Partial disabled, global cancellation still preserves an
             * existing complete best through the normal timeout delivery gate. */
            result.outcome = FEEDBACK_TIMEOUT_NO_TRUSTED_NODES;
        }
        if (discard_connection)
        {
            PQfinish(*connection);
            *connection = NULL;
        }
        return result;
    }
    PlanNode *plan = NULL;
    char *outline_sql = NULL;

    if (build_explain_node_tree(conn, original_sql, &outline, query_info, pretty,
        &plan, &outline_sql, options->deadline_end_time_usec, summary) != 0)
    {
        return result;
    }
    return feedback_publish_complete(result, plan, outline_sql, elapsed);
}

/*
 * Interpret a fully drained, restored candidate result. Both non-ANALYZE
 * replans use exactly candidate_sql; only final Outline assembly uses the
 * original SQL. Failed snapshots/mappings publish no plan or execution time.
 */
static FeedbackResult feedback_resolve(PGconn *conn, const char *original_sql,
    const char *candidate_sql, const FrontendQueryInfo *query_info,
    ConnectionExecution execution, PartialSnapshot *snapshot,
    HintAdviserOutlineHints *outline, List *pretty, double elapsed,
    FeedbackResult result, HintAdviserRunSummary *summary,
    int64 global_deadline, bool allow_replan)
{
    if (!execution.success && (!execution.query_cancelled || !execution.healthy ||
        execution.handler_failed))
    {
        if (execution.handler_failed)
        {
            result.timeout_reason = "handler_failed";
        }
        return result;
    }
    if (snapshot->invalid)
    {
        result.timeout_reason = snapshot->failure_reason;
        return result;
    }
    if (execution.query_cancelled && !snapshot->begun)
    {
        result.outcome = FEEDBACK_TIMEOUT_NO_TRUSTED_NODES;
        return result;
    }
    if (execution.query_cancelled && !partial_snapshot_finalize(snapshot))
    {
        result.timeout_reason = snapshot->failure_reason;
        return result;
    }
    if (!allow_replan)
    {
        /* Exhaustion forbids more SQL, but cannot erase a protocol/handler
         * failure or turn an incomplete snapshot into a deliverable timeout. */
        result.outcome = FEEDBACK_TIMEOUT_NO_TRUSTED_NODES;
        result.timeout_reason = "global_deadline_after_cancel";
        return result;
    }
    int64 replan_deadline = connection_phase_deadline(global_deadline,
                                                    FEEDBACK_REPLAN_TIMEOUT_MS);

    if (execution.query_cancelled)
    {
        *outline = (HintAdviserOutlineHints) {0};
        pretty = NIL;
        if (execute_explain_analyze(conn, candidate_sql, outline, &pretty, false,
            NULL, replan_deadline, summary) != 0)
        {
            result.timeout_reason = "partial_pretty_replan_failed";
            return result;
        }
    }
    PlanNode *plan = NULL;

    if (candidate_plan_skeleton(conn, candidate_sql, pretty,
        &plan, replan_deadline, summary) != 0 || plan == NULL)
    {
        result.timeout_reason = "candidate_json_replan_failed";
        return result;
    }
    if (execution.query_cancelled)
    {
        PartialProofResult proof = partial_prove(snapshot, pretty, plan);

        log_message(false, "INFO", "diagnostic=partial_proof valid=%s trusted_nodes=%d reason=%s",
                    proof.valid ? "true" : "false", proof.trusted_nodes, proof.reason);
        if (!proof.valid)
        {
            result.timeout_reason = proof.reason;
            return result;
        }
        if (proof.trusted_nodes == 0)
        {
            result.outcome = FEEDBACK_TIMEOUT_NO_TRUSTED_NODES;
            return result;
        }
        result.outcome = FEEDBACK_PARTIAL;
    }
    if (candidate_plan_complete(plan, outline, query_info) != 0)
    {
        result.outcome = FEEDBACK_ERROR;
        result.timeout_reason = "candidate_alias_mapping_failed";
        return result;
    }
    if (execution.query_cancelled)
    {
        result.plan = plan;
        return result;
    }
    char *outline_sql = build_hinted_sql(original_sql, NULL, NULL, outline);

    if (outline_sql == NULL)
    {
        result.timeout_reason = "complete_outline_missing";
        return result;
    }
    return feedback_publish_complete(result, plan, outline_sql, elapsed);
}

/*
 * Own one feedback execution while borrowing the persistent connection. The
 * only connection mutation is recovery; metadata/query objects stay sealed.
 */
FeedbackResult feedback_round_execute(PGconn **conn, const char *original_sql,
    const char *candidate_sql, const FrontendQueryInfo *query_info,
    const HintAdviserOptions *options, HintAdviserRunSummary *summary)
{
    FeedbackResult result = {.outcome = FEEDBACK_ERROR, .timeout_reason = "setup_failed"};
    bool complete_best = summary->best_iteration > 0 && summary->best_outline_sql != NULL;

    result.effective_timeout_ms = feedback_effective_timeout(options->explain_timeout_ms,
        options->deadline_end_time_usec, complete_best, summary->best_execution_time_ms);
    if (result.effective_timeout_ms == 0)
    {
        result.timeout_reason = "global_budget_before_round";
        summary->finish_reason = HINT_ADVISER_FINISH_TIME_BUDGET_REACHED;
        return result;
    }
    if (candidate_sql == NULL || *candidate_sql == '\0' ||
        !connection_recover_until(conn, &options->connection, false,
                                connection_phase_deadline(options->deadline_end_time_usec, 3000)))
    {
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        return result;
    }
    if (options->explain_timeout_ms == -1)
    {
        return feedback_complete(conn, original_sql, candidate_sql, query_info, options, summary);
    }
    char *saved_timeout = NULL;
    int64 setup_deadline = connection_phase_deadline(options->deadline_end_time_usec, 3000);

    if (exec_async(*conn, "show statement_timeout", "save timeout", setup_deadline,
        feedback_scalar, &saved_timeout, true) != 0)
    {
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        return result;
    }
    char *quoted = PQescapeLiteral(*conn, saved_timeout, strlen(saved_timeout));

    if (quoted == NULL)
    {
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        return result;
    }
    char *restore = psprintf("SET statement_timeout=%s; SET explain_perf_mode=normal", quoted);

    PQfreemem(quoted);
    pfree(saved_timeout);
    static uint64_t last_id = 0;
    uint64_t id = Max((uint64_t) connection_monotonic_usec(), last_id + 1);

    last_id = id;
    bool ready = feedback_arm(*conn, id, setup_deadline);

    result.effective_timeout_ms = feedback_effective_timeout(options->explain_timeout_ms,
        options->deadline_end_time_usec, complete_best, summary->best_execution_time_ms);
    char *setup = psprintf("SET explain_perf_mode=pretty; SET statement_timeout=%d",
                           result.effective_timeout_ms);

    ready = ready && result.effective_timeout_ms > 0 && feedback_command(*conn, setup, setup_deadline);
    PartialSnapshot snapshot;
    bool discard_connection = false;

    partial_snapshot_init(&snapshot, id);
    if (ready)
    {
        PQsetNoticeReceiver(*conn, connection_notice_receiver, &snapshot);
    }
    HintAdviserOutlineHints outline = {0};
    List *pretty = NIL;
    double elapsed = 0.0;
    ConnectionExecution execution = {.failure_reason = "capability_or_setup_failed"};

    if (ready)
    {
        int64 watchdog = connection_phase_deadline(options->deadline_end_time_usec,
            feedback_watchdog_timeout(result.effective_timeout_ms));

        execution = connection_feedback_analyze(*conn, candidate_sql, watchdog, &outline,
                                              &pretty, &elapsed);
        result.attempted = execution.sent;
    }
    PQsetNoticeReceiver(*conn, connection_notice_receiver, NULL);
    result.timeout_reason = execution.failure_reason;
    int64 recovery_deadline = connection_phase_deadline(options->deadline_end_time_usec, 3000);
    bool skip_expired_cleanup = execution.query_cancelled && execution.healthy &&
        options->deadline_end_time_usec >= 0 &&
        connection_monotonic_usec() >= options->deadline_end_time_usec;

    discard_connection = skip_expired_cleanup;
    /* PQcancel can reach the backend after its terminal result. Never let that
     * delayed signal affect cleanup SQL or the next feedback round. */
    bool force_reconnect = ready && !skip_expired_cleanup &&
        (!execution.healthy || execution.cancel_sent);
    bool recovered = skip_expired_cleanup ||
        connection_recover_until(conn, &options->connection, force_reconnect,
                               recovery_deadline);

    if (recovered && !skip_expired_cleanup)
    {
        recovered = feedback_command(*conn, "select extopt.partial_capability(0)",
                                    recovery_deadline) &&
            feedback_restore_session(*conn, restore, options->deadline_end_time_usec,
                                   recovery_deadline, false);
    }
    if (ready && recovered)
    {
        result = feedback_resolve(*conn, original_sql, candidate_sql, query_info,
            execution, &snapshot, &outline, pretty, elapsed, result, summary,
            options->deadline_end_time_usec, !discard_connection);
    }
    else if (!recovered)
    {
        result.timeout_reason = "connection_recovery_failed";
    }
    if (discard_connection && *conn != NULL)
    {
        /* The session settings were not restored because the global budget was
         * exhausted; never expose that dirty connection to a later phase. */
        PQfinish(*conn);
        *conn = NULL;
    }
    partial_snapshot_free(&snapshot);
    pfree(setup);
    pfree(restore);
    if (result.outcome == FEEDBACK_ERROR)
    {
        if (result.effective_timeout_ms == 0 && !result.attempted)
        {
            result.timeout_reason = "global_budget_before_round";
            summary->finish_reason = HINT_ADVISER_FINISH_TIME_BUDGET_REACHED;
        }
        else
        {
            summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        }
    }
    return result;
}
