/*-------------------------------------------------------------------------
 *
 * hint_adviser_rows_key.c
 *    Typed Rows targets and canonical key rendering.
 *
 *-------------------------------------------------------------------------
 */
/*
 * This module defines the value semantics of Rows hint targets and the
 * canonical string key used to deduplicate persistent Rows hints.
 *
 * Flow:
 *   Plan target -> RowsHintTarget -> RowsHintKey -> sorted rendered key
 */
#include "postgres_fe.h"

#include "hint_adviser_log.h"
#include "hint_adviser_rows_private.h"
#include "hint_adviser_string.h"

/*
 * Brief: Allocate one typed Rows hint target.
 * Inputs: name - Target relation or alias; block_name - Origin query block,
 *         or NULL for an alias; kind - Relation or alias target kind.
 * Return: Target allocated in the current memory context.
 */
RowsHintTarget *
rows_hint_target_create(const char *name,
                        const char *block_name,
                        RowsHintTargetKind kind)
{
    RowsHintTarget *target = palloc(sizeof(RowsHintTarget));

    target->name = pstrdup(name);
    target->block_name = block_name == NULL ? NULL : pstrdup(block_name);
    target->kind = kind;
    return target;
}

/*
 * Brief: Compare two Rows targets by kind, name, and origin block.
 * Inputs: left - First target; right - Second target.
 * Return: True when both targets have identical value semantics.
 */
bool
rows_hint_targets_equal(const RowsHintTarget *left, const RowsHintTarget *right)
{
    if (left->kind != right->kind || strcmp(left->name, right->name) != 0) {
        return false;
    }
    if (left->block_name == NULL || right->block_name == NULL) {
        return left->block_name == right->block_name;
    }
    return strcmp(left->block_name, right->block_name) == 0;
}

/*
 * Brief: Compare the visible names of two Rows targets.
 * Inputs: left - First target; right - Second target.
 * Return: True when both targets have the same name regardless of kind/block.
 */
bool
rows_hint_targets_have_same_name(const RowsHintTarget *left,
                                 const RowsHintTarget *right)
{
    return strcmp(left->name, right->name) == 0;
}

/*
 * Brief: Test whether a target list contains an equivalent target.
 * Inputs: targets - RowsHintTarget pointer list; target - Target to find.
 * Return: True when an equivalent target is present.
 */
bool
rows_hint_target_list_contains(const List *targets,
                               const RowsHintTarget *target)
{
    for (ListCell *cell = list_head(targets);
         cell != NULL;
         cell = lnext(targets, cell)) {
        if (rows_hint_targets_equal(lfirst(cell), target)) {
            return true;
        }
    }
    return false;
}

/*
 * Brief: Order two Rows targets for deterministic key rendering.
 * Inputs: left - First target; right - Second target.
 * Return: Negative, zero, or positive comparison result.
 */
static int
compare_targets(const RowsHintTarget *left, const RowsHintTarget *right)
{
    int name_comparison = strcmp(left->name, right->name);

    if (name_comparison != 0) {
        return name_comparison;
    }
    if (left->block_name == NULL || right->block_name == NULL) {
        if (left->block_name == right->block_name) {
            return 0;
        }
        return left->block_name == NULL ? -1 : 1;
    }
    return strcmp(left->block_name, right->block_name);
}

/*
 * Brief: Return a shallow, deterministically ordered target list.
 * Inputs: targets - RowsHintTarget pointer list.
 * Return: New list container with borrowed targets; caller must list_free.
 */
static List *
sorted_target_list(const List *targets)
{
    List *sorted = NIL;

    for (ListCell *target_cell = list_head(targets);
         target_cell != NULL;
         target_cell = lnext(targets, target_cell)) {
        RowsHintTarget *target = lfirst(target_cell);
        int insert_pos = 0;

        for (ListCell *sorted_cell = list_head(sorted);
             sorted_cell != NULL;
             sorted_cell = lnext(sorted, sorted_cell)) {
            if (compare_targets(target, lfirst(sorted_cell)) < 0) {
                break;
            }
            insert_pos++;
        }
        sorted = list_insert_nth(sorted, insert_pos, target);
    }
    return sorted;
}

/*
 * Brief: Append one target in Rows hint syntax.
 * Inputs: output - Destination string buffer; target - Target to render.
 * Outputs: output - Receives an escaped target and optional origin block.
 */
static void
append_target(PQExpBufferData *output, const RowsHintTarget *target)
{
    append_rows_hint_identifier(output, target->name);
    if (target->kind == ROWS_HINT_TARGET_RELATION) {
        appendPQExpBufferChar(output, '@');
        append_rows_hint_identifier(output, target->block_name);
    }
}

/*
 * Brief: Render a canonical Rows hint key from its block and targets.
 * Inputs: key - Rows hint key with a non-NULL query block.
 * Return: Canonical key text allocated in the current memory context.
 */
char *
rows_hint_key_render(const RowsHintKey *key)
{
    PQExpBufferData output;
    List *sorted_targets = sorted_target_list(key->targets);

    Assert(key->block_name != NULL);
    initPQExpBuffer(&output);
    appendPQExpBuffer(&output, "@%s", key->block_name);
    for (ListCell *cell = list_head(sorted_targets);
         cell != NULL;
         cell = lnext(sorted_targets, cell)) {
        appendPQExpBufferChar(&output, ' ');
        append_target(&output, lfirst(cell));
    }
    if (PQExpBufferDataBroken(output)) {
        pg_fatal("out of memory");
    }

    char *result = pstrdup(output.data);

    list_free(sorted_targets);
    termPQExpBuffer(&output);
    return result;
}
