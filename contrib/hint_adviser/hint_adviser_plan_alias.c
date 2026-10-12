/*-------------------------------------------------------------------------
 *
 * hint_adviser_plan_alias.c
 *    Restore parent-visible subquery aliases in a plan tree.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "hint_adviser_log.h"
#include "hint_adviser_plan_alias.h"

typedef enum SetopAliasMatch
{
    SETOP_ALIAS_ABSENT = 0,
    SETOP_ALIAS_WRAPPED,
    SETOP_ALIAS_CONFLICT
} SetopAliasMatch;

/*
 * Brief: Check whether a plan node is a Subquery Scan wrapper.
 * Inputs: node - Plan node.
 * Return: true when node is a Subquery Scan.
 */
static bool
is_subquery_scan_node(const PlanNode *node)
{
    return node != NULL &&
        node->node_type != NULL &&
        strcmp(node->node_type, "Subquery Scan") == 0;
}

/*
 * Brief: Check whether a plan node can represent a pulled-up set operation.
 * Inputs: node - Plan node.
 * Return: true for Append or Merge Append.
 */
static bool
is_append_node(const PlanNode *node)
{
    return node != NULL &&
        node->node_type != NULL &&
        (strcmp(node->node_type, "Append") == 0 ||
         strcmp(node->node_type, "Merge Append") == 0);
}

/*
 * Brief: Fill or validate the alias of an existing Subquery Scan.
 * Inputs: wrapper - Subquery Scan node; alias - Query-tree alias.
 * Outputs: wrapper - Missing alias is filled.
 */
static void
set_subquery_scan_alias(PlanNode *wrapper, const char *alias)
{
    if (wrapper->alias == NULL) {
        wrapper->alias = pstrdup(alias);
    } else if (strcmp(wrapper->alias, alias) != 0) {
        pg_fatal("subquery alias mismatch: plan alias=%s query alias=%s",
                 wrapper->alias, alias);
    }
}

/*
 * Brief: Replace one child pointer under a parent plan node.
 * Inputs: parent - Parent plan node; old_child - Child to replace;
 *         new_child - Replacement child.
 * Outputs: parent - Children list points to new_child.
 */
static void
replace_plan_child(PlanNode *parent,
                   PlanNode *old_child,
                   PlanNode *new_child)
{
    for (ListCell *cell = list_head(parent->children);
         cell != NULL;
         cell = lnext(parent->children, cell)) {
        if (lfirst(cell) == old_child) {
            lfirst(cell) = new_child;
            return;
        }
    }
    pg_fatal("failed to replace pulled-up subquery child in plan tree");
}

/*
 * Brief: Build a structural Subquery Scan wrapper for a pulled-up subquery.
 * Inputs: child - Wrapped plan subtree; alias - Parent-visible alias;
 *         parent_block_name - Parent query block.
 * Return: Synthetic wrapper without a physical operator's runtime metrics.
 */
static PlanNode *
make_synthetic_subquery_scan(PlanNode *child,
                             const char *alias,
                             const char *parent_block_name)
{
    PlanNode *wrapper = palloc0(sizeof(PlanNode));

    wrapper->node_type = pstrdup("Subquery Scan");
    wrapper->alias = pstrdup(alias);
    wrapper->block_name = pstrdup(parent_block_name);
    wrapper->parent = child->parent;
    /* The wrapper replaces this physical edge; preserve Nested Loop Inner
     * suppression after alias restoration instead of losing its role. */
    wrapper->parent_relationship = child->parent_relationship;
    wrapper->children = list_make1(child);
    child->parent = wrapper;
    child->parent_relationship = pstrdup("Subquery");
    return wrapper;
}

/*
 * Brief: Copy runtime metadata when exact block matching preserves old behavior.
 * Inputs: target - Synthetic wrapper; source - Matched physical plan node.
 * Outputs: target - Runtime values and presence flags are copied.
 */
static void
copy_plan_runtime_metrics(PlanNode *target, const PlanNode *source)
{
	target->actual_rows = source->actual_rows;
    target->rows_evidence = source->rows_evidence;
	target->actual_loops = source->actual_loops;
    target->parallel_aware = source->parallel_aware;
    target->has_actual_rows = source->has_actual_rows;
    target->has_actual_loops = source->has_actual_loops;
    target->has_parallel_aware = source->has_parallel_aware;
}

