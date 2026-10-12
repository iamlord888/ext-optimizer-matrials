/*-------------------------------------------------------------------------
 *
 * extopt_partial_gaussdb.cpp
 *      GaussDB executor adapter for partial EXPLAIN capture.
 *
 *-------------------------------------------------------------------------
 */
/*
 * This is the only extopt module allowed to know the commercial GaussDB
 * executor's private layouts and mangled C++ symbols. The extension and server
 * are built and delivered together from the matching GaussDB SDK.
 * The capture module receives normalized observations through
 * extopt_partial_gaussdb.h and therefore remains independent of these offsets.
 */
#include "gaussdb.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lib/stringinfo.h"
#include "nodes/execnodetags.h"
#include "utils/node/list.h"
#include "utils/errcodes.h"

#include "extopt_partial_error.h"
#include "extopt_partial_gaussdb.h"
#include "extopt_partial_protocol.h"

typedef struct PlannedStmt PlannedStmt;

/*
 * The installed commercial SDK omits Plan, PlanState, and QueryDesc. These
 * views contain only fields read by extopt; they are not portable substitutes
 * for the kernel definitions.
 */
typedef struct ExtoptPlan
{
    NodeTag type;
    int plan_node_id;
    char pad1[12 - 8];
    int exec_type;
    char pad2[32 - 16];
    double plan_rows;
    char pad3[52 - 40];
    int dop;
} ExtoptPlan;

typedef struct ExtoptInstrumentationView
{
    /*
     * GaussDB 507's instr_start/instr_stop disassembly shows the timer flag at
     * byte 0, the running flag at byte 5, and a timeval-style start timestamp
     * at 0x08/0x10. PlanState::collect_instrument() sets status at 0x472
     * whenever a row executor method returns EOF. instr_end() adds one to the
     * nloops counter at 0xc0 when an execution epoch is closed; the snapshot is
     * taken before PRETTY can close the current epoch. Hash is excluded because
     * MultiExec does not use this EOF status.
     */
    uint8_t timing_enabled;
    char pad1[4];
    uint8_t running;
    char pad2[2];
    int64_t starttime_seconds;
    int64_t starttime_microseconds;
    char pad3[0xb8 - 0x18];
    double ntuples;
    double nloops;
    char pad4[0x472 - 0xc8];
    uint8_t status;
} ExtoptInstrumentationView;

struct ExtoptPlanState
{
    NodeTag type;
    const ExtoptPlan *plan;
    const void *estate;
    char pad1[0x28 - 24];
    const ExtoptInstrumentationView *instrument;
    char pad2[0x40 - 0x30];
    const ExtoptPlanState *lefttree;
    const ExtoptPlanState *righttree;
    const List *init_plan;
    const List *sub_plan;
};

typedef struct ExtoptAppendStateView
{
    char pad1[0x128];
    const ExtoptPlanState *const *children;
    int child_count;
} ExtoptAppendStateView;

typedef struct ExtoptMergeAppendStateView
{
    char pad1[0x200];
    const ExtoptPlanState *const *children;
    int child_count;
} ExtoptMergeAppendStateView;

typedef struct ExtoptBitmapStateView
{
    char pad1[0x128];
    const ExtoptPlanState *const *children;
    int child_count;
} ExtoptBitmapStateView;

typedef struct ExtoptSubqueryScanStateView
{
    char pad1[0x200];
    const ExtoptPlanState *subplan;
} ExtoptSubqueryScanStateView;

typedef struct ExtoptSubPlanStateView
{
    char pad1[0xc0];
    const ExtoptPlanState *planstate;
} ExtoptSubPlanStateView;

struct QueryDesc
{
    char pad1[8];
    PlannedStmt *plannedstmt;
    char pad2[0x58 - 0x10];
    const void *estate;
    const ExtoptPlanState *planstate;
};

struct OptUtilExplainPlanTable;
typedef struct OptUtilExplainPlanInformation
{
    OptUtilExplainPlanTable *plan_table;
} OptUtilExplainPlanInformation;

