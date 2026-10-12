/* Shared bounds and normalized execution states for Partial feedback. */
#ifndef EXTOPT_PARTIAL_PROTOCOL_H
#define EXTOPT_PARTIAL_PROTOCOL_H

#define HAPF_MAX_NODES 10000
#define HAPF_MAX_PAYLOAD (8 * 1024 * 1024)
#define HAPF_MAX_MESSAGE 16384
#define HAPF_SNAPSHOT_TIMEOUT_MS 2000

typedef enum PartialExecutionState
{
    PARTIAL_EXECUTION_INCOMPLETE = 0,
    PARTIAL_EXECUTION_ACTIVE,
    PARTIAL_EXECUTION_TERMINAL
} PartialExecutionState;

#endif /* EXTOPT_PARTIAL_PROTOCOL_H */
