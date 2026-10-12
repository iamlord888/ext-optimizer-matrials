/*-------------------------------------------------------------------------
 *
 * extopt_partial_capture.cpp
 *      Partial EXPLAIN executor observation and snapshot transport.
 *
 *-------------------------------------------------------------------------
 */
/*
 * This module owns the capture lifecycle and protocol. It deliberately
 * records sparse node-local facts only: topology is reconstructed from the
 * client's replan, while all private GaussDB layout access stays in
 * extopt_partial_gaussdb.cpp.
 */
#include "gaussdb.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utils/elog.h"
#include "utils/errcodes.h"

#include "extopt_partial_capture.h"
#include "extopt_partial_error.h"
#include "extopt_partial_gaussdb.h"
#include "extopt_partial_protocol.h"

/*
 * The SDK omits PG_TRY. The local exception boundary must preserve the kernel
 * error/context stacks: cancellation is intercepted while PlanState is alive,
 * and snapshot errors must never replace the original cancellation SQLSTATE.
 */
#define EXTOPT_ERR_TRY()                                                       \
    do {                                                                       \
        sigjmp_buf *save_exception_stack = *g_thr_err_except_stack;             \
        ErrProcHook *save_context_stack = *g_thr_error_context_stack;           \
        sigjmp_buf local_sigjmp_buf;                                            \
        if (sigsetjmp(local_sigjmp_buf, 0) == 0) {                              \
            *g_thr_err_except_stack = &local_sigjmp_buf

#define EXTOPT_ERR_CATCH()                                                     \
        } else {                                                               \
            *g_thr_err_except_stack = save_exception_stack;                      \
            *g_thr_error_context_stack = save_context_stack

#define EXTOPT_ERR_END_TRY()                                                   \
        }                                                                      \
        *g_thr_err_except_stack = save_exception_stack;                        \
        *g_thr_error_context_stack = save_context_stack;                       \
    } while (0)


/*
 * At timeout, the pre-collected PlanState tree and read-only Instrumentation
 * evidence add executed nodes while leaving unexecuted branches absent.
 */
typedef struct CapturedObservation
{
    const ExtoptPlanState *state;
    ExtoptPlanObservation observation;
    PartialExecutionState execution_state;
    int rescan_count;
    double returned_rows;
} CapturedObservation;

typedef struct PartialCaptureState
{
    QueryDesc *query_desc;
    CapturedObservation *observations;
    int observation_count;
    bool structural_invalid;
    const char *structural_failure_reason;
    int64 execution_id;
    char *snapshot_payload;
    size_t snapshot_length;
    size_t snapshot_capacity;
    const ExtoptPlanState **plan_states;
    int plan_state_count;
    bool plan_states_valid;
    const char *plan_states_failure_reason;
} PartialCaptureState;

static THR_LOCAL int64 armed_execution_id = 0;
static THR_LOCAL PartialCaptureState *active_capture = NULL;

/* Remember the first structural failure and reject this execution's facts. */
static void
invalidate_capture(PartialCaptureState *capture, const char *reason)
{
    capture->structural_invalid = true;
    if (capture->structural_failure_reason == NULL) {
        capture->structural_failure_reason = reason;
    }
}

/* Collect stable PlanState pointers before cancellation can unwind execution. */
static void
start_completion_capture(QueryDesc *query_desc)
{
    if (armed_execution_id <= 0 ||
        query_desc == NULL || active_capture != NULL) {
        return;
    }

    PartialCaptureState *capture =
        (PartialCaptureState *) calloc(1, sizeof(PartialCaptureState));
    if (capture == NULL) {
        partial_report_error(ERRCODE_OUT_OF_MEMORY,
                                "could not allocate partial capture state");
    }
    capture->observations = (CapturedObservation *) calloc(
        HAPF_MAX_NODES, sizeof(CapturedObservation));
    if (capture->observations == NULL) {
        free(capture);
        partial_report_error(ERRCODE_OUT_OF_MEMORY,
                                "could not allocate partial observations");
    }
    capture->plan_states = (const ExtoptPlanState **) calloc(
        HAPF_MAX_NODES, sizeof(ExtoptPlanState *));
    if (capture->plan_states == NULL) {
        free(capture->observations);
        free(capture);
        partial_report_error(ERRCODE_OUT_OF_MEMORY,
                                "could not allocate partial PlanState snapshot");
    }
    capture->query_desc = query_desc;
    capture->execution_id = armed_execution_id;
    active_capture = capture;
    /* The topology is stable here. Save pointers before a timeout longjmp
     * interrupts a node, then read only Instrumentation at snapshot time. */
    capture->plan_states_valid = partial_gaussdb_execution_collect_plan_states(
        query_desc, capture->plan_states, HAPF_MAX_NODES,
        &capture->plan_state_count, &capture->plan_states_failure_reason);
    if (!capture->plan_states_valid)
    {
        invalidate_capture(capture,
            capture->plan_states_failure_reason == NULL ?
            "plan_state_collection_failed" : capture->plan_states_failure_reason);
    }
}

