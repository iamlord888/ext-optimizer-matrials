/*-------------------------------------------------------------------------
 *
 * hint_adviser_connection.c
 *    Database connection and SQL text helpers for hint_adviser.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    contrib/hint_adviser/hint_adviser_connection.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * The connection module handles database connections, nonblocking libpq
 * execution, and SQL text boundaries.
 *
 * Key entry points:
 * connect_database()   Create libpq connection and apply GaussDB
 *                                  optimizer session settings
 * exec_async()         Execute one SQL command through nonblocking libpq
 * execute_explain_analyze()    Run pretty EXPLAIN OUTLINE/BLOCKNAME,
 *                                  optionally with ANALYZE, and collect
 *                                  outline plus pretty plan rows
 *
 * Reading guide:
 * Start with connect_database for connection ownership, then exec_async for
 * query execution, then explain_analyze for GaussDB plan metadata collection.
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "common/connect.h"
#include "common/string.h"
#include "libpq/pqcomm.h"
#include "hint_adviser_log.h"
#include "mb/pg_wchar.h"

#include "hint_adviser_connection.h"
#include "hint_adviser_plan.h"
#include "hint_adviser_sql_log.h"
#include "hint_adviser_string.h"
#include "hint_adviser_partial_snapshot.h"
#include "hint_adviser_pretty_parser.h"

#define HINT_ADVISER_THINKING_INTERVAL_USEC 1000000
#define HINT_ADVISER_THINKING_CLEAR_WIDTH 64

typedef struct HintAdviserThinkingProgress
{
    bool enabled;
    bool printed;
    int64 start_time_usec;
    int64 next_report_time_usec;
} HintAdviserThinkingProgress;

static HintAdviserThinkingProgress thinking_progress = {0};

/*
 * Brief: Read elapsed-time clock shared by deadlines, duration and watchdogs.
 * Return: CLOCK_MONOTONIC microseconds; wall dates remain in the log module.
 */
int64 connection_monotonic_usec(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        pg_fatal("could not read current time: %m");
    }
    return ((int64) now.tv_sec * 1000000) + now.tv_nsec / 1000;
}

/* Intersect a local phase budget with the enclosing search deadline. */
int64 connection_phase_deadline(int64 global_deadline, int local_timeout_ms)
{
    int64 local_deadline = deadline_start(local_timeout_ms);

    if (global_deadline < 0)
    {
        return local_deadline;
    }
    return Min(global_deadline, local_deadline);
}

/*
 * Brief: Refresh the feedback execution progress line when one second passes.
 * Inputs: now_usec - Current monotonic timestamp.
 * Inputs/Outputs: progress - Thinking progress state.
 */
static void report_thinking_progress(HintAdviserThinkingProgress *progress,
                                     int64 now_usec)
{
    if (progress == NULL || !progress->enabled ||
        now_usec < progress->next_report_time_usec) {
        return;
    }

    fprintf(stderr,
            "\rTuningAgent Thinking %.0f s",
            (double) (now_usec - progress->start_time_usec) / 1000000.0);
    fflush(stderr);
    progress->printed = true;
    do {
        progress->next_report_time_usec +=
            HINT_ADVISER_THINKING_INTERVAL_USEC;
    } while (progress->next_report_time_usec <= now_usec);
}

/*
 * Brief: Finish a previously printed feedback execution progress line.
 * Inputs: progress - Thinking progress state.
 */
static void finish_thinking_progress(HintAdviserThinkingProgress *progress)
{
    if (progress != NULL && progress->printed) {
        fputc('\n', stderr);
        fflush(stderr);
        progress->printed = false;
    }
}

/*
 * Brief: Start one process-wide tuning progress clock.
 * Inputs: start_time_usec - Overall tuning start timestamp.
 */
void start_tuning_progress(int64 start_time_usec, HintAdviserOptions *options)
{
    if (options->verbose) {
        print_output_separator();
        log_message(true, "INFO", "Tuning step:");
    }
    thinking_progress.enabled = true;
    thinking_progress.printed = false;
    thinking_progress.start_time_usec = start_time_usec;
    thinking_progress.next_report_time_usec =
        start_time_usec + HINT_ADVISER_THINKING_INTERVAL_USEC;
}

/*
 * Brief: Stop the process-wide tuning progress clock.
 */
void stop_tuning_progress(void)
{
    finish_thinking_progress(&thinking_progress);
    thinking_progress.enabled = false;
}

/*
 * Brief: Clear the current progress line before a verbose stderr message.
 */
void clear_tuning_progress_line(void)
{
    if (!thinking_progress.enabled || !thinking_progress.printed) {
        return;
    }

    fprintf(stderr, "\r%*s\r", HINT_ADVISER_THINKING_CLEAR_WIDTH, "");
    fflush(stderr);
    thinking_progress.printed = false;
}

/*
 * Brief: Start one monotonic deadline.
 * Inputs: timeout_ms - Positive timeout in milliseconds, or -1 when disabled.
 * Return: Absolute deadline timestamp in microseconds, or no-deadline marker.
 */
