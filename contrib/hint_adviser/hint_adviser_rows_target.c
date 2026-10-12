/*-------------------------------------------------------------------------
 *
 * hint_adviser_rows_target.c
 *    Complete PlanTree Rows targets and collect persistent hints.
 *
 *-------------------------------------------------------------------------
 */
/*
 * This module writes Rows targets into PlanNode, collects eligible scan and
 * JOIN observations in traversal order, then applies them to RowsHintSet.
 *
 * Flow:
 *   PlanTree completion -> PlanNode.targets -> RowsHintSet
 *              ^
 *              | guarded outline fallback for JOIN nodes
 */
#include "postgres_fe.h"

#include "hint_adviser_log.h"
#include "hint_adviser_rows_private.h"

/*
 * Brief: Classification used while resolving one PlanNode into Rows targets.
 * Description: Transparent nodes pass one child through; semantic boundaries
 *              and unsupported nodes prevent a Rows hint from being emitted.
 */
typedef enum PlanTargetNodeKind
{
    PLAN_TARGET_NODE_UNKNOWN = 0,
    PLAN_TARGET_NODE_JOIN,
    PLAN_TARGET_NODE_BASE_SCAN,
    PLAN_TARGET_NODE_NAMED_WRAPPER,
    PLAN_TARGET_NODE_TRANSPARENT,
    PLAN_TARGET_NODE_SEMANTIC_BOUNDARY
} PlanTargetNodeKind;

typedef enum RowsTargetIncompleteReason
{
    ROWS_TARGET_INCOMPLETE_NONE = 0,
    ROWS_TARGET_INCOMPLETE_SEMANTIC_BOUNDARY = (1U << 0),
    ROWS_TARGET_INCOMPLETE_CROSS_QUERY_BLOCK = (1U << 1),
    ROWS_TARGET_INCOMPLETE_UNSUPPORTED_NODE = (1U << 2),
    ROWS_TARGET_INCOMPLETE_CHILD = (1U << 3),
    ROWS_TARGET_INCOMPLETE_MISSING_TARGET = (1U << 4),
    ROWS_TARGET_INCOMPLETE_TARGET_COLLISION = (1U << 5),
    ROWS_TARGET_INCOMPLETE_INVALID_ARITY = (1U << 6)
} RowsTargetIncompleteReason;

/*
 * Brief: Test whether a plan node has a supported Rows join type.
 * Inputs: node - Plan node to classify.
 * Return: True for Nested Loop, Hash Join, or Merge Join; callers validate
 *         child arity separately.
 */
bool
rows_plan_node_is_join(const PlanNode *node)
{
    if (node->node_type == NULL) {
        return false;
    }
    return strcmp(node->node_type, "Nested Loop") == 0 ||
           strcmp(node->node_type, "Hash Join") == 0 ||
           strcmp(node->node_type, "Merge Join") == 0;
}

/*
 * Brief: Test whether a plan node directly scans a Rows hint target.
 * Inputs: node - Plan node to classify.
 * Return: True for a supported base scan node type.
 */
static bool
is_base_scan_target_node(const PlanNode *node)
{
    if (node->node_type == NULL) {
        return false;
    }
    return strcmp(node->node_type, "Seq Scan") == 0 ||
           strcmp(node->node_type, "Index Scan") == 0 ||
           strcmp(node->node_type, "Index Only Scan") == 0 ||
           strcmp(node->node_type, "Bitmap Heap Scan") == 0 ||
           strcmp(node->node_type, "Tid Scan") == 0 ||
           strcmp(node->node_type, "TID Scan") == 0 ||
           strcmp(node->node_type, "Foreign Scan") == 0 ||
           strcmp(node->node_type, "Function Scan") == 0 ||
           strcmp(node->node_type, "Values Scan") == 0 ||
           strcmp(node->node_type, "Named Tuplestore Scan") == 0;
}

/*
 * Brief: Test whether a plan node exposes a named child target.
 * Inputs: node - Plan node to classify.
 * Return: True for supported subquery, CTE, or worktable wrappers.
 */
static bool
is_named_wrapper_node(const PlanNode *node)
{
    return node->node_type != NULL &&
        (strcmp(node->node_type, "Subquery Scan") == 0 ||
         strcmp(node->node_type, "CTE Scan") == 0 ||
         strcmp(node->node_type, "WorkTable Scan") == 0);
}

/*
 * Brief: Classify a plan node by its Rows hint target behavior.
 * Inputs: node - Plan node to classify.
 * Return: Target-resolution category for the node.
 */