/* Release libc capture storage after success or error, then disarm the session. */
static void
finish_completion_capture(void)
{
    PartialCaptureState *capture = active_capture;

    active_capture = NULL;
    armed_execution_id = 0;
    if (capture == NULL) {
        return;
    }
    free(capture->snapshot_payload);
    free(capture->plan_states);
    free(capture->observations);
    free(capture);
}

/* Read the same monotonic clock used by the snapshot deadline. */
static int64
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        int saved_errno = errno;
        char detail[160];

        (void) snprintf(detail,
                           sizeof(detail),
                           "Reading the monotonic clock failed: %s.",
                           strerror(saved_errno));
        partial_report_error_detail(ERRCODE_INTERNAL_ERROR,
                               "could not read monotonic clock",
                               detail);
    }
    return ((int64) now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

/* Copy an error field outside the resettable GaussDB error memory context. */
static char *
duplicate_error_string(const char *source)
{
    char *copy;
    size_t length;

    if (source == NULL) {
        return NULL;
    }
    length = strlen(source) + 1;
    copy = (char *) malloc(length);
    if (copy != NULL) {
        memcpy(copy, source, length);
    }
    return copy;
}

/* Release the copied cancellation error and all its owned strings. */
static void
free_snapshot_error(ErrData *error)
{
    if (error == NULL) {
        return;
    }
    free(error->filename);
    free(error->funcname);
    free(error->message);
    free(error->detail);
    free(error->detail_log);
    free(error->hint);
    free(error->context);
    free(error->internalquery);
    free(error->backtrace_log);
    free(error->cause);
    free(error->action);
    free(error->errmsgsqlm);
    free(error);
}

/*
 * err_copy_data() allocates in GaussDB's current memory controller, which is
 * the error controller after a longjmp and is reset by err_flush_data().
 * Move every owned string to libc memory before emitting snapshot notices.
 * The domain pointer is intentionally retained: GaussDB treats it as constant
 * and err_re_throw_error() does not duplicate it either.
 */
/* Copy the original error into libc-owned memory, or return NULL on allocation failure. */
static ErrData *
copy_error_for_snapshot(void)
{
    ErrData *source = err_copy_data();
    ErrData *copy = (ErrData *) calloc(1, sizeof(ErrData));

    if (copy == NULL) {
        err_free_data(source);
        return NULL;
    }
    *copy = *source;
    copy->filename = duplicate_error_string(source->filename);
    copy->funcname = duplicate_error_string(source->funcname);
    copy->message = duplicate_error_string(source->message);
    copy->detail = duplicate_error_string(source->detail);
    copy->detail_log = duplicate_error_string(source->detail_log);
    copy->hint = duplicate_error_string(source->hint);
    copy->context = duplicate_error_string(source->context);
    copy->internalquery = duplicate_error_string(source->internalquery);
    copy->backtrace_log = duplicate_error_string(source->backtrace_log);
    copy->cause = duplicate_error_string(source->cause);
    copy->action = duplicate_error_string(source->action);
    copy->errmsgsqlm = duplicate_error_string(source->errmsgsqlm);

    bool complete =
        (source->filename == NULL || copy->filename != NULL) &&
        (source->funcname == NULL || copy->funcname != NULL) &&
        (source->message == NULL || copy->message != NULL) &&
        (source->detail == NULL || copy->detail != NULL) &&
        (source->detail_log == NULL || copy->detail_log != NULL) &&
        (source->hint == NULL || copy->hint != NULL) &&
        (source->context == NULL || copy->context != NULL) &&
        (source->internalquery == NULL || copy->internalquery != NULL) &&
        (source->backtrace_log == NULL || copy->backtrace_log != NULL) &&
        (source->cause == NULL || copy->cause != NULL) &&
        (source->action == NULL || copy->action != NULL) &&
        (source->errmsgsqlm == NULL || copy->errmsgsqlm != NULL);

    err_free_data(source);
    if (!complete) {
        free_snapshot_error(copy);
        return NULL;
    }
    return copy;
}

/* Preserve the original SQLSTATE while freeing its libc-owned copy. */
static void
rethrow_snapshot_error(ErrData *error)
{
    EXTOPT_ERR_TRY();
    {
        err_re_throw_error(error);
    }
    EXTOPT_ERR_CATCH();
    {
        free_snapshot_error(error);
        err_re_throw();
    }
    EXTOPT_ERR_END_TRY();
    abort();
}

/* Abort snapshot construction when its finite processing budget expires. */
static void
check_snapshot_deadline(int64 deadline_ms)
{
    if (monotonic_milliseconds() > deadline_ms) {
        partial_report_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                "partial snapshot time limit exceeded");
    }
}