/*
 * Brief: Restore an alias by exact child query-block match.
 * Inputs: root - Plan root; block_name - Child query block;
 *         alias - Parent-visible alias.
 * Outputs: Matching pulled-up block receives a synthetic wrapper.
 * Return: true when the alias is represented in the plan tree.
 */
static bool
wrap_subquery_block_in_plan(PlanNode *root,
                            const char *block_name,
                            const char *alias)
{
    if (root == NULL) {
        return false;
    }

    if (root->block_name != NULL &&
        strcmp(root->block_name, block_name) == 0) {
        PlanNode *parent = root->parent;

        if (is_subquery_scan_node(parent)) {
            set_subquery_scan_alias(parent, alias);
            return true;
        }

        if (parent == NULL || parent->block_name == NULL) {
            pg_fatal("pulled-up subquery block %s has no parent block",
                     block_name);
        }

        PlanNode *wrapper =
            make_synthetic_subquery_scan(root, alias, parent->block_name);
        copy_plan_runtime_metrics(wrapper, root);
        replace_plan_child(parent, root, wrapper);
        return true;
    }

    for (ListCell *cell = list_head(root->children);
         cell != NULL;
         cell = lnext(root->children, cell)) {
        if (wrap_subquery_block_in_plan(lfirst(cell), block_name, alias)) {
            return true;
        }
    }
    return false;
}

/*
 * Brief: Find highest plan roots carrying one member query block.
 * Inputs: node - Search root; block_name - Member query block.
 * Outputs: match - Last matching root; match_count - Number of roots found.
 */
static void
find_block_roots(PlanNode *node,
                 const char *block_name,
                 PlanNode **match,
                 int *match_count)
{
    if (node == NULL) {
        return;
    }

    bool matches = node->block_name != NULL &&
        strcmp(node->block_name, block_name) == 0;
    bool parent_matches = node->parent != NULL &&
        node->parent->block_name != NULL &&
        strcmp(node->parent->block_name, block_name) == 0;

    if (matches && !parent_matches) {
        *match = node;
        (*match_count)++;
        return;
    }

    for (ListCell *cell = list_head(node->children);
         cell != NULL;
         cell = lnext(node->children, cell)) {
        find_block_roots(lfirst(cell), block_name, match, match_count);
    }
}

/*
 * Brief: Check whether one plan node is an ancestor of another.
 * Inputs: ancestor - Candidate ancestor; node - Descendant candidate.
 * Return: true when node is in ancestor's subtree.
 */