#define EXPLAIN_PLAN_TABLE_ROW_COUNT_OFFSET 8
#define EXPLAIN_PLAN_TABLE_TUPLE_DESC_OFFSET 24
#define EXPLAIN_PLAN_TABLE_COLUMNS_OFFSET 120
#define EXPLAIN_PLAN_TABLE_WIDTHS_OFFSET 152
#define EXPLAIN_PLAN_TABLE_VALUES_OFFSET 216
typedef struct OptUtilExplainPlanTable
{
    char pad1[EXPLAIN_PLAN_TABLE_ROW_COUNT_OFFSET];
    int64 row_count;
    char pad2[EXPLAIN_PLAN_TABLE_TUPLE_DESC_OFFSET -
                  EXPLAIN_PLAN_TABLE_ROW_COUNT_OFFSET - sizeof(int64)];
    void *tuple_desc;
    char pad3[EXPLAIN_PLAN_TABLE_COLUMNS_OFFSET -
                  EXPLAIN_PLAN_TABLE_TUPLE_DESC_OFFSET - sizeof(void *)];
    short columns[15];
    char pad4[EXPLAIN_PLAN_TABLE_WIDTHS_OFFSET -
                  EXPLAIN_PLAN_TABLE_COLUMNS_OFFSET - sizeof(short) * 15];
    int widths[15];
    char pad5[EXPLAIN_PLAN_TABLE_VALUES_OFFSET -
                  EXPLAIN_PLAN_TABLE_WIDTHS_OFFSET - sizeof(int) * 15];
    Datum **values;
} OptUtilExplainPlanTable;

#define EXPLAIN_STATE_ANALYZE_OFFSET 18
#define EXPLAIN_STATE_PLAN_INFORMATION_OFFSET 72
#define EXPLAIN_STATE_BLOCKNAME_OFFSET 144
#define EXPLAIN_STATE_OUTLINE_OFFSET 152
#define EXPLAIN_STATE_SIZE 388
typedef struct OptUtilExplainState
{
    GsString str;
    char pad1[EXPLAIN_STATE_ANALYZE_OFFSET - sizeof(GsString)];
    bool analyze;
    char pad2[EXPLAIN_STATE_PLAN_INFORMATION_OFFSET -
                  EXPLAIN_STATE_ANALYZE_OFFSET - sizeof(bool)];
    OptUtilExplainPlanInformation *plan_information;
    char pad3[EXPLAIN_STATE_BLOCKNAME_OFFSET -
                  EXPLAIN_STATE_PLAN_INFORMATION_OFFSET -
                  sizeof(OptUtilExplainPlanInformation *)];
    bool blockname;
    char pad4[EXPLAIN_STATE_OUTLINE_OFFSET -
                  EXPLAIN_STATE_BLOCKNAME_OFFSET - sizeof(bool)];
    bool outline;
    char pad5[EXPLAIN_STATE_SIZE - EXPLAIN_STATE_OUTLINE_OFFSET -
                  sizeof(bool)];
} OptUtilExplainState;

extern THR_LOCAL ExtoptExecutorRun ExecutorRun_hook;
extern void exec_standard_executor_run(QueryDesc *query_desc,
                                      ScanDirection direction,
                                      long count);
extern void optutil_explain_proc_init_state(OptUtilExplainState *state);
extern void optutil_explain_set_perf_mode(OptUtilExplainState *state,
                                          PlannedStmt *planned_stmt);
extern void optutil_explain_proc_begin_output(OptUtilExplainState *state);
extern void optutil_explain_proc_print_plan(OptUtilExplainState *state,
                                        QueryDesc *query_desc);
extern void optutil_explain_proc_end_output(OptUtilExplainState *state);
extern void optutil_explain_plan_information_dump_string(
    OptUtilExplainPlanInformation *plan_information,
    GsString output)
    __asm__("_ZN29OptUtilExplainPlanInformation4dumpIP12GsStringDataS2_EEvT_");
extern GsString optutil_explain_append_title_string(
    GsString output, void *tuple_desc, short *columns, int *widths)
    __asm__("_Z28optutil_explain_append_titleP12GsStringDataP9tupleDescPsPi");
extern void optutil_explain_plan_table_row_to_string(
    GsString output, Datum *values, short *columns, int *widths)
    __asm__("_Z40optutil_explain_plan_table_col_to_stringP12GsStringDataPmPsPi");
extern void exec_end_tup_output_string(GsString output)
    __asm__("_Z19exec_end_tup_outputP12GsStringData");

typedef struct ExecutionNodeBinding
{
    const char *plan_name;
    NodeTag state_tag;
} ExecutionNodeBinding;