/* Adapt the snapshot deadline check to PRETTY formatting callbacks. */
static void
check_snapshot_progress(void *context)
{
    check_snapshot_deadline(*(const int64 *) context);
}

/* Append within a hard payload limit using libc memory across error cleanup. */
static void
append_snapshot_bytes(PartialCaptureState *capture,
                          const char *data,
                          size_t length)
{
    size_t required;

    if (length > HAPF_MAX_PAYLOAD -
        capture->snapshot_length) {
        partial_report_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                "partial snapshot byte limit exceeded");
    }
    required = capture->snapshot_length + length + 1;
    if (required > capture->snapshot_capacity) {
        size_t capacity = capture->snapshot_capacity == 0 ?
            4096 : capture->snapshot_capacity;

        while (capacity < required &&
               capacity < HAPF_MAX_PAYLOAD + 1) {
            capacity *= 2;
        }
        if (capacity > HAPF_MAX_PAYLOAD + 1) {
            capacity = HAPF_MAX_PAYLOAD + 1;
        }
        char *payload = (char *) realloc(capture->snapshot_payload, capacity);
        if (payload == NULL) {
            partial_report_error(ERRCODE_OUT_OF_MEMORY,
                                    "could not allocate partial snapshot payload");
        }
        capture->snapshot_payload = payload;
        capture->snapshot_capacity = capacity;
    }

    memcpy(capture->snapshot_payload + capture->snapshot_length, data, length);
    capture->snapshot_length += length;
    capture->snapshot_payload[capture->snapshot_length] = '\0';
}

/* Capture original execution PRETTY before serializing observations. */
static void
append_partial_pretty_snapshot(PartialCaptureState *capture,
                                   int64 deadline_ms)
{
    ExtoptPrettyPlan pretty = {NULL, 0};
    char header[160];
    int header_length;

    partial_gaussdb_render_pretty_plan(capture->query_desc,
                                     check_snapshot_progress,
                                     &deadline_ms,
                                     &pretty);
    header_length = snprintf(header, sizeof(header),
                             "PRETTY-BYTES=%d\n", pretty.length);
    if (header_length < 0 || (size_t) header_length >= sizeof(header)) {
        partial_report_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                "partial pretty header is too large");
    }
    append_snapshot_bytes(capture, header, (size_t) header_length);
    append_snapshot_bytes(capture, pretty.data, (size_t) pretty.length);
    append_snapshot_bytes(capture,
        "\nOBSERVATIONS\n",
        strlen("\nOBSERVATIONS\n"));
}

/* Serialize one normalized execution fact; emitted nodes have executed. */
static void
append_snapshot_observation(PartialCaptureState *capture,
                                const CapturedObservation *captured)
{
    const ExtoptPlanObservation *observation = &captured->observation;
    char record[384];
    int written = snprintf(
        record,
        sizeof(record),
        "%d,%d,%d,%.17g,%.17g\n",
        observation->plan_id,
        (int) captured->execution_state,
        captured->rescan_count,
        captured->returned_rows,
        observation->plan_rows);

    if (written < 0 || (size_t) written >= sizeof(record)) {
        partial_report_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                "partial snapshot byte limit exceeded");
    }
    append_snapshot_bytes(capture, record, (size_t) written);
}

