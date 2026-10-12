/*-------------------------------------------------------------------------
 *
 * hint_adviser_plan.h
 *    Plan tree representation shared by hint_adviser modules.
 *
 *-------------------------------------------------------------------------
 */
/*
 * PlanNode is the shared data structure between json and hint modules.
 *
 * Reading guide:
 * JSON fills structural fields such as node_type/relation_name/alias and child
 * links. Pretty EXPLAIN rows may then attach block names, E-rows, A-rows, and
 * anchors to the same PlanNode tree.
 */
#ifndef HINT_ADVISER_PLAN_H
#define HINT_ADVISER_PLAN_H

#include "nodes/pg_list.h"

typedef enum RowsEvidence
{
    ROWS_EVIDENCE_NONE,
    ROWS_EVIDENCE_SAMPLING,
    ROWS_EVIDENCE_LOWER_BOUND,
    ROWS_EVIDENCE_EXACT
} RowsEvidence;

typedef struct PlanNode
{
    char *node_type;            /* EXPLAIN "Node Type" */
    char *relation_name;        /* EXPLAIN "Relation Name" */
    char *schema_name;          /* EXPLAIN "Schema" */
    char *alias;                /* EXPLAIN "Alias" */
	char *index_name;		   /* EXPLAIN "Index Name" */
    char *cte_name;             /* EXPLAIN "CTE Name" for CTE Scan */
    char *block_name;           /* Pretty "Query Block", e.g. sel$1 */
    char *origin_block_name;    /* Base relation block from alias@"block" */
    char *parent_relationship;  /* EXPLAIN "Parent Relationship" */
    char *subplan_name;         /* EXPLAIN "Subplan Name" */
    char *pretty_operation;     /* Pretty EXPLAIN operation text */
    char *pretty_anchor_name;   /* Pretty CTE/InitPlan/SubPlan anchor */
	char *filter;               /* EXPLAIN "Filter" */
	char *index_cond;           /* EXPLAIN "Index Cond" */
	char *recheck_cond;         /* EXPLAIN "Recheck Cond" */
    double plan_rows;           /* EXPLAIN "Plan Rows" or pretty "E-rows" */
    double json_plan_rows;      /* Preserve independent skeleton estimate for proof. */
    bool has_json_plan_rows;
    double actual_rows;         /* EXPLAIN "Actual Rows" or pretty "A-rows" */
    RowsEvidence rows_evidence; /* Proof strength; actual_rows remains unclamped. */
    double actual_loops;        /* EXPLAIN "Actual Loops" */
    int pretty_id;              /* Pretty EXPLAIN plan row id */
    bool parallel_aware;        /* EXPLAIN "Parallel Aware" */
    bool has_plan_rows;         /* Whether Plan Rows/E-rows was seen */
    bool has_actual_rows;       /* Whether Actual Rows/A-rows was seen */
    bool has_actual_loops;      /* Whether Actual Loops was seen */
    bool has_parallel_aware;    /* Whether Parallel Aware was seen */
    List *targets;              /* RowsHintTarget pointers visible at this node */
    bool target_incomplete;     /* Whether Rows targets are unsafe to consume */
    List *children;             /* Child PlanNode list */
    struct PlanNode *parent;    /* Parent PlanNode */
} PlanNode;

#endif                          /* HINT_ADVISER_PLAN_H */
