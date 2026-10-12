/*-------------------------------------------------------------------------
 *
 * hint_adviser_plan_tree.c
 *    Extract and apply pretty EXPLAIN plan rows.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    contrib/hint_adviser/hint_adviser_plan_tree.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * The plan tree module bridges already parsed GaussDB pretty plan metadata
 * and JSON plan skeletons when JSON format cannot print everything hint
 * generation needs.
 *
 * Important entry points:
 * build_explain_node_tree()
 *                                          Build and fully complete one
 *                                          outlined PlanTree.
 *
 * Reading guide:
 * Start with build_explain_node_tree for the complete flow, then read
 * apply_pretty_anchors for subplan expansion handling and
 * complete_rows_hint_targets for target completion.
 */
#include "postgres_fe.h"
#include "hint_adviser_pretty_parser.h"

#include "hint_adviser_log.h"

#include "hint_adviser_plan_tree.h"
#include "hint_adviser_connection.h"
#include "hint_adviser_json.h"
#include "hint_adviser_rows_private.h"
#include "hint_adviser_sql_log.h"
#include "hint_adviser_string.h"

/*
 * Brief: Pretty EXPLAIN anchor for a JSON named subplan subtree.
 * Description: Operation text such as "[13, CTE frequent_ss_items]" or
 *              "[11, InitPlan 1 (returns $0)]" or "[4, SubPlan 1]" tells
 *              where the named JSON subplan subtree is rendered.
 */
typedef struct PrettyAnchor
{
    char *subplan_name;         /* JSON Subplan Name to match */
    PlanNode *row;              /* First pretty row of the anchored subtree */
} PrettyAnchor;

/*
 * Brief: Copy the JSON text returned by EXPLAIN FORMAT JSON.
 * Inputs: res - Query result; data - Output char **.
 * Outputs: data - Receives owned JSON text.
 * Return: 0 on success, 1 on malformed result.
 */
static int copy_explain_json_result(PGresult *res, void *data)
{
    char **explain_json_text = data;

    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) < 1 ||
        PQnfields(res) < 1 ||
        PQgetisnull(res, 0, 0)) {
        pg_log_error("EXPLAIN JSON SKELETON returned no usable first text column: status %d ntuples %d nfields %d",
                     PQresultStatus(res),
                     PQntuples(res),
                     PQnfields(res));
        return 1;
    }
    *explain_json_text = pstrdup(PQgetvalue(res, 0, 0));
    return 0;
}

/*
 * Description: Build outline SQL, execute JSON EXPLAIN, and return a node tree.
 * Inputs: conn - database connection; sql - original SQL, or empty for no-op;
 *         outline_hints - parsed outline hints;
 *         query_tree_info - External optimizer query-tree metadata;
 *         explain_plan_nodes - pretty rows collected before JSON skeleton.
 * Outputs: plan_tree - completed plan tree with pretty metadata and Rows
 *          targets attached;
 *          outline_sql - outline-hinted SQL used for this JSON EXPLAIN.
 * Return: 0 on success or no-op, 1 on failure.
 */
int build_explain_node_tree(PGconn *conn,
                            const char *sql,
                            const HintAdviserOutlineHints *outline_hints,
                            const FrontendQueryInfo *query_tree_info,
                            const List *explain_plan_nodes,
                            PlanNode **plan_tree,
                            char **outline_sql,
                            int64 deadline_end_time_usec,
                            HintAdviserRunSummary *summary)
{
    *plan_tree = NULL;
    *outline_sql = NULL;
    if (sql == NULL || sql[0] == '\0') {
        return 0;
    }
    *outline_sql = build_hinted_sql(sql,
                                    NULL,
                                    NULL,
                                    outline_hints);
    if (*outline_sql == NULL) {
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        pg_log_warning("stage=outline_sql could not build hinted SQL");
        return 1;
    }
    return candidate_plan_build(conn, *outline_sql, outline_hints, query_tree_info,
                               explain_plan_nodes, plan_tree, deadline_end_time_usec, summary);
}

/* Complete a verified physical plan's aliases and Rows targets; return 1 on failure. */
int candidate_plan_complete(PlanNode *plan, const HintAdviserOutlineHints *outline,
    const FrontendQueryInfo *query_info)
{
    int rc = complete_plan_subquery_aliases(query_info, plan);

    if (rc == 0)
    {
        complete_rows_hint_targets(plan, (const char *const *) outline->items, outline->count);
    }
    return rc;
}

/*
 * Reconstruct exactly candidate_sql without replacing its hints. original SQL
 * and deliverable Outline assembly belong to the caller; this function only
 * supplies the JSON topology for the PRETTY plan actually being validated.
 */
