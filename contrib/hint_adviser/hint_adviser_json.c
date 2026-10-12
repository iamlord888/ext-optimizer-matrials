/*-------------------------------------------------------------------------
 *
 * hint_adviser_json.c
 *    Convert EXPLAIN (FORMAT JSON) output to a lightweight plan tree.
 *
 *-------------------------------------------------------------------------
 */
/*
 * The json module converts EXPLAIN JSON into a lightweight PlanNode tree.
 * Feedback execution timing and actual rows normally come from GaussDB pretty
 * EXPLAIN ANALYZE; JSON is kept as the structural plan skeleton.
 *
 * Key entry point:
 * parse_explain_json() configures pg_parse_json callbacks
 * and returns a PlanNode tree.
 *
 * Reading guide:
 * First read the EXPLAIN JSON parse flow comment below to understand callback
 * ordering, then read object_start for root/child PlanNode creation, and
 * finally scalar for field extraction into current_plan.
 */
#include "postgres_fe.h"

#include "common/jsonapi.h"
#include "hint_adviser_log.h"
#include "mb/pg_wchar.h"
#include "pqexpbuffer.h"

#include "hint_adviser_json.h"
/*
 * Brief: JSON stack item
 * Description: pg_parse_json is event-driven and reports only current token.
 *              This stores object/array hierarchy context for later callbacks.
 */
typedef struct JsonStackItem
{
    bool is_array;              /* Whether current level is a JSON array */
    bool is_plans_array;        /* Whether current level is EXPLAIN plan's Plans array */
    PlanNode *plan;             /* PlanNode for current object; NULL for arrays/non-plan objects */
} JsonStackItem;

/*
 * Brief: EXPLAIN JSON parse state
 * Description: Runtime context for event-driven JSON parsing; not a JSON DOM.
 *              Stores only minimum state required to build PlanNode tree.
 */
typedef struct ExplainJsonParseState
{
    PlanNode *root;             /* Final plan root */
    PlanNode *current_plan;     /* Plan node currently being filled */
    char *current_field;        /* Most recently read JSON field name */
    JsonStackItem *stack;       /* JSON object/array hierarchy stack */
    int stack_count;            /* Current stack item count */
    int stack_capacity;         /* Current stack array capacity */
} ExplainJsonParseState;

/*
 * Brief: Create plan node
 * Inputs: parent - parent plan node
 * Outputs: parent - parent after child append
 * Return: newly created plan node
 * Description: Creates internal PlanNode when parser recognizes an EXPLAIN
 *              plan object; links to parent children list when parent exists.
 */
static PlanNode * create_plan_node(PlanNode *parent)
{
    PlanNode *node = palloc0(sizeof(PlanNode));

    node->actual_loops = 1;
    node->parent = parent;

    if (parent != NULL) {
        parent->children = lappend(parent->children, node);
    }

    return node;
}

/*
 * Brief: Push JSON parse stack
 * Inputs: state - parse state; is_array - whether array; is_plans_array - whether Plans array; plan - plan node
 * Outputs: state - updated parse state
 * Description: Records object/array hierarchy so callbacks can determine
 *              whether current location is in Plan or Plans subtrees.
 */
static void push_json_stack(ExplainJsonParseState *state,
                bool is_array,
                bool is_plans_array,
                PlanNode *plan)
{
    if (state->stack_count >= state->stack_capacity) {
        state->stack_capacity =
            state->stack_capacity == 0 ? 16 : state->stack_capacity * 2;
        if (state->stack == NULL) {
            state->stack = palloc_array(JsonStackItem,
                                        state->stack_capacity);
        } else {
            state->stack = repalloc_array(state->stack,
                                          JsonStackItem,
                                          state->stack_capacity);
        }
    }

    state->stack[state->stack_count].is_array = is_array;
    state->stack[state->stack_count].is_plans_array = is_plans_array;
    state->stack[state->stack_count].plan = plan;
    state->stack_count++;
}

