/* Persistent state keeps master EXACT overwrite behavior plus Partial bounds. */
#include "postgres_fe.h"

#include <math.h>

#include "hint_adviser_log.h"
#include "hint_adviser_rows_private.h"

/* Keep the persisted set in the order consumed by the existing SQL renderer. */
static int rows_compare_entry(const void *left, const void *right)
{
    return strcmp(((const RowsHintEntry *) left)->key,
                  ((const RowsHintEntry *) right)->key);
}

/* Find an existing key; missing keys must not be confused with zero rows. */
static RowsHintEntry *rows_find(RowsHintSet *set, const char *key)
{
    for (int i = 0; i < set->count; i++)
    {
        if (strcmp(set->entries[i].key, key) == 0)
        {
            return &set->entries[i];
        }
    }
    return NULL;
}

/* Match the existing #%.0f wire value without losing the raw evidence. */
static double rows_output_value(double rows)
{
    char rendered[512];

    snprintf(rendered, sizeof(rendered), "%.0f", Max(rows, 1.0));
    return strtod(rendered, NULL);
}

/* Append a validated entry; callers reacquire pointers after array growth. */
static RowsHintEntry *rows_append(RowsHintSet *set, const char *key)
{
    if (set->count == set->capacity)
    {
        set->capacity = set->capacity == 0 ? 8 : set->capacity * 2;
        set->entries = set->entries == NULL ?
            palloc_array(RowsHintEntry, set->capacity) :
            repalloc_array(set->entries, RowsHintEntry, set->capacity);
    }
    RowsHintEntry *entry = &set->entries[set->count++];

    memset(entry, 0, sizeof(*entry));
    entry->key = pstrdup(key);
    return entry;
}

/* Apply one authoritative runtime row count using master overwrite semantics. */
static bool rows_apply_exact(RowsHintSet *set, const RowsObservation *observation,
                           int round, bool verbose)
{
    double rows = rows_output_value(observation->rows);
    RowsHintEntry *entry = rows_find(set, observation->key);

    if (entry == NULL)
    {
        entry = rows_append(set, observation->key);
        entry->rows = rows;
        entry->observed_rows = observation->rows;
        entry->evidence = ROWS_EVIDENCE_EXACT;
        entry->last_updated_round = round;
        return true;
    }
    if (entry->evidence == ROWS_EVIDENCE_EXACT &&
        entry->last_updated_round == round)
    {
        if (entry->rows != rows)
        {
            log_message(false, "WARNING",
                        "diagnostic=rows_hint_overwrite round=%d key='%s' "
                        "previous_round=%d previous_rows=%.0f rows=%.0f",
                        round, observation->key, entry->last_updated_round,
                        entry->rows, rows);
        }
        return false;
    }
    bool changed = entry->rows != rows;

    if (changed)
    {
        log_message(verbose, "INFO",
                    "diagnostic=rows_hint_overwrite round=%d key='%s' "
                    "previous_round=%d previous_rows=%.0f rows=%.0f",
                    round, observation->key, entry->last_updated_round,
                    entry->rows, rows);
    }
    entry->rows = rows;
    entry->observed_rows = observation->rows;
    entry->evidence = ROWS_EVIDENCE_EXACT;
    entry->has_lower_bound = false;
    entry->lower_bound = 0.0;
    entry->last_updated_round = round;
    return changed;
}

/* Sampling may refresh sampling, but cannot weaken runtime evidence. */
static bool rows_apply_sampling(RowsHintSet *set, const RowsObservation *observation,
                              int round)
{
    double rows = rows_output_value(observation->rows);
    RowsHintEntry *entry = rows_find(set, observation->key);

    if (entry == NULL)
    {
        entry = rows_append(set, observation->key);
        entry->rows = rows;
        entry->observed_rows = observation->rows;
        entry->evidence = ROWS_EVIDENCE_SAMPLING;
        entry->last_updated_round = round;
        return true;
    }
    if (entry->evidence != ROWS_EVIDENCE_SAMPLING)
    {
        return false;
    }
    if (entry->last_updated_round == round && rows <= entry->rows)
    {
        return false;
    }
    bool changed = entry->rows != rows;

    entry->rows = rows;
    entry->observed_rows = observation->rows;
    entry->last_updated_round = round;
    return changed;
}