int candidate_plan_build(PGconn *conn, const char *candidate_sql,
    const HintAdviserOutlineHints *outline_hints, const FrontendQueryInfo *query_tree_info,
    const List *explain_plan_nodes, PlanNode **plan_tree, int64 deadline_end_time_usec,
    HintAdviserRunSummary *summary)
{
    int rc = candidate_plan_skeleton(conn, candidate_sql, explain_plan_nodes,
                                   plan_tree, deadline_end_time_usec, summary);

    if (rc == 0)
    {
        rc = candidate_plan_complete(*plan_tree, outline_hints, query_tree_info);
    }
    if (rc != 0)
    {
        *plan_tree = NULL;
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
    }
    return rc;
}

/* Return physical execution topology before alias-only wrappers are added. */
int candidate_plan_skeleton(PGconn *conn, const char *candidate_sql,
    const List *explain_plan_nodes, PlanNode **plan_tree, int64 deadline_end_time_usec,
    HintAdviserRunSummary *summary)
{
    *plan_tree = NULL;
    char *explain_sql = psprintf("EXPLAIN (FORMAT JSON) %s", candidate_sql);
    char *explain_json_text = NULL;
    if (logged_exec_each_result(conn,
                                explain_sql,
                                "EXPLAIN JSON SKELETON",
                                deadline_end_time_usec,
                                copy_explain_json_result,
                                &explain_json_text,
                                false) != 0) {
        summary->finish_reason = deadline_end_time_usec >= 0 ?
            HINT_ADVISER_FINISH_TIME_BUDGET_REACHED :
            HINT_ADVISER_FINISH_ERROR;
        pg_log_warning("stage=json_skeleton EXPLAIN failed");
        return 1;
    }

    char *explain_json = normalize_explain_json_text(explain_json_text);

    *plan_tree = parse_explain_json(explain_json);
    if (*plan_tree == NULL) {
        pg_log_error("EXPLAIN JSON skeleton did not return a plan root");
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        pg_log_warning("stage=json_skeleton plan root is missing");
        return 1;
    }

    int rc = complete_actual_rows_and_blockname(*plan_tree,
                                                explain_plan_nodes);
    if (rc != 0) {
        *plan_tree = NULL;
        summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        pg_log_warning("stage=complete_explain_plan_tree failed");
    }
    return rc;
}

/*
 * Brief: Check whether a plan node is rendered through a pretty anchor.
 * Inputs: node - Plan node.
 * Return: true if node is a named JSON subplan with a pretty anchor.
 */
static bool is_pretty_anchored_plan(const PlanNode *node)
{
    return node != NULL &&
        node->subplan_name != NULL &&
        node->parent_relationship != NULL &&
        (strcmp(node->parent_relationship, "exec_init_plan") == 0 ||
         strcmp(node->parent_relationship, "SubPlan") == 0);
}

/*
 * Brief: Match JSON named subplan with a pretty anchor.
 * Inputs: node - Plan node; subplan_name - Pretty anchor subplan name.
 * Return: true on name match.
 */
static bool pretty_anchor_matches(const PlanNode *node, const char *subplan_name)
{
    if (!is_pretty_anchored_plan(node) || subplan_name == NULL) {
        return false;
    }
    return strcmp(node->subplan_name, subplan_name) == 0;
}

/*
 * Brief: Find one pretty-anchored JSON subplan by name.
 * Inputs: node - Current plan node; subplan_name - JSON Subplan Name.
 * Return: Matching plan node, or NULL.
 */
static PlanNode *find_pretty_anchored_plan(PlanNode *node, const char *subplan_name)
{
    if (pretty_anchor_matches(node, subplan_name)) {
        return node;
    }

    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell)) {
        PlanNode *found = find_pretty_anchored_plan(lfirst(cell), subplan_name);

        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/*
 * Brief: Collect plan nodes in the order GaussDB pretty prints one subtree.
 * Inputs: node - Current plan node; include_anchor_root - Whether a
 *         pretty-anchored subplan root should be included.
 * Outputs: nodes - Receives borrowed PlanNode pointers.
 * Description: Pretty-anchored children are consumed by anchor matching, so
 *              this traversal skips them below the current root.
 */
static void collect_plan_nodes_in_pretty_order(PlanNode *node,
                                               List **nodes,
                                               bool include_anchor_root)
{
    if (node == NULL ||
        (is_pretty_anchored_plan(node) && !include_anchor_root)) {
        return;
    }

    *nodes = lappend(*nodes, node);
    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell)) {
        PlanNode *child = lfirst(cell);

        if (is_pretty_anchored_plan(child)) {
            continue;
        }
        collect_plan_nodes_in_pretty_order(child, nodes, false);
    }
}

