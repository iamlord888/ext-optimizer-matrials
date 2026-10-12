/*-------------------------------------------------------------------------
 *
 * hint_adviser_archive.c
 *    Per-SQL archive and plantrace capture for hint_adviser.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    contrib/hint_adviser/hint_adviser_archive.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * The archive module owns per-input SQL archive directories and GaussDB
 * plantrace capture.
 *
 * Important entry points:
 * begin_sql_archive()  Create a timestamped per-SQL directory
 *                                  and derive fixed file paths inside it.
 * capture_one_plantrace()  Execute EXPLAIN under enable_plan_trace
 *                                  and dump gs_my_plan_trace output to a log.
 * dump_log_tar_gz()    Package an existing archive/log directory
 *                                  with optional DB stats into a tar.gz file.
 *
 * Reading guide:
 * Start with begin_sql_archive() for directory naming and
 * ownership, then read capture_one_plantrace() for database side
 * effects and cleanup behavior.  Read dump_log_tar_gz() separately
 * for the local dump-log command.
 *
 *   SQL input
 *    |
 *    v
 * [archive directory]
 *    |
 *    v
 *  tuning_agent.log + plantrace logs
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "hint_adviser_log.h"
#include "libpq-fe.h"
#include "pqexpbuffer.h"
#include "port.h"

#include "hint_adviser_archive.h"
#include "hint_adviser_connection.h"
#include "hint_adviser_sql_log.h"

#define HINT_ADVISER_PLANTRACE_QUERY \
    "SELECT * FROM gs_my_plan_trace ORDER BY modifydate DESC LIMIT 1"
#define HINT_ADVISER_PLANTRACE_LOG_MAX_BYTES 100000
#define HINT_ADVISER_PLANTRACE_MAX_TABLE_COUNT 8
#define HINT_ADVISER_DEFAULT_LOGDIR "tuning_agent_log"
#define HINT_ADVISER_MAX_ARCHIVE_DIRS 200
#define HINT_ADVISER_EXTERNAL_OPTIMIZER_LOG "external_optimizer.log"
#define HINT_ADVISER_SCHEMA_FILE "schema.sql"
#define HINT_ADVISER_SCHEMA_TOOL "gs_dump"
#define HINT_ADVISER_STATS_DIR "stats"
#define HINT_ADVISER_STATS_TOOL "dbe_stats_tools.py"
#define HINT_ADVISER_SUMMARY_HEADER \
    "source\tfinish_reason\tinitial_exec_time_ms\t" \
    "sample_tuned_exec_time_ms\tbest_execution_time_ms\t" \
    "sampling_wall_time_ms\ttotal_tuning_wall_time_ms\titerations\t" \
	"best_iteration\ttarget_time_ms\tsearch_timeout_ms\tdelivery_status\t" \
	"external_outcome\tplantrace_status\t" \
	"schema_version\tarchive_dir\texplain_timeout_ms\tpartial_iterations\n"

/*
 * Brief: Convert finish reason enum to the TSV/log value.
 * Inputs: reason - Final finish reason enum.
 * Return: Stable lowercase finish reason.
 */
const char *finish_reason_name(HintAdviserFinishReason reason)
{
    switch (reason) {
		case HINT_ADVISER_FINISH_UNSUPPORTED:
			return "unsupported";
		case HINT_ADVISER_FINISH_FEEDBACK_INCOMPLETE:
			return "Feedback incomplete";
        case HINT_ADVISER_FINISH_TIME_BUDGET_REACHED:
            return "Tuning time budget reached";
		case HINT_ADVISER_FINISH_TARGET_RUNTIME_REACHED:
            return "Target execution time reached";
        case HINT_ADVISER_FINISH_CONVERGED:
            return "Converged";
        case HINT_ADVISER_FINISH_NO_SIGNIFICANT_IMPROVEMENT:
            return "Last several iters have no significant improvement";
        case HINT_ADVISER_FINISH_MAX_ITERATIONS_REACHED:
            return "Maximum iterations reached";
        case HINT_ADVISER_FINISH_ERROR:
        default:
            return "Error";
    }
}

static const char *
plantrace_status_name(HintAdviserPlantraceStatus status)
{
	switch (status)
	{
		case HINT_ADVISER_PLANTRACE_OK:
			return "ok";
		case HINT_ADVISER_PLANTRACE_FAILED:
			return "failed";
		case HINT_ADVISER_PLANTRACE_NOT_RUN:
		default:
			return "not_run";
	}
}

