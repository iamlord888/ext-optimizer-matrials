/*-------------------------------------------------------------------------
 *
 * hint_adviser_option.c
 *    Command-line option parsing for hint_adviser.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    contrib/hint_adviser/hint_adviser_option.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * The option module owns argv parsing and SQL input source loading.
 *
 * Important entry points:
 * parse_options()     Build one HintAdviserOptions object from argv.
 * read_query_input()  Materialize the configured -c or -f SQL input.
 *
 * Reading guide:
 * Read parse_options() for CLI behavior, then read read_query_input() for the
 * command/file input boundary.
 */
#include "postgres_fe.h"

#include <errno.h>
#include <limits.h>
#include <math.h>

#include "common/connect.h"
#include "common/string.h"
#include "getopt_long.h"
#include "pg_getopt.h"

#include "hint_adviser_connection.h"
#include "hint_adviser_log.h"
#include "hint_adviser_option.h"
#include "hint_adviser_string.h"

#define HINT_ADVISER_DEFAULT_LOGDIR "tuning_agent_log"
#define HINT_ADVISER_MAX_PASSWORD_BYTES 4096

/*
 * Description: Print tool usage
 * Inputs: progname - program name
 */
static void usage(const char *progname)
{
    printf("%s generates outline-hinted SQL using an internal rows-hint feedback loop.\n\n", progname);
    printf("Usage:\n");
    printf("  %s [connection options] [(-c SQL | -f FILE) ...]\n\n", progname);
    printf("  %s dump-log [connection options] [--logdir=DIR] [--dump-stats=on|off]\n\n", progname);
    printf("Commands:\n");
    printf("  dump-log                 package tuning_agent logs into a tar.gz file\n");
    printf("\n");
    printf("Connection options:\n");
    printf("  -h, --host=HOST           database server host\n");
    printf("  -p, --port=PORT           database server port\n");
    printf("  -U, --username=USER       database user name\n");
    printf("  -d, --dbname=DBNAME       database name, URI, or keyword connection string\n");
    printf("  -s, --schema=SCHEMA       schema to use as the session search_path\n");
    printf("  -W, --password=PASSWORD   database user password\n");
    printf("      --password-stdin      read password from standard input\n");
    printf("  -w, --no-password         never prompt for password\n");
    printf("  -t, --connect-timeout=S   connection timeout in seconds\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --command=SQL         execute one SELECT statement\n");
    printf("  -f, --file=SQLFILE        SQL file path\n");
    printf("  -S, --setting=NAME=VALUE\n");
    printf("                           set one external optimizer configuration value;\n");
    printf("                           may be specified more than once\n");
    printf("      --target-time-ms=MS   enable feedback loop until execution time <= MS\n");
    printf("      --search-timeout-ms=MS\n");
    printf("      --explain-timeout-ms=MS  per-candidate ANALYZE limit; -1 disables it\n");
    printf("                           limit each input SQL search wall-clock time\n");
    printf("      --sampling-blocks=N   target sampled blocks; 0 uses catalog statistics (default: %d)\n",
           HINT_ADVISER_DEFAULT_SAMPLING_BLOCKS);
    printf("      --baseline-ref-time-ms=MS\n");
    printf("                           externally measured original SQL time\n");
    printf("                           (normal mode: verified plan or first iteration)\n");
    printf("      --patience-iters=N    stop after N sub-threshold iterations (default: %d)\n",
           HINT_ADVISER_DEFAULT_PATIENCE_ITERS);
    printf("      --max-iterations=N    stop after at most N feedback iterations\n");
    printf("      --iter-min-improvement=RATIO\n");
    printf("                           required improvement over the previous best (default: %.2f)\n",
           HINT_ADVISER_DEFAULT_ITER_MIN_IMPROVEMENT);
    printf("      --logdir=DIR          archive root for per-SQL logs\n");
    printf("                           each per-SQL archive also stores external_optimizer.log\n");
    printf("                           (default: $TUNING_AGENT_LOG, then ./tuning_agent_log)\n");
    printf("      --verify-initial-plan[=BOOL]\n");
    printf("                           EXPLAIN ANALYZE bare SQL before tuning (default: off)\n");
    printf("      --enable-extoptimizer[=BOOL]\n");
    printf("                           build candidate SQL through the external optimizer (default: on)\n");
    printf("  -v, --verbose             print iteration info to stderr (default: off)\n");
    printf("      --dump-stats[=BOOL]   dump database stats before dump-log tar.gz (default: on)\n");
    printf("  -V, --version             output version information, then exit\n");
    printf("  -?, --help                show this help, then exit\n");
    printf("\n");
    printf("Report bugs to <%s>.\n", PACKAGE_BUGREPORT);
    printf("%s home page: <%s>\n", PACKAGE_NAME, PACKAGE_URL);
}

