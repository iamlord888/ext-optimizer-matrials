/*-------------------------------------------------------------------------
 *
 * hint_adviser_rows_private.h
 *    Private Rows hint domain model and implementation contracts.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HINT_ADVISER_ROWS_PRIVATE_H
#define HINT_ADVISER_ROWS_PRIVATE_H

#include "hint_adviser_hint.h"

typedef enum RowsHintTargetKind
{
    ROWS_HINT_TARGET_RELATION = 0,
    ROWS_HINT_TARGET_ALIAS
} RowsHintTargetKind;

typedef struct RowsHintTarget
{
    char               *name;
    char               *block_name; /* NULL for a parent-visible alias */
    RowsHintTargetKind  kind;
} RowsHintTarget;

typedef struct RowsHintKey
{
    const char *block_name;
    List       *targets; /* RowsHintTarget pointers */
} RowsHintKey;

typedef struct RowsHintOutlineJoin
{
    const char *node_type;
    char       *block_name;
    List       *targets; /* RowsHintTarget pointers */
    bool        matched;
} RowsHintOutlineJoin;

typedef struct RowsHintOutlineAssignment
{
    const PlanNode      *node;
    RowsHintOutlineJoin *outline_join;
} RowsHintOutlineAssignment;

typedef struct RowsOutlineMapping
{
    List *joins;       /* RowsHintOutlineJoin pointers */
    List *assignments; /* RowsHintOutlineAssignment pointers */
    bool  failed;
} RowsOutlineMapping;

extern RowsOutlineMapping rows_outline_mapping_build(
                                            const PlanNode *root,
                                            const char *const *outline_hints,
                                            int outline_hint_count);
extern const List *rows_outline_targets_for_node(
                                            const RowsOutlineMapping *mapping,
                                            const PlanNode *node);

extern RowsHintTarget *rows_hint_target_create(const char *name,
                                                const char *block_name,
                                                RowsHintTargetKind kind);
extern bool rows_plan_node_is_join(const PlanNode *node);
extern bool rows_hint_targets_equal(const RowsHintTarget *left,
                                    const RowsHintTarget *right);
extern bool rows_hint_targets_have_same_name(const RowsHintTarget *left,
                                             const RowsHintTarget *right);
extern bool rows_hint_target_list_contains(const List *targets,
                                           const RowsHintTarget *target);

extern void complete_rows_hint_targets(PlanNode *root,
                                       const char *const *outline_hints,
                                       int outline_hint_count);
extern char *rows_hint_key_render(const RowsHintKey *key);

#endif                          /* HINT_ADVISER_ROWS_PRIVATE_H */