/*
 * Brief: Collect pretty anchors in reverse pretty order.
 * Inputs: explain_plan_nodes - Pretty rows.
 * Return: Owned PrettyAnchor list.
 */
static List *collect_pretty_anchor_descriptors(const List *explain_plan_nodes)
{
    List *anchors = NIL;

    for (const ListCell *cell = list_head(explain_plan_nodes);
         cell != NULL;
         cell = lnext(explain_plan_nodes, cell)) {
        PlanNode *row = lfirst(cell);
        char *subplan_name = row == NULL || row->pretty_anchor_name == NULL ?
            NULL : pstrdup(row->pretty_anchor_name);

        if (subplan_name == NULL) {
            continue;
        }

        PrettyAnchor *anchor = palloc0(sizeof(PrettyAnchor));
        anchor->subplan_name = subplan_name;
        anchor->row = row;
        anchors = lcons(anchor, anchors);
    }
    return anchors;
}

/*
 * Brief: Consume unclaimed pretty rows from one subplan anchor.
 * Inputs: explain_plan_nodes - Pretty rows; anchor - First row of the subplan;
 *         needed_rows - Number of JSON nodes to match.
 * Inputs: claimed - Rows already paired with other anchors in this mapping.
 * Return: Borrowed pretty row list, or NULL on shortage.
 */
static List *consume_anchor_pretty_rows(const List *explain_plan_nodes,
                                        const PrettyAnchor *anchor,
                                        int needed_rows,
                                        int *consumed_rows,
                                        const List *claimed)
{
    List *rows = NIL;
    bool found_anchor = false;

    for (const ListCell *cell = list_head(explain_plan_nodes);
         cell != NULL;
         cell = lnext(explain_plan_nodes, cell)) {
        PlanNode *row = lfirst(cell);

        if (row == anchor->row) {
            found_anchor = true;
        }
        if (!found_anchor || row == NULL || list_member_ptr(claimed, row)) {
            continue;
        }

        rows = lappend(rows, row);
        if (list_length(rows) == needed_rows) {
            return rows;
        }
    }

    if (consumed_rows != NULL) {
        *consumed_rows = list_length(rows);
    }
    return NIL;
}

/*
 * Brief: Pair every pretty-anchored subplan with its JSON subtree.
 * Inputs: root - JSON plan root; explain_plan_nodes - Pretty rows.
 * Outputs: paired_nodes/paired_rows - Borrowed pairs; no PlanNode is modified.
 * Return: 0 on success, 1 on mismatch.
 */
static int collect_anchored_plan_pairs(PlanNode *root,
                                       const List *explain_plan_nodes,
                                       List **paired_nodes,
                                       List **paired_rows)
{
    List *anchors = collect_pretty_anchor_descriptors(explain_plan_nodes);

    for (ListCell *cell = list_head(anchors);
         cell != NULL;
         cell = lnext(anchors, cell)) {
        PrettyAnchor *anchor = lfirst(cell);
        PlanNode *anchored_plan =
            find_pretty_anchored_plan(root, anchor->subplan_name);

        if (anchored_plan == NULL) {
            pg_log_error("pretty EXPLAIN anchored subplan \"%s\" has no matching JSON subplan",
                         anchor->subplan_name);
            return 1;
        }

        List *nodes = NIL;
        int consumed_rows = 0;
        collect_plan_nodes_in_pretty_order(anchored_plan, &nodes, true);
        List *rows = consume_anchor_pretty_rows(explain_plan_nodes, anchor,
                                                list_length(nodes),
                                                &consumed_rows, *paired_rows);
        if (rows == NIL) {
            pg_log_error("pretty EXPLAIN anchored subplan \"%s\" row count mismatch: plan nodes %d, pretty rows %d",
                         anchor->subplan_name,
                         list_length(nodes),
                         rows == NIL ? consumed_rows : list_length(rows));
            return 1;
        }
        *paired_nodes = list_concat(*paired_nodes, nodes);
        *paired_rows = list_concat(*paired_rows, rows);
    }

    return 0;
}

/*
 * Brief: Collect pretty rows that were not consumed by anchored subplans.
 * Inputs: explain_plan_nodes - Pretty rows.
 * Return: Borrowed pretty row list for main-tree matching.
 */
static List *collect_unconsumed_pretty_rows(const List *explain_plan_nodes,
                                           const List *claimed)
{
    List *rows = NIL;

    for (const ListCell *cell = list_head(explain_plan_nodes);
         cell != NULL;
         cell = lnext(explain_plan_nodes, cell)) {
        PlanNode *row = lfirst(cell);

        if (row == NULL || !list_member_ptr(claimed, row)) {
            rows = lappend(rows, row);
        }
    }
    return rows;
}