/*
 * Brief: Print dump-log command usage.
 * Inputs: progname - Program name.
 */
static void usage_dump_log(const char *progname)
{
    printf("%s dump-log packages a tuning_agent log directory into tar.gz.\n\n",
           progname);
    printf("Usage:\n");
    printf("  %s dump-log -d DBNAME -p PORT [connection options] [--logdir=DIR]\n\n",
           progname);
    printf("Options:\n");
    printf("      --dump-stats[=BOOL]   export schema.sql and DB stats before tar.gz (default: on)\n");
    printf("      --logdir=DIR          log directory to package\n");
    printf("                           including per-SQL external_optimizer.log files\n");
    printf("                           (default: $TUNING_AGENT_LOG, then ./tuning_agent_log)\n");
}

/*
 * Description: Validate whether port value is legal
 * Inputs: port - port string
 */
static void validate_port(const char *port)
{
    char *endptr = NULL;

    errno = 0;
    long portnum = strtol(port, &endptr, 10);
    if (errno != 0 || endptr == port || *endptr != '\0' ||
        portnum < 1 || portnum > 65535) {
        pg_fatal("invalid port number: %s", port);
    }
}

/*
 * Description: Set the single SQL input source.
 * Inputs: options - tool options; kind - input type; value - SQL text or file path
 * Outputs: options - updated tool options
 */
static void set_query_input(HintAdviserOptions *options,
                            HintAdviserQueryKind kind,
                            const char *value)
{
    if (options->has_query) {
        pg_fatal("only one SQL input is supported");
    }

    options->query.kind = kind;
    options->query.value = pstrdup(value);
    options->has_query = true;
}

/*
 * Append one structurally valid external optimizer setting.
 * Inputs: options - Options-owned destination; raw_setting - NAME=VALUE text.
 * Outputs: options - Appended name/value pair in command-line order.
 * Name and value semantics remain owned by ExtOptSetConfig().
 */
static void
hint_adviser_append_setting(HintAdviserOptions *options, const char *raw_setting)
{
    const char *separator;

    separator = strchr(raw_setting, '=');

    if (separator == NULL)
    {
        pg_fatal("setting must use NAME=VALUE");
    }

    char *name = copy_trimmed_span(raw_setting, separator);
    char *value = copy_trimmed_span(separator + 1,
                                    raw_setting + strlen(raw_setting));

    if (name[0] == '\0')
    {
        pg_fatal("setting name must not be empty");
    }
    if (value[0] == '\0')
    {
        pg_fatal("setting value must not be empty");
    }
    if (options->setting_count == INT_MAX)
    {
        pg_fatal("too many external optimizer settings");
    }

    HintAdviserSetting *setting;

    if (options->settings == NULL)
    {
        options->settings = palloc_array(HintAdviserSetting, 1);
    }
    else
    {
        options->settings = repalloc_array(options->settings,
                                            HintAdviserSetting,
                                            options->setting_count + 1);
    }
    setting = &options->settings[options->setting_count++];
    setting->name = name;
    setting->value = value;
}

/*
 * Description: Read SQL file content
 * Inputs: path - SQL file path
 * Return: SQL file content
 */
static char *read_sql_file(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        pg_fatal("could not open file \"%s\": %m", path);
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        pg_fatal("could not seek in file \"%s\": %m", path);
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        pg_fatal("could not determine size of file \"%s\": %m", path);
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        pg_fatal("could not seek in file \"%s\": %m", path);
    }

    char *sql = palloc(file_size + 1);
    size_t bytes_read = fread(sql, 1, file_size, file);
    if (bytes_read != (size_t) file_size) {
        pg_fatal("could not read file \"%s\": %m", path);
    }
    sql[bytes_read] = '\0';

    if (fclose(file) != 0) {
        pg_fatal("could not close file \"%s\": %m", path);
    }

    if (bytes_read == 0) {
        pg_fatal("SQL file \"%s\" is empty", path);
    }

    return sql;
}

/*
 * Description: Parse a double option that must be greater than 0
 * Inputs: value - option value string; option - option name
 * Return: parsed double value
 */
static double parse_positive_double_option(const char *value, const char *option)
{
    char *endptr = NULL;

    errno = 0;
    double result = strtod(value, &endptr);
    if (errno != 0 || endptr == value || *endptr != '\0' ||
        !isfinite(result) || result <= 0) {
        pg_fatal("invalid value for %s: %s", option, value);
    }

    return result;
}