int64 deadline_start(int timeout_ms)
{
    int64 now;

    if (timeout_ms == HINT_ADVISER_NO_DEADLINE) {
        return HINT_ADVISER_NO_DEADLINE;
    }
    Assert(timeout_ms > 0);

    now = connection_monotonic_usec();
    if (now > PG_INT64_MAX - ((int64) timeout_ms * 1000)) {
        return PG_INT64_MAX;
    }
    return now + ((int64) timeout_ms * 1000);
}

/*
 * Brief: Wait for libpq socket readiness until a deadline.
 * Inputs: conn - Database connection; for_read/for_write - requested readiness;
 *         deadline_end_time_usec - optional query deadline.
 * Return: 1 when ready, 0 on timeout, -1 on socket error.
 */
static int wait_for_socket(PGconn *conn,
                           bool for_read,
                           bool for_write,
                           int64 deadline_end_time_usec)
{
    int sock;

    if (deadline_end_time_usec >= 0 &&
        connection_monotonic_usec() >= deadline_end_time_usec) {
        return 0;
    }

    sock = PQsocket(conn);
    if (sock < 0) {
        pg_log_error("libpq connection has no valid socket");
        return -1;
    }

    for (;;) {
        fd_set input_mask;
        fd_set output_mask;
        fd_set *input = NULL;
        fd_set *output = NULL;
        struct timeval timeout;
        struct timeval *timeout_ptr = NULL;
        int rc;

        if (for_read) {
            FD_ZERO(&input_mask);
            FD_SET(sock, &input_mask);
            input = &input_mask;
        }
        if (for_write) {
            FD_ZERO(&output_mask);
            FD_SET(sock, &output_mask);
            output = &output_mask;
        }
        int64 now = connection_monotonic_usec();
        int64 wait_until_usec = deadline_end_time_usec;

        report_thinking_progress(&thinking_progress, now);
        if (deadline_end_time_usec >= 0 && now >= deadline_end_time_usec) {
            return 0;
        }
        if (thinking_progress.enabled &&
            (wait_until_usec < 0 ||
             thinking_progress.next_report_time_usec < wait_until_usec)) {
            wait_until_usec = thinking_progress.next_report_time_usec;
        }
        if (wait_until_usec >= 0) {
            int64 remaining = wait_until_usec - now;

            if (remaining < 0) {
                remaining = 0;
            }
            timeout.tv_sec = remaining / 1000000;
            timeout.tv_usec = remaining % 1000000;
            timeout_ptr = &timeout;
        }

        rc = select(sock + 1, input, output, NULL, timeout_ptr);
        if (rc > 0) {
            return 1;
        }
        if (rc == 0) {
            now = connection_monotonic_usec();
            if (deadline_end_time_usec >= 0 && now >= deadline_end_time_usec) {
                return 0;
            }
            report_thinking_progress(&thinking_progress, now);
            if (thinking_progress.enabled) {
                continue;
            }
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        pg_log_error("libpq socket wait failed: %s", strerror(errno));
        return -1;
    }
}

/*
 * Brief: Send cancellation for one expired query.
 * Inputs: conn - Database connection; context - caller context.
 * Outputs: SQL log records cancellation state.
 */
static bool cancel_expired_query(PGconn *conn,
                                 const char *context,
                                 bool log_to_sql_log)
{
    char errbuf[256];
    PGcancel *cancel;

    errbuf[0] = '\0';
    cancel = PQgetCancel(conn);
    if (cancel == NULL) {
        if (log_to_sql_log) {
            log_timeout(context, "deadline_reached", false,
                        "PQgetCancel failed");
        }
        return false;
    }

    if (PQcancel(cancel, errbuf, sizeof(errbuf)) == 0) {
        if (log_to_sql_log) {
            log_timeout(context, "deadline_reached", false, errbuf);
        }
        PQfreeCancel(cancel);
        return false;
    } else {
        if (log_to_sql_log) {
            log_timeout(context, "deadline_reached", true, NULL);
        }
    }
    PQfreeCancel(cancel);
    return true;
}

/*
 * Execute one query, retaining its terminal SQLSTATE and draining after
 * cancellation. A sent cancel never stands in for a server result. All waits
 * after cancellation or a handler error use an independent finite deadline.
 */
ConnectionExecution connection_execute(PGconn *conn, const char *sql,
    const char *context, int64 deadline, int drain_timeout_ms,
    HintAdviserResultHandler handler, void *data, bool log_to_sql_log)
{
    ConnectionExecution execution = {.failure_reason = "none"};
    int old_nonblocking = PQisnonblocking(conn);
    int64 active_deadline = deadline;
    bool draining = false;
    bool transport_failed = false;
    bool result_failed = false;

    if (deadline >= 0 && connection_monotonic_usec() >= deadline)
    {
        execution.failure_reason = "deadline_before_send";
        execution.healthy = PQstatus(conn) == CONNECTION_OK &&
            PQtransactionStatus(conn) == PQTRANS_IDLE;
        return execution;
    }
    if (PQstatus(conn) != CONNECTION_OK || PQtransactionStatus(conn) == PQTRANS_ACTIVE ||
        PQtransactionStatus(conn) == PQTRANS_UNKNOWN || PQsetnonblocking(conn, 1) != 0)
    {
        execution.failure_reason = "connection_not_idle";
        return execution;
    }
    if (PQsendQuery(conn, sql) == 0)
    {
        execution.failure_reason = "send_failed";
        (void) PQsetnonblocking(conn, old_nonblocking);
        return execution;
    }
    execution.sent = true;
    bool flushed = false;
    bool finished = false;

    while (!finished && !transport_failed)
    {
        if (!flushed)
        {
            int flush = PQflush(conn);

            if (flush < 0)
            {
                execution.failure_reason = "flush_failed";
                transport_failed = true;
                continue;
            }
            flushed = flush == 0;
        }
        if (flushed && !PQisBusy(conn))
        {
            PGresult *res = PQgetResult(conn);

            if (res == NULL)
            {
                finished = true;
                continue;
            }
            ExecStatusType status = PQresultStatus(res);

            execution.terminal_seen = true;
            if (log_to_sql_log)
            {
                log_result(conn, context, res);
            }
            if (!result_status_accepted(status))
            {
                const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

                result_failed = true;
                if (sqlstate != NULL)
                {
                    strlcpy(execution.sqlstate, sqlstate, sizeof(execution.sqlstate));
                }
                execution.query_cancelled = sqlstate != NULL &&
                    strcmp(sqlstate, "57014") == 0;
                execution.failure_reason = execution.query_cancelled ?
                    (execution.cancel_sent ? "client_cancel" : "server_cancel") : "sql_error";
            }
            else if (!execution.handler_failed && handler != NULL &&
                     handler(res, data) != 0)
            {
                execution.handler_failed = true;
                execution.failure_reason = "handler_failed";
            }
            if (!draining && (result_failed || execution.handler_failed))
            {
                draining = true;
                active_deadline = deadline_start(drain_timeout_ms);
            }
            PQclear(res);
            continue;
        }
        int wait = wait_for_socket(conn, true, !flushed, active_deadline);

        if (wait == 0 && !draining)
        {
            execution.cancel_sent = cancel_expired_query(conn, context, log_to_sql_log);
            if (!execution.cancel_sent)
            {
                execution.failure_reason = "cancel_failed";
                transport_failed = true;
                continue;
            }
            draining = true;
            active_deadline = deadline_start(drain_timeout_ms);
            continue;
        }
        else if (wait <= 0)
        {
            execution.failure_reason = wait == 0 ? "drain_timeout" : "socket_failed";
            transport_failed = true;
            continue;
        }
        if (PQconsumeInput(conn) == 0)
        {
            execution.failure_reason = "consume_failed";
            transport_failed = true;
        }
    }
    execution.healthy = !transport_failed && finished &&
        PQstatus(conn) == CONNECTION_OK && PQtransactionStatus(conn) == PQTRANS_IDLE;
    if (PQsetnonblocking(conn, old_nonblocking) != 0)
    {
        execution.healthy = false;
        transport_failed = true;
        execution.failure_reason = "blocking_restore_failed";
    }
    /* Generic callers such as plantrace deliberately execute inside BEGIN.
     * Candidate reuse remains stricter: ConnectionRecover requires idle. */
    execution.success = !transport_failed && finished && PQstatus(conn) == CONNECTION_OK &&
        execution.terminal_seen &&
        !result_failed && !execution.handler_failed && !execution.cancel_sent;
    if (!execution.success && strcmp(execution.failure_reason, "none") == 0)
    {
        execution.failure_reason = "missing_terminal_or_nonidle";
    }
    return execution;
}

/* Preserve the legacy SQL interface while using the bounded execution engine. */
int exec_async(PGconn *conn, const char *sql, const char *context,
                int64 deadline, HintAdviserResultHandler handler, void *data,
                bool log_to_sql_log)
{
    ConnectionExecution execution = connection_execute(conn, sql, context,
        deadline, 3000, handler, data, log_to_sql_log);

    return execution.success ? 0 : 1;
}

/* Log every structured notice; route only HAPF primary messages to this round. */
void connection_notice_receiver(void *arg, const PGresult *notice)
{
    const char *primary = PQresultErrorField(notice, PG_DIAG_MESSAGE_PRIMARY);

    log_server_notice(NULL, PQresultErrorMessage(notice));
    if (arg != NULL && primary != NULL && strncmp(primary, "HAPF ", 5) == 0)
    {
        (void) partial_snapshot_accept((PartialSnapshot *) arg, primary);
    }
}

/*
 * Description: Attempt one database connection with the given password
 * Inputs: options - connection options; password - password
 * Return: database connection
 */
static PGconn * connect_once(const HintAdviserConnectionOptions *options, const char *password)
{
    const char *keywords[] = {
        "dbname",
        "host",
        "port",
        "user",
        "connect_timeout",
        "password",
        "compatibility",
        "fallback_application_name",
        NULL
    };
    const char *values[] = {
        options->database,
        options->host,
        options->port,
        options->user,
        options->connect_timeout,
        password,
        "gaussdb",
        options->progname,
        NULL
    };

    return PQconnectdbParams(keywords, values, true);
}

/*
 * Finish session setup on an already connected PGconn, or establish an initial
 * connection when started is NULL. Only initial connections may prompt.
 * session_deadline bounds recovery SQL; return NULL after closing on failure.
 */
static PGconn * connect_database_until_internal(
    const HintAdviserConnectionOptions *options, PGconn *started,
    int64 session_deadline)
{
    PGconn *conn = NULL;
    char *password = options->password == NULL ? NULL : pstrdup(options->password);
    bool new_pass;

    do {
        new_pass = false;
        conn = started == NULL ? connect_once(options, password) : started;
        if (conn == NULL) {
            pg_log_error("connection to database failed: out of memory");
            return NULL;
        }

        if (PQstatus(conn) != CONNECTION_OK &&
            session_deadline < 0 && options->password_mode != HINT_ADVISER_PASSWORD_NEVER &&
            password == NULL &&
            PQconnectionNeedsPassword(conn)) {
            PQfinish(conn);
            password = simple_prompt("Password: ", false);
            new_pass = true;
        }
    } while (new_pass);

	if (PQstatus(conn) != CONNECTION_OK) {
		pg_log_error("%s", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	PQsetNoticeReceiver(conn, connection_notice_receiver, NULL);

    const char *schema_option = options->schema == NULL ? "" :
        psprintf("SET search_path TO %s;\n",
                 escape_identifier_or_fatal(conn, options->schema));
    char *session_options = psprintf("%sSET query_dop = 1;\n"
                                     "SET htap_router_mode = 'row';\n"
                                     "SET cost_param = 2",
                                     schema_option);
    if (logged_exec_each_result(conn,
                                session_options,
                                "SET session options for plan acquisition",
                                session_deadline,
                                command_result_handler,
                                "SET session options for plan acquisition",
                                true) != 0) {
		PQfinish(conn);
		return NULL;
	}

    return conn;
}

PGconn *connect_database(const HintAdviserConnectionOptions *options)
{
    return connect_database_until_internal(options, NULL, HINT_ADVISER_NO_DEADLINE);
}

/*
 * Reconnect to the selected endpoint using its resolved address and complete
 * libpq options (including TLS/auth). The old PGconn is closed before starting
 * its replacement. No DNS lookup or interactive prompt is allowed here.
 * Return NULL on expiry, missing endpoint identity or connection failure.
 */
static PGconn *connection_reconnect_start(PGconn **old, int64 deadline)
{
    if (*old == NULL)
    {
        return NULL;
    }
    PQconninfoOption *info = PQconninfo(*old);
    char *host = pstrdup(PQhost(*old));
    char *address = pstrdup(PQhostaddr(*old));
    char *port = pstrdup(PQport(*old));
    char *password = pstrdup(PQpass(*old));
    struct in6_addr numeric;

    if (*address == '\0' &&
        (inet_pton(AF_INET, host, &numeric) == 1 ||
         inet_pton(AF_INET6, host, &numeric) == 1))
    {
        pfree(address);
        address = pstrdup(host);
    }
    bool endpoint_known = *address != '\0' || is_unixsock_path(host);

    PQfinish(*old);
    *old = NULL;
    int count = 0;

    if (info != NULL)
    {
        while (info[count].keyword != NULL)
        {
            count++;
        }
    }
    const char **keywords = palloc0(sizeof(char *) * (count + 5));
    const char **values = palloc0(sizeof(char *) * (count + 5));

    int used = 0;

    for (int i = 0; i < count; i++)
    {
        if (strcmp(info[i].keyword, "host") != 0 &&
            strcmp(info[i].keyword, "hostaddr") != 0 &&
            strcmp(info[i].keyword, "port") != 0 &&
            strcmp(info[i].keyword, "password") != 0)
        {
            keywords[used] = info[i].keyword;
            values[used++] = info[i].val;
        }
    }
    count = used;
    /* Override multihost input with the endpoint of the preceding session. */
    keywords[count] = "host";
    values[count++] = host;
    keywords[count] = "hostaddr";
    values[count++] = address;
    keywords[count] = "port";
    values[count++] = port;
    keywords[count] = "password";
    values[count++] = password;
    PGconn *conn = NULL;

    if (info != NULL && endpoint_known && connection_monotonic_usec() < deadline)
    {
        conn = PQconnectStartParams(keywords, values, false);
    }
    else
    {
        pg_log_error("recovery deadline expired or resolved endpoint unavailable");
    }
    PQconninfoFree(info);
    pfree(keywords);
    pfree(values);
    pfree(host);
    pfree(address);
    pfree(port);
    explicit_bzero(password, strlen(password));
    pfree(password);
    if (conn == NULL)
    {
        return NULL;
    }
    PostgresPollingStatusType status = PGRES_POLLING_WRITING;

    while (connection_monotonic_usec() < deadline)
    {
        if (wait_for_socket(conn, status == PGRES_POLLING_READING,
                            status == PGRES_POLLING_WRITING, deadline) <= 0)
        {
            break;
        }
        status = PQconnectPoll(conn);
        if (status == PGRES_POLLING_OK)
        {
            if (connection_monotonic_usec() < deadline)
            {
                return conn;
            }
            break;
        }
        if (status == PGRES_POLLING_FAILED)
        {
            pg_log_error("%s", PQerrorMessage(conn));
            break;
        }
    }
    /* Startup has no query to cancel; closing the half-open socket releases it. */
    PQfinish(conn);
    return NULL;
}

/* Reuse an idle connection, or bound all recovery stages by a finite deadline. */
bool connection_recover_until(PGconn **conn,
    const HintAdviserConnectionOptions *options, bool force_reconnect,
    int64 deadline)
{
    Assert(deadline >= 0);
    if (!force_reconnect && *conn != NULL && PQstatus(*conn) == CONNECTION_OK &&
        PQtransactionStatus(*conn) == PQTRANS_IDLE)
    {
        return true;
    }
    if (connection_monotonic_usec() >= deadline)
    {
        log_message(false, "INFO", "diagnostic=connection_recovery result=deadline_expired");
        if (*conn != NULL)
        {
            PQfinish(*conn);
            *conn = NULL;
        }
        return false;
    }
    PGconn *started = connection_reconnect_start(conn, deadline);

    if (started != NULL)
    {
        *conn = connect_database_until_internal(options, started, deadline);
    }
    log_message(false, "INFO", "diagnostic=connection_recovery result=%s",
                *conn == NULL ? "failed" : "reconnected");
    return *conn != NULL;
}

/*
 * Description: Append one parsed outline hint
 * Inputs: outline_hints - parsed outline hints; hint - hint text
 * Outputs: outline_hints - parsed outline hints with one appended item
 */
static void append_outline_hint(HintAdviserOutlineHints *outline_hints,
								const char *hint)
{
	if (outline_hints->items == NULL)
		outline_hints->items = palloc_array(char *, outline_hints->count + 1);
	else
		outline_hints->items = repalloc_array(outline_hints->items,
											  char *,
											  outline_hints->count + 1);
	outline_hints->items[outline_hints->count] = pstrdup(hint);
	outline_hints->count++;
}

/*
 * Brief: Combined EXPLAIN OUTLINE and pretty row parse state.
 * Description: The multi-result EXPLAIN output contains both one-column
 *              outline sections and table-shaped pretty plan sections.
 */
typedef struct OutlinePrettyRowsParseState
{
    HintAdviserOutlineHints *outline_hints; /* Parsed full Outline Data lines */
    List                  **explain_plan_nodes; /* Parsed pretty plan rows */
    double                 *execution_time_ms; /* Pretty Query Summary runtime */
    bool                    runtime_seen;
} OutlinePrettyRowsParseState;

/*
 * Brief: Dynamic sampling relation-size query result.
 */
typedef struct RelationBlocksResult
{
    double relation_blocks;     /* Heap block count used to choose sample percent */
} RelationBlocksResult;

/*
 * Brief: Dynamic sampling SQL parse callback state.
 */
typedef struct SamplingSqlResult
{
    const char *relation_name;  /* Target relation for diagnostics */
    const char *alias_name;     /* Target alias for diagnostics */
    const char *block_name;     /* Target query block for diagnostics */
    SamplingResult *result;     /* Parsed sampled row counts */
} SamplingSqlResult;

/*
 * Description: Parse full outline data lines from EXPLAIN (OUTLINE) text rows.
 * Inputs: res - EXPLAIN result with one text column
 * Outputs: outline_hints - parsed outline data lines
 */
static int parse_outline_hints(PGresult *res,
                               HintAdviserOutlineHints *outline_hints)
{
    bool in_outline_data = false;

    if (PQnfields(res) != 1) {
        pg_log_error("EXPLAIN OUTLINE returned unexpected result shape: columns=%d",
                     PQnfields(res));
        return 1;
    }
    for (int i = 0; i < PQntuples(res); i++) {
        char *line_copy = pstrdup(PQgetvalue(res, i, 0));
        char *line = trim_whitespace(line_copy);

        if (strcmp(line, "begin_outline_data") == 0) {
            in_outline_data = true;
        }
        if (in_outline_data) {
            append_outline_hint(outline_hints, line);
        }
        if (strcmp(line, "end_outline_data") == 0) {
            break;
        }
    }
    return 0;
}

/*
 * Brief: Parse extopt.query_source(sql) result.
 * Inputs: res - Query result; data - Output char **.
 * Outputs: data - Receives owned query tree source.
 * Return: 0 on success, 1 on malformed result.
 */
static int parse_query_source_result(PGresult *res, void *data)
{
    char **query_source = data;

    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1 ||
        PQnfields(res) != 1) {
        pg_log_error("extopt.query_source returned unexpected result: status %d ntuples %d nfields %d",
                     PQresultStatus(res),
                     PQntuples(res),
                     PQnfields(res));
        return 1;
    }
    if (PQgetisnull(res, 0, 0)) {
        pg_log_error("extopt.query_source returned NULL");
        return 1;
    }
    *query_source = pstrdup(PQgetvalue(res, 0, 0));
    return 0;
}

/*
 * Brief: Parse one dynamic sampling relation-size result.
 * Inputs: res - Query result; data - RelationBlocksResult pointer.
 * Outputs: data - Receives relation block count.
 * Return: 0 on success, 1 on malformed result.
 */
static int parse_relation_blocks_result(PGresult *res, void *data)
{
    RelationBlocksResult *state = data;

    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1 ||
        PQnfields(res) != 1) {
        return 1;
    }
    state->relation_blocks = parse_double_field(res, 0, 0, "relation_blocks");
    return state->relation_blocks < 0 ? 1 : 0;
}

/*
 * Brief: Parse one dynamic sampling count result.
 * Inputs: res - Query result; data - SamplingSqlResult pointer.
 * Outputs: data - Receives sampled total and filtered row counts.
 * Return: 0 on success, 1 on malformed result.
 */
static int parse_sampling_sql_result(PGresult *res, void *data)
{
    SamplingSqlResult *state = data;

    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) != 1 ||
        PQnfields(res) != 2) {
        const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);

        log_sampling_result_skip(state->relation_name,
                                 state->alias_name,
                                 state->block_name,
                                 "sampling SQL execution",
                                 sqlstate,
                                 PQresultErrorMessage(res));
        return 1;
    }

    state->result->sampled_total_rows =
        parse_double_field(res, 0, 0, "sampled_total_rows");
    state->result->sampled_rows =
        parse_double_field(res, 0, 1, "sampled_rows");
    return 0;
}