/*
 * Brief: Pop JSON parse stack
 * Inputs: state - parse state
 * Outputs: state - updated parse state
 * Return: popped stack item
 * Description: Restores upper-level JSON context on object/array end.
 *              Returns an empty item when stack is unexpectedly empty.
 */
static JsonStackItem pop_json_stack(ExplainJsonParseState *state)
{
    JsonStackItem empty = {0};

    if (state->stack_count <= 0) {
        return empty;
    }

    state->stack_count--;
    return state->stack[state->stack_count];
}

/*
 * Brief: Check current JSON field name
 * Inputs: state - parse state; field - expected field name
 * Return: true if current field name matches
 * Description: Scalar callbacks only receive values. This helper uses stored
 *              current_field to classify values by EXPLAIN field.
 */
static bool current_field_is(ExplainJsonParseState *state, const char *field)
{
	return state->current_field != NULL &&
		strcmp(state->current_field, field) == 0;
}

/*
 * Brief: Remove GaussDB non-JSON tags from EXPLAIN JSON text.
 * Inputs: json - EXPLAIN JSON text returned by server.
 * Return: Normalized JSON text; caller must free.
 * Description: Some GaussDB EXPLAIN JSON output embeds display tags such as
 *              [Parameterized] inside JSON objects.  Strip those tags before
 *              handing the text to PostgreSQL's strict JSON parser.
 */
char *normalize_explain_json_text(const char *json)
{
	const char *parameterized_tag = "[Parameterized]";
	int			parameterized_tag_len = strlen(parameterized_tag);
	PQExpBufferData normalized;

	initPQExpBuffer(&normalized);
	for (const char *pos = json; *pos != '\0';) {
		if (strncmp(pos, parameterized_tag, parameterized_tag_len) == 0) {
			pos += parameterized_tag_len;
			continue;
		}
		appendPQExpBufferChar(&normalized, *pos);
		pos++;
	}

	if (PQExpBufferDataBroken(normalized)) {
		pg_fatal("out of memory");
	}

	char	   *result = pstrdup(normalized.data);

	termPQExpBuffer(&normalized);
	return result;
}

/*
 * Brief: Check whether current location is inside Plans array
 * Inputs: state - parse state
 * Return: true if inside Plans array
 * Description: Child plans appear under parent Plans arrays; this check is used
 *              to convert array element objects into child PlanNodes.
 */
static bool inside_plans_array(ExplainJsonParseState *state)
{
    JsonStackItem *item;

    if (state->stack_count <= 0) {
        return false;
    }

    item = &state->stack[state->stack_count - 1];
    return item->is_array && item->is_plans_array;
}

/*
 * Brief: Handle JSON object start event
 * Inputs: state - parse state
 * Outputs: state - updated parse state
 * Return: JSON parse status code
 * Description: Creates root PlanNode for "Plan": {...}, child PlanNode for
 *              objects inside Plans array, and only pushes stack for others.
 */
static JsonParseErrorType explain_json_object_start(void *state)
{
    ExplainJsonParseState *parse = state;
    PlanNode *plan = NULL;

    /* Root plan in EXPLAIN JSON is under the first-level Plan field */
    if (current_field_is(parse, "Plan")) {
        plan = create_plan_node(NULL);
        if (parse->root == NULL) {
            parse->root = plan;
        }
    } else if (inside_plans_array(parse)) {
        /* Child plans are attached under Plans array */
        plan = create_plan_node(parse->current_plan);
    }

    push_json_stack(parse, false, false, plan);
    if (plan != NULL) {
        parse->current_plan = plan;
    }

    return JSON_SUCCESS;
}

/*
 * Brief: Handle JSON object end event
 * Inputs: state - parse state
 * Outputs: state - updated parse state
 * Return: JSON parse status code
 * Description: Pops JSON stack on object end. If object maps to a PlanNode,
 *              current_plan rolls back to parent to avoid writing wrong node.
 */
