/* Proof consumes uniform execution state; target eligibility lives in Rows. */
#include "postgres_fe.h"

#include <ctype.h>
#include <math.h>

#include "hint_adviser_partial_proof.h"
#include "hint_adviser_pretty_parser.h"

/* Local EOF is exact only before any prior rescan and outside an active call.
 * A-rows greater than E-rows remains a usable lower bound even while a call is
 * active or after a rescan; those states only prevent the stronger EXACT fact.
 */
RowsEvidence partial_observation_evidence(const PartialObservation *observation)
{
    if (observation->execution_state == PARTIAL_EXECUTION_TERMINAL &&
        observation->rescan_count == 0)
    {
        return ROWS_EVIDENCE_EXACT;
    }
    if (observation->actual_rows > observation->plan_rows)
    {
        return ROWS_EVIDENCE_LOWER_BOUND;
    }
    return ROWS_EVIDENCE_NONE;
}

/* Treat absent block names consistently, never substitute a parent block. */
static bool partial_text_equal(const char *left, const char *right)
{
    return left == NULL || right == NULL ? left == right : strcmp(left, right) == 0;
}

/* PRETTY rounds E-rows for display; compare only that declared representation. */
static bool partial_displayed_rows(double actual, double displayed)
{
    char text[512];

    snprintf(text, sizeof(text), "%.0f", actual);
    return isfinite(actual) && actual >= 0 && strtod(text, NULL) == displayed;
}

/* Find a unique rendered id in a PRETTY table. Duplicate ids invalidate mapping. */
static const PlanNode *partial_pretty_find(const List *rows, int id)
{
    const PlanNode *found = NULL;

    for (const ListCell *cell = list_head(rows); cell != NULL; cell = lnext(rows, cell))
    {
        const PlanNode *row = lfirst(cell);

        if (row->pretty_id == id)
        {
            if (found != NULL)
            {
                return NULL;
            }
            found = row;
        }
    }
    return found;
}

/* Flatten JSON topology with a resource/depth guard and validate parent links. */
static bool partial_flatten(PlanNode *node, PlanNode *parent, List **nodes, int depth)
{
    if (node == NULL || node->parent != parent || depth > 256 ||
        list_length(*nodes) >= HAPF_MAX_NODES)
    {
        return false;
    }
    const char *arrow = node->pretty_operation == NULL ? NULL : strstr(node->pretty_operation, "->");

    if (arrow == NULL || arrow - node->pretty_operation != depth * 3)
    {
        /* Equal preorders are insufficient: a different parent can otherwise
         * silently bind a fact to another same-type physical operator. */
        return false;
    }
    *nodes = lappend(*nodes, node);
    for (ListCell *cell = list_head(node->children); cell != NULL; cell = lnext(node->children, cell))
    {
        if (!partial_flatten(lfirst(cell), node, nodes, depth + 1))
        {
            return false;
        }
    }
    return true;
}

/*
 * Validate the complete original/replanned identity before applying any fact.
 * PRETTY operation text retains indentation, child ids and anchor information;
 * exact equality checks the original topology alongside JSON parent links.
 * Any contradiction fails the whole round, never only the mismatched node.
 */
PartialProofResult partial_prove(const PartialSnapshot *snapshot,
    const List *replanned_pretty, PlanNode *plan)
{
    PartialProofResult result = {.reason = "partial_mapping_invalid"};

    if (!snapshot->parsed || snapshot->invalid || plan == NULL)
    {
        return result;
    }
    List *original = pretty_parse_snapshot(snapshot->pretty);
    List *nodes = NIL;

    if (original == NIL || list_length(original) != list_length(replanned_pretty) ||
        !partial_flatten(plan, NULL, &nodes, 0) || list_length(nodes) != list_length(original))
    {
        result.reason = "partial_topology_or_pretty_invalid";
        return result;
    }
    for (const ListCell *cell = list_head(nodes); cell != NULL; cell = lnext(nodes, cell))
    {
        PlanNode *node = lfirst(cell);
        const PlanNode *before = partial_pretty_find(original, node->pretty_id);
        const PlanNode *after = partial_pretty_find(replanned_pretty, node->pretty_id);

        if (before == NULL || after == NULL ||
            partial_pretty_find(nodes, node->pretty_id) != node ||
            !partial_text_equal(node->pretty_operation, after->pretty_operation) ||
            !partial_text_equal(before->pretty_operation, after->pretty_operation) ||
            !partial_text_equal(before->block_name, after->block_name) ||
            !partial_text_equal(node->block_name, after->block_name) ||
            !node->has_plan_rows ||
            !after->has_plan_rows || before->plan_rows != after->plan_rows ||
            node->plan_rows != after->plan_rows ||
            !node->has_json_plan_rows || !partial_displayed_rows(node->json_plan_rows, after->plan_rows))
        {
            result.reason = "partial_plan_identity_disagreement";
            return result;
        }
    }
    PlanNode **mapped = palloc0(sizeof(PlanNode *) * Max(snapshot->observation_count, 1));

    for (int i = 0; i < snapshot->observation_count; i++)
    {
        const PartialObservation *observation = &snapshot->observations[i];
        const PlanNode *before = partial_pretty_find(original, observation->plan_id);

        for (const ListCell *cell = list_head(nodes); cell != NULL; cell = lnext(nodes, cell))
        {
            PlanNode *node = lfirst(cell);

            if (node->pretty_id == observation->plan_id)
            {
                if (mapped[i] != NULL)
                {
                    result.reason = "partial_node_mapping_ambiguous";
                    return result;
                }
                mapped[i] = node;
            }
        }
        if (before == NULL || mapped[i] == NULL ||
            !partial_displayed_rows(observation->plan_rows, before->plan_rows) ||
            (before->has_actual_rows && !partial_displayed_rows(observation->actual_rows,
                                                            before->actual_rows)))
        {
            result.reason = "partial_observation_mapping_disagreement";
            return result;
        }
    }
    /* No state is published until all eligible mappings have passed. */
    for (ListCell *cell = list_head(nodes); cell != NULL; cell = lnext(nodes, cell))
    {
        PlanNode *node = lfirst(cell);

        node->has_actual_rows = false;
        node->rows_evidence = ROWS_EVIDENCE_NONE;
    }
    for (int i = 0; i < snapshot->observation_count; i++)
    {
        RowsEvidence evidence = partial_observation_evidence(&snapshot->observations[i]);

        if (evidence != ROWS_EVIDENCE_NONE)
        {
            mapped[i]->rows_evidence = evidence;
            mapped[i]->has_actual_rows = true;
            mapped[i]->actual_rows = snapshot->observations[i].actual_rows;
            result.trusted_nodes++;
        }
    }
    pfree(mapped);
    result.valid = true;
    result.reason = result.trusted_nodes > 0 ? "trusted_nodes" : "no_trusted_nodes";
    return result;
}