/*
 * Brief: Execute extopt.query_source(sql).
 * Inputs: conn - Database connection; sql - Original SQL;
 *         deadline_end_time_usec - Optional query deadline.
 * Outputs: query_source - Receives query tree text on success;
 *          error_message - Receives a diagnostic on failure.
 * Return: External-optimizer status for this provider operation.
 */
ExtOptStatus fetch_query_tree_source(PGconn *conn,
                                     const char *sql,
                                     int64 deadline_end_time_usec,
                                     char **query_source,
                                     char **error_message)
{
    Assert(conn != NULL);
    Assert(sql != NULL);
    Assert(query_source != NULL);
    Assert(error_message != NULL);

    *query_source = NULL;
    *error_message = NULL;
    char *quoted_sql = escape_literal_or_fatal(conn, sql);
    char *command = psprintf("SELECT query_payload FROM extopt.query_source(%s)",
                             quoted_sql);

    if (logged_exec_each_result(conn,
                                command,
                                "extopt query tree",
                                deadline_end_time_usec,
                                parse_query_source_result,
                                query_source,
                                true) != 0) {
        ExtOptStatus status;

        if (PQstatus(conn) != CONNECTION_OK) {
            status = EXTOPT_STATUS_CONNECTION_BAD;
        } else if (deadline_end_time_usec >= 0 &&
                   connection_monotonic_usec() >= deadline_end_time_usec) {
            status = EXTOPT_STATUS_DEADLINE_EXCEEDED;
        } else {
            status = EXTOPT_STATUS_PROVIDER_ERROR;
        }

        const char *provider_error = PQerrorMessage(conn);
        *error_message = psprintf(
            "fetch_query_tree_source failed while executing extopt.query_source: %s",
            provider_error != NULL && provider_error[0] != '\0' ?
            provider_error : "query execution or result validation failed");
        return status;
    }
    Assert(*query_source != NULL);
    return EXTOPT_STATUS_OK;
}