static JsonParseErrorType explain_json_object_end(void *state)
{
    ExplainJsonParseState *parse = state;
    JsonStackItem item;

    item = pop_json_stack(parse);

    if (item.plan != NULL) {
        parse->current_plan = item.plan->parent;
    }

    return JSON_SUCCESS;
}

/*
 * Brief: Handle JSON array start event
 * Inputs: state - parse state
 * Outputs: state - updated parse state
 * Return: JSON parse status code
 * Description: Records hierarchy on array start; marks as child-plan array
 *              when current field is Plans and current_plan exists.
 */
static JsonParseErrorType explain_json_array_start(void *state)
{
    ExplainJsonParseState *parse = state;
    bool is_plans_array;

    is_plans_array = current_field_is(parse, "Plans") &&
                     parse->current_plan != NULL;

    push_json_stack(parse, true, is_plans_array, NULL);
    return JSON_SUCCESS;
}

/*
 * Brief: Handle JSON array end event
 * Inputs: state - parse state
 * Outputs: state - updated parse state
 * Return: JSON parse status code
 * Description: Restores JSON hierarchy on array end; plan rollback is done in object_end.
 */
static JsonParseErrorType explain_json_array_end(void *state)
{
    ExplainJsonParseState *parse = state;

    (void) pop_json_stack(parse);
    return JSON_SUCCESS;
}

/*
 * Brief: Record current JSON object field name
 * Inputs: state - parse state; fname - field name; isnull - whether field is null
 * Outputs: state - updated parse state
 * Return: JSON parse status code
 * Description: Stores latest field name for object_start checks and scalar mapping.
 */
static JsonParseErrorType explain_json_object_field_start(void *state, char *fname, bool isnull)
{
    ExplainJsonParseState *parse = state;

    (void) isnull;

    if (parse->current_field != NULL) {
    }
    parse->current_field = pstrdup(fname);

    return JSON_SUCCESS;
}

/*
 * Brief: Extract required JSON scalar fields for current plan node
 * Inputs: state - parse state; token - scalar text; tokentype - scalar type
 * Outputs: state - updated parse state
 * Return: JSON parse status code
 * Description: Extracts only fields needed by rows hints: Node Type, Relation Name,
 *              Schema, Alias, scan quals, Plan Rows, Actual Rows,
 *              Actual Loops, Parallel Aware.
 */
static JsonParseErrorType explain_json_scalar(void *state, char *token, JsonTokenType tokentype)
{
    ExplainJsonParseState *parse = state;
    PlanNode *plan = parse->current_plan;
    char *endptr = NULL;
    double value;

    if (parse->current_field == NULL || token == NULL) {
        return JSON_SUCCESS;
    }

    if (plan == NULL) {
        return JSON_SUCCESS;
    }

    /* For string fields, only extract node type and object names */
    if (tokentype == JSON_TOKEN_STRING) {
        if (current_field_is(parse, "Node Type")) {
            plan->node_type = pstrdup(token);
        } else if (current_field_is(parse, "Relation Name")) {
            plan->relation_name = pstrdup(token);
		} else if (current_field_is(parse, "Schema")) {
			plan->schema_name = pstrdup(token);
        } else if (current_field_is(parse, "Alias")) {
            plan->alias = pstrdup(token);
		} else if (current_field_is(parse, "Index Name")) {
			plan->index_name = pstrdup(token);
        } else if (current_field_is(parse, "CTE Name")) {
            plan->cte_name = pstrdup(token);
        } else if (current_field_is(parse, "Parent Relationship")) {
            plan->parent_relationship = pstrdup(token);
        } else if (current_field_is(parse, "Subplan Name")) {
            plan->subplan_name = pstrdup(token);
        } else if (current_field_is(parse, "Filter Unordered")) {
            if (plan->filter != NULL) {
            }
            plan->filter = pstrdup(token);
        } else if (current_field_is(parse, "Filter") && plan->filter == NULL) {
            plan->filter = pstrdup(token);
		} else if (current_field_is(parse, "Index Cond")) {
			plan->index_cond = pstrdup(token);
		} else if (current_field_is(parse, "Recheck Cond")) {
			plan->recheck_cond = pstrdup(token);
        }
    } else if (tokentype == JSON_TOKEN_NUMBER) {
        errno = 0;
        value = strtod(token, &endptr);
        if (errno == 0 && endptr != token && *endptr == '\0') {
            if (current_field_is(parse, "Plan Rows")) {
                plan->plan_rows = value;
                plan->has_plan_rows = true;
                plan->json_plan_rows = value;
                plan->has_json_plan_rows = true;
            } else if (current_field_is(parse, "Actual Rows") ||
                /* GaussDB Streaming nodes expose rows as Actual Total Rows. */
                current_field_is(parse, "Actual Total Rows")) {
                plan->actual_rows = value;
                plan->has_actual_rows = true;
            } else if (current_field_is(parse, "Actual Loops")) {
                plan->actual_loops = value;
                plan->has_actual_loops = true;
            }
        }
    } else if (tokentype == JSON_TOKEN_TRUE || tokentype == JSON_TOKEN_FALSE) {
        /* Parallel Aware is kept as plan metadata for scan hint policy. */
        if (current_field_is(parse, "Parallel Aware")) {
            plan->parallel_aware = tokentype == JSON_TOKEN_TRUE;
            plan->has_parallel_aware = true;
        }
    }

    return JSON_SUCCESS;
}

