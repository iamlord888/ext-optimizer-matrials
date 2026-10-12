/* HAPF framing and row-executor payload validation; no plan proof lives here. */
#include "postgres_fe.h"

#include <errno.h>
#include <math.h>

#include "common/base64.h"
#include "hint_adviser_partial_snapshot.h"

/* Reject the whole execution permanently, preserving its first diagnostic. */
static bool partial_reject(PartialSnapshot *snapshot, const char *reason)
{
    snapshot->invalid = true;
    if (snapshot->failure_reason == NULL)
    {
        snapshot->failure_reason = reason;
    }
    return false;
}

/* Parse canonical nonnegative decimal fields without signs or overflow. */
static bool partial_unsigned(const char *text, uint64_t limit, uint64_t *value)
{
    uint64_t parsed = 0;

    if (text == NULL || *text == '\0' || (text[0] == '0' && text[1] != '\0'))
    {
        return false;
    }
    for (const char *p = text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9' || parsed > limit / 10 ||
            (parsed == limit / 10 && (uint64_t) (*p - '0') > limit % 10))
        {
            return false;
        }
        parsed = parsed * 10 + (*p - '0');
    }
    *value = parsed;
    return true;
}

/* Split strict fields, retaining empty values so malformed records fail. */
static bool partial_split(char *text, char separator, char **fields, int count)
{
    for (int i = 0; i < count; i++)
    {
        char *next = strchr(text, separator);

        fields[i] = text;
        if ((i == count - 1) != (next == NULL) || *text == '\0')
        {
            return false;
        }
        if (next != NULL)
        {
            *next = '\0';
            text = next + 1;
        }
    }
    return true;
}

/* Initialize a fresh assembler; the caller frees the prior execution first. */
void partial_snapshot_init(PartialSnapshot *snapshot, uint64_t execution_id)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->execution_id = execution_id;
}

/* Free only owned buffers. Plan mapping must copy any facts it retains. */
void partial_snapshot_free(PartialSnapshot *snapshot)
{
    if (snapshot->payload != NULL)
    {
        pfree(snapshot->payload);
    }
    if (snapshot->observations != NULL)
    {
        pfree(snapshot->observations);
    }
    if (snapshot->pretty != NULL)
    {
        pfree(snapshot->pretty);
    }
    memset(snapshot, 0, sizeof(*snapshot));
}

/* Decode one bounded base64 payload chunk. */
static bool partial_append(PartialSnapshot *snapshot, const char *encoded)
{
    size_t length = strlen(encoded);
    uint8 decoded[HAPF_MAX_MESSAGE];

    if (length == 0 || length > HAPF_MAX_MESSAGE)
    {
        return partial_reject(snapshot, "invalid_base64");
    }
    int bytes = pg_b64_decode(encoded, (int) length, decoded, sizeof(decoded));

    if (bytes <= 0)
    {
        return partial_reject(snapshot, "invalid_base64");
    }
    if ((size_t) bytes > HAPF_MAX_PAYLOAD - snapshot->length)
    {
        return partial_reject(snapshot, "payload_limit_exceeded");
    }
    size_t needed = snapshot->length + bytes + 1;

    if (needed > snapshot->capacity)
    {
        snapshot->capacity = Min((size_t) HAPF_MAX_PAYLOAD + 1,
            Max(needed, Max(snapshot->capacity * 2, (size_t) 4096)));
        snapshot->payload = snapshot->payload == NULL ?
            palloc(snapshot->capacity) : repalloc(snapshot->payload, snapshot->capacity);
    }
    memcpy(snapshot->payload + snapshot->length, decoded, bytes);
    snapshot->length += bytes;
    snapshot->payload[snapshot->length] = '\0';
    return true;
}

/* Split a fixed positional message without accepting empty or extra fields. */
static int partial_tokenize(char *text, char **fields, int capacity)
{
    int count = 0;

    while (*text != '\0')
    {
        if (count >= capacity || *text == ' ')
        {
            return -1;
        }
        fields[count++] = text;
        char *separator = strchr(text, ' ');

        if (separator == NULL)
        {
            break;
        }
        *separator = '\0';
        text = separator + 1;
        if (*text == '\0')
        {
            return -1;
        }
    }
    return count;
}

/* Consume one DATA or END notice for the current execution. */
static bool partial_message(PartialSnapshot *snapshot, char *message)
{
    char *fields[4] = {0};
    char *cursor = message;

    if (strncmp(cursor, "HAPF ", 5) != 0)
    {
        return partial_reject(snapshot, "invalid_prefix");
    }
    cursor += 5;
    int field_count = partial_tokenize(cursor, fields, lengthof(fields));
    uint64_t execution, sequence;

    if (field_count < 3 ||
        !partial_unsigned(fields[0], INT64_MAX, &execution) ||
        execution != snapshot->execution_id || execution == 0 ||
        !partial_unsigned(fields[1], HAPF_MAX_PAYLOAD + 1, &sequence) ||
        sequence != snapshot->next_sequence)
    {
        return partial_reject(snapshot, "identity_or_sequence_mismatch");
    }
    if (strcmp(fields[2], "DATA") == 0)
    {
        if (field_count != 4)
        {
            return partial_reject(snapshot, "invalid_data");
        }
        if (!partial_append(snapshot, fields[3]))
        {
            return false;
        }
        snapshot->begun = true;
    }
    else if (strcmp(fields[2], "END") == 0)
    {
        if (field_count != 3 || !snapshot->begun)
        {
            return partial_reject(snapshot, "invalid_end_or_missing_data");
        }
        snapshot->ended = true;
    }
    else
    {
        return partial_reject(snapshot, "unknown_kind");
    }
    snapshot->next_sequence++;
    return true;
}