/*
 * Brief: Read current heap block count for one relation.
 * Inputs: conn - Database connection; options - Tool options;
 *         schema_name - Optional schema name; relation_name - Relation name.
 * Return: At least one relation block, or -1 on query failure.
 */
double fetch_relation_blocks(PGconn *conn,
                             const HintAdviserOptions *options,
                             const char *schema_name,
                             const char *relation_name)
{
    char *qualified_relation =
        quote_qualified_relation(conn, schema_name, relation_name);
    char *relation_literal = PQescapeLiteral(conn,
                                             qualified_relation,
                                             strlen(qualified_relation));
    if (relation_literal == NULL) {
        pg_fatal("out of memory");
    }

    char *command = psprintf("SELECT greatest(\n"
                             "         1.0,\n"
                             "         ceil(pg_relation_size(%s::regclass)::float8 /\n"
                             "              current_setting('block_size')::float8)\n"
                             "       ) AS relation_blocks",
                             relation_literal);
    RelationBlocksResult result = {0};
    if (logged_exec_each_result(conn,
                                command,
                                "DYNAMIC SAMPLING RELATION SIZE",
                                options->deadline_end_time_usec,
                                parse_relation_blocks_result,
                                &result,
                                true) != 0) {
        return -1;
    }
    return result.relation_blocks;
}