/*
 * Brief: Parse EXPLAIN JSON into a PlanNode tree.
 * Inputs: json - EXPLAIN JSON string
 * Return: PlanNode root, or NULL when the JSON has no root plan.
 * Description: Configures pg_parse_json callbacks, converts Plan/Plans in the
 *              first EXPLAIN JSON item into PlanNode tree used by hint_adviser.
 */
PlanNode *parse_explain_json(const char *json)
{
    JsonLexContext *lex;
    ExplainJsonParseState parse = {0};
    JsonSemAction sem = {0};
    JsonParseErrorType err;

    lex = makeJsonLexContextCstringLen(NULL,
                                       json,
                                       strlen(json),
                                       PG_UTF8,
                                       true);

    setJsonLexContextOwnsTokens(lex, true);

    sem.semstate = &parse;
    sem.object_start = explain_json_object_start;
    sem.object_end = explain_json_object_end;
    sem.array_start = explain_json_array_start;
    sem.array_end = explain_json_array_end;
    sem.object_field_start = explain_json_object_field_start;
    sem.scalar = explain_json_scalar;
    /*
     * pg_parse_json() performs parsing and the parse framework calls these callbacks, e.g.:
     * explain (format json) select * from a a1 join a a2 on a1.i=a2.i;
     *      "Plan": {                        
     *      "Node Type": "Hash Join", ...        
     *      "Plans": [                       
     *          {                            
     *          "Node Type": "Seq Scan", ...
     *          },                           
     *          {                            
     *          "Node Type": "Hash", ...         
     *          "Plans": [                   
     *              {                        
     *              "Node Type": "Seq Scan", ... 
     *              }                        
     *          ]                            
     *          }                            
     *      ]                                
     *      }       
     *  (1 row)
     *   sees {        -> explain_json_object_start()
     *   sees }        -> explain_json_object_end()
     *   sees [        -> explain_json_array_start()
     *   sees ]        -> explain_json_array_end()
     *   sees field    -> explain_json_object_field_start()
     *   sees scalar   -> explain_json_scalar()
     */
    err = pg_parse_json(lex, &sem);
    if (err != JSON_SUCCESS) {
        pg_fatal("could not parse EXPLAIN JSON: %s",
                 json_errdetail(err, lex));
    }

    freeJsonLexContext(lex);
    if (parse.current_field != NULL) {
    }
    if (parse.stack != NULL) {
    }

    return parse.root;
}
