# An Iterative Query Execution Plan Optimization Method Based on Partial Feedback from Time-Limited Execution: Slide Text

Source: [Patent disclosure draft](patent-timeout-feedback-disclosure-draft.md). Seven slides: one background slide, four technical approach slides, one proposed protection slide, and one technical effects slide. [Chinese version](patent-timeout-feedback-idea-slides.md).

## Slide 1 — Business context: Slow candidates delay automated SQL tuning

**Business setting:** Database operations teams and automated tuning platforms need to find faster plans for a batch of slow SQL queries within a limited tuning window.

**Tuning process:** Execute a candidate each round, use operator row counts to correct cardinality inputs, and generate the next candidate. Complete runtimes support candidate comparisons.

**The problem with slow candidates:** Continued waiting consumes time available for later iterations and may leave no time for further optimization before the global budget ends. Cancellation interrupts the normal `EXPLAIN ANALYZE` result path; without preserving statistics, observations already produced are difficult to reuse for tuning.

**Useful information can exist before timeout:** Some operators have returned all relevant rows; others remain unfinished but have returned far more rows than estimated. Their state and counting ranges determine whether those observations can inform the next round.

**Idea:** Give each candidate an execution deadline, preserve operator statistics before cancellation cleanup, qualify and merge counts, and feed the next candidate for the same SQL.

**Partial feedback:** Usable operator statistics from a candidate whose execution has not completed, including qualified exact counts and lower bounds.

**Business objective: Bound each slow candidate's trial execution and retain its usable operator statistics to reduce overall tuning time, leave time for useful iterations within the global tuning budget, and increase the chance of finding a better execution plan.**

**Suggested visual:** [Figure 1: Execution deadline and feedback timing](patent-timeout-feedback-figures/png/fig1-conventional.png), comparing continued waiting, direct cancellation and preserving feedback on cancellation.

## Slide 2 — Technical approach 1: Iterative tuning with a deadline per candidate

**Scope: Successive candidate plans for the same SQL.** Inputs such as Rows Hints change planning estimates and produce new candidates, each with an independent execution context.

1. **Generate a candidate:** Give the optimizer the original SQL and current cardinality feedback.
2. **Run with a deadline:** Execute the candidate through `EXPLAIN ANALYZE`. Set the initial per-candidate timeout cap `T₀` to ten seconds in the current experiment; this setting is configurable.
3. **On normal completion:** Collect operator statistics and update row-count feedback. Compare complete runtimes and retain the best candidate.
4. **On timeout cancellation:** Preserve the original plan and operator observations before executor cleanup, qualify the row counts, and pass accepted feedback to the next candidate after cancellation finishes.

**Update the timeout:** Use `T₀` until a complete runtime is available. Once the same SQL has a best complete runtime `t_best`, set the cap for later candidates to **`min(T₀, 1.25 × t_best)`**. For example, a best runtime of four seconds reduces the cap to five seconds.

**Both normally completed and timed-out candidates can provide row-count feedback. Only complete runtimes update the best candidate and timeout cap. The actual deadline also respects the remaining global tuning budget.**

**Suggested visual:** [Figure 2: System and feedback position](patent-timeout-feedback-figures/png/fig2-system.png). Show candidate i feeding operator statistics to row-count feedback after either normal completion or timeout cancellation, which supports candidate i+1. Normal completion also supplies runtime for comparing completed candidates.

## Slide 3 — Technical approach 2: Return operator row counts and execution state on timeout cancellation

**Core operation: On timeout cancellation, read and return operator row counts and execution state before executor cleanup, for subsequent feedback qualification.**

1. **Save operator pointers:** Follow the `planstate` pointer in `QueryDesc` to the top-level operator's `PlanState` object, then traverse the execution-state tree and save operator pointers.
2. **Read plan information and counts:** The `plan` pointer in `PlanState` points to a `Plan` object containing the node ID and estimate E. The `instrument` pointer points to an `Instrumentation` object containing cumulative returned rows A and Loops.
3. **Read execution state:** Derive `active` from the call-start timestamp in `Instrumentation`. `active=false` means no `get_next` call is in progress. If `Instrumentation.status != 0` also holds, record `terminal=true` to indicate local EOF.
4. **Return round statistics:** Return the executed plan text, operator counts and state so the client can qualify counts and update later cardinality feedback.

