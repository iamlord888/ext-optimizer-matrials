/*-------------------------------------------------------------------------
 *
 * hint_adviser_sampling.c
 *    Seed base-table rows hints with built-in percentage TABLESAMPLE.
 *
 *-------------------------------------------------------------------------
 */
/*
 * The sampling module seeds RowsHintSet with fixed-block dynamic sampling.
 *
 * Important entry points:
 * get_rowshints_by_dynamic_sampling()
 *     Build a JSON PlanNode tree, collect base scan nodes, execute sampling
 *     SQL for each base table, and upsert rows hints before the first feedback
 *     iteration.
 * sample_target()
 *     Convert target block count to TABLESAMPLE system(percent), execute the
 *     count SQL, and write the biased selectivity rows estimate.
 *
 * Reading guide:
 * Start with get_rowshints_by_dynamic_sampling() for the module boundary, then
 * read collect_sampling_targets_from_plan() for PlanNode traversal and
 * sample_target() for database side effects. build_sampling_sql() contains the
 * exact SQL shape sent through the SQL execution path.
 *
 * Flow:
 *   EXPLAIN (OUTLINE ON, BLOCKNAME ON)
 *    |
 *    v
 *   shared JSON skeleton builder
 *    |
 *    v
 *  existing JSON parser + pre-collected block names -> PlanNode tree
 *    |
 *    v
 *  base scan relation + alias + block name + scan predicate
 *    |
 *    v
 *  pg_relation_size / block_size -> TABLESAMPLE system(percent)
 *    |
 *    v
 *  RowsHintSet upsert
 */
#include "postgres_fe.h"

#include <sys/time.h>

#include "hint_adviser_connection.h"
#include "hint_adviser_hint.h"
#include "hint_adviser_json.h"
#include "hint_adviser_log.h"
#include "hint_adviser_plan_tree.h"
#include "hint_adviser_sampling.h"
#include "hint_adviser_string.h"
#include "pqexpbuffer.h"

#define HINT_ADVISER_SAMPLING_SEED 8642719
#define HINT_ADVISER_SELECTIVITY_BIAS 0.001

/*
 * Brief: Read the shared monotonic duration clock in microseconds.
 * Return: Current time in microseconds.
 */
static pg_usec_time_t sampling_current_time_usec(void)
{
    return connection_monotonic_usec();
}

/*
 * Brief: Calculate elapsed milliseconds between two timestamps.
 * Inputs: start_time - Start timestamp; end_time - End timestamp.
 * Return: Non-negative elapsed milliseconds.
 */
static double sampling_elapsed_ms_between(pg_usec_time_t start_time,
                                          pg_usec_time_t end_time)
{
    if (end_time <= start_time) {
        return 0.0;
    }
    return (double) (end_time - start_time) / 1000.0;
}

/*
 * Brief: One base relation target collected from a PlanNode.
 * Description: Relation, alias, and block fields are borrowed from PlanNode;
 *              predicate_sql is owned by this target.
 */
typedef struct SamplingTarget
{
    const char *schema_name;    /* EXPLAIN "Schema", optional */
    const char *relation_name;  /* EXPLAIN "Relation Name" */
    const char *alias_name;     /* EXPLAIN "Alias", or relation name fallback */
    const char *block_name;     /* Pretty Query Block for rows hint scope */
    const char *origin_block_name; /* Base relation block for target suffix */
    char *predicate_sql;        /* Scan qual SQL, or true */
} SamplingTarget;

static double ceil_positive(double value);

static const char *
sampling_target_value(const char *value)
{
    return value == NULL || value[0] == '\0' ? "NULL" : value;
}

static void
log_sampling_target_skip(const SamplingTarget *target,
                         const char *context,
                         const char *sqlstate,
                         const char *detail)
{
    pg_log_warning("dynamic sampling skipped target: relation=%s alias=%s block=%s context=\"%s\" sqlstate=%s detail=%s",
                   sampling_target_value(target->relation_name),
                   sampling_target_value(target->alias_name),
                   sampling_target_value(target->block_name),
                   context == NULL ? "unknown" : context,
                   sqlstate == NULL || sqlstate[0] == '\0' ? "none" : sqlstate,
                   detail == NULL || detail[0] == '\0' ? "none" : detail);
}

/*
 * Brief: Remove GaussDB display-only markers from one JSON qual string.
 * Inputs: qual - Raw EXPLAIN predicate text.
 * Return: Clean predicate SQL, or NULL when the input has no usable predicate.
 */
