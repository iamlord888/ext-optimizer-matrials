/* Loop bookkeeping deliberately distinguishes attempts from feedback outcomes. */
#include "postgres_fe.h"
#include "hint_adviser_feedback_policy.h"

/*
 * A cancelled candidate has no complete runtime.  It still proves that the
 * required improvement was missed when it did not finish inside a timeout
 * longer than the largest runtime that would satisfy the improvement ratio.
 */
static bool
partial_proves_no_improvement(const HintAdviserOptions *options,
                              const FeedbackResult *result,
                              const HintAdviserRunSummary *summary)
{
    if (result->outcome != FEEDBACK_PARTIAL ||
        result->effective_timeout_ms <= 0 ||
        !summary->has_best_execution_time_ms ||
        summary->best_execution_time_ms <= 0 ||
        summary->best_iteration <= 0 ||
        summary->best_outline_sql == NULL)
    {
        return false;
    }

    double maximum_improved_runtime_ms = summary->best_execution_time_ms *
        (1.0 - options->iter_min_improvement);

    return result->effective_timeout_ms > maximum_improved_runtime_ms;
}

/*
 * Account for one outcome and select the existing stop policy. Returns true
 * when no next candidate should run. Only COMPLETE updates timing and best;
 * a bounded PARTIAL contributes to patience only when its timeout proves that
 * the candidate missed the configured improvement threshold.
 */
bool feedback_loop_advance(const HintAdviserOptions *options,
    FeedbackLoopState *state, const FeedbackResult *result, const char *candidate_sql,
    bool changed, HintAdviserRunSummary *summary)
{
    if (result->attempted)
    {
        state->attempts++;
    }
    if (result->outcome == FEEDBACK_ERROR)
    {
        if (result->attempted || result->effective_timeout_ms != 0)
        {
            summary->finish_reason = HINT_ADVISER_FINISH_ERROR;
        }
        return true;
    }
    if (result->outcome == FEEDBACK_TIMEOUT_NO_TRUSTED_NODES)
    {
        summary->finish_reason = HINT_ADVISER_FINISH_TIME_BUDGET_REACHED;
        return true;
    }
    summary->iterations++;
    bool patience_observed = false;

    if (result->outcome == FEEDBACK_PARTIAL)
    {
        summary->partial_iterations++;
        if (partial_proves_no_improvement(options, result, summary))
        {
            state->no_improvement_iters++;
            patience_observed = true;
        }
    }
    else if (result->outcome == FEEDBACK_COMPLETE)
    {
        patience_observed = true;
        if (summary->has_best_execution_time_ms && summary->best_execution_time_ms > 0)
        {
            double improvement = (summary->best_execution_time_ms - result->execution_time_ms) /
                summary->best_execution_time_ms;

            state->no_improvement_iters = improvement >= options->iter_min_improvement ?
                0 : state->no_improvement_iters + 1;
        }
        if (!summary->has_sample_tuned_exec_time_ms)
        {
            summary->has_sample_tuned_exec_time_ms = true;
            summary->sample_tuned_exec_time_ms = result->execution_time_ms;
        }
        (void) update_best_candidate(summary, result->outline_sql, candidate_sql,
                                     result->execution_time_ms, state->attempts);
        if (options->has_target_time && result->execution_time_ms <= options->target_time_ms)
        {
            summary->finish_reason = HINT_ADVISER_FINISH_TARGET_RUNTIME_REACHED;
            return true;
        }
    }
    if (options->max_iterations > 0 && state->attempts >= options->max_iterations)
    {
        summary->finish_reason = HINT_ADVISER_FINISH_MAX_ITERATIONS_REACHED;
        return true;
    }
    if (!changed)
    {
        summary->finish_reason = HINT_ADVISER_FINISH_CONVERGED;
        return true;
    }
    if (patience_observed && state->no_improvement_iters >= options->patience_iters)
    {
        summary->finish_reason = HINT_ADVISER_FINISH_NO_SIGNIFICANT_IMPROVEMENT;
        return true;
    }
    return false;
}