/*
 * Brief: Apply pretty plan rows to collected plan nodes.
 * Inputs: nodes - Plan nodes in matching order;
 *         explain_plan_nodes - Pretty rows in matching order.
 * Outputs: nodes - Plan nodes receive metadata only after every pair validates.
 * Return: 0 on success, 1 on mismatch or incomplete data.
 */
static int apply_pretty_rows_to_nodes(const List *nodes,
                                      const List *explain_plan_nodes)
{
    int node_count = list_length(nodes);
    int pretty_row_count = list_length(explain_plan_nodes);
    if (node_count != pretty_row_count) {
        return 1;
    }

    const ListCell *node_cell = list_head(nodes);
    const ListCell *pretty_row_cell = list_head(explain_plan_nodes);
    List *seen = NIL;

    /* Validate minimal structural invariants before publishing any row facts. */
    for (int index = 0; index < node_count; index++) {
        PlanNode *node = lfirst(node_cell);
        PlanNode *row = lfirst(pretty_row_cell);
        if (list_member_ptr(seen, node) || row == NULL ||
            (node->block_name != NULL && row->block_name != NULL &&
             strcmp(node->block_name, row->block_name) != 0) ||
            (node->relation_name != NULL &&
             (row->block_name == NULL || row->block_name[0] == '\0')))
        {
            /* Traversal order alone cannot establish identity across replans. */
            pg_log_error("JSON/PRETTY identity mismatch at plan node %d", index + 1);
            list_free(seen);
            return 1;
        }
        seen = lappend(seen, node);
        node_cell = lnext(nodes, node_cell);
        pretty_row_cell = lnext(explain_plan_nodes, pretty_row_cell);
    }
    list_free(seen);
    node_cell = list_head(nodes);
    pretty_row_cell = list_head(explain_plan_nodes);
    for (int index = 0; index < node_count; index++)
    {
        PlanNode *node = lfirst(node_cell);
        PlanNode *row = lfirst(pretty_row_cell);
        const char *block_name = row->block_name;
        const char *origin_block_name = row->origin_block_name;

		if (block_name == NULL || block_name[0] == '\0') {
			node_cell = lnext(nodes, node_cell);
			pretty_row_cell = lnext(explain_plan_nodes, pretty_row_cell);
			continue;
		}

        if (node->block_name == NULL) {
            node->block_name = pstrdup(block_name);
        }
        if (node->relation_name != NULL && origin_block_name != NULL) {
            node->origin_block_name = pstrdup(origin_block_name);
        }
        if (row != NULL && row->has_actual_rows) {
            node->actual_rows = row->actual_rows;
            node->has_actual_rows = true;
            node->rows_evidence = row->rows_evidence;
        }
        if (row != NULL && row->has_plan_rows) {
            node->plan_rows = row->plan_rows;
            node->has_plan_rows = true;
        }
		/* Replay correlates the JSON operator with this exact Pretty row. */
		if (row != NULL)
		{
			node->pretty_id = row->pretty_id;
			node->pretty_operation = row->pretty_operation;
		}
        node_cell = lnext(nodes, node_cell);
        pretty_row_cell = lnext(explain_plan_nodes, pretty_row_cell);
    }

    return 0;
}

/*
 * Brief: Apply pretty plan rows to a plan tree.
 * Inputs: root - Plan tree root; explain_plan_nodes - Pretty plan rows.
 * Outputs: root - Plan nodes receive missing block names.
 * Return: 0 on success, 1 on mismatch or incomplete data.
 */
int complete_actual_rows_and_blockname(PlanNode *root,
                                       const List *explain_plan_nodes)
{
    List *nodes = NIL;
    List *rows = NIL;
    if (collect_anchored_plan_pairs(root, explain_plan_nodes, &nodes, &rows) != 0)
    {
        list_free(nodes);
        list_free(rows);
        return 1;
    }
    List *main_rows = collect_unconsumed_pretty_rows(explain_plan_nodes, rows);

    collect_plan_nodes_in_pretty_order(root, &nodes, false);
    rows = list_concat(rows, main_rows);
    int rc = apply_pretty_rows_to_nodes(nodes, rows);
    if (rc != 0) {
        pg_log_error("pretty EXPLAIN row count mismatch: plan nodes %d, pretty rows %d",
                     list_length(nodes),
                     list_length(rows));
    }
    list_free(nodes);
    list_free(rows);
    return rc;
}
