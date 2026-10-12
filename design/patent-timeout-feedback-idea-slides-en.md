# Timeout Execution Feedback Idea: Slide Text

Source: [Patent disclosure draft](patent-timeout-feedback-disclosure-draft.md). Seven slides: one background slide, four technical approach slides, one proposed protection slide, and one technical effects slide. [Chinese version](patent-timeout-feedback-idea-slides.md).

## Slide 1 — Background: Slow candidates delay execution-feedback tuning

**Cardinality estimates influence plan selection.** The optimizer uses estimated scan and join row counts to choose access paths, join order, and join methods. Estimation errors can produce candidates that run for a long time.

**Execution feedback supports repeated tuning of the same SQL.** Each round executes a candidate, uses observed operator row counts to correct cardinality inputs, and generates the next candidate. Row counts inform planning; complete runtimes support performance comparisons.

**Slow candidates create a dilemma.** Waiting for completion may consume much of the tuning budget. Cancelling at a deadline interrupts the normal `EXPLAIN ANALYZE` result path and makes operator statistics difficult to use promptly.

**A cancelled round may already contain useful information.** Some scans have finished; some joins are still running but have emitted far more rows than estimated; other operators have not started. These observations require interpretation according to execution state.

**Idea: Give each candidate for the same SQL an execution deadline, preserve operator facts when cancellation occurs, and use qualified row counts in the next round.**

**Suggested visual:** [Figure 1: Execution deadline and feedback timing](patent-timeout-feedback-figures/png/fig1-conventional.png), comparing waiting for completion, ordinary cancellation, and capture on cancellation.

## Slide 2 — Technical approach 1: Iterative tuning with a deadline per candidate

**Scope: Successive candidate plans for the same SQL.** Inputs such as Rows Hints change planning estimates and produce new candidates, each with an independent execution context.

1. **Generate a candidate:** Give the optimizer the original SQL and current cardinality feedback.
2. **Run with a deadline:** Execute the candidate through `EXPLAIN ANALYZE`. Set the initial per-candidate timeout cap `T₀` to ten seconds in the current experiment; this setting is configurable.
3. **On normal completion:** Collect operator statistics and update row-count feedback. Compare complete runtimes and retain the best candidate.
4. **On timeout cancellation:** Preserve the original plan and operator observations before executor cleanup, qualify the row counts, and pass accepted feedback to the next candidate after cancellation finishes.

**Update the timeout:** Use `T₀` until a complete runtime is available. Once the same SQL has a best complete runtime `t_best`, set the cap for later candidates to **`min(T₀, 1.25 × t_best)`**. For example, a best runtime of four seconds reduces the cap to five seconds.

**Both normally completed and timed-out candidates can provide row-count feedback. Only complete runtimes update the best candidate and timeout cap. The actual deadline also respects the remaining global tuning budget.**

**Suggested visual:** [Figure 2: System and feedback position](patent-timeout-feedback-figures/png/fig2-system.png). Show candidate i feeding operator statistics to row-count feedback after either normal completion or timeout cancellation, which supports candidate i+1. Normal completion also supplies runtime for comparing completed candidates.

## Slide 3 — Technical approach 2: Read and return operator execution statistics

**Core operation: On cancellation, read each operator’s row counts and execution state before releasing executor objects, then return them for the next feedback round.**

1. **Traverse operators.** Obtain the top-level operator’s `PlanState` from `QueryDesc.planstate`, traverse the execution-state tree and save operator pointers.
2. **Read row counts.** `PlanState.plan` supplies the node ID and estimated rows E. `PlanState.instrument` supplies cumulative returned rows A and Loops.
3. **Identify local EOF.** `active=false` means no `get_next` call is in progress. If `Instrumentation.status != 0` also holds, set `terminal=true` to record local EOF.
4. **Return statistics.** Return the executed plan text and operator statistics. The client runs `EXPLAIN` without `ANALYZE` for the same candidate SQL, checks plan structure and node IDs, and writes the statistics into corresponding nodes.

**Local EOF ends only the current invocation. The parent may call the operator again.**