/* Return the stable delivery value shared by TSV and JSONL output. */
static const char *
delivery_status_name(const HintAdviserRunSummary *summary)
{
	if (summary->delivered)
	{
		return "delivered";
	}
	if (summary->tuning_failed)
	{
		return "tuning_failed";
	}
	return "failed";
}

/*
 * Brief: Join a directory and file name.
 * Inputs: directory - Parent directory; filename - Child file name.
 * Return: Joined path owned by the caller.
 */
static char *join_archive_path(const char *directory, const char *filename)
{
    char *path = psprintf("%s/%s", directory, filename);

    canonicalize_path(path);
    return path;
}

/*
 * Brief: Quote one argument for a POSIX shell command.
 * Inputs: value - Argument text.
 * Return: Shell-quoted argument owned by the caller.
 */
static char *shell_quote_arg(const char *value)
{
    size_t max_len = strlen(value) * 4 + 3;
    char *quoted = palloc(max_len);
    char *dst = quoted;

    *dst++ = '\'';
    for (const char *src = value; *src != '\0'; src++) {
        if (*src == '\'') {
            memcpy(dst, "'\\''", 4);
            dst += 4;
        } else {
            *dst++ = *src;
        }
    }
    *dst++ = '\'';
    *dst = '\0';
    return quoted;
}

/*
 * Brief: Return the final path component.
 * Inputs: path - Canonicalized path.
 * Return: Pointer inside path.
 */
static const char *last_path_component(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL ? path : slash + 1;
}

/*
 * Brief: Build a tar parent path for tar -C.
 * Inputs: path - Canonicalized directory path.
 * Return: Parent path owned by the caller.
 */
static char *build_tar_parent_path(const char *path)
{
    char *parent = pstrdup(path);
    char *slash = strrchr(parent, '/');

    if (slash == NULL) {
        return pstrdup(".");
    }
    if (slash == parent) {
        slash[1] = '\0';
        return parent;
    }
    *slash = '\0';
    return parent;
}

/*
 * Brief: Build the default tar.gz output file name.
 * Inputs: logdir - Canonicalized log directory.
 * Return: Output tar.gz path owned by the caller.
 */
static char *build_default_log_tar_gz_path(const char *logdir)
{
    const char *base = last_path_component(logdir);
    char *parent = build_tar_parent_path(logdir);
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    char timestamp[32];

    if (base[0] == '\0') {
        base = HINT_ADVISER_DEFAULT_LOGDIR;
    }
    if (local == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", local) == 0) {
        strcpy(timestamp, "unknown_time");
    }
    char *path = psprintf("%s/%s_%s.tar.gz", parent, base, timestamp);
    return path;
}

/*
 * Brief: Resolve package tools/<tool_name> from the executable path.
 * Inputs: argv0 - Original program path from argv[0];
 *         tool_name - Tool file name under package tools.
 * Return: Tool path owned by the caller, or NULL on failure.
 */
static char *build_package_tool_path(const char *argv0, const char *tool_name)
{
    char exec_path[MAXPGPATH];

    if (find_my_exec(argv0, exec_path) != 0) {
        pg_fatal("could not resolve tuning_agent executable path");
    }

    char bin_dir[MAXPGPATH];
    strlcpy(bin_dir, exec_path, sizeof(bin_dir));
    get_parent_directory(bin_dir);

    char package_dir[MAXPGPATH];
    strlcpy(package_dir, bin_dir, sizeof(package_dir));
    get_parent_directory(package_dir);

    char *tool_path = psprintf("%s/tools/%s",
                               package_dir,
                               tool_name);
    canonicalize_path(tool_path);
    return tool_path;
}

/* Resolve a GaussDB-owned executable without copying it into the package. */
static char *
build_gaussdb_tool_path(const char *tool_name)
{
	const char *gausshome = getenv("GAUSSHOME");
	char *tool_path;

	if (gausshome == NULL || gausshome[0] == '\0' ||
		!is_absolute_path(gausshome))
	{
		pg_log_error("GAUSSHOME must be an absolute path");
		return NULL;
	}
	tool_path = psprintf("%s/bin/%s", gausshome, tool_name);
	canonicalize_path(tool_path);
	return tool_path;
}

/*
 * Brief: Export GaussDB schema-only DDL into the log directory.
 * Inputs: options - Dump-log options; logdir - Canonicalized log directory.
 * Return: 0 on success, 1 on failure.
 */