static PlanTargetNodeKind
classify_target_node(const PlanNode *node)
{
    if (rows_plan_node_is_join(node)) {
        return PLAN_TARGET_NODE_JOIN;
    }
    if (is_named_wrapper_node(node)) {
        return PLAN_TARGET_NODE_NAMED_WRAPPER;
    }
    if (is_base_scan_target_node(node)) {
        return PLAN_TARGET_NODE_BASE_SCAN;
    }
    if (node->node_type == NULL) {
        return PLAN_TARGET_NODE_UNKNOWN;
    }
    if (strcmp(node->node_type, "Hash") == 0 ||
        strcmp(node->node_type, "Materialize") == 0 ||
        strcmp(node->node_type, "Sort") == 0) {
        return PLAN_TARGET_NODE_TRANSPARENT;
    }
    if (strcmp(node->node_type, "Aggregate") == 0 ||
        strcmp(node->node_type, "Group") == 0 ||
        strcmp(node->node_type, "WindowAgg") == 0 ||
        strcmp(node->node_type, "Unique") == 0 ||
        strcmp(node->node_type, "SetOp") == 0 ||
        strcmp(node->node_type, "Append") == 0 ||
        strcmp(node->node_type, "Merge Append") == 0 ||
        strcmp(node->node_type, "Limit") == 0 ||
        strcmp(node->node_type, "Result") == 0) {
        return PLAN_TARGET_NODE_SEMANTIC_BOUNDARY;
    }
    return PLAN_TARGET_NODE_UNKNOWN;
}

/*
 * Brief: Append a target unless an equivalent target already exists.
 * Inputs: targets - RowsHintTarget pointer list; target - Target to append.
 * Outputs: targets - Receives target when it is non-NULL and unique.
 */
static void
target_list_add(List **targets, RowsHintTarget *target)
{
    if (target != NULL &&
        !rows_hint_target_list_contains(*targets, target)) {
        *targets = lappend(*targets, target);
    }
}

/*
 * Brief: Append every unique target from one list to another.
 * Inputs: destination - Destination target list; source - Source target list.
 * Outputs: destination - Receives unique borrowed targets from source.
 */
static void
target_list_copy(List **destination, const List *source)
{
    for (ListCell *cell = list_head(source);
         cell != NULL;
         cell = lnext(source, cell)) {
        target_list_add(destination, lfirst(cell));
    }
}

/*
 * Brief: Test whether two target lists share an equivalent target.
 * Inputs: left - First target list; right - Second target list.
 * Return: True when at least one target occurs in both lists.
 */
static bool
target_lists_overlap(const List *left, const List *right)
{
    for (ListCell *cell = list_head(left);
         cell != NULL;
         cell = lnext(left, cell)) {
        if (rows_hint_target_list_contains(right, lfirst(cell))) {
            return true;
        }
    }
    return false;
}

/*
 * Brief: Build a Rows target from a scan or named wrapper node.
 * Inputs: node - Supported plan node with target metadata.
 * Return: Typed target in the current memory context, or NULL when metadata is
 *         missing or the node is an unaddressable Append child.
 */
static RowsHintTarget *
build_scan_target(const PlanNode *node)
{
    if (!is_base_scan_target_node(node) && !is_named_wrapper_node(node)) {
        return NULL;
    }

    const char *target_name = node->alias != NULL ?
        node->alias : node->relation_name;

    if (target_name == NULL || node->block_name == NULL) {
        return NULL;
    }
    if (node->relation_name == NULL && node->parent != NULL &&
        node->parent->node_type != NULL &&
        (strcmp(node->parent->node_type, "Append") == 0 ||
         strcmp(node->parent->node_type, "Merge Append") == 0)) {
        return NULL;
    }
    if (node->relation_name == NULL) {
        return rows_hint_target_create(target_name,
                                       NULL,
                                       ROWS_HINT_TARGET_ALIAS);
    }
    return rows_hint_target_create(target_name,
                                   node->origin_block_name != NULL ?
                                   node->origin_block_name : node->block_name,
                                   ROWS_HINT_TARGET_RELATION);
}

/*
 * Brief: Mark one PlanNode target set incomplete.
 * Inputs: node - Plan node; reasons - Accumulated reason mask;
 *         new_reasons - Reasons discovered at this node.
 * Outputs: node - Receives target_incomplete; reasons - Receives new reasons.
 */
static void mark_target_incomplete(PlanNode *node,
                       uint32 *reasons,
                       uint32 new_reasons)
{
    node->target_incomplete = true;
    *reasons |= new_reasons;
}

/*
 * Brief: Test whether a child stays in its parent's query block.
 * Inputs: node - Parent PlanNode; child - Child PlanNode.
 * Return: True when either block is unknown or both names match.
 */
