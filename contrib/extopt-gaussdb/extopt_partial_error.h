/*-------------------------------------------------------------------------
 *
 * extopt_partial_error.h
 *      Backend error helpers shared by the extopt implementation modules.
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTOPT_PARTIAL_ERROR_H
#define EXTOPT_PARTIAL_ERROR_H

#include "gaussdb.h"

#include "utils/elog.h"
#include "utils/errcodes.h"

/* Raise a backend error without returning to an invalid capture path. */
static inline void
partial_report_error(int sqlerrcode, const char *message)
{
    ERR_PROC(GAUSS_25637,
             ERR_LEVEL_ERROR,
             (err_ecode(sqlerrcode), err_msg("%s", message)));
}

/* Raise a backend error with an explicit layout or capture diagnostic. */
static inline void
partial_report_error_detail(int sqlerrcode,
                           const char *message,
                           const char *detail)
{
    ERR_PROC(GAUSS_25637,
             ERR_LEVEL_ERROR,
             (err_ecode(sqlerrcode),
              err_msg("%s", message),
              err_detail("%s", detail)));
}

#endif /* EXTOPT_PARTIAL_ERROR_H */