**Visual:** [Figure 3: Reading GaussDB operator statistics](patent-timeout-feedback-figures/png/fig3-executor-observation-flow.png). The figure identifies structure fields and state formulas. [Editable PowerPoint](patent-timeout-feedback-figures/pptx/fig3-executor-observation-flow.pptx).

## Slide 4 — Technical approach 3: Check repeated execution and whether row counts can be compared

**Identify the scan or join represented by the count.** Query blocks, SQL predicates, join types and runtime parameters must agree before updating that scan or join's Rows Hint.

**Distinguish rows from one invocation from cumulative rows.** Actual and estimated counts must represent the same number of executions. If A accumulates output from several invocations, E must cover those same invocations.

**Check whether the parent will execute the child again.** EOF ends the current invocation. Inspect parent-child calls to establish that the scan or join has returned all relevant results.

**Examples:**

- After an inner join under Nested Loop returns EOF, the next outer row may cause it to execute again.
- An inner operator executes twenty times, estimating ten rows and returning five per invocation. Cumulative output of 100 cannot be compared directly with the per-invocation estimate of ten. Compare five with ten, or scale the estimate to twenty invocations.

**Visual:** [Figure 7: Repeated execution and cumulative row counts](patent-timeout-feedback-figures/png/fig7-reentry-scope.png). [Editable PowerPoint](patent-timeout-feedback-figures/pptx/fig7-reentry-scope.pptx).

## Slide 5 — Technical approach 4: Select usable row counts and update Rows Hints

**Rules: Exact counts replace; bounds only raise; unusable counts leave the Hint unchanged.** A is output returned by cancellation. E is estimated output for the same number of executions.

| Check result | Feedback | Next-round update |
| --- | --- | --- |
| The scan or join has returned all results and its parent will not execute it again | **Exact count `EXACT`** | Replace with A, upward or downward, and clear the old bound |
| All results are not yet confirmed, returned rows only increase, and A > E | **Row-count bound `LOWER_BOUND`** | Without an exact count, retain the greater of the existing Hint and A; otherwise retain the exact count |
| Not executed, actual and estimated counts cannot cover the same executions, or not finished with A ≤ E | **No update `NONE`** | Preserve the Hint |

**Merge rules:** Retain the maximum of multiple bounds. Sampling does not replace exact counts or bounds. Within one round, the first exact count for a scan or join in plan-tree traversal order wins.

**Example:** Start at sampling 5,000. A usable bound of 2,400 leaves the Hint at 5,000 while storing the bound. A bound of 6,400 raises the Hint to 6,400. An exact count of 7,200 replaces it and clears the bound.

**Unfinished output with A ≤ E does not establish overestimation.** Unexecuted operators have no A-rows observation. An executed operator returning zero rows has A-rows=0.

**Visual:** [Figure 5: Row-count classification and Rows Hint updates](patent-timeout-feedback-figures/png/fig5-evidence.png). [Editable PowerPoint](patent-timeout-feedback-figures/pptx/fig5-evidence.pptx).

## Slide 6 — Proposed protection: Combine timeout capture with iterative feedback for the same SQL

**Proposed chain:**

**Successive candidates for the same SQL → deadline per candidate → capture operator facts on cancellation → qualify row counts → correct cardinality for the next candidate.**

1. **Link cancellation and capture:** The tuner sets a deadline per candidate. When the deadline triggers cancellation, preserve the original plan, operator execution state, and cumulative output before cleanup.
2. **Qualify counts per target:** Check target semantics and the number of invocations. Use local completion state and possible future operator invocations to distinguish exact values, usable lower bounds, and unusable observations.
3. **Feed the next candidate for the same SQL:** Convert accepted observations into cardinality inputs for an independent next candidate, accumulating feedback within one tuning task.

**Possible dependent limitations:** Raise an incomplete target's estimate only when A > E in the same scope; exact values take precedence over lower bounds; retain the maximum of multiple bounds; a cancellation deadline does not stand in for complete runtime.

**Relationship to reviewed mechanisms:** The proposed combination uses each candidate's deadline as a trigger for qualified feedback. Oracle also discloses subsequent feedback for identical or equivalent queries. The same-SQL scope should therefore be combined with the deadline, capture timing, and evidence qualification.