/* These bindings use the supported ordinary row-executor Instrumentation layout. */
static const ExecutionNodeBinding execution_node_bindings[] = {
    {"SeqScan", T_SeqScanOperator},
    {"IndexScan", T_IndexScanOperator},
    {"IndexOnlyScan", T_IndexOnlyScanOperator},
    {"BitmapHeapScan", T_BitmapHeapScanOperator},
    {"TidScan", T_TidScanOperator},
    {"SubqueryScan", T_SubqueryScanOperator},
    {"FunctionScan", T_FunctionScanOperator},
    {"ValuesScan", T_ValuesScanOperator},
    {"CteScan", T_CteScanOperator},
    {"NestLoop", T_NestLoopOperator},
    {"HashJoin", T_HashJoinOperator},
    {"Material", T_MaterialOperator},
    {"Sort", T_SortOperator},
    {"Agg", T_AggOperator}
};

#define EXTOPT_EXECUTION_NODE_COUNT \
    ((int) (sizeof(execution_node_bindings) / sizeof(execution_node_bindings[0])))

static THR_LOCAL ExtoptCaptureCallbacks capture_callbacks = {NULL};
static THR_LOCAL ExtoptExecutorRun previous_executor_run = NULL;

/* Return a supported row executor binding; MultiExec Hash has none. */
static const ExecutionNodeBinding *
find_execution_node_binding(NodeTag state_tag)
{
    for (int index = 0; index < EXTOPT_EXECUTION_NODE_COUNT; index++) {
        if (execution_node_bindings[index].state_tag == state_tag) {
            return &execution_node_bindings[index];
        }
    }
    return NULL;
}

/* Validate a semantic plan tag against its server binding. */
static bool
plan_tag_has_name(NodeTag plan_tag, const char *expected_name)
{
    const char *actual_name = semtc_node_tag_to_string(plan_tag);

    return actual_name != NULL && actual_name[0] != '\0' &&
        expected_name != NULL && strcmp(actual_name, expected_name) == 0;
}

/* Describe a supported ordinary row state; return false for excluded node kinds. */
bool
partial_gaussdb_describe_plan_state(const ExtoptPlanState *state,
                                        ExtoptPlanObservation *observation)
{
    const ExecutionNodeBinding *binding;

    if (state == NULL || state->plan == NULL || observation == NULL) {
        return false;
    }
    binding = find_execution_node_binding(state->type);
    if (binding == NULL ||
        !plan_tag_has_name(state->plan->type, binding->plan_name)) {
        return false;
    }

    observation->plan_id = state->plan->plan_node_id;
    observation->dop = state->plan->dop;
    /* In centralized GaussDB, ordinary row plans also carry the DATANODES
     * placement value. Executor node identity, not placement alone, proves the
     * local row layout; remote/vector containers are handled by topology guards. */
    observation->exec_type = state->plan->exec_type;
    observation->plan_rows = state->plan->plan_rows;
    return true;
}

/* Deduplicate stable executor identities; return false on resource overflow. */
static bool
append_plan_state(const ExtoptPlanState **states,
                  int capacity,
                  int *state_count,
                  const ExtoptPlanState *state,
                  const char **failure_reason)
{
    if (state == NULL) {
        return true;
    }
    for (int index = 0; index < *state_count; index++) {
        if (states[index] == state) {
            /* PlanState subplans can be referenced from more than one edge. */
            return true;
        }
    }
    if (*state_count >= capacity) {
        if (failure_reason != NULL) {
            *failure_reason = "plan_state_limit_exceeded";
        }
        return false;
    }
    states[(*state_count)++] = state;
    return true;
}

/* Validate and collect the children of a special n-ary executor. */
static bool
append_plan_state_array(const ExtoptPlanState **states,
                        int capacity,
                        int *state_count,
                        const ExtoptPlanState *const *children,
                        int child_count,
                        const char **failure_reason)
{
    if (child_count < 0 || child_count > HAPF_MAX_NODES ||
        (child_count > 0 && children == NULL)) {
        if (failure_reason != NULL) {
            *failure_reason = "invalid_special_plan_state_array";
        }
        return false;
    }
    for (int index = 0; index < child_count; index++) {
        if (children[index] == NULL) {
            if (failure_reason != NULL) {
                *failure_reason = "null_special_plan_state_child";
            }
            return false;
        }
        if (!append_plan_state(states, capacity, state_count,
                                  children[index], failure_reason)) {
            return false;
        }
    }
    return true;
}