/* Preserve the strongest proven lower bound without replacing an EXACT value. */
static bool rows_apply_lower_bound(RowsHintSet *set,
                                const RowsObservation *observation,
                                int round, bool verbose)
{
    RowsHintEntry *entry = rows_find(set, observation->key);

    if (entry != NULL && entry->evidence == ROWS_EVIDENCE_EXACT)
    {
        if (observation->rows > entry->observed_rows)
        {
            log_message(verbose, "WARNING",
                        "diagnostic=rows_evidence_conflict round=%d key='%s' "
                        "action=retain_previous", round, observation->key);
        }
        return false;
    }
    if (entry == NULL)
    {
        entry = rows_append(set, observation->key);
        entry->evidence = ROWS_EVIDENCE_LOWER_BOUND;
        entry->observed_rows = observation->rows;
        entry->has_lower_bound = true;
        entry->lower_bound = observation->rows;
        entry->rows = rows_output_value(observation->rows);
        entry->last_updated_round = round;
        return true;
    }
    double bound = Max(entry->has_lower_bound ? entry->lower_bound : 0.0,
                       observation->rows);
    double next_rows = entry->evidence == ROWS_EVIDENCE_SAMPLING &&
        entry->observed_rows >= bound ? entry->rows : rows_output_value(bound);
    bool changed = entry->rows != next_rows;

    entry->has_lower_bound = true;
    entry->lower_bound = bound;
    if (entry->evidence != ROWS_EVIDENCE_SAMPLING || bound > entry->observed_rows)
    {
        entry->evidence = ROWS_EVIDENCE_LOWER_BOUND;
        entry->observed_rows = bound;
    }
    entry->rows = next_rows;
    if (changed)
    {
        entry->last_updated_round = round;
    }
    return changed;
}

/* Apply observations in PlanTree traversal order, matching master first-wins. */
bool rows_hint_merge(RowsHintSet *set, const RowsObservation *observations,
                   int count, int round, bool verbose)
{
    if (count <= 0)
    {
        return false;
    }
    bool changed = false;

    for (int index = 0; index < count; index++)
    {
        const RowsObservation *observation = &observations[index];

        if (observation->key == NULL || !isfinite(observation->rows) ||
            observation->rows < 0.0)
        {
            log_message(verbose, "WARNING",
                        "diagnostic=rows_evidence_invalid round=%d action=skip",
                        round);
            continue;
        }
        if (observation->evidence == ROWS_EVIDENCE_EXACT)
        {
            changed |= rows_apply_exact(set, observation, round, verbose);
        }
        else if (observation->evidence == ROWS_EVIDENCE_LOWER_BOUND)
        {
            changed |= rows_apply_lower_bound(set, observation, round, verbose);
        }
        else if (observation->evidence == ROWS_EVIDENCE_SAMPLING)
        {
            changed |= rows_apply_sampling(set, observation, round);
        }
        else
        {
            log_message(verbose, "WARNING",
                        "diagnostic=rows_evidence_invalid round=%d key='%s' action=skip",
                        round, observation->key);
        }
    }
    if (set->count > 1)
    {
        qsort(set->entries, set->count, sizeof(RowsHintEntry), rows_compare_entry);
    }
    return changed;
}

/* Sampling's existing insertion API now records explicitly unproven evidence. */
bool rows_hint_set_upsert(RowsHintSet *set, const char *key, double rows,
                           int round, bool verbose)
{
    RowsObservation observation = {key, rows, ROWS_EVIDENCE_SAMPLING};

    return rows_hint_merge(set, &observation, 1, round, verbose);
}