/*
 * Brief: Execute one dynamic sampling SQL.
 * Inputs: conn - Database connection; options - Tool options;
 *         relation_name/alias_name/block_name - Sampling target diagnostics;
 *         sampling_sql - SQL to execute.
 * Outputs: result - Parsed total and filtered sampled rows.
 * Return: Whether sampling produced a usable result.
 */
bool execute_sampling_sql(PGconn *conn,
                          const HintAdviserOptions *options,
                          const char *relation_name,
                          const char *alias_name,
                          const char *block_name,
                          const char *sampling_sql,
                          SamplingResult *result)
{
    SamplingSqlResult sql_result = {
        relation_name,
        alias_name,
        block_name,
        result
    };
    if (logged_exec_each_result(conn,
                                sampling_sql,
                                "DYNAMIC SAMPLING",
                                options->deadline_end_time_usec,
                                parse_sampling_sql_result,
                                &sql_result,
                                true) != 0) {
        return false;
    }

    if (result->sampled_rows < 0 ||
        result->sampled_total_rows < 0 ||
        result->sampled_rows > result->sampled_total_rows) {
        log_sampling_result_skip(sql_result.relation_name,
                                 sql_result.alias_name,
                                 sql_result.block_name,
                                 "sampling SQL result",
                                 NULL,
                                 "invalid sampled counts");
        return false;
    }
    return true;
}