**Local EOF alone does not establish an exact count. Possible further invocations and comparability between actual and estimated counts still require checking.**

**Visual:** [Figure 3: Reading GaussDB operator statistics](patent-timeout-feedback-figures/png/fig3-executor-observation-flow.png). The figure identifies structure fields and state formulas. [Editable PowerPoint](patent-timeout-feedback-figures/pptx/fig3-executor-observation-flow.pptx).

## Slide 4 — Technical approach 3: Check repeated execution and whether row counts can be compared

**Identify the scan or join represented by the count.** Query blocks, SQL predicates, join types and runtime parameters must agree before updating that scan or join's Rows Hint.

**Distinguish rows from one invocation from cumulative rows.** Actual and estimated counts must represent the same number of executions. If A accumulates output from several invocations, E must cover those same invocations.

**Check whether the parent will execute the child again.** EOF ends the current invocation. Check whether continuing the candidate could request further output from that scan or join before treating its relevant results as complete.

**Examples:**

- After an inner join under Nested Loop returns EOF, the next outer row may cause it to execute again.
- An inner operator executes twenty times, estimating ten rows and returning five per invocation. Cumulative output of 100 cannot be compared directly with the per-invocation estimate of ten. Compare five with ten, or scale the estimate to twenty invocations.

**Visual:** [Figure 7: Repeated execution and cumulative row counts](patent-timeout-feedback-figures/png/fig7-reentry-scope.png). [Editable PowerPoint](patent-timeout-feedback-figures/pptx/fig7-reentry-scope.pptx).

## Slide 5 — Technical approach 4: Qualify and merge operator row-count feedback

**Rules: Exact counts replace; bounds only raise; unusable counts leave the Hint unchanged.** A is output returned by the statistics collection point. E is estimated output for the same number of executions. These conditions apply to both normally completed and cancelled candidates.

| Check result | Feedback | Next-round update |
| --- | --- | --- |
| The scan or join has returned all results and its parent will not execute it again | **Exact count `EXACT`** | Replace with A, upward or downward, and clear the old bound |
| All results are not yet confirmed, returned rows only increase, and A > E | **Row-count bound `LOWER_BOUND`** | Without an exact count, retain the greater of the existing Hint and A; otherwise retain the exact count |
| Not executed, actual and estimated counts cannot cover the same executions, or not finished with A ≤ E | **No update `NONE`** | Preserve the Hint |

**Merge rules:** Retain the maximum of multiple bounds. Sampling does not replace adopted exact counts or bounds. Within one round, the first exact count for a scan or join in plan-tree traversal order wins.

**Example:** Start at sampling 5,000. A usable bound of 2,400 leaves the Hint at 5,000 while storing the bound. A bound of 6,400 raises the Hint to 6,400. An exact count of 7,200 replaces it and clears the bound.

**Unfinished output with A ≤ E does not establish overestimation.** Unexecuted operators have no A-rows observation. An executed operator returning zero rows has A-rows=0.

**Visual:** [Figure 5: Row-count classification and Rows Hint updates](patent-timeout-feedback-figures/png/fig5-evidence.png). [Editable PowerPoint](patent-timeout-feedback-figures/pptx/fig5-evidence.pptx).

## Slide 6 — Proposed protection: Qualify operator row counts from bounded execution and merge feedback across rounds

**Proposed chain:**

**Bounded execution of a candidate for the same SQL → capture operator statistics before cancellation cleanup → qualify exact counts, usable bounds or no feedback → merge by type → correct next-round cardinality.**

1. **Preserve statistics on timeout cancellation:** Before executor cleanup, capture operator estimates, returned rows and execution state when the candidate reaches its deadline.
2. **Qualify feedback counts:** Use execution state, possible further invocations and compatible counting ranges to distinguish exact counts, usable lower bounds and no feedback.
3. **Merge by feedback type across rounds:** Exact counts can raise or lower the prior value; qualified bounds only raise it; unusable observations preserve it. Exact values take precedence and bounds accumulate by maximum.
4. **Correct the next independent candidate:** Feed the merged state into the next plan for the same SQL. Normal completion also provides row feedback. Complete runtimes determine the best measured candidate and subsequent adaptive limits.