static int dump_database_schema(const HintAdviserOptions *options,
                                const char *logdir)
{
	char *gs_dump_path = build_gaussdb_tool_path(HINT_ADVISER_SCHEMA_TOOL);

    if (gs_dump_path == NULL) {
        return 1;
    }

    struct stat tool_stat;
    if (stat(gs_dump_path, &tool_stat) != 0 || !S_ISREG(tool_stat.st_mode)) {
        pg_fatal("could not find GaussDB schema dump tool \"%s\": %m",
                 gs_dump_path);
    }

    char *schema_path = join_archive_path(logdir, HINT_ADVISER_SCHEMA_FILE);
    char *quoted_tool = shell_quote_arg(gs_dump_path);
    char *quoted_db = shell_quote_arg(options->connection.database);
    char *quoted_port = shell_quote_arg(options->connection.port);
    char *quoted_schema_path = shell_quote_arg(schema_path);
    char *quoted_host = options->connection.host == NULL ? NULL :
        shell_quote_arg(options->connection.host);
    char *quoted_user = options->connection.user == NULL ? NULL :
        shell_quote_arg(options->connection.user);
    char *quoted_password = options->connection.password == NULL ? NULL :
        shell_quote_arg(options->connection.password);

    char *command = psprintf("%s -p %s -s -f %s%s%s%s%s%s%s %s",
                             quoted_tool,
                             quoted_port,
                             quoted_schema_path,
                             quoted_host == NULL ? "" : " -h ",
                             quoted_host == NULL ? "" : quoted_host,
                             quoted_user == NULL ? "" : " -U ",
                             quoted_user == NULL ? "" : quoted_user,
                             quoted_password == NULL ? "" : " -W ",
                             quoted_password == NULL ? "" : quoted_password,
                             quoted_db);
    int status = system(command);

    if (status != 0) {
        pg_fatal("could not dump database schema");
    }
    return 0;
}

/*
 * Brief: Export GaussDB statistics into the log directory.
 * Inputs: options - Dump-log options; logdir - Canonicalized log directory.
 * Return: 0 on success, 1 on failure.
 */
static int dump_database_stats(const HintAdviserOptions *options,
                               const char *logdir)
{
    char *tool_path =
        build_package_tool_path(options->program_path, HINT_ADVISER_STATS_TOOL);

    if (tool_path == NULL) {
        return 1;
    }
    struct stat tool_stat;
    if (stat(tool_path, &tool_stat) != 0 || !S_ISREG(tool_stat.st_mode)) {
        pg_fatal("could not find package stats tool \"%s\": %m",
                 tool_path);
    }

    char *stats_dir = join_archive_path(logdir, HINT_ADVISER_STATS_DIR);
    char *quoted_tool = shell_quote_arg(tool_path);
    char *quoted_db = shell_quote_arg(options->connection.database);
    char *quoted_port = shell_quote_arg(options->connection.port);
    char *quoted_stats_dir = shell_quote_arg(stats_dir);
    char *quoted_host = options->connection.host == NULL ? NULL :
        shell_quote_arg(options->connection.host);
    char *quoted_user = options->connection.user == NULL ? NULL :
        shell_quote_arg(options->connection.user);
    char *quoted_password = options->connection.password == NULL ? NULL :
        shell_quote_arg(options->connection.password);

    char *command = psprintf("python3 %s -m dump -d %s -p %s --output %s%s%s%s%s%s%s",
                             quoted_tool,
                             quoted_db,
                             quoted_port,
                             quoted_stats_dir,
                             quoted_host == NULL ? "" : " -H ",
                             quoted_host == NULL ? "" : quoted_host,
                             quoted_user == NULL ? "" : " -U ",
                             quoted_user == NULL ? "" : quoted_user,
                             quoted_password == NULL ? "" : " -W ",
                             quoted_password == NULL ? "" : quoted_password);
    int status = system(command);

    if (status != 0) {
        pg_fatal("could not dump database stats");
    }
    return 0;
}

static void append_tsv_text_field(PQExpBuffer output, const char *value)
{
    if (value == NULL) {
        return;
    }
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (*cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            appendPQExpBufferChar(output, ' ');
        } else {
            appendPQExpBufferChar(output, *cursor);
        }
    }
}

/*
 * Brief: Open and exclusively lock the shared summary file.
 * Outputs: fd - Locked file descriptor; needs_header - Whether file is empty.
 */