/*
 * Description: Parse a positive or nonnegative int option
 * Inputs: value - option value string; option - option name;
 *         positive - whether the value must be greater than or equal to 1
 * Return: parsed int value
 */
static int parse_int_option(const char *value, const char *option, bool positive)
{
    char *endptr = NULL;

    errno = 0;
    long result = strtol(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' ||
        result < (positive ? 1 : 0) || result > INT_MAX) {
        pg_fatal("invalid value for %s: %s", option, value);
    }

    return (int) result;
}

/*
 * Brief: Parse a boolean command-line option.
 * Inputs: value - Optional option value string; option - Option name.
 * Return: Parsed boolean; no value means true.
 */
static bool parse_bool_option(const char *value, const char *option)
{
    if (value == NULL || value[0] == '\0') {
        return true;
    }
    if (pg_strcasecmp(value, "on") == 0 ||
        pg_strcasecmp(value, "true") == 0 ||
        pg_strcasecmp(value, "yes") == 0 ||
        strcmp(value, "1") == 0) {
        return true;
    }
    if (pg_strcasecmp(value, "off") == 0 ||
        pg_strcasecmp(value, "false") == 0 ||
        pg_strcasecmp(value, "no") == 0 ||
        strcmp(value, "0") == 0) {
        return false;
    }
    pg_fatal("invalid value for %s: %s", option, value);
}

/*
 * Brief: Read the complete password payload from standard input.
 * Return: Null-terminated password bytes.
 */
static char *read_password_from_stdin(void)
{
    char *password = palloc(HINT_ADVISER_MAX_PASSWORD_BYTES + 2);
    size_t bytes_read = fread(password,
                              1,
                              HINT_ADVISER_MAX_PASSWORD_BYTES + 1,
                              stdin);

    if (ferror(stdin)) {
        pg_fatal("could not read password from standard input: %m");
    }
    if (bytes_read == 0) {
        pg_fatal("password from standard input is empty");
    }
    if (bytes_read > HINT_ADVISER_MAX_PASSWORD_BYTES) {
        pg_fatal("password from standard input exceeds %d bytes",
                 HINT_ADVISER_MAX_PASSWORD_BYTES);
    }
    if (memchr(password, '\0', bytes_read) != NULL) {
        pg_fatal("password from standard input contains a null byte");
    }
    password[bytes_read] = '\0';
    return password;
}

/*
 * Resolve the one archive root used by every native logging consumer.
 * Command-line parsing owns the explicit value; this helper supplies only
 * the environment and built-in fallbacks so downstream code never repeats
 * path-source precedence.
 * Inputs/Outputs: options - Tool options receiving the resolved root.
 */
static void
hint_adviser_resolve_log_directory(HintAdviserOptions *options)
{
    const char *logdir;

    if (options->logdir != NULL)
    {
        return;
    }

    logdir = getenv("TUNING_AGENT_LOG");
    if (logdir == NULL || logdir[0] == '\0')
    {
        logdir = HINT_ADVISER_DEFAULT_LOGDIR;
    }
    options->logdir = pstrdup(logdir);
}

/*
 * Description: Parse command-line options
 * Inputs: argc - number of arguments; argv - argument array
 * Outputs: options - parsed tool options
 */