/* Encode bounded payload chunks without whitespace. */
static size_t
base64_encode(const unsigned char *input, size_t length, char *output)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t input_at = 0;
    size_t output_at = 0;

    while (input_at < length) {
        size_t remaining = length - input_at;
        unsigned int a = input[input_at++];
        unsigned int b = remaining > 1 ? input[input_at++] : 0;
        unsigned int c = remaining > 2 ? input[input_at++] : 0;

        output[output_at++] = alphabet[a >> 2];
        output[output_at++] = alphabet[((a & 0x03) << 4) | (b >> 4)];
        output[output_at++] = remaining > 1 ?
            alphabet[((b & 0x0f) << 2) | (c >> 6)] : '=';
        output[output_at++] = remaining > 2 ? alphabet[c & 0x3f] : '=';
    }
    output[output_at] = '\0';
    return output_at;
}

/* Emit one bounded positional NOTICE for the current execution. */
static void
emit_protocol_message(int64 execution_id,
                      int sequence,
                      const char *kind,
                      const char *payload)
{
    char message[HAPF_MAX_MESSAGE];
    int written = snprintf(message,
                           sizeof(message),
                           "HAPF %lld %d %s%s%s",
                           (long long) execution_id,
                           sequence,
                           kind,
                           payload == NULL || payload[0] == '\0' ? "" : " ",
                           payload == NULL ? "" : payload);

    if (written < 0 || (size_t) written >= sizeof(message)) {
        partial_report_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                "partial protocol message limit exceeded");
    }
    ERR_PROC(GAUSS_16018,
             ERR_LEVEL_NOTICE,
             (err_msg("%s", message), err_set_hidestmt(true)));
}


/* Reject inconsistent identities or lifecycle values before serialization. */
static bool
append_captured_observation(PartialCaptureState *capture,
                                const ExtoptPlanState *state,
                                const ExtoptPlanObservation *observation,
                                const ExtoptExecutionEvidence *evidence)
{
    const char *failure_reason = NULL;
    CapturedObservation *captured;

    if (observation->plan_id <= 0) {
        failure_reason = "invalid_plan_identity";
    } else if (observation->dop < 0 || observation->dop > 1) {
        failure_reason = "invalid_dop";
    } else if (observation->exec_type < 0 || observation->exec_type > 2) {
        failure_reason = "invalid_exec_type";
    } else if (evidence->rescan_count < 0) {
        failure_reason = "invalid_lifecycle_counter";
    } else if (evidence->returned_rows < 0 ||
               !isfinite(evidence->returned_rows)) {
        failure_reason = "execution_evidence_rows_invalid";
    } else if (observation->plan_rows < 0 ||
               !isfinite(observation->plan_rows)) {
        failure_reason = "invalid_plan_rows";
    } else if (capture->observation_count >=
               HAPF_MAX_NODES) {
        failure_reason = "observation_limit_exceeded";
    }
    if (failure_reason != NULL) {
        invalidate_capture(capture, failure_reason);
        return false;
    }

    for (int index = 0; index < capture->observation_count; index++) {
        const CapturedObservation *other = &capture->observations[index];

        if (other->state == state) {
            invalidate_capture(capture, "duplicate_observation_identity");
            return false;
        }
        if (other->observation.plan_id == observation->plan_id) {
            invalidate_capture(capture, "duplicate_plan_id");
            return false;
        }
    }

    captured = &capture->observations[capture->observation_count++];
    captured->state = state;
    captured->observation = *observation;
    captured->execution_state = evidence->terminal ? PARTIAL_EXECUTION_TERMINAL :
        evidence->active ? PARTIAL_EXECUTION_ACTIVE : PARTIAL_EXECUTION_INCOMPLETE;
    captured->rescan_count = evidence->rescan_count;
    captured->returned_rows = evidence->returned_rows;
    return true;
}

/* Read stable executor Instrumentation before PRETTY closes epochs. */
static bool
collect_execution_evidence(PartialCaptureState *capture)
{
    if (capture == NULL || capture->structural_invalid ||
        !capture->plan_states_valid || capture->observations == NULL ||
        capture->plan_states == NULL || capture->plan_state_count <= 0 ||
        capture->plan_state_count > HAPF_MAX_NODES ||
        capture->observation_count != 0) {
        if (capture != NULL) {
            invalidate_capture(capture,
                capture->plan_states_failure_reason == NULL ?
                "plan_state_collection_failed" :
                capture->plan_states_failure_reason);
        }
        return false;
    }

    for (int index = 0; index < capture->plan_state_count; index++) {
        const ExtoptPlanState *state = capture->plan_states[index];
        ExtoptPlanObservation observation;
        ExtoptExecutionEvidence evidence = {};

        /* Unsupported container nodes remain outside the observation protocol. */
        if (!partial_gaussdb_describe_plan_state(state, &observation)) {
            continue;
        }
        if (!partial_gaussdb_execution_capture_evidence(
                state, &observation, &evidence)) {
            invalidate_capture(capture, "execution_evidence_unavailable");
            return false;
        }
        if (!evidence.executed) {
            /* A branch with no Instrumentation activity is absent by contract. */
            continue;
        }
        if (!append_captured_observation(capture,
                                         state,
                                         &observation,
                                         &evidence)) {
            return false;
        }
    }
    return !capture->structural_invalid;
}

