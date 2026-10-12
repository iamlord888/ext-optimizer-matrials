# Patent slide review and proposed protection refinements

Reviewed on 2026-10-12 against the slide text provided in this conversation, the current source tree, and primary patent texts. [Chinese version](patent-timeout-feedback-protection-review.md). [Primary-source review and claim locations](patent-timeout-feedback-primary-review-20261012.md).

The main workflow is clear. The material needs stronger conditions for using operator counts, explicit precedence between exact values and lower bounds, and more accurate comparisons with existing patents. The proposed technical combination is cancellation-time capture of the executed plan and operator observations, validation against that plan, qualification of counts, and merging into the next candidate's cardinality inputs.

## 1. Content corrections

- Ordinary cancellation interrupts the normal result path and can leave collected operator statistics unavailable to tuning. It should not be described as inherently wasting the entire execution attempt.
- Captured counts are observations; some are only lower bounds. They are not all final cardinalities.
- Normal completion also provides row-count feedback. Cancellation leads to another round only when usable feedback exists and the stopping policy permits it.
- Local EOF does not establish that all relevant output for a scan or join has been returned. A parent may invoke the operator again.
- Replace “no rows returned” with “not executed, incomparable statistics, or partial output with A ≤ E.” An executed operator can produce an exact zero-row observation.
- The lower-bound update must preserve an existing exact value. The unconditional formula `max(previous Hint, A)` omits this precedence.
- Simplify the example to sampling 5,000 → qualified bound 6,400 → exact count 7,200. A bound of 2,400 would need an explicit current E and qualification condition. A current plan estimate is not automatically identical to a historical Hint value.

Add a documented real candidate, plan estimates, captured counts, and cancellation result to the background when available. Illustrative numbers and aggregate performance results cannot establish that every operator observation is valid feedback.

## 2. Implementation evidence and remaining work