static int open_locked_summary_file(const char *path,
                                    int *fd,
                                    bool *needs_header)
{
    struct flock lock = {0};
    struct stat statbuf;

    *fd = open(path, O_RDWR | O_CREAT | O_APPEND, pg_file_create_mode);
    if (*fd < 0) {
        pg_log_error("could not open summary file \"%s\": %m", path);
        return 1;
    }

    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    while (fcntl(*fd, F_SETLKW, &lock) != 0) {
        if (errno == EINTR) {
            continue;
        }
        pg_log_error("could not lock summary file \"%s\": %m", path);
        close(*fd);
        return 1;
    }

    if (fstat(*fd, &statbuf) != 0) {
        pg_log_error("could not stat summary file \"%s\": %m", path);
        close(*fd);
        return 1;
    }
    *needs_header = statbuf.st_size == 0;
    if (!*needs_header)
    {
        char header[sizeof(HINT_ADVISER_SUMMARY_HEADER)];
        size_t length = strlen(HINT_ADVISER_SUMMARY_HEADER);
        ssize_t bytes = pread(*fd, header, length, 0);

        if (bytes != (ssize_t) length || memcmp(header, HINT_ADVISER_SUMMARY_HEADER, length) != 0)
        {
            /* Never append v14 rows beneath another schema's column names. */
            pg_log_error("summary header differs from v14: %s; use a new logdir", path);
            close(*fd);
            return 1;
        }
    }
    return 0;
}

/*
 * Brief: Write all bytes while the caller holds the summary file lock.
 */
static int write_archive_bytes(int fd,
                               const char *path,
                               const char *data,
                               size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, data, length);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
			pg_log_error("could not write archive file \"%s\": %m", path);
            return 1;
        }
        data += written;
        length -= written;
    }
    return 0;
}

/*
 * Brief: Return the input file stem or command label.
 * Inputs: input - SQL input source.
 * Return: Unsanitized source stem owned by the caller.
 */
static char *archive_source_stem(const HintAdviserQueryInput *input)
{
    if (input->kind == HINT_ADVISER_QUERY_TEXT) {
        return pstrdup("command");
    }

    const char *base = input->value;
    const char *slash = strrchr(base, '/');
    const char *backslash = strrchr(base, '\\');

    if (slash != NULL && slash + 1 > base) {
        base = slash + 1;
    }
    if (backslash != NULL && backslash + 1 > base) {
        base = backslash + 1;
    }

    char *stem = pstrdup(base);
    char *dot = strrchr(stem, '.');

    if (dot != NULL && dot != stem) {
        *dot = '\0';
    }
    if (stem[0] == '\0') {
        return pstrdup("query");
    }
    return stem;
}

/*
 * Brief: Replace path-unsafe characters in a directory name.
 * Inputs: name - Mutable directory name.
 * Outputs: name - Sanitized directory name.
 * Return: true when the sanitized name still has visible content.
 */
static bool sanitize_archive_name(char *name)
{
    bool has_visible_char = false;

    for (char *cursor = name; *cursor != '\0'; cursor++) {
        unsigned char ch = (unsigned char) *cursor;

        if (ch < 0x20 || strchr("<>:\"/\\|?*", ch) != NULL) {
            *cursor = '_';
        } else if (!isspace(ch)) {
            has_visible_char = true;
        }
    }

    return has_visible_char;
}

/*
 * Brief: Build a timestamped archive directory base name.
 * Inputs: input - SQL input source.
 * Return: Directory base name owned by the caller.
 */
static char *build_archive_dir_name(const HintAdviserQueryInput *input)
{
    char *stem = archive_source_stem(input);
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    char timestamp[32];

    if (!sanitize_archive_name(stem)) {
        stem = pstrdup("query");
    }
    if (local == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", local) == 0) {
        strcpy(timestamp, "unknown_time");
    }

    char *name = psprintf("%s_%s", stem, timestamp);
    return name;
}

/*
 * Brief: Ensure the archive root directory exists.
 * Inputs: root - Archive root path.
 * Return: 0 on success, 1 on failure.
 */