**Suggested visual:** Show the proposed chain as a data flow, emphasizing timeout triggering, qualification, and the next candidate for the same SQL.

## Slide 7 — Technical effects: Reduce waiting and retain feedback from slow candidates

**Mechanism benefits:** When usable observations exist, a timed-out round can still provide row-count evidence. Complete targets can correct overestimation or underestimation; incomplete targets can correct underestimation using proven lower bounds. Slow candidates consequently occupy less tuning time.

**Experiment:** JOB: 113 SQL queries; JOB-complex: 30 queries. Compare no per-candidate deadline against ten-second partial feedback. Both groups used a 300-second global tuning search budget and `patience-iters=5`. Ten seconds is the initial per-candidate cap; later limits shrink according to the best complete runtime.

**Aggregate tuning duration:**

| Workload | No per-candidate deadline | Ten-second partial feedback | Reduction |
| --- | ---: | ---: | ---: |
| JOB | 2,348 s | 1,599 s | 31.89% |
| JOB-complex | 1,002 s | 583 s | 41.84% |

**Aggregate complete execution time, extended evaluation:**

| Workload | Original SQL | No per-candidate deadline | Ten-second partial feedback |
| --- | ---: | ---: | ---: |
| JOB | 456 s | 190 s | 168 s |
| JOB-complex | 332 s | 48 s | 47 s |

**Evaluation scope:** Times are rounded; percentages use unrounded data. The extended JOB evaluation includes recovered candidates for `16b` and `18c` that were not formally delivered. Using only delivered candidates and substituting original SQL where necessary gives approximately 205 seconds for the ten-second group. The experiment supports workload-level tuning-duration reductions; execution-time conclusions depend on evaluation scope and do not imply improvement for every query. The groups used different code revisions, so the results describe the integrated implementations.

**Suggested visual:** Place [Figure 8: Tuning duration](patent-timeout-feedback-figures/png/fig8-tuning-duration.png) and [Figure 9: Execution results](patent-timeout-feedback-figures/png/fig9-execution-effect.png) side by side. Keep the caption describing recovered JOB candidates.

## Source and presentation notes

- Slide 2 was checked against [`feedback_effective_timeout`](../contrib/hint_adviser/hint_adviser_feedback_round.c) and [requirements walkthrough section 3.1](超时EXPLAIN_ANALYZE部分反馈需求串讲.md). The [experiment configuration](../data/patience5-ab-20260928/run-manifest.txt) sets the configurable initial cap to ten seconds. The global budget also reserves eight seconds for cancellation, capture, and replanning.
- Slide 3 was checked against server functions [`start_completion_capture`, `collect_execution_evidence`, and `emit_partial_snapshot`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp), [`partial_gaussdb_execution_capture_evidence`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp), and [requirements walkthrough sections 4.4–4.5](<../sources/requirements/超时EXPLAIN_ANALYZE需求串讲.md>). The server snapshot contains the executed plan text and the operator statistics obtained so far. The client obtains the complete JSON plan tree by replanning the same candidate. Operators that have not run do not publish zero-row statistics.
- Slides 4–5 describe the complete qualification and consumption conditions proposed in the disclosure. The prototype already captures, classifies, and merges feedback. Complete coverage of future operator invocations, normalization across invocations, and semantic consistency across rounds still requires work; aggregate performance comparisons do not establish that these checks are fully validated.
- Slide 5 was checked against [requirements walkthrough section 4.8](<../sources/requirements/超时EXPLAIN_ANALYZE需求串讲.md>) and the [`rows_hint_merge` implementation](../contrib/hint_adviser/hint_adviser_rows_state.c). Exact values use first-wins within a round and can replace earlier-round exact values. Bounds accumulate by maximum. A new bound exceeding an existing exact value produces only a conflict diagnostic and leaves the exact value intact. Strengthening an internal bound without changing the Hint output does not count as a Rows output change. The example concerns qualified evidence and does not assume that the current plan estimate always equals a historical Hint value.
- Experimental values were recalculated from the [per-query input records](../data/patience5-ab-20260928/results/workbook-input.tsv). Complete candidate runtimes use the third independent execution selected in the disclosure.