/* Collect InitPlan/SubPlan roots using the matching SDK list layout. */
static bool
append_subplan_states(const ExtoptPlanState **states,
                          int capacity,
                          int *state_count,
                          const List *list,
                          const char **failure_reason)
{
    const ListCell *cell;

    if (list == NULL) {
        return true;
    }
    if (list->type != T_List || list->length <= 0 ||
        list->length > HAPF_MAX_NODES ||
        list->head == NULL || list->tail == NULL) {
        if (failure_reason != NULL) {
            *failure_reason = "invalid_subplan_state_list";
        }
        return false;
    }

    cell = list->head;
    for (int index = 0; index < list->length; index++) {
        const ExtoptSubPlanStateView *subplan;

        if (cell == NULL || cell->data.ptr_value == NULL) {
            if (failure_reason != NULL) {
                *failure_reason = "invalid_subplan_state_cell";
            }
            return false;
        }
        subplan = (const ExtoptSubPlanStateView *) cell->data.ptr_value;
        if (subplan->planstate == NULL) {
            if (failure_reason != NULL) {
                *failure_reason = "null_subplan_plan_state";
            }
            return false;
        }
        if (!append_plan_state(states, capacity, state_count,
                                  subplan->planstate, failure_reason)) {
            return false;
        }
        if (index == list->length - 1 && cell != list->tail) {
            if (failure_reason != NULL) {
                *failure_reason = "subplan_state_tail_mismatch";
            }
            return false;
        }
        cell = cell->next;
    }
    if (cell != NULL) {
        if (failure_reason != NULL) {
            *failure_reason = "subplan_state_length_mismatch";
        }
        return false;
    }
    return true;
}

/* Follow non-binary executor children at the matching SDK offsets. */
static bool
append_special_plan_states(const ExtoptPlanState **states,
                           int capacity,
                           int *state_count,
                           const ExtoptPlanState *state,
                           const char **failure_reason)
{
    switch (state->type) {
        case T_AppendOperator: {
            const ExtoptAppendStateView *append =
                (const ExtoptAppendStateView *) state;

            if (!plan_tag_has_name(state->plan->type, "Append")) {
                break;
            }
            return append_plan_state_array(
                states, capacity, state_count, append->children,
                append->child_count, failure_reason);
        }
        case T_MergeAppendOperator: {
            const ExtoptMergeAppendStateView *merge_append =
                (const ExtoptMergeAppendStateView *) state;

            if (!plan_tag_has_name(state->plan->type, "MergeAppend")) {
                break;
            }
            return append_plan_state_array(
                states, capacity, state_count, merge_append->children,
                merge_append->child_count, failure_reason);
        }
        case T_BitmapAndOperator:
        case T_BitmapOrOperator: {
            const ExtoptBitmapStateView *bitmap =
                (const ExtoptBitmapStateView *) state;
            const char *expected_plan_name =
                state->type == T_BitmapAndOperator ? "BitmapAnd" : "BitmapOr";

            if (!plan_tag_has_name(state->plan->type, expected_plan_name)) {
                break;
            }
            return append_plan_state_array(
                states, capacity, state_count, bitmap->children,
                bitmap->child_count, failure_reason);
        }
        case T_SubqueryScanOperator: {
            const ExtoptSubqueryScanStateView *subquery =
                (const ExtoptSubqueryScanStateView *) state;

            if (!plan_tag_has_name(state->plan->type, "SubqueryScan")) {
                break;
            }
            if (subquery->subplan == NULL) {
                if (failure_reason != NULL) {
                    *failure_reason = "null_subquery_plan_state";
                }
                return false;
            }
            return append_plan_state(
                states, capacity, state_count, subquery->subplan,
                failure_reason);
        }
        default:
            return true;
    }

    if (failure_reason != NULL) {
        *failure_reason = "special_plan_state_tag_mismatch";
    }
    return false;
}

/*
 * Recognize only executor containers with supported PlanState child layout.
 * Unknown/vector/remote nodes are opaque, even if they contain row children.
 */
static bool partial_traversable(NodeTag type)
{
    if (find_execution_node_binding(type) != NULL)
    {
        return true;
    }
    switch (type)
    {
        case T_HashOperator:
        case T_ResultOperator:
        case T_AppendOperator:
        case T_MergeAppendOperator:
        case T_RecursiveUnionOperator:
        case T_BitmapAndOperator:
        case T_BitmapOrOperator:
        case T_BitmapIndexScanOperator:
        case T_WorkTableScanOperator:
        case T_MergeJoinOperator:
        case T_GroupOperator:
        case T_WindowAggOperator:
        case T_UniqueOperator:
        case T_SetOpOperator:
        case T_LimitOperator:
            return true;
        default:
            return false;
    }
}