static int ensure_archive_root(const char *root)
{
    char *mkdir_path = pstrdup(root);
    struct stat statbuf;

    canonicalize_path(mkdir_path);
    if (pg_mkdir_p(mkdir_path, pg_dir_create_mode) != 0) {
        if (errno == EEXIST &&
            stat(root, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
            return 0;
        }
        pg_fatal("could not create logdir \"%s\": %m", root);
    }
    return 0;
}

/*
 * Brief: Create a unique per-SQL archive directory.
 * Inputs: root - Archive root; base_name - Timestamped base directory name.
 * Return: Created directory path, or NULL on failure.
 */
static char *create_unique_archive_dir(const char *root, const char *base_name)
{
    for (int attempt = 0; attempt < 1000; attempt++) {
        char *name = attempt == 0 ? pstrdup(base_name) :
            psprintf("%s_%d", base_name, attempt + 1);
        char *directory = join_archive_path(root, name);

        if (mkdir(directory, pg_dir_create_mode) == 0) {
            return directory;
        }
        if (errno != EEXIST) {
            pg_fatal("could not create SQL archive directory \"%s\": %m",
                     directory);
        }
    }

    pg_fatal("could not create unique SQL archive directory under \"%s\"",
             root);
}

/*
 * Brief: Remove the oldest per-SQL archive directory when root is full.
 * Inputs: root - Archive root directory.
 * Return: number of archive directories seen before removal, or -1 on error.
 */
static int prune_one_oldest_archive_dir(const char *root)
{
    char **names = pgfnames(root);

    if (names == NULL) {
        return -1;
    }
    int archive_count = 0;
    char *oldest_path = NULL;
    for (char **name = names; *name != NULL; name++) {
        char *entry_path = join_archive_path(root, *name);
        struct stat statbuf;

        if (stat(entry_path, &statbuf) != 0 || !S_ISDIR(statbuf.st_mode)) {
            continue;
        }
        archive_count++;
        if (oldest_path == NULL) {
            oldest_path = entry_path;
            continue;
        }

        struct stat oldest_statbuf;
        if (stat(oldest_path, &oldest_statbuf) != 0 ||
            statbuf.st_mtime < oldest_statbuf.st_mtime) {
            oldest_path = entry_path;
        }
    }
    if (archive_count < HINT_ADVISER_MAX_ARCHIVE_DIRS ||
        oldest_path == NULL) {
        return archive_count;
    }
    if (!rmtree(oldest_path, true)) {
        pg_log_error("could not remove old SQL archive directory \"%s\"",
                     oldest_path);
        return -1;
    }
    return archive_count;
}

/*
 * Brief: Create one per-SQL archive directory and fixed file paths.
 * Inputs: options - Tool options; input - SQL input source.
 * Outputs: archive - Per-SQL archive paths.
 * Return: 0 on success, 1 on failure.
 */
int begin_sql_archive(const HintAdviserOptions *options,
                                   const HintAdviserQueryInput *input,
                                   HintAdviserSqlArchive *archive,
                                   HintAdviserRunSummary *summary)
{
    *archive = (HintAdviserSqlArchive) {0};
	archive->summary = summary;
	summary->plantrace_status = HINT_ADVISER_PLANTRACE_NOT_RUN;
    const char *archive_root =
        options->logdir == NULL || options->logdir[0] == '\0' ?
        HINT_ADVISER_DEFAULT_LOGDIR : options->logdir;
    char *root = pstrdup(archive_root);
    canonicalize_path(root);
    if (ensure_archive_root(root) != 0) {
        pg_log_warning("stage=begin_archive could not prepare archive root");
        return 1;
    }
    for (;;) {
        int archive_count = prune_one_oldest_archive_dir(root);

        if (archive_count < HINT_ADVISER_MAX_ARCHIVE_DIRS) {
            break;
        }
    }

    char *base_name = build_archive_dir_name(input);
    archive->directory = create_unique_archive_dir(root, base_name);
    if (archive->directory == NULL) {
        pg_log_warning("stage=begin_archive could not create SQL archive directory");
        return 1;
    }

    archive->summary_path = join_archive_path(root, "summary.tsv");
    archive->external_optimizer_log_path =
        join_archive_path(archive->directory,
                          HINT_ADVISER_EXTERNAL_OPTIMIZER_LOG);
    archive->sql_log_path = join_archive_path(archive->directory,
                                              "tuning_agent.log");
    archive->baseline_plantrace_path = join_archive_path(archive->directory,
                                                         "baseline_plantrace.log");
    archive->final_plantrace_path = join_archive_path(archive->directory,
                                                      "final_rowshint_plantrace.log");
    sql_log_path = archive->sql_log_path;
    return 0;
}

int append_run_summary(const HintAdviserSqlArchive *archive,
                                    const HintAdviserOptions *options,
                                    const HintAdviserQueryInput *input,
                                    HintAdviserRunSummary *summary)
{
    PQExpBufferData row;
    int fd;
    bool needs_header;

    if (archive == NULL || archive->summary_path == NULL) {
        return 0;
    }

    initPQExpBuffer(&row);
    append_tsv_text_field(&row, input == NULL ? NULL : input->value);
    appendPQExpBuffer(&row, "\t%s\t",
                      finish_reason_name(summary->finish_reason));
    if (options->has_initial_exec_time_ms) {
        appendPQExpBuffer(&row, "%.3f", options->initial_exec_time_ms);
    }
    appendPQExpBufferChar(&row, '\t');
    if (summary->has_sample_tuned_exec_time_ms) {
        appendPQExpBuffer(&row, "%.3f", summary->sample_tuned_exec_time_ms);
    }
    appendPQExpBufferChar(&row, '\t');
    if (summary->has_best_execution_time_ms) {
        appendPQExpBuffer(&row, "%.3f", summary->best_execution_time_ms);
    }
    appendPQExpBufferChar(&row, '\t');
    appendPQExpBuffer(&row, "%.3f\t%.3f\t%d\t",
                      summary->sampling_wall_time_ms,
                      summary->total_tuning_wall_time_ms,
                      summary->iterations);
    if (summary->best_iteration > 0) {
        appendPQExpBuffer(&row, "%d", summary->best_iteration);
    }
    appendPQExpBufferChar(&row, '\t');
    if (options->has_target_time) {
        appendPQExpBuffer(&row, "%.3f", options->target_time_ms);
    }
    appendPQExpBufferChar(&row, '\t');
    if (options->has_search_timeout) {
        appendPQExpBuffer(&row, "%d", options->search_timeout_ms);
    }
	appendPQExpBuffer(&row, "\t%s\t%s\t%s\t14\t",
					  delivery_status_name(summary),
					  hint_adviser_external_outcome_name(summary->external_outcome),
					  plantrace_status_name(summary->plantrace_status));
    append_tsv_text_field(&row, archive->directory);
    appendPQExpBuffer(&row, "\t%d\t%d", options->explain_timeout_ms,
                       summary->partial_iterations);
    appendPQExpBufferChar(&row, '\n');

    if (PQExpBufferDataBroken(row)) {
        pg_log_error("could not build summary row: out of memory");
        pg_log_warning("stage=append_run_summary could not build summary row");
        return 1;
    }
    if (open_locked_summary_file(archive->summary_path,
                                 &fd,
                                 &needs_header) != 0) {
        pg_log_warning("stage=append_run_summary could not open summary file");
        return 1;
    }
    if ((needs_header &&
		 write_archive_bytes(fd,
                             archive->summary_path,
                             HINT_ADVISER_SUMMARY_HEADER,
                             strlen(HINT_ADVISER_SUMMARY_HEADER)) != 0) ||
		write_archive_bytes(fd,
                            archive->summary_path,
                            row.data,
                            row.len) != 0) {
        close(fd);
        pg_log_warning("stage=append_run_summary could not write summary file");
        return 1;
    }
    if (close(fd) != 0) {
        pg_log_error("could not close summary file \"%s\": %m",
                     archive->summary_path);
        pg_log_warning("stage=append_run_summary could not close summary file");
        return 1;
    }
    return 0;
}

/*
 * Brief: Package a tuning_agent log directory into a tar.gz file.
 * Inputs: options - Dump-log options.
 * Return: 0 on success, 1 on failure.
 */
int dump_log_tar_gz(const HintAdviserOptions *options)
{
    const char *input_logdir =
        options->logdir == NULL || options->logdir[0] == '\0' ?
        HINT_ADVISER_DEFAULT_LOGDIR : options->logdir;
    char *tar_logdir = pstrdup(input_logdir);
    canonicalize_path(tar_logdir);

    if (options->dump_stats) {
        if (ensure_archive_root(tar_logdir) != 0 ||
            dump_database_schema(options, tar_logdir) != 0 ||
            dump_database_stats(options, tar_logdir) != 0) {
            return 1;
        }
    }

    struct stat statbuf;
    if (stat(tar_logdir, &statbuf) != 0) {
        pg_fatal("could not stat logdir \"%s\": %m", tar_logdir);
    }
    if (!S_ISDIR(statbuf.st_mode)) {
        pg_fatal("logdir is not a directory: %s", tar_logdir);
    }

    const char *base = last_path_component(tar_logdir);
    if (base[0] == '\0') {
        pg_fatal("could not determine logdir base name: %s", tar_logdir);
    }

    char *parent = build_tar_parent_path(tar_logdir);
    char *out = build_default_log_tar_gz_path(tar_logdir);
    char *quoted_parent = shell_quote_arg(parent);
    char *quoted_out = shell_quote_arg(out);
    char *quoted_base = shell_quote_arg(base);
    char *command = psprintf("tar -C %s -czf %s %s",
                             quoted_parent,
                             quoted_out,
                             quoted_base);
    int status = system(command);


    if (status != 0) {
        pg_fatal("could not package logdir with tar.gz");
    }

    printf("%s\n", out);
    return 0;
}

/*
 * Brief: Write a plantrace query result in expanded unaligned form.
 * Inputs: file - Output file; conn - Database connection;
 *         res - gs_my_plan_trace result, possibly NULL.
 * Description: This mirrors the useful part of psql -x -A for large multiline
 *              trace values: each field is printed as a standalone block and
 *              field values are written without table wrapping.
 */
static void write_plantrace_result(FILE *file, PGconn *conn, PGresult *res)
{
    fprintf(file, "CONTEXT PLANTRACE RESULT\n");
    if (res == NULL) {
        fprintf(file, "STATUS <NULL>\nERROR %s\n\n", PQerrorMessage(conn));
        return;
    }

    fprintf(file,
            "STATUS %s\nNTUPLES %d\nNFIELDS %d\nCMD_STATUS %s\nERROR %s\n",
            PQresStatus(PQresultStatus(res)),
            PQntuples(res),
            PQnfields(res),
            PQcmdStatus(res),
            PQresultErrorMessage(res));
    for (int row = 0; row < PQntuples(res); row++) {
        fprintf(file, "-[ RECORD %d ]\n", row + 1);
        for (int field = 0; field < PQnfields(res); field++) {
            const char *name = PQfname(res, field);
            const char *value = PQgetisnull(res, row, field) ?
                "<NULL>" : PQgetvalue(res, row, field);
            size_t value_len = strlen(value);
            bool complete_value =
                value_len <= HINT_ADVISER_PLANTRACE_LOG_MAX_BYTES;

            fprintf(file, "----- BEGIN %s -----\n", name);
            fwrite(value,
                   1,
                   complete_value ? value_len :
                   HINT_ADVISER_PLANTRACE_LOG_MAX_BYTES,
                   file);
            if (!complete_value) {
                fprintf(file,
                        "\n[TRUNCATED: plantrace field exceeded %d bytes]\n",
                        HINT_ADVISER_PLANTRACE_LOG_MAX_BYTES);
            }
            if (complete_value &&
                (value[0] == '\0' || value[value_len - 1] != '\n')) {
                fprintf(file, "\n");
            }
            fprintf(file, "----- END %s -----\n", name);
        }
    }
    fprintf(file, "\n");
}

typedef struct PlantraceResult
{
    PGconn *conn;
    FILE   *file;
    bool    has_rows;
} PlantraceResult;

static int write_plantrace_result_handler(PGresult *res, void *data)
{
    PlantraceResult *state = data;

    write_plantrace_result(state->file, state->conn, res);
    fflush(state->file);
    if (PQresultStatus(res) != PGRES_TUPLES_OK ||
        PQntuples(res) < 1) {
        pg_log_error("plantrace query returned no rows");
        return 1;
    }
    state->has_rows = true;
    return 0;
}

/*
 * Brief: Query plantrace and write a sidecar plantrace log.
 * Inputs: conn - Database connection; file - Plantrace log file.
 * Outputs: file - Receives the gs_my_plan_trace query and result.
 * Return: 0 on success, 1 on failure.
 */
static int dump_plantrace_result(PGconn *conn,
                                 FILE *file,
                                 int64 deadline_end_time_usec)
{
    PlantraceResult result = {conn, file, false};

    fprintf(file,
            "SQL PLANTRACE RESULT\n%s\n\n",
            HINT_ADVISER_PLANTRACE_QUERY);

    if (logged_exec_each_result(conn,
                                HINT_ADVISER_PLANTRACE_QUERY,
                                "PLANTRACE RESULT",
                                deadline_end_time_usec,
                                write_plantrace_result_handler,
                                &result,
                                false) != 0 ||
        !result.has_rows) {
        pg_log_error("plantrace query returned no rows");
        return 1;
    }
    return 0;
}

static int expect_tuple_result(PGresult *res, void *data)
{
    const char *context = data;

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        pg_log_error("%s failed: %s", context, PQresultErrorMessage(res));
        return 1;
    }
    return 0;
}