/*
 * Brief: Find a named pretty EXPLAIN column.
 * Inputs: res - Pretty EXPLAIN result; column_name - Expected column name.
 * Return: Zero-based column index, or -1 when not present.
 */
static int find_pretty_column(PGresult *res, const char *column_name)
{
    for (int field = 0; field < PQnfields(res); field++) {
        const char *name = PQfname(res, field);

        if (name != NULL && pg_strcasecmp(name, column_name) == 0) {
            return field;
        }
    }
    return -1;
}

/* Return one nullable libpq field for the shared PRETTY parser. */
static const char *pretty_field(PGresult *res, int row, int column)
{
    return column < 0 || PQgetisnull(res, row, column) ? NULL : PQgetvalue(res, row, column);
}

/* Parse complete or non-ANALYZE PRETTY through the same row identity rules. */
static bool parse_explain_nodes(PGresult *res, List **rows)
{
    int block = find_pretty_column(res, "Query Block");

    if (block < 0)
    {
        return true;
    }
    int operation = find_pretty_column(res, "operation");
    int id = find_pretty_column(res, "id");
    int estimated = find_pretty_column(res, "E-rows");
    int actual = find_pretty_column(res, "A-rows");

    for (int row = 0; row < PQntuples(res); row++)
    {
        PrettyParseStatus status;
        PlanNode *node = pretty_parse_row(pretty_field(res, row, id),
            pretty_field(res, row, operation), pretty_field(res, row, estimated),
            pretty_field(res, row, actual), pretty_field(res, row, block), &status);

        if (!status.id_valid || !status.plan_rows_valid || node->pretty_operation == NULL)
        {
            return false;
        }
        *rows = lappend(*rows, node);
    }
    return true;
}