void parse_options(int argc, char **argv, HintAdviserOptions *options)
{
    static struct option long_options[] = {
        {"host", required_argument, NULL, 'h'},
        {"dbname", required_argument, NULL, 'd'},
        {"schema", required_argument, NULL, 's'},
        {"username", required_argument, NULL, 'U'},
        {"command", required_argument, NULL, 'c'},
        {"file", required_argument, NULL, 'f'},
        {"setting", required_argument, NULL, 'S'},
        {"port", required_argument, NULL, 'p'},
        {"password", required_argument, NULL, 'W'},
        {"password-stdin", no_argument, NULL, 1010},
        {"no-password", no_argument, NULL, 'w'},
        {"connect-timeout", required_argument, NULL, 't'},
        {"target-time-ms", required_argument, NULL, 1001},
        {"sampling-blocks", required_argument, NULL, 1004},
        {"logdir", required_argument, NULL, 1005},
        {"search-timeout-ms", required_argument, NULL, 1006},
        {"explain-timeout-ms", required_argument, NULL, 1016},
        {"dump-stats", optional_argument, NULL, 1007},
        {"verify-initial-plan", optional_argument, NULL, 1008},
        {"verify_initial_plan", optional_argument, NULL, 1008},
        {"baseline-ref-time-ms", required_argument, NULL, 1011},
        {"patience-iters", required_argument, NULL, 1012},
        {"iter-min-improvement", required_argument, NULL, 1013},
        {"max-iterations", required_argument, NULL, 1015},
        {"enable-extoptimizer", optional_argument, NULL, 1014},
        {"verbose", no_argument, NULL, 'v'},
        {"version", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, '?'},
        {NULL, 0, NULL, 0}
    };

    
    options->connection.password_mode = HINT_ADVISER_PASSWORD_DEFAULT;
    const char *progname = get_progname(argv[0]);
    bool is_dump_log_command = argc > 1 && strcmp(argv[1], "dump-log") == 0;
    bool password_stdin = false;

    options->program_path = pstrdup(argv[0]);
    options->connection.progname = progname;
    options->command_mode = is_dump_log_command ?
        HINT_ADVISER_COMMAND_DUMP_LOG : HINT_ADVISER_COMMAND_ADVISE;
    options->search_timeout_ms = HINT_ADVISER_NO_DEADLINE;
    options->explain_timeout_ms = -1;
    options->deadline_end_time_usec = HINT_ADVISER_NO_DEADLINE;
    options->sampling_blocks = HINT_ADVISER_DEFAULT_SAMPLING_BLOCKS;
    options->patience_iters = HINT_ADVISER_DEFAULT_PATIENCE_ITERS;
    options->iter_min_improvement =
        HINT_ADVISER_DEFAULT_ITER_MIN_IMPROVEMENT;
    options->dump_stats = true;
    options->enable_extoptimizer = true;

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0) {
            usage(progname);
            exit(0);
        }
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            printf("%s (PostgreSQL) %s\n", progname, PG_VERSION);
            exit(0);
        }
        if (is_dump_log_command && argc > 2 &&
            (strcmp(argv[2], "--help") == 0 || strcmp(argv[2], "-?") == 0)) {
            usage_dump_log(progname);
            exit(0);
        }
    }

    int c;
    int optindex = 0;
    if (is_dump_log_command) {
        optind = 2;
    }
    while ((c = getopt_long(argc, argv, "h:p:U:d:s:W:wc:f:t:S:vV", long_options, &optindex)) != -1) {
        switch (c) {
            case 'h':
                options->connection.host = pstrdup(optarg);
                break;
            case 'd':
                options->connection.database = pstrdup(optarg);
                break;
            case 's':
                if (optarg[0] == '\0') {
                    pg_fatal("invalid value for --schema: empty schema name");
                }
                options->connection.schema = pstrdup(optarg);
                break;
            case 'U':
                options->connection.user = pstrdup(optarg);
                break;
            case 'c':
                if (is_dump_log_command) {
                    pg_fatal("option -c is not valid with dump-log");
                }
                set_query_input(options, HINT_ADVISER_QUERY_TEXT, optarg);
                break;
            case 'f':
                if (is_dump_log_command) {
                    pg_fatal("option -f is not valid with dump-log");
                }
                set_query_input(options, HINT_ADVISER_QUERY_FILE, optarg);
                break;
            case 'p':
                validate_port(optarg);
                options->connection.port = pstrdup(optarg);
                break;
            case 'W':
                options->connection.password = pstrdup(optarg);
                break;
            case 'w':
                options->connection.password_mode = HINT_ADVISER_PASSWORD_NEVER;
                break;
            case 't':
                options->connection.connect_timeout = pstrdup(optarg);
                break;
            case 'S':
                if (is_dump_log_command)
                {
                    pg_fatal("option -S/--setting is not valid with dump-log");
                }
                hint_adviser_append_setting(options, optarg);
                break;
            case 'v':
                options->verbose = true;
                break;
            case 1001:
                if (is_dump_log_command) {
                    pg_fatal("option --target-time-ms is not valid with dump-log");
                }
                options->target_time_ms = parse_positive_double_option(optarg,
                                                                       "--target-time-ms");
                options->has_target_time = true;
                break;
            case 1004:
                if (is_dump_log_command) {
                    pg_fatal("option --sampling-blocks is not valid with dump-log");
                }
                options->sampling_blocks = parse_int_option(optarg,
                                                            "--sampling-blocks",
                                                            false);
                break;
            case 1005:
                if (optarg[0] == '\0') {
                    pg_fatal("invalid value for --logdir: empty path");
                }
                options->logdir = pstrdup(optarg);
                break;
            case 1006:
                if (is_dump_log_command) {
                    pg_fatal("option --search-timeout-ms is not valid with dump-log");
                }
                options->search_timeout_ms =
                    parse_int_option(optarg, "--search-timeout-ms", true);
                options->has_search_timeout = true;
                break;
            case 1016:
                if (options->command_mode == HINT_ADVISER_COMMAND_DUMP_LOG)
                {
                    pg_fatal("option --explain-timeout-ms is not valid with dump-log");
                }
                options->explain_timeout_ms = strcmp(optarg, "-1") == 0 ? -1 :
                    parse_int_option(optarg, "--explain-timeout-ms", true);
                break;
            case 1007:
                if (!is_dump_log_command) {
                    pg_fatal("option --dump-stats is only valid with dump-log");
                }
                options->dump_stats = parse_bool_option(optarg, "--dump-stats");
                break;
            case 1008:
                if (is_dump_log_command) {
                    pg_fatal("option --verify-initial-plan is not valid with dump-log");
                }
                options->verify_initial_plan =
                    parse_bool_option(optarg, "--verify-initial-plan");
                break;
            case 1010:
                if (is_dump_log_command) {
                    pg_fatal("option --password-stdin is not valid with dump-log");
                }
                password_stdin = true;
                break;
            case 1011:
                if (is_dump_log_command) {
                    pg_fatal("option --baseline-ref-time-ms is not valid with dump-log");
                }
                options->initial_exec_time_ms =
                    parse_positive_double_option(optarg,
                                                 "--baseline-ref-time-ms");
                options->has_initial_exec_time_ms = true;
                break;
            case 1012:
                if (is_dump_log_command) {
                    pg_fatal("option --patience-iters is not valid with dump-log");
                }
                options->patience_iters =
                    parse_int_option(optarg, "--patience-iters", true);
                break;
            case 1015:
                if (is_dump_log_command) {
                    pg_fatal("option --max-iterations is not valid with dump-log");
                }
                options->max_iterations =
                    parse_int_option(optarg, "--max-iterations", true);
                break;
            case 1013:
                if (is_dump_log_command) {
                    pg_fatal("option --iter-min-improvement is not valid with dump-log");
                }
                options->iter_min_improvement =
                    parse_positive_double_option(optarg,
                                                 "--iter-min-improvement");
                if (!(options->iter_min_improvement < 1)) {
                    pg_fatal("invalid value for --iter-min-improvement: %s",
                             optarg);
                }
                break;
            case 1014:
                if (is_dump_log_command) {
                    pg_fatal("option --enable-extoptimizer is not valid with dump-log");
                }
                options->enable_extoptimizer =
                    parse_bool_option(optarg, "--enable-extoptimizer");
                break;
            case 'V':
                printf("%s (PostgreSQL) %s\n", progname, PG_VERSION);
                exit(0);
            default:
                pg_fatal("invalid command-line arguments; try \"%s --help\" for more information",
                         progname);
        }
    }

    if (password_stdin && options->connection.password != NULL) {
        pg_fatal("--password-stdin cannot be used together with -W/--password");
    }
    if (password_stdin &&
        options->connection.password_mode == HINT_ADVISER_PASSWORD_NEVER) {
        pg_fatal("--password-stdin cannot be used together with -w/--no-password");
    }
    if (options->connection.password != NULL &&
        options->connection.password_mode == HINT_ADVISER_PASSWORD_NEVER) {
        pg_fatal("-W/--password cannot be used together with -w/--no-password");
    }
    if (password_stdin) {
        options->connection.password = read_password_from_stdin();
    }

    hint_adviser_resolve_log_directory(options);

    if ((!is_dump_log_command || options->dump_stats) &&
        options->connection.database == NULL) {
        pg_fatal("missing required option: -d DBNAME; try \"%s --help\" for more information",
                 progname);
    }
    if (is_dump_log_command && options->dump_stats &&
        options->connection.port == NULL) {
        pg_fatal("missing required option for dump-log --dump-stats: -p PORT; try \"%s dump-log --help\" for more information",
                 progname);
    }
    if (optind < argc) {
        pg_fatal("too many command-line arguments; try \"%s --help\" for more information",
                 progname);
    }
    options->deadline_end_time_usec =
        deadline_start(options->search_timeout_ms);
}

/*
 * Description: Read one SQL input
 * Inputs: input - SQL input source
 * Return: SQL text
 */
char *read_query_input(const HintAdviserQueryInput *input)
{
    if (input->kind == HINT_ADVISER_QUERY_TEXT) {
        return pstrdup(input->value);
    }
    return read_sql_file(input->value);
}