**Possible dependent limitations:** Accept an unfinished count as a bound only when A and E are comparable and A > E. The execution limit respects the configured cap, best complete runtime and remaining tuning budget.

**Technical focus:** Deadline-triggered cancellation, capture before cleanup, count qualification and merging across rounds form a continuous process for retaining usable feedback and evaluating candidate performance.

**Suggested visual:** Show timeout triggering, count qualification, evidence-dependent merging and cardinality correction for the next candidate.

## Slide 7 — Business value and validation: Reduce waiting and leave time for useful iterations

**Value 1: Bound trial execution, retain usable feedback, and reduce overall tuning time.** Limit the time spent on a slow candidate and use qualified statistics captured before timeout to correct later cardinality inputs. In the workload comparison, aggregate tuning time fell by about 32% on JOB and 42% on JOB-complex relative to the group without a deadline for each candidate.

**Value 2: Leave time for useful iterations within the global budget and increase the chance of finding a better execution plan.** When feedback can change cardinality inputs, the time saved can support later candidate trials. For JOB `16b`, the group without a deadline for each candidate stopped after four rounds under the global budget condition and delivered a candidate with a complete runtime of about 36 seconds. The ten-second group ran eleven partial-feedback rounds and produced a faster recovered candidate, independently measured at about eleven seconds. That recovered candidate was not formally delivered.

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

- The title is “An Iterative Query Execution Plan Optimization Method Based on Partial Feedback from Time-Limited Execution.” “Partial” describes the unfinished candidate execution; qualified completed operators can still provide exact counts. Technical facts, embodiment conditions and experimental results remain in the disclosure, while literature assessments and implementation gaps are kept in the [independent audit record](patent-timeout-feedback-manuscript-audit.md). Replanning and original-plan matching are implementation safeguards, excluded from Slide 6's proposed protection elements.
- Slide 2 was checked against [`feedback_effective_timeout`](../contrib/hint_adviser/hint_adviser_feedback_round.c) and [requirements walkthrough section 3.1](超时EXPLAIN_ANALYZE部分反馈需求串讲.md). The [experiment configuration](../data/patience5-ab-20260928/run-manifest.txt) sets the configurable initial cap to ten seconds. The global budget also reserves eight seconds for cancellation, capture, and replanning.
- Slide 3 was checked against server functions [`start_completion_capture`, `collect_execution_evidence`, and `emit_partial_snapshot`](../contrib/extopt-gaussdb/extopt_partial_capture.cpp), [`partial_gaussdb_execution_capture_evidence`](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp), and [requirements walkthrough sections 4.4–4.5](<../sources/requirements/超时EXPLAIN_ANALYZE需求串讲.md>). The server snapshot contains the executed plan text and the operator statistics obtained so far. The client obtains the complete JSON plan tree by replanning the same candidate. Operators that have not run do not publish zero-row statistics.
- Slides 4–5 describe the complete qualification and consumption conditions proposed in the disclosure. The prototype already captures, classifies, and merges feedback. Complete coverage of future operator invocations, normalization across invocations, and semantic consistency across rounds still requires work; aggregate performance comparisons do not establish that these checks are fully validated.
- Slide 5 was checked against [requirements walkthrough section 4.8](<../sources/requirements/超时EXPLAIN_ANALYZE需求串讲.md>) and the [`rows_hint_merge` implementation](../contrib/hint_adviser/hint_adviser_rows_state.c). Exact values use first-wins within a round and can replace earlier-round exact values. Bounds accumulate by maximum. A new bound exceeding an existing exact value produces only a conflict diagnostic and leaves the exact value intact. Strengthening an internal bound without changing the Hint output does not count as a Rows output change. The example concerns qualified evidence and does not assume that the current plan estimate always equals a historical Hint value.
- Business value separates reduced candidate waiting from opportunities for useful subsequent iterations. Total rounds need not increase: JOB had 569 → 528 and JOB-complex 166 → 182. Resource usage has not been independently measured. See the [per-query analysis](patent-timeout-feedback-job-execution-attribution.md) for the distinction between recovered and delivered 16b candidates.
- Experimental values were recalculated from the [per-query input records](../data/patience5-ab-20260928/results/workbook-input.tsv). Complete candidate runtimes use the third independent execution selected in the disclosure.