/* Collect the bounded physical executor graph while all state pointers are alive. */
bool
partial_gaussdb_execution_collect_plan_states(
    QueryDesc *query_desc,
    const ExtoptPlanState **states,
    int capacity,
    int *state_count,
    const char **failure_reason)
{
    const ExtoptPlanState *root;

    if (state_count != NULL) {
        *state_count = 0;
    }
    if (failure_reason != NULL) {
        *failure_reason = NULL;
    }
    if (query_desc == NULL || states == NULL || capacity <= 0 ||
        state_count == NULL) {
        if (failure_reason != NULL) {
            *failure_reason = "invalid_plan_state_arguments";
        }
        return false;
    }
    root = query_desc->planstate;
    if (query_desc->estate == NULL || root == NULL || root->plan == NULL ||
        root->estate != query_desc->estate) {
        if (failure_reason != NULL) {
            *failure_reason = "invalid_query_desc_planstate_root";
        }
        return false;
    }
    if (!append_plan_state(states, capacity, state_count, root,
                                  failure_reason)) {
        return false;
    }

    for (int index = 0; index < *state_count; index++) {
        const ExtoptPlanState *state = states[index];

        if (state != NULL && !partial_traversable(state->type))
        {
            /* No layout/row semantics are claimed below an opaque executor. */
            *failure_reason = "unsupported_executor_model";
            return false;
        }
        if (state == NULL || state->plan == NULL ||
            state->estate != query_desc->estate) {
            if (failure_reason != NULL) {
                *failure_reason = "invalid_plan_state";
            }
            return false;
        }
        if (!append_plan_state(states, capacity, state_count,
                                state->lefttree, failure_reason) ||
            !append_plan_state(states, capacity, state_count,
                                 state->righttree, failure_reason) ||
            !append_special_plan_states(states, capacity, state_count,
                                state, failure_reason) ||
            !append_subplan_states(states, capacity, state_count,
                               state->init_plan, failure_reason) ||
            !append_subplan_states(states, capacity, state_count,
                               state->sub_plan, failure_reason)) {
            return false;
        }
    }
    return true;
}

/* Read stable raw row counts; missing Instrumentation means this node has no evidence. */
bool
partial_gaussdb_execution_capture_evidence(
    const ExtoptPlanState *state,
    const ExtoptPlanObservation *observation,
    ExtoptExecutionEvidence *evidence)
{
    const ExtoptInstrumentationView *instrument;
    bool active;
    bool running;
    bool status;
    int rescan_count;

    if (state == NULL || observation == NULL || evidence == NULL) {
        return false;
    }
    memset(evidence, 0, sizeof(*evidence));
    /* A PlanState can exist before GaussDB publishes local runtime
     * instrumentation. Omit only this node so valid sibling facts survive. */
    if (state->instrument == NULL) {
        return true;
    }
    instrument = state->instrument;
    if (instrument->timing_enabled != 1 || instrument->running > 1 ||
        instrument->status > 1 || !isfinite(instrument->ntuples) ||
        instrument->ntuples < 0 || !isfinite(instrument->nloops) ||
        instrument->nloops < 0 || instrument->nloops > INT_MAX) {
        return false;
    }
    rescan_count = (int) instrument->nloops;
    if ((double) rescan_count != instrument->nloops) {
        return false;
    }
    active = instrument->starttime_seconds != 0 ||
        instrument->starttime_microseconds != 0;
    running = instrument->running != 0;
    status = instrument->status != 0;
    evidence->executed = status || running || active ||
        instrument->nloops > 0 || instrument->ntuples > 0;
    evidence->active = active;
    evidence->terminal = !active && status;
    evidence->rescan_count = rescan_count;
    /* GaussDB PRETTY A-rows uses cumulative ntuples directly, never loops. */
    evidence->returned_rows = instrument->ntuples;
    return true;
}