static bool
is_plan_ancestor(const PlanNode *ancestor, const PlanNode *node)
{
    for (const PlanNode *current = node;
         current != NULL;
         current = current->parent) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

/*
 * Brief: Find the lowest common ancestor of two plan nodes.
 * Inputs: left, right - Nodes in the same plan tree.
 * Return: Lowest common ancestor, or NULL.
 */
static PlanNode *
find_lowest_common_ancestor(PlanNode *left, PlanNode *right)
{
    for (PlanNode *candidate = left;
         candidate != NULL;
         candidate = candidate->parent) {
        if (is_plan_ancestor(candidate, right)) {
            return candidate;
        }
    }
    return NULL;
}

/*
 * Brief: Find the direct child of an ancestor containing a descendant.
 * Inputs: ancestor - Plan ancestor; descendant - Node in its subtree.
 * Return: Direct child below ancestor, or NULL.
 */
static PlanNode *
find_direct_child(PlanNode *ancestor, PlanNode *descendant)
{
    PlanNode *current = descendant;

    while (current != NULL && current->parent != ancestor) {
        current = current->parent;
    }
    return current;
}

/*
 * Brief: Restore an optimized-away set-operation subquery alias.
 * Inputs: root - Plan root; item - Alias and set-operation leaf metadata.
 * Outputs: The unique parent-block Append LCA receives a synthetic wrapper.
 * Return: Wrapped, fully absent, or conflicting structural evidence.
 */
static SetopAliasMatch
wrap_pulled_up_setop_in_plan(PlanNode *root,
                             const ExtOptSubqueryAlias *item)
{
    if (item->setop_member_count < 2 ||
        item->setop_member_block_names == NULL ||
        item->parent_block_name == NULL ||
        item->parent_block_name[0] == '\0') {
        return SETOP_ALIAS_CONFLICT;
    }

    PlanNode **member_roots =
        palloc_array(PlanNode *, item->setop_member_count);
    PlanNode *setop_root = NULL;
    int present_count = 0;

    for (int i = 0; i < item->setop_member_count; i++) {
        const char *member_block = item->setop_member_block_names[i];
        int match_count = 0;

        if (member_block == NULL || member_block[0] == '\0') {
            return SETOP_ALIAS_CONFLICT;
        }
        member_roots[i] = NULL;
        find_block_roots(root,
                         member_block,
                         &member_roots[i],
                         &match_count);
        if (match_count == 0) {
            continue;
        }
        if (match_count != 1) {
            return SETOP_ALIAS_CONFLICT;
        }
        present_count++;
    }
    if (present_count == 0) {
        return SETOP_ALIAS_ABSENT;
    }
    if (present_count != item->setop_member_count) {
        return SETOP_ALIAS_CONFLICT;
    }

    for (int i = 0; i < item->setop_member_count; i++) {
        setop_root = setop_root == NULL ? member_roots[i] :
            find_lowest_common_ancestor(setop_root, member_roots[i]);
        if (setop_root == NULL) {
            return SETOP_ALIAS_CONFLICT;
        }
    }

    if (!is_append_node(setop_root) ||
        setop_root->block_name == NULL ||
        strcmp(setop_root->block_name, item->parent_block_name) != 0) {
        return SETOP_ALIAS_CONFLICT;
    }
    if (list_length(setop_root->children) != item->setop_member_count) {
        return SETOP_ALIAS_CONFLICT;
    }

    for (int i = 0; i < item->setop_member_count; i++) {
        PlanNode *branch = find_direct_child(setop_root, member_roots[i]);

        if (branch == NULL) {
            return SETOP_ALIAS_CONFLICT;
        }
        for (int j = 0; j < i; j++) {
            if (branch == find_direct_child(setop_root, member_roots[j])) {
                return SETOP_ALIAS_CONFLICT;
            }
        }
    }

    PlanNode *parent = setop_root->parent;

    if (is_subquery_scan_node(parent)) {
        set_subquery_scan_alias(parent, item->alias);
        return SETOP_ALIAS_WRAPPED;
    }
    if (parent == NULL) {
        return SETOP_ALIAS_CONFLICT;
    }

    PlanNode *wrapper = make_synthetic_subquery_scan(setop_root,
                                                     item->alias,
                                                     item->parent_block_name);
    replace_plan_child(parent, setop_root, wrapper);
    return SETOP_ALIAS_WRAPPED;
}

/*
 * Brief: Complete parent-visible subquery aliases in one plan tree.
 * Inputs: query_tree_info - Extracted query-tree metadata; root - Plan root.
 * Outputs: Pulled-up subqueries receive synthetic Subquery Scan wrappers.
 * Return: 0 on success, 1 when set-operation evidence is incomplete.
 */
int
complete_plan_subquery_aliases(const FrontendQueryInfo *query_tree_info,
                               PlanNode *root)
{
    if (query_tree_info == NULL) {
        return 1;
    }

    if (query_tree_info->count > 0 && query_tree_info->items == NULL) {
        pg_fatal("invalid subquery alias metadata: missing alias array");
    }

    for (int i = 0; i < query_tree_info->count; i++) {
        const ExtOptSubqueryAlias *item = &query_tree_info->items[i];

        if (item->block_name == NULL || item->block_name[0] == '\0' ||
            item->alias == NULL || item->alias[0] == '\0') {
            pg_fatal("invalid subquery alias metadata: empty block name or alias");
        }
        if (wrap_subquery_block_in_plan(root, item->block_name, item->alias)) {
            continue;
        }
        if (item->setop_member_count > 0) {
            SetopAliasMatch match =
                wrap_pulled_up_setop_in_plan(root, item);

            if (match != SETOP_ALIAS_CONFLICT) {
                continue;
            }
            pg_log_error("could not restore set-operation subquery alias \"%s\" for block %s",
                         item->alias,
                         item->block_name);
            return 1;
        }
    }
    return 0;
}