/*
 * Brief: Parse one result from EXPLAIN OUTLINE/BLOCKNAME.
 * Inputs: res - One libpq result; data - OutlinePrettyRowsParseState pointer.
 * Outputs: data - Appended outline hints and pretty plan rows when present.
 * Return: 0 on success.
 */
static int parse_outline_pretty_rows_result(PGresult *res, void *data)
{
    OutlinePrettyRowsParseState *state = data;

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        if (PQnfields(res) == 1) {
            const char *section_name = PQfname(res, 0);

            Assert(section_name != NULL);
            if (state->execution_time_ms != NULL &&
                strcmp(section_name, "====== Query Summary =====") == 0)
            {
                for (int row = 0; row < PQntuples(res); row++)
                {
                    if (strstr(PQgetvalue(res, row, 0), "Total runtime:") != NULL)
                    {
                        state->runtime_seen = true;
                    }
                }
            }
            if (parse_pretty_total_runtime(res,
                                           state->execution_time_ms) != 0) {
                return 1;
            }
            if (strcmp(section_name, "====== Outline Data =====") == 0) {
                return parse_outline_hints(res, state->outline_hints);
            }
            /* Query Others only echoes the submitted SQL for diagnostics. */
            return 0;
        }
        if (!parse_explain_nodes(res, state->explain_plan_nodes))
        {
            return 1;
        }
    }
    return 0;
}

/*
 * Brief: Mark pretty EXPLAIN failure.
 * Inputs: deadline_end_time_usec - Active deadline, or no-deadline marker.
 * Outputs: explain_plan_nodes - Cleared; summary - Receives finish reason.
 */