/* Render original PRETTY rows including the omitted query-block column. */
static void
dump_pretty_plan_information(OptUtilExplainPlanInformation *plan_information,
                                 GsString output)
{
    OptUtilExplainPlanTable *plan_table = plan_information->plan_table;

    if (plan_table == NULL || plan_table->columns[14] == -1) {
        optutil_explain_plan_information_dump_string(plan_information, output);
        return;
    }
    optutil_explain_append_title_string(output,
                                     plan_table->tuple_desc,
                                     plan_table->columns,
                                     plan_table->widths);

    int query_block_value_index = 0;
    for (int column = 0; column < 14; column++) {
        if (plan_table->columns[column] != -1) {
            query_block_value_index++;
        }
    }

    short query_block_columns[15];
    int query_block_widths[15] = {0};
    for (int column = 0; column < 15; column++) {
        query_block_columns[column] = -1;
    }
    query_block_columns[1] = 0;
    query_block_widths[1] = plan_table->widths[14];

    /*
     * The private dumper omits the query-block column. Append that value beside
     * each original row so the client can align observations with the replan.
     */
    for (int64 row = 0; row < plan_table->row_count; row++) {
        GsString plan_row = gs_make_string();

        optutil_explain_plan_table_row_to_string(plan_row,
                                         plan_table->values[row],
                                         plan_table->columns,
                                         plan_table->widths);
        if (plan_row->len > 0 && plan_row->data[plan_row->len - 1] == '\n') {
            plan_row->data[--plan_row->len] = '\0';
        }
        gs_append_string(output, plan_row->data);

        Datum query_block_value =
            plan_table->values[row][query_block_value_index];
        GsString query_block = gs_make_string();
        optutil_explain_plan_table_row_to_string(query_block,
                                         &query_block_value,
                                         query_block_columns,
                                         query_block_widths);
        gs_append_string(output, query_block->data);
    }
    exec_end_tup_output_string(output);

    plan_information->plan_table = NULL;
    optutil_explain_plan_information_dump_string(plan_information, output);
    plan_information->plan_table = plan_table;
}

/* Apply the snapshot deadline between expensive formatting stages. */
static void
check_pretty_progress(ExtoptProgressCheck progress_check, void *context)
{
    if (progress_check != NULL) {
        (*progress_check)(context);
    }
}

/* Render original execution PRETTY into borrowed backend memory within the deadline. */
void
partial_gaussdb_render_pretty_plan(QueryDesc *query_desc,
                                      ExtoptProgressCheck progress_check,
                                      void *progress_context,
                                      ExtoptPrettyPlan *pretty)
{
    OptUtilExplainState state;

    if (query_desc == NULL || pretty == NULL) {
        partial_report_error(ERRCODE_INVALID_PARAMETER_VALUE,
                                "partial pretty plan arguments are invalid");
    }
    check_pretty_progress(progress_check, progress_context);
    optutil_explain_proc_init_state(&state);
    state.analyze = true;
    state.blockname = true;
    state.outline = true;
    optutil_explain_set_perf_mode(&state, query_desc->plannedstmt);
    if (state.plan_information == NULL) {
        partial_report_error(ERRCODE_INTERNAL_ERROR,
                                "partial pretty plan information is unavailable");
    }
    optutil_explain_proc_begin_output(&state);
    optutil_explain_proc_print_plan(&state, query_desc);
    check_pretty_progress(progress_check, progress_context);
    state.str = gs_make_string();
    dump_pretty_plan_information(state.plan_information, state.str);
    optutil_explain_proc_end_output(&state);
    check_pretty_progress(progress_check, progress_context);
    if (state.str == NULL || state.str->data == NULL || state.str->len <= 0) {
        partial_report_error(ERRCODE_INTERNAL_ERROR,
                                "partial pretty output is empty");
    }

    pretty->data = state.str->data;
    pretty->length = state.str->len;
}

/* Preserve the previous extension hook, otherwise call the kernel executor. */
static void
call_executor_run(QueryDesc *query_desc,
                  ScanDirection direction,
                  long count)
{
    if (previous_executor_run != NULL) {
        (*previous_executor_run)(query_desc, direction, count);
    } else {
        exec_standard_executor_run(query_desc, direction, count);
    }
}

/* Dispatch through capture without changing the executor call contract. */
static void
observe_executor_run(QueryDesc *query_desc,
                     ScanDirection direction,
                     long count)
{
    /* Initialize validates the callback before publishing this hook. */
    Assert(capture_callbacks.executor_run != NULL);
    (*capture_callbacks.executor_run)(query_desc,
                                  direction,
                                  count,
                                  call_executor_run);
}

/* Install the thread's capture callback once, preserving a previously installed hook. */
void
partial_gaussdb_initialize(const ExtoptCaptureCallbacks *callbacks)
{
    if (callbacks == NULL || callbacks->executor_run == NULL) {
        partial_report_error(ERRCODE_INVALID_PARAMETER_VALUE,
                            "partial capture callbacks are incomplete");
    }

    capture_callbacks = *callbacks;
    if (ExecutorRun_hook != observe_executor_run)
    {
        previous_executor_run = ExecutorRun_hook;
        ExecutorRun_hook = observe_executor_run;
    }
}