/*
 * Brief: Capture GaussDB plantrace for one SQL.
 * Inputs: conn - Database connection; sql - SQL to wrap in EXPLAIN;
 *         output_path - Plantrace log file path.
 * Return: 0 on success, 1 on failure.
 */
int capture_one_plantrace(PGconn *conn,
                          const char *sql,
                          const char *output_path,
                          int64 deadline_end_time_usec,
                          HintAdviserRunSummary *summary)
{
    FILE *file = fopen(output_path, "w");
    if (file == NULL) {
        pg_log_error("could not open plantrace log \"%s\": %m", output_path);
		summary->plantrace_status = HINT_ADVISER_PLANTRACE_FAILED;
		pg_log_warning("stage=plantrace could not open output file");
        return 1;
    }

    int rc = 0;
    bool transaction_started = false;

    if (logged_exec_each_result(conn,
                                "BEGIN",
                                "PLANTRACE BEGIN",
                                deadline_end_time_usec,
                                command_result_handler,
                                "PLANTRACE BEGIN",
                                true) != 0) {
        rc = 1;
        goto cleanup;
    }
    transaction_started = true;

    if (logged_exec_each_result(conn,
                                "SET LOCAL enable_plan_trace=on",
                                "PLANTRACE SET LOCAL ON",
                                deadline_end_time_usec,
                                command_result_handler,
                                "PLANTRACE SET LOCAL ON",
                                true) != 0) {
        rc = 1;
        goto cleanup;
    }

    char *explain_sql = psprintf("EXPLAIN %s", sql);
    fprintf(file, "TRACE SQL\n%s\n\n", explain_sql);
    fflush(file);
    if (logged_exec_each_result(conn,
                                explain_sql,
                                "PLANTRACE EXPLAIN",
                                deadline_end_time_usec,
                                expect_tuple_result,
                                "PLANTRACE EXPLAIN",
                                true) != 0) {
        rc = 1;
    }

cleanup:
    if (transaction_started) {
        if (rc == 0 &&
            dump_plantrace_result(conn, file, deadline_end_time_usec) != 0) {
            rc = 1;
        }

        if (logged_exec_each_result(conn,
                                    "ROLLBACK",
                                    "PLANTRACE ROLLBACK",
                                    HINT_ADVISER_NO_DEADLINE,
                                    command_result_handler,
                                    "PLANTRACE ROLLBACK",
                                    true) != 0) {
            rc = 1;
        }
    }

    if (fclose(file) != 0) {
        pg_log_error("could not close plantrace log \"%s\": %m", output_path);
        rc = 1;
    }
    if (rc != 0) {
		summary->plantrace_status = HINT_ADVISER_PLANTRACE_FAILED;
		pg_log_warning("stage=plantrace capture failed");
    }
    return rc;
}