static void mark_explain_failure(List **explain_plan_nodes,
                                 int64 deadline_end_time_usec,
                                 HintAdviserRunSummary *summary)
{
    *explain_plan_nodes = NIL;
    summary->finish_reason =
        deadline_end_time_usec >= 0 && connection_monotonic_usec() >= deadline_end_time_usec ?
        HINT_ADVISER_FINISH_TIME_BUDGET_REACHED :
        HINT_ADVISER_FINISH_ERROR;
}

/*
 * Execute only the candidate ANALYZE statement and retain its terminal state.
 * The round owns PRETTY/statement_timeout setup and restoration so no reset
 * statement can hide the SQLSTATE or race an undrained cancellation result.
 */
ConnectionExecution connection_feedback_analyze(PGconn *conn,
    const char *candidate_sql, int64 deadline, HintAdviserOutlineHints *outline,
    List **pretty, double *execution_time_ms)
{
    OutlinePrettyRowsParseState state = {0};
    char *sql = psprintf("EXPLAIN (ANALYZE, OUTLINE ON, BLOCKNAME ON) %s",
                         candidate_sql);

    state.outline_hints = outline;
    state.explain_plan_nodes = pretty;
    state.execution_time_ms = execution_time_ms;
    log_message(false, "INFO", "feedback_candidate_sql=%s", candidate_sql);
    ConnectionExecution execution = connection_execute(conn, sql,
        "FEEDBACK EXPLAIN ANALYZE", deadline, 3000,
        parse_outline_pretty_rows_result, &state, true);

    if (execution.success && (!state.runtime_seen || !isfinite(*execution_time_ms) ||
                              *execution_time_ms < 0))
    {
        execution.success = false;
        execution.handler_failed = true;
        execution.failure_reason = "complete_runtime_missing";
    }

    pfree(sql);
    return execution;
}

/*
 * Description: Execute pretty EXPLAIN OUTLINE/BLOCKNAME and return metadata.
 *              When analyze is true, the pretty result is also the feedback
 *              execution source and provides Total runtime plus A-rows.
 * Inputs: conn - database connection; sql - SQL to explain, or NULL for no-op
 * Outputs: outline_hints - parsed outline hint lines;
 *          explain_plan_nodes - pretty plan rows in display order;
 *          execution_time_ms - pretty Query Summary Total runtime
 * Return: 0 on success or no-op, 1 on failure
 */
int execute_explain_analyze(PGconn *conn,
								 const char *sql,
								 HintAdviserOutlineHints *outline_hints,
                                 List **explain_plan_nodes,
                                 bool analyze,
                                 double *execution_time_ms,
                                 int64 deadline_end_time_usec,
                                 HintAdviserRunSummary *summary)
{
	outline_hints->items = NULL;
	outline_hints->count = 0;
    *explain_plan_nodes = NIL;
    if (execution_time_ms != NULL) {
        *execution_time_ms = 0;
    }
    if (sql == NULL) {
        return 0;
    }
    const char *select_end = NULL;
    find_select_hint_insert_pos(sql, &select_end);

    if (logged_exec_each_result(conn,
                                "SET explain_perf_mode=pretty",
                                "SET explain_perf_mode=pretty",
                                deadline_end_time_usec,
                                command_result_handler,
                                "SET explain_perf_mode=pretty",
                                true) != 0) {
        mark_explain_failure(explain_plan_nodes,
                             deadline_end_time_usec,
                             summary);
        return 1;
    }

    char *explain_sql = psprintf(analyze ?
                                 "EXPLAIN (ANALYZE, OUTLINE ON, BLOCKNAME ON) %s" :
                                 "EXPLAIN (OUTLINE ON, BLOCKNAME ON) %s",
                                 sql);

    OutlinePrettyRowsParseState parse_state = {0};
    parse_state.outline_hints = outline_hints;
    parse_state.explain_plan_nodes = explain_plan_nodes;
    parse_state.execution_time_ms = execution_time_ms;
    int rc = logged_exec_each_result(conn,
                                     explain_sql,
                                     analyze ?
                                     "EXPLAIN ANALYZE OUTLINE BLOCKNAME" :
                                     "EXPLAIN OUTLINE BLOCKNAME",
                                     deadline_end_time_usec,
                                     parse_outline_pretty_rows_result,
                                     &parse_state,
                                     true);
    if (rc == 0 && analyze && execution_time_ms != NULL &&
        (!parse_state.runtime_seen || !isfinite(*execution_time_ms) || *execution_time_ms < 0))
    {
        rc = 1;
    }
    int reset_rc = logged_exec_each_result(conn,
                                           "SET explain_perf_mode=normal",
                                           "SET explain_perf_mode=normal",
                                           deadline_end_time_usec,
                                           command_result_handler,
                                           "SET explain_perf_mode=normal",
                                           true);
    if (rc != 0) {
        pg_log_error("EXPLAIN OUTLINE/BLOCKNAME failed: %s",
                     PQerrorMessage(conn));
    }
    if (rc != 0 || reset_rc != 0) {
        mark_explain_failure(explain_plan_nodes,
                             deadline_end_time_usec,
                             summary);
        return 1;
    }
    return 0;
}