static char *clean_plan_qual(const char *qual)
{
    if (qual == NULL || qual[0] == '\0') {
        return NULL;
    }

    char *clean = pstrdup(qual);
    static const char *markers[] = {
        ", (Expression Flatten Optimized)",
        ", (Predicates Reordered)",
        NULL
    };

    for (int i = 0; markers[i] != NULL; i++) {
        const char *marker = markers[i];
        char *pos = NULL;

        while ((pos = strstr(clean, marker)) != NULL) {
            memmove(pos, pos + strlen(marker), strlen(pos + strlen(marker)) + 1);
        }
    }
    if (strcmp(clean, "(Expression Flatten Optimized)") == 0 ||
        strcmp(clean, "(Predicates Reordered)") == 0) {
        return NULL;
    }
    return clean;
}

/*
 * Brief: Merge one EXPLAIN-rendered scan qual into a sampling predicate.
 * Inputs: predicate_sql - Existing predicate; qual - Raw EXPLAIN qual.
 * Return: Combined predicate SQL.
 */
static char *merge_sampling_qual(char *predicate_sql, const char *qual)
{
    char *clean = clean_plan_qual(qual);

    if (clean == NULL) {
        return predicate_sql;
    }
    if (predicate_sql == NULL) {
        return psprintf("(%s)", clean);
    }
    return psprintf("%s AND (%s)", predicate_sql, clean);
}

/*
 * Brief: Build predicate SQL from PlanNode scan quals.
 * Inputs: node - Base scan PlanNode.
 * Return: Predicate SQL for sampling; caller must pfree.
 */
static char *build_target_predicate_sql(const PlanNode *node)
{
    char *predicate_sql = NULL;

    predicate_sql = merge_sampling_qual(predicate_sql, node->filter);
    predicate_sql = merge_sampling_qual(predicate_sql, node->index_cond);
    predicate_sql = merge_sampling_qual(predicate_sql, node->recheck_cond);
    return predicate_sql == NULL ? pstrdup("true") : predicate_sql;
}

/*
 * Brief: Fetch a structure PlanNode tree for dynamic sampling.
 * Inputs: conn - Database connection; options - Tool options;
 *         sql - Original SQL; query_tree_info - External optimizer metadata.
 * Outputs: plan_tree - Parsed plan with query block names completed.
 * Return: 0 on success, 1 on failure.
 */
static int fetch_sampling_plan(PGconn *conn,
                               const HintAdviserOptions *options,
                               const char *sql,
                               const FrontendQueryInfo *query_tree_info,
                               PlanNode **plan_tree,
                               HintAdviserRunSummary *summary)
{
    *plan_tree = NULL;
    HintAdviserOutlineHints outline_hints = {0};
    List *explain_plan_nodes = NIL;
    char *prefetch_sql = build_hinted_sql(sql,
                                          NULL,
                                          "set(enable_index_nestloop off)",
                                          NULL);
    char *outline_sql = NULL;

    if (execute_explain_analyze(conn,
                        prefetch_sql,
                        &outline_hints,
                        &explain_plan_nodes,
                        false,
                        NULL,
                        options->deadline_end_time_usec,
                        summary) != 0) {
        *plan_tree = NULL;
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        pg_log_warning("stage=dynamic_sampling_prefetch EXPLAIN failed");
        return 1;
    }

    if (build_explain_node_tree(conn,
                                sql,
                                &outline_hints,
                                query_tree_info,
                                explain_plan_nodes,
                                plan_tree,
                                &outline_sql,
                                options->deadline_end_time_usec,
                                summary) != 0) {
        *plan_tree = NULL;
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        pg_log_warning("stage=dynamic_sampling_plan_tree failed");
        return 1;
    }
    return 0;
}

/*
 * Brief: Append one sampling target from a base scan PlanNode.
 * Inputs: node - Base scan PlanNode.
 * Outputs: targets - Receives a new SamplingTarget.
 * Return: 0 on success, 1 when required metadata is missing.
 */
