/* SQL entry point for arming one execution-scoped Partial capture. */
#include "gaussdb.h"
#include "fmgr.h"

#include "extopt_partial_capture.h"
#include "extopt_partial_error.h"

extern "C" Datum extopt_partial_capability(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(extopt_partial_capability);

/*
 * Arm execution_id for this backend. Zero disarms after an error before
 * ExecutorRun; negative ids are invalid.
 */
Datum extopt_partial_capability(PG_FUNCTION_ARGS)
{
    int64 execution_id = PG_GETARG_INT64(0);
    if (execution_id < 0)
    {
        partial_report_error(ERRCODE_INVALID_PARAMETER_VALUE,
                             "partial execution ID must be nonnegative");
    }
    partial_capture_prepare(execution_id);
    PG_RETURN_BOOL(true);
}