static bool child_is_same_block(const PlanNode *node, const PlanNode *child)
{
    return node->block_name == NULL || child->block_name == NULL ||
        strcmp(node->block_name, child->block_name) == 0;
}

/*
 * Brief: Recursively test whether a subtree exposes a target name.
 * Inputs: node - Subtree root; target - Target name to locate.
 * Return: True when a scan or named wrapper has the same visible name.
 */
static bool
subtree_has_target_name(const PlanNode *node, const RowsHintTarget *target)
{
    PlanTargetNodeKind kind = classify_target_node(node);

    if (kind == PLAN_TARGET_NODE_BASE_SCAN ||
        kind == PLAN_TARGET_NODE_NAMED_WRAPPER) {
        RowsHintTarget *scan_target = build_scan_target(node);

        if (scan_target != NULL &&
            rows_hint_targets_have_same_name(target, scan_target)) {
            return true;
        }
    }
    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell)) {
        if (subtree_has_target_name(lfirst(cell), target)) {
            return true;
        }
    }
    return false;
}

/*
 * Brief: Verify relation targets from an outline against a plan subtree.
 * Inputs: node - Plan subtree root; outline_targets - Fallback targets.
 * Return: True when every relation target has a matching visible scan name.
 * Description: Alias-only targets may represent wrappers and are allowed when
 *              they have no direct scan name in the subtree.
 */
static bool
outline_targets_are_grounded(const PlanNode *node, const List *outline_targets)
{
    for (ListCell *cell = list_head(outline_targets);
         cell != NULL;
         cell = lnext(outline_targets, cell)) {
        RowsHintTarget *target = lfirst(cell);

        if (subtree_has_target_name(node, target)) {
            continue;
        }
        if (target->kind == ROWS_HINT_TARGET_RELATION) {
            return false;
        }
    }
    return true;
}

/*
 * Brief: Replace a recoverable incomplete JOIN target set with outline targets.
 * Inputs: node - Incomplete JOIN node; mapping - Outline mapping;
 *         reasons - Current incomplete reason mask.
 * Outputs: node - Receives validated fallback targets when safe;
 *          reasons - Cleared or updated with fallback validation failures.
 */
static void restore_join_targets_from_outline(PlanNode *node,
                                  const RowsOutlineMapping *mapping,
                                  uint32 *reasons)
{
    const uint32 unsafe_reasons = ROWS_TARGET_INCOMPLETE_UNSUPPORTED_NODE |
        ROWS_TARGET_INCOMPLETE_TARGET_COLLISION;
    const List *outline_targets =
        rows_outline_targets_for_node(mapping, node);

    if (list_length(outline_targets) < 2 ||
        (*reasons & unsafe_reasons) != 0 ||
        !outline_targets_are_grounded(node, outline_targets)) {
        return;
    }

    node->targets = NIL;
    node->target_incomplete = false;
    *reasons = ROWS_TARGET_INCOMPLETE_NONE;
    for (ListCell *cell = list_head(outline_targets);
         cell != NULL;
         cell = lnext(outline_targets, cell)) {
        RowsHintTarget *target = lfirst(cell);

        if (rows_hint_target_list_contains(node->targets, target)) {
            mark_target_incomplete(node, reasons,
                                   ROWS_TARGET_INCOMPLETE_TARGET_COLLISION);
            continue;
        }
        target_list_add(&node->targets, target);
    }
    if (list_length(node->targets) < 2) {
        mark_target_incomplete(node, reasons,
                               ROWS_TARGET_INCOMPLETE_MISSING_TARGET);
    }
}

/*
 * Brief: Complete Rows hint targets on one PlanNode subtree.
 * Inputs: node - Plan subtree root; mapping - Outline fallback mapping.
 * Outputs: node - Receives targets and target_incomplete recursively.
 * Return: Incomplete reason mask for the current subtree root.
 * Description: Complete every child before resolving the current node so a
 *              rejected parent never prevents usable child hints.
 */