/*
 * Brief: Capture final plantrace files when a deliverable candidate exists.
 * Inputs: conn - Database connection; sql - Original SQL;
 *         archive - Per-SQL archive paths; verbose - Whether INFO is mirrored
 *         to stderr.
 * Outputs: summary - Records plantrace status without changing terminal cause.
 */
void capture_plantrace(PGconn *session_connection,
	                   const HintAdviserConnectionOptions *connection_options,
	                       const char *sql,
	                       const HintAdviserSqlArchive *archive,
	                       const FrontendQueryInfo *query_tree_info,
	                   int64 overall_deadline_end_time_usec,
                       HintAdviserRunSummary *summary,
                       bool verbose)
{
	PGconn *plantrace_conn;
	bool owns_connection = false;
	int64 deadline_end_time_usec;

    if (summary->finish_reason == HINT_ADVISER_FINISH_ERROR ||
        summary->best_outline_sql == NULL) {
        return;
    }
    if (query_tree_info->table_count > HINT_ADVISER_PLANTRACE_MAX_TABLE_COUNT) {
        log_message(verbose, "INFO",
                    "skip plantrace: table_count=%d max_table_count=%d",
                    query_tree_info->table_count,
                    HINT_ADVISER_PLANTRACE_MAX_TABLE_COUNT);
        return;
    }

	plantrace_conn = session_connection;
	if (plantrace_conn == NULL) {
		plantrace_conn = connect_database(connection_options);
		owns_connection = true;
	}
	if (plantrace_conn == NULL) {
		summary->plantrace_status = HINT_ADVISER_PLANTRACE_FAILED;
		pg_log_warning("stage=plantrace could not connect to database");
        return;
	}
	log_message(verbose, "INFO",
			"tuning_agent: diagnostic=plantrace_session role=plantrace "
			"backend_pid=%d isolated=%s",
			PQbackendPID(plantrace_conn),
			owns_connection ? "true" : "false");

	deadline_end_time_usec = deadline_start(HINT_ADVISER_PLANTRACE_TIMEOUT_MS);
	if (overall_deadline_end_time_usec >= 0) {
		deadline_end_time_usec = Min(deadline_end_time_usec,
									 overall_deadline_end_time_usec);
	}
    if (capture_one_plantrace(plantrace_conn,
                              sql,
                              archive->baseline_plantrace_path,
                              deadline_end_time_usec,
                              summary) != 0) {
        goto finish;
    }
    if (capture_one_plantrace(plantrace_conn,
                              summary->best_candidate_sql,
                              archive->final_plantrace_path,
                              deadline_end_time_usec,
                              summary) != 0) {
		goto finish;
    }
	summary->plantrace_status = HINT_ADVISER_PLANTRACE_OK;

finish:
	if (owns_connection) {
		PQfinish(plantrace_conn);
	}
}
