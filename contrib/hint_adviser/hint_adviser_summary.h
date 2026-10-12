/*-------------------------------------------------------------------------
 *
 * hint_adviser_summary.h
 *    Per-SQL summary state shared by hint_adviser modules.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HINT_ADVISER_SUMMARY_H
#define HINT_ADVISER_SUMMARY_H

#include "postgres_fe.h"

struct HintAdviserOptions;

typedef enum HintAdviserFinishReason
{
    HINT_ADVISER_FINISH_ERROR = 0,
    HINT_ADVISER_FINISH_UNSUPPORTED,
    HINT_ADVISER_FINISH_FEEDBACK_INCOMPLETE,
    HINT_ADVISER_FINISH_TIME_BUDGET_REACHED,
    HINT_ADVISER_FINISH_TARGET_RUNTIME_REACHED,
    HINT_ADVISER_FINISH_TARGET_EXECUTION_TIME_REACHED =
        HINT_ADVISER_FINISH_TARGET_RUNTIME_REACHED,
    HINT_ADVISER_FINISH_CONVERGED,
    HINT_ADVISER_FINISH_CYCLE_DETECTED,
    HINT_ADVISER_FINISH_NO_SIGNIFICANT_IMPROVEMENT,
    HINT_ADVISER_FINISH_MAX_ITERATIONS_REACHED
} HintAdviserFinishReason;

typedef enum HintAdviserPlantraceStatus
{
	HINT_ADVISER_PLANTRACE_NOT_RUN = 0,
	HINT_ADVISER_PLANTRACE_OK,
	HINT_ADVISER_PLANTRACE_FAILED
} HintAdviserPlantraceStatus;

typedef enum HintAdviserExternalOutcome
{
	HINT_ADVISER_EXTERNAL_NOT_ATTEMPTED = 0,
	HINT_ADVISER_EXTERNAL_NORMAL,
	HINT_ADVISER_EXTERNAL_UNSUPPORTED,
	HINT_ADVISER_EXTERNAL_ERROR
} HintAdviserExternalOutcome;

typedef struct HintAdviserRunSummary
{
    HintAdviserFinishReason finish_reason; /* Final control reason */
	HintAdviserPlantraceStatus plantrace_status;
	HintAdviserExternalOutcome external_outcome;
	bool		delivered;		/* Final outline delivery succeeded */
	bool		tuning_failed;	/* Best result did not beat the baseline */
    int iterations;                     /* COMPLETE + PARTIAL outcomes, not attempts. */
    int partial_iterations;             /* All PARTIAL outcomes, independent of changed. */
    int best_iteration;                 /* One-based best measured iteration */
    double sampling_wall_time_ms;       /* Dynamic sampling wall-clock cost */
    double total_tuning_wall_time_ms;   /* Whole per-SQL tuning wall-clock cost */
    bool has_sample_tuned_exec_time_ms;
    double sample_tuned_exec_time_ms;   /* First post-sampling Actual Total Time */
    bool has_best_execution_time_ms;
    double best_execution_time_ms;      /* Fastest measured execution time */
    char *best_outline_sql;             /* Final outline SQL for stdout */
    char *best_candidate_sql;             /* Row-hint SQL used for final plantrace */
} HintAdviserRunSummary;

extern bool update_best_candidate(HintAdviserRunSummary *summary,
                                  const char *outline_sql,
                                  const char *candidate_sql,
                                  double execution_time_ms,
                                  int iteration);
extern bool hint_adviser_summary_delivery_succeeded(
											const HintAdviserRunSummary *summary);
extern void hint_adviser_summary_finalize_delivery(
	HintAdviserRunSummary *summary,
	const struct HintAdviserOptions *options);
extern const char *hint_adviser_external_outcome_name(
												  HintAdviserExternalOutcome outcome);

#endif                          /* HINT_ADVISER_SUMMARY_H */