/* Serialize and frame one cancellation snapshot within its own finite deadline. */
static void
emit_partial_snapshot(void)
{
    PartialCaptureState *capture = active_capture;
    int sequence = 0;
    int64 deadline_ms = monotonic_milliseconds() +
        HAPF_SNAPSHOT_TIMEOUT_MS;

    if (capture != NULL && !collect_execution_evidence(capture)) {
        /* collect_execution_evidence records the fail-closed reason. */
    }
    if (capture == NULL || capture->structural_invalid ||
        !capture->plan_states_valid ||
        capture->observation_count > HAPF_MAX_NODES) {
        if (capture != NULL) {
            char message[512];
            const char *reason = capture->structural_failure_reason == NULL ?
                "invalid_observations" : capture->structural_failure_reason;

            (void) snprintf(message,
                           sizeof(message),
                           "HAPF_OBSERVATION_REJECT EXEC=%lld REASON=%s",
                           (long long) capture->execution_id,
                           reason);
            ERR_PROC(GAUSS_16018,
                     ERR_LEVEL_NOTICE,
                     (err_msg("%s", message), err_set_hidestmt(true)));
        }
        return;
    }
    append_partial_pretty_snapshot(capture, deadline_ms);
    for (int index = 0; index < capture->observation_count; index++) {
        check_snapshot_deadline(deadline_ms);
        append_snapshot_observation(capture, &capture->observations[index]);
    }

    const size_t raw_chunk_size = 2048;
    for (size_t offset = 0; offset < capture->snapshot_length;
         offset += raw_chunk_size) {
        size_t chunk_length = Min(raw_chunk_size,
                                  capture->snapshot_length - offset);
        char encoded[(raw_chunk_size / 3 + 1) * 4 + 1];

        check_snapshot_deadline(deadline_ms);
        base64_encode((const unsigned char *) capture->snapshot_payload + offset,
                      chunk_length,
                      encoded);
        emit_protocol_message(capture->execution_id,
                              sequence++,
                              "DATA",
                              encoded);
    }
    check_snapshot_deadline(deadline_ms);
    emit_protocol_message(capture->execution_id,
                          sequence,
                          "END",
                          NULL);
}

/* Catch cancellation while execution objects are valid, then rethrow the original error. */
static void
capture_executor_run(QueryDesc *query_desc,
                     ScanDirection direction,
                     long count,
                     ExtoptExecutorRun original)
{
    start_completion_capture(query_desc);
    if (active_capture == NULL || active_capture->query_desc != query_desc) {
        (*original)(query_desc, direction, count);
        return;
    }

    EXTOPT_ERR_TRY();
    {
        (*original)(query_desc, direction, count);
        finish_completion_capture();
    }
    EXTOPT_ERR_CATCH();
    {
        ErrData *original_error = copy_error_for_snapshot();
        if (original_error == NULL) {
            finish_completion_capture();
            err_re_throw();
        }
        bool is_query_cancel =
            original_error->sqlerrcode == ERRCODE_QUERY_CANCELED;

        err_flush_data();
        if (is_query_cancel) {
            /* Snapshot failures never replace the original timeout error. */
            EXTOPT_ERR_TRY();
            {
                emit_partial_snapshot();
            }
            EXTOPT_ERR_CATCH();
            {
                err_flush_data();
            }
            EXTOPT_ERR_END_TRY();
        }
        finish_completion_capture();
        rethrow_snapshot_error(original_error);
    }
    EXTOPT_ERR_END_TRY();
}

/*
 * Install the thread-local executor hook before arming execution_id. Zero
 * disarms; the next ExecutorRun otherwise consumes the id and clears it on
 * both normal and error completion.
 */
void partial_capture_prepare(int64 execution_id)
{
    static const ExtoptCaptureCallbacks callbacks = {capture_executor_run};

    partial_gaussdb_initialize(&callbacks);
    armed_execution_id = execution_id;
}