static uint32 complete_node_targets(PlanNode *node,
                                    const RowsOutlineMapping *mapping)
{
    node->targets = NIL;
    node->target_incomplete = false;
    List *child_reasons = NIL;

    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell))
    {
        uint32 child_reason = complete_node_targets(lfirst(cell), mapping);

        child_reasons = lappend_int(child_reasons, (int) child_reason);
    }
    PlanTargetNodeKind kind = classify_target_node(node);
    uint32 reasons = ROWS_TARGET_INCOMPLETE_NONE;

    if (kind == PLAN_TARGET_NODE_BASE_SCAN ||
        kind == PLAN_TARGET_NODE_NAMED_WRAPPER)
    {
        RowsHintTarget *target = build_scan_target(node);

        if (target == NULL)
        {
            mark_target_incomplete(node, &reasons,
                                   ROWS_TARGET_INCOMPLETE_MISSING_TARGET);
            return reasons;
        }
        target_list_add(&node->targets, target);
        return reasons;
    }
    if (kind == PLAN_TARGET_NODE_SEMANTIC_BOUNDARY)
    {
        mark_target_incomplete(node, &reasons,
                               ROWS_TARGET_INCOMPLETE_SEMANTIC_BOUNDARY);
        return reasons;
    }
    if (kind == PLAN_TARGET_NODE_UNKNOWN)
    {
        mark_target_incomplete(node, &reasons,
                               ROWS_TARGET_INCOMPLETE_UNSUPPORTED_NODE);
        return reasons;
    }

    int child_count = list_length(node->children);

    if (kind == PLAN_TARGET_NODE_TRANSPARENT)
    {
        if (child_count != 1)
        {
            mark_target_incomplete(node, &reasons,
                                   ROWS_TARGET_INCOMPLETE_INVALID_ARITY);
            return reasons;
        }
        PlanNode *child = linitial(node->children);

        if (node->block_name == NULL && child->block_name != NULL)
        {
            node->block_name = pstrdup(child->block_name);
        }
        node->target_incomplete = child->target_incomplete;
        reasons = (uint32) linitial_int(child_reasons);
        target_list_copy(&node->targets, child->targets);
        if (!child_is_same_block(node, child))
        {
            mark_target_incomplete(node, &reasons,
                                   ROWS_TARGET_INCOMPLETE_CROSS_QUERY_BLOCK);
        }
        return reasons;
    }

    if (kind == PLAN_TARGET_NODE_JOIN && child_count < 2)
    {
        mark_target_incomplete(node, &reasons,
                               ROWS_TARGET_INCOMPLETE_INVALID_ARITY);
    }
    ListCell *reason_cell;
    ListCell *cell;

    forboth(cell, node->children, reason_cell, child_reasons)
    {
        PlanNode *child = lfirst(cell);
        uint32 child_reason = (uint32) lfirst_int(reason_cell);

        if (node->block_name == NULL && child->block_name != NULL)
        {
            node->block_name = pstrdup(child->block_name);
        }
        if (!child_is_same_block(node, child))
        {
            mark_target_incomplete(node, &reasons,
                                   ROWS_TARGET_INCOMPLETE_CROSS_QUERY_BLOCK);
            continue;
        }
        if (child->target_incomplete)
        {
            mark_target_incomplete(node, &reasons,
                                   ROWS_TARGET_INCOMPLETE_CHILD |
                                   child_reason);
        }
        if (kind == PLAN_TARGET_NODE_JOIN)
        {
            for (ListCell *other_cell = list_head(node->children);
                 other_cell != cell;
                 other_cell = lnext(node->children, other_cell))
            {
                PlanNode *other = lfirst(other_cell);

                if (target_lists_overlap(other->targets, child->targets))
                {
                    mark_target_incomplete(
                        node,
                        &reasons,
                        ROWS_TARGET_INCOMPLETE_TARGET_COLLISION);
                }
            }
        }
        target_list_copy(&node->targets, child->targets);
    }

    if (kind == PLAN_TARGET_NODE_JOIN && list_length(node->targets) < 2)
    {
        mark_target_incomplete(node, &reasons,
                               ROWS_TARGET_INCOMPLETE_MISSING_TARGET);
    }
    if (kind == PLAN_TARGET_NODE_JOIN && node->target_incomplete)
    {
        restore_join_targets_from_outline(node, mapping, &reasons);
    }
    return reasons;
}

/*
 * Brief: Complete Rows targets on a PlanTree.
 * Inputs: root - Plan tree root; outline_hints - EXPLAIN outline hints;
 *         outline_hint_count - Number of outline hints.
 * Outputs: root - Receives targets and target_incomplete on every node.
 * Description: Outline mapping is temporary fallback state and does not escape
 *              this completion step.
 */
void complete_rows_hint_targets(PlanNode *root,
                           const char *const *outline_hints,
                           int outline_hint_count)
{
    if (root == NULL) {
        return;
    }
    RowsOutlineMapping mapping = rows_outline_mapping_build(root,
                                                            outline_hints,
                                                            outline_hint_count);

    if (mapping.failed) {
        log_message(false, "WARNING",
                    "warning=rows_outline_mapping_failed "
                    "action=use_plan_derived_targets");
    }
    (void) complete_node_targets(root, &mapping);
}