The executor wrapper saves operator-state objects before running the candidate, collects supported executed operators on cancellation, emits the snapshot, and propagates the original cancellation error. [Capture implementation](../contrib/extopt-gaussdb/extopt_partial_capture.cpp#L103).

A comes from cumulative `Instrumentation.ntuples`. `active` is derived from the start timestamp, and `terminal = !active && status`. The saved `nloops` counter does not establish whether later calls would be required. [GaussDB adapter](../contrib/extopt-gaussdb/extopt_partial_gaussdb.cpp#L581).

After cancellation, the client replans exactly the same candidate SQL without `ANALYZE`, obtaining plan text and JSON topology without executing the candidate again. It compares the executed and replanned representations, including IDs, operation text, query blocks, estimates at the displayed precision, and parent-child structure. Any contradiction rejects the round; observations are published only after the checks pass. [Recovery](../contrib/hint_adviser/hint_adviser_feedback_round.c#L219), [JSON topology](../contrib/hint_adviser/hint_adviser_plan_tree.c#L154), [validation](../contrib/hint_adviser/hint_adviser_partial_proof.c#L97).

Current classification is `TERMINAL && rescan_count == 0 → EXACT`, otherwise `A > E → LOWER_BOUND`, otherwise `NONE`. This is an implementation rule, not a general proof of complete target output or compatible counting units. [Classifier](../contrib/hint_adviser/hint_adviser_partial_proof.c#L14). For example, two invocations returning 80 rows each produce cumulative A=160, which cannot be compared directly with a per-invocation estimate E=100 to prove underestimation. Dividing by `nloops` is not a universal solution because the current unfinished invocation may not be included.

Persistent Rows keys contain the query block and relation targets; they do not independently encode all predicates, join semantics, parameters, or data versions. Exact observations take precedence over bounds; multiple bounds accumulate by maximum; sampling cannot replace established execution evidence. [Targets](../contrib/hint_adviser/hint_adviser_rows_target.c#L529), [keys](../contrib/hint_adviser/hint_adviser_rows_key.c#L160), [merge](../contrib/hint_adviser/hint_adviser_rows_state.c#L56).

Raw `EXACT(0)` is retained, while the encoded Hint has a minimum value of one. A stronger internal bound does not necessarily change the Hint output; if no outputs change, the loop stops for convergence. [Encoding](../contrib/hint_adviser/hint_adviser_rows_state.c#L30), [stop policy](../contrib/hint_adviser/hint_adviser_feedback_policy.c#L99).

Both complete and partial rounds collect counts. Only complete candidates update the best measured runtime; partial elapsed time can still establish failure to meet an improvement threshold. The adaptive timeout uses the minimum available configured cap, global remaining budget minus eight seconds, and 1.25 times the best completed candidate's runtime. Ten seconds is the current experimental cap. [Loop](../contrib/hint_adviser/hint_adviser.c#L345), [runtime policy](../contrib/hint_adviser/hint_adviser_feedback_policy.c#L37), [timeout](../contrib/hint_adviser/hint_adviser_feedback_round.c#L51).

Remaining design work includes excluding inappropriate repeated invocations, aligning cumulative observations with planning counts, and verifying the semantics of targets reused across candidates. These conditions must not be presented as universally implemented.

## 3. Suggested technical slide text

**Technical approach 2: Capture cancellation-time statistics and validate the executed plan.** Save operator objects from `QueryDesc.planstate`; read IDs and estimates through `PlanState.plan` and cumulative output, Loops and state through `PlanState.instrument`; record local EOF; return the executed plan and statistics. Replan the same candidate without `ANALYZE`, validate all corresponding plan structures, then attach statistics or reject the round.

The replan reconstructs the cancelled round. The next candidate is generated later with updated feedback. Physical node IDs identify observations within a round; cross-round merging uses the logical scan/join target.

**Technical approach 3: Qualify and merge operator row-count feedback.** A and E must cover the same scan or join and comparable invocation counts.

| Qualification | Type | Next-round update |
| --- | --- | --- |
| All relevant target output is established as complete | `EXACT(A)` | Replace the value and clear the old bound |
| Output is not established as complete, A is a valid bound for the same target, and A > E | `LOWER_BOUND(A)` | Without an exact value, retain the greater of the current Hint and A; otherwise preserve the exact value |
| Not executed, incomparable counts, or partial output with A ≤ E | `NONE` | Preserve the Hint |

Exact values take precedence; multiple bounds accumulate by maximum; sampling cannot overwrite execution evidence. A conflicting bound above an existing exact value currently produces a diagnostic while retaining that exact value. Example: sampling 5,000 → qualified bound 6,400 → exact value 7,200, clearing the bound. These are proposed strict qualification conditions; the current local-state classifier covers only part of them.

## 4. Patent comparisons

[IBM US7716215B2](https://patents.google.com/patent/US7716215B2/en) already discloses CHECK operators, time/error conditions, prefix threshold violations and EOF-dependent decisions. Intermediate-result reuse is selected by the optimizer. [Oracle US11321317B2](https://patents.google.com/patent/US11321317B2/en) discloses partially or fully executed statistics used for later identical/equivalent queries; claim 1 separately concerns a non-equivalent second query.

[Microsoft US7493337B2](https://patents.google.com/patent/US7493337B2/en) discloses output lower bounds, propagation and estimate refinement for query progress. The closer [Microsoft US11334538B2](https://patents.google.com/patent/US11334538B2/en) also discloses node cardinality feedback, later execution of the same query, separate feedback services and hints. It should be a main comparison, with progress estimation as supplementary material when slide space is limited. [Sybase US9213740B2](https://patents.google.com/patent/US9213740B2/en) shows that aborted elapsed time can establish that a candidate is slower; it should not be called entirely useless.

## 5. Suggested protection slide

**Proposed protection: Validate cancellation-time operator statistics and merge cardinality feedback across rounds.**

1. **Preserve execution facts before cleanup:** Capture the executed plan, operator counts and state when a candidate reaches its deadline and is cancelled.
2. **Validate against the executed plan:** Replan the same candidate without running it again. Attach statistics only after the plan structures match; reject contradictory round data.
3. **Merge by evidence type:** For compatible targets, exact output replaces the prior value, a qualified bound only raises it, and unusable observations preserve it. Exact values take precedence; bounds accumulate by maximum.
4. **Feed an independent next candidate:** Both normal completion and cancellation may provide count feedback. Complete runtimes determine the best measured candidate and subsequent adaptive limits.

Further limitations may cover repeated-invocation and counting safeguards, the configured cap, best complete runtime and remaining tuning budget. The strict count safeguards still need additional implementation and validation.

The technical dependencies matter: cancellation triggers capture; the captured plan supplies the identity basis; successful mapping precedes publication; the evidence type determines the update; the resulting state drives the next independent plan. The specific non-executing reconstruction and whole-plan validation path has clear code support. A broader version could require validation against the executed plan without mandating reconstruction, with the reconstruction path as a further limitation; that broader wording overlaps more with existing feedback mechanisms.

Ten seconds, 1.25, enum labels, external services and the same-SQL scope should not independently carry the principal distinction. Use a general multiplier α with 1.25 as an implementation example. `QueryDesc`, `PlanState`, JSON, messages and Rows Hints explain the implementation but need not define the entire general method.

Useful supporting cases are plan changes after replanning, local EOF followed by another required call, cumulative-versus-per-invocation mismatches, and exact/bound/zero-row merge behavior. This review changed no code and ran no database experiments. The reviewed individual patent texts do not explicitly disclose the complete specific processing chain, but that does not establish novelty or inventive step against combinations and other prior art.