static int append_sampling_target_from_plan(const PlanNode *node,
                                            List **targets)
{
    if (node->relation_name == NULL) {
        return 0;
    }
    if (node->alias == NULL || node->block_name == NULL) {
        pg_log_error("dynamic sampling scan node is incomplete: relation=%s alias=%s block=%s",
                     node->relation_name,
                     node->alias == NULL ? "NULL" : node->alias,
                     node->block_name == NULL ? "NULL" : node->block_name);
        return 1;
    }
    SamplingTarget *target = palloc0(sizeof(SamplingTarget));
    target->schema_name = node->schema_name;
    target->relation_name = node->relation_name;
    target->alias_name = node->alias;
    target->block_name = node->block_name;
    target->origin_block_name = node->origin_block_name != NULL ?
        node->origin_block_name : node->block_name;
    target->predicate_sql = build_target_predicate_sql(node);
    *targets = lappend(*targets, target);
    return 0;
}

/*
 * Brief: Recursively collect base relation sampling targets from a PlanNode.
 * Inputs: node - Current PlanNode.
 * Outputs: targets - Appended SamplingTarget pointers.
 * Return: 0 on success, 1 on malformed required metadata.
 */
static int collect_sampling_targets_from_plan(const PlanNode *node,
                                              List **targets)
{
    if (node == NULL) {
        return 0;
    }
    if (append_sampling_target_from_plan(node, targets) != 0) {
        return 1;
    }

    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell)) {
        if (collect_sampling_targets_from_plan(lfirst(cell), targets) != 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Brief: Build one TABLESAMPLE count SQL for a base relation.
 * Inputs: conn - Database connection; target - Base relation target;
 *         sample_percent - TABLESAMPLE system percentage.
 * Return: Sampling SQL text; caller must pfree.
 */
static char *build_sampling_sql(PGconn *conn,
                                const SamplingTarget *target,
                                double sample_percent)
{
    char *qualified_relation = quote_qualified_relation(conn,
                                                        target->schema_name,
                                                        target->relation_name);
    bool needs_alias = strcmp(target->alias_name, target->relation_name) != 0;
    char *quoted_alias = NULL;

    if (needs_alias) {
        quoted_alias = escape_identifier_or_fatal(conn, target->alias_name);
    }

    return psprintf("SELECT count(1)::float8 AS sampled_total_rows,\n"
                    "       coalesce(sum(CASE WHEN (%s) THEN 1 ELSE 0 END), 0)::float8 AS sampled_rows\n"
                    "FROM %s%s%s TABLESAMPLE system(%.15g) REPEATABLE (%d)",
                    target->predicate_sql,
                    qualified_relation,
                    needs_alias ? " AS " : "",
                    needs_alias ? quoted_alias : "",
                    sample_percent,
                    HINT_ADVISER_SAMPLING_SEED);
}

/*
 * Brief: Estimate rows with a fixed selectivity-side pessimistic bias.
 * Inputs: result - Sampling result with matched and total sampled rows;
 *         sample_percent - TABLESAMPLE percentage used by SQL.
 * Return: Biased full-table rows estimate, or -1 when sample percent is invalid.
 * Description: A full-table sample uses the exact matched row count.  For a
 *              partial sample, the 0.001 bias is added to selectivity, not
 *              multiplied into scaled rows.  When sampling observes no table
 *              rows or the estimate is below one row, return the one-row
 *              lower bound used by rows hints.
 */
static double estimate_biased_rows(const SamplingResult *result,
                                   double sample_percent)
{
    if (sample_percent <= 0 || sample_percent > 100) {
        pg_log_error("dynamic sampling has invalid sample percent");
        return -1;
    }
    if (result->sampled_total_rows <= 0) {
        return 0;
    }
    if (sample_percent == 100.0) {
        double rows = ceil_positive(result->sampled_rows);

        return rows;
    }

    double selectivity = result->sampled_rows / result->sampled_total_rows;
    double biased_selectivity = selectivity + HINT_ADVISER_SELECTIVITY_BIAS;
    if (biased_selectivity > 1.0) {
        biased_selectivity = 1.0;
    }

    double full_rows = result->sampled_total_rows * 100.0 / sample_percent;
    double rows = ceil_positive(full_rows * biased_selectivity);
    /* The Rows state owns the output minimum; preserve the sampling value. */
    return rows;
}

/*
 * Brief: Apply ceil() for non-negative dynamic sampling estimates.
 * Inputs: value - Estimated row count.
 * Return: Ceil value, preserving very large double values.
 */
static double ceil_positive(double value)
{
    if (value <= 0) {
        return 0;
    }
    if (value >= (double) PG_INT64_MAX) {
        return value;
    }

    int64 truncated = (int64) value;
    if ((double) truncated < value) {
        truncated++;
    }
    return (double) truncated;
}

/*
 * Brief: Build rows hint key for one sampled base relation.
 * Inputs: target - Base relation target.
 * Return: Key text like "@sel$1 a@\"sel$1\""; caller must pfree.
 */
static char *build_rows_hint_key(const SamplingTarget *target)
{
    PQExpBufferData key;

    initPQExpBuffer(&key);
    appendPQExpBuffer(&key, "@%s ", target->block_name);
    append_rows_hint_identifier(&key, target->alias_name);
    appendPQExpBufferChar(&key, '@');
    append_rows_hint_identifier(&key, target->origin_block_name);
    if (PQExpBufferDataBroken(key)) {
        pg_fatal("out of memory");
    }

    char *result = pstrdup(key.data);
    termPQExpBuffer(&key);
    return result;
}

/*
 * Brief: Sample one base relation and upsert its rows hint seed.
 * Inputs: conn - Database connection; options - Tool options;
 *         target - Base relation target.
 * Outputs: hint_set - Receives or updates one rows hint when sampling succeeds.
 * Return: 0 on success, 1 on failure.
 */
static int sample_target(PGconn *conn,
                         const HintAdviserOptions *options,
                         const SamplingTarget *target,
                         RowsHintSet *hint_set,
                         bool *hint_written)
{
    double relation_blocks = fetch_relation_blocks(conn,
                                                   options,
                                                   target->schema_name,
                                                   target->relation_name);
    if (relation_blocks <= 0) {
        log_sampling_target_skip(target,
                                 "relation size lookup",
                                 NULL,
                                 PQerrorMessage(conn));
        return 0;
    }

    double sample_percent =
        ((double) options->sampling_blocks / relation_blocks) * 100.0;
    if (sample_percent > 100.0) {
        sample_percent = 100.0;
    }

    char *sampling_sql = build_sampling_sql(conn, target, sample_percent);
    SamplingResult result = {0};
    if (!execute_sampling_sql(conn,
                              options,
                              target->relation_name,
                              target->alias_name,
                              target->block_name,
                              sampling_sql,
                              &result)) {
        return 0;
    }

    double rows = estimate_biased_rows(&result, sample_percent);
    if (rows < 0) {
        log_sampling_target_skip(target,
                                 "selectivity estimate",
                                 NULL,
                                 "invalid sample percent");
        return 0;
    }
    char *hint_key = build_rows_hint_key(target);
    rows_hint_set_upsert(hint_set, hint_key, rows, 0, options->verbose);
    if (hint_written != NULL) {
        *hint_written = true;
    }
    return 0;
}

/*
 * Brief: Seed rows hints from fixed-block dynamic sampling.
 * Inputs: conn - Database connection; options - Tool options;
 *         sql - Original SQL text; query_tree_info - External optimizer metadata.
 * Outputs: hint_set - Receives base-table rows hint seeds;
 *          summary - Receives the sampling outline fallback candidate.
 * Return: 0 on success, 1 on failure.
 */
int get_rowshints_by_dynamic_sampling(PGconn *conn,
                                     const HintAdviserOptions *options,
                                     const char *sql,
                                     const FrontendQueryInfo *query_tree_info,
                                     RowsHintSet *hint_set,
                                     HintAdviserRunSummary *summary)
{
    if (options->sampling_blocks == 0) {
        return 0;
    }

    log_message(options->verbose, "INFO", "progress=dynamic_sampling_start");
    pg_usec_time_t sampling_start_time = sampling_current_time_usec();

    int rc = 0;
    PlanNode *sampling_plan_tree = NULL;
    if (fetch_sampling_plan(conn,
                            options,
                            sql,
                            query_tree_info,
                            &sampling_plan_tree,
                            summary) == 0) {
        List *targets = NIL;
        rc = collect_sampling_targets_from_plan(sampling_plan_tree, &targets);
        for (ListCell *cell = list_head(targets);
            rc == 0 && cell != NULL;
            cell = lnext(targets, cell)) {
            (void) sample_target(conn, options, lfirst(cell), hint_set, NULL);
        }
    } else {
        rc = 1;
    }

    summary->sampling_wall_time_ms = sampling_elapsed_ms_between(sampling_start_time,
                                    sampling_current_time_usec());
    if (rc != 0) {
        pg_log_warning("stage=dynamic_sampling failed");
    }
    log_message(options->verbose, "INFO",
                "progress=dynamic_sampling_finish status=%s sampling_wall_time_ms=%.3f",
                rc == 0 ? "success" : "failed",
                summary->sampling_wall_time_ms);
    return rc;
}