/*
 * Brief: Recursively collect eligible scan and join Rows hints.
 * Inputs: node - Current completed PlanNode; observations - Round-local facts;
 *         suppress_scan_hint - Whether scan hints in this branch are suppressed;
 *         round - Feedback round; verbose - Whether INFO is mirrored to stderr.
 * Outputs: observations - Receives eligible facts without changing persistent state.
 * Description: Preserve master behavior: Nested Loop inner scans are
 *              suppressed, while eligible joins in that subtree remain usable.
 *              Parent Relationship avoids treating a preceding InitPlan as the
 *              outer input. Partial nodes with an observed rescan are already
 *              rejected by partial_observation_evidence; the accepted first-epoch
 *              future-rescan window is documented in the integration spec.
 */
static void
collect_node_rows_hints(const PlanNode *node,
                        List **observations,
                        bool suppress_scan_hint,
                        int round,
                        bool verbose)
{
    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell)) {
        const PlanNode *child = lfirst(cell);
        bool child_suppress_scan_hint = suppress_scan_hint;

        if (node->node_type != NULL && strcmp(node->node_type, "Nested Loop") == 0)
        {
            const char *role = child->parent_relationship;

            /* EXPLAIN lists InitPlans before Outer/Inner. Only explicit Outer
             * and auxiliary roles are independent of the loop's repeated Inner;
             * an absent/unknown role cannot safely authorize feedback. */
            child_suppress_scan_hint |= role == NULL ||
                (strcmp(role, "Outer") != 0 && strcmp(role, "InitPlan") != 0 &&
                 strcmp(role, "exec_init_plan") != 0 && strcmp(role, "SubPlan") != 0);
        }
        collect_node_rows_hints(child,
                                           observations,
                                           child_suppress_scan_hint,
                                           round,
                                           verbose);
    }

    PlanTargetNodeKind kind = classify_target_node(node);
    bool emits_scan_hint = kind == PLAN_TARGET_NODE_BASE_SCAN ||
        kind == PLAN_TARGET_NODE_NAMED_WRAPPER;
    bool emits_join_hint = kind == PLAN_TARGET_NODE_JOIN;
    bool emits_rows_hint = (emits_scan_hint && !suppress_scan_hint) ||
        emits_join_hint;

    if (!emits_rows_hint) {
        return;
    }
    if (node->target_incomplete || !node->has_actual_rows ||
        node->rows_evidence == ROWS_EVIDENCE_NONE ||
        node->block_name == NULL) {
        log_message(verbose, "WARNING",
                    "warning=rows_hint_skipped round=%d plan_id=%d "
                    "node_type=\"%s\" query_block=\"%s\" "
                    "target_incomplete=%s targets=%d has_actual_rows=%s",
                    round,
                    node->pretty_id,
                    node->node_type == NULL ? "unknown" : node->node_type,
                    node->block_name == NULL ? "unknown" : node->block_name,
                    node->target_incomplete ? "true" : "false",
                    list_length(node->targets),
                    node->has_actual_rows ? "true" : "false");
        return;
    }

    RowsHintKey key = {
        .block_name = node->block_name,
        .targets = node->targets
    };
    char *rendered_key = rows_hint_key_render(&key);

    RowsObservation *observation = palloc(sizeof(*observation));

    *observation = (RowsObservation) {rendered_key, node->actual_rows,
                                     node->rows_evidence};
    *observations = lappend(*observations, observation);
}

/*
 * Brief: Collect one completed PlanTree into persistent Rows hints.
 * Inputs: plan - Completed PlanTree; hint_set - Persistent Rows hints;
 *         round - Feedback round; verbose - Whether INFO is mirrored.
 * Outputs: hint_set - Receives eligible scan and JOIN observations using
 *          master update order plus Partial lower-bound handling.
 * Return: True when the persistent hint set changed.
 */
bool collect_rows_hints(const PlanNode *plan,
                   RowsHintSet *hint_set,
                   int round,
                   bool verbose)
{
    if (plan == NULL) {
        return false;
    }
    List *observations = NIL;

    collect_node_rows_hints(plan, &observations, false, round, verbose);
    int count = list_length(observations);
    RowsObservation *items = palloc_array(RowsObservation, Max(count, 1));
    int index = 0;

    for (ListCell *cell = list_head(observations); cell != NULL;
         cell = lnext(observations, cell))
    {
        items[index++] = *(RowsObservation *) lfirst(cell);
    }
    bool changed = rows_hint_merge(hint_set, items, count, round, verbose);

    for (int i = 0; i < count; i++)
    {
        pfree((void *) items[i].key);
    }
    list_free_deep(observations);
    pfree(items);
    return changed;
}