/* Accept framing only for this execution; callers route ordinary notices away. */
bool partial_snapshot_accept(PartialSnapshot *snapshot, const char *message)
{
    if (snapshot->invalid)
    {
        return false;
    }
    if (snapshot->ended || message == NULL || strlen(message) > HAPF_MAX_MESSAGE)
    {
        return partial_reject(snapshot, "message_after_end_or_oversized");
    }
    char *copy = pstrdup(message);
    bool accepted = partial_message(snapshot, copy);

    pfree(copy);
    return accepted;
}

/* Parse finite nonnegative A/E rows, retaining the unrounded runtime count. */
static bool partial_rows(const char *text, double *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' &&
        isfinite(*value) && *value >= 0.0;
}

/* Decode one normalized execution record. */
static bool partial_read_observation(char *line, PartialObservation *observation)
{
    char *fields[5];
    uint64_t integers[3];
    const uint64_t limits[] = {
        INT32_MAX, PARTIAL_EXECUTION_TERMINAL, INT32_MAX
    };

    if (!partial_split(line, ',', fields, lengthof(fields)))
    {
        return false;
    }
    for (int i = 0; i < 3; i++)
    {
        if (!partial_unsigned(fields[i], limits[i], &integers[i]))
        {
            return false;
        }
    }
    observation->plan_id = integers[0];
    observation->execution_state = (PartialExecutionState) integers[1];
    observation->rescan_count = integers[2];
    return observation->plan_id > 0 &&
        partial_rows(fields[3], &observation->actual_rows) &&
        partial_rows(fields[4], &observation->plan_rows);
}

/*
 * Verify the entire payload before publishing PRETTY and observations. A valid
 * empty observation set remains distinguishable from damaged transport.
 */
bool partial_snapshot_finalize(PartialSnapshot *snapshot)
{
    const char prefix[] = "PRETTY-BYTES=";
    const char marker[] = "\nOBSERVATIONS\n";

    if (snapshot->parsed)
    {
        return !snapshot->invalid;
    }
    if (snapshot->invalid || !snapshot->begun || !snapshot->ended ||
        snapshot->length < strlen(prefix) || snapshot->payload == NULL ||
        memchr(snapshot->payload, '\0', snapshot->length) != NULL ||
        memcmp(snapshot->payload, prefix, strlen(prefix)) != 0)
    {
        return partial_reject(snapshot, "incomplete_or_invalid_payload");
    }
    char *size_text = (char *) snapshot->payload + strlen(prefix);
    char *newline = strchr(size_text, '\n');
    uint64_t pretty_bytes;

    if (newline == NULL)
    {
        return partial_reject(snapshot, "missing_pretty_size");
    }
    *newline = '\0';
    bool valid_size = partial_unsigned(size_text, HAPF_MAX_PAYLOAD, &pretty_bytes);

    *newline = '\n';
    size_t header = newline + 1 - (char *) snapshot->payload;

    if (!valid_size || pretty_bytes == 0 ||
        pretty_bytes > snapshot->length - header ||
        snapshot->length - header - pretty_bytes < strlen(marker) ||
        memcmp(newline + 1 + pretty_bytes, marker, strlen(marker)) != 0)
    {
        return partial_reject(snapshot, "invalid_pretty_size");
    }
    snapshot->pretty = pnstrdup(newline + 1, pretty_bytes);
    char *cursor = newline + 1 + pretty_bytes + strlen(marker);
    int observation_capacity = 0;

    while (*cursor != '\0')
    {
        char *next = strchr(cursor, '\n');

        if (next == NULL || next == cursor ||
            snapshot->observation_count >= HAPF_MAX_NODES)
        {
            return partial_reject(snapshot, "missing_observation");
        }
        if (snapshot->observation_count == observation_capacity)
        {
            observation_capacity = observation_capacity == 0 ? 8 : observation_capacity * 2;
            snapshot->observations = snapshot->observations == NULL ?
                palloc_array(PartialObservation, observation_capacity) :
                repalloc_array(snapshot->observations, PartialObservation,
                              observation_capacity);
        }
        char *copy = pnstrdup(cursor, next - cursor);
        PartialObservation *observation =
            &snapshot->observations[snapshot->observation_count];
        bool valid = partial_read_observation(copy, observation);

        pfree(copy);
        if (!valid)
        {
            return partial_reject(snapshot, "invalid_observation");
        }
        for (int prior = 0; prior < snapshot->observation_count; prior++)
        {
            if (snapshot->observations[prior].plan_id == observation->plan_id)
            {
                return partial_reject(snapshot, "duplicate_observation");
            }
        }
        snapshot->observation_count++;
        cursor = next + 1;
    }
    snapshot->parsed = true;
    return true;
}
