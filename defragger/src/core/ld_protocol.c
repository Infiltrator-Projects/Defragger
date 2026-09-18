// SPDX-License-Identifier: GPL-3.0-or-later
#include "ld_protocol.h"

#include "ld_runtime.h"

#include "infiltratr/escape.h"

#include <stdlib.h>

static char *escape_json_text(const char *text)
{
    size_t required = 0U;
    if (!infiltratr_escape_json(text, NULL, 0U, &required) || required == 0U)
        return NULL;
    char *escaped = ld_xmalloc(required);
    if (!infiltratr_escape_json(text, escaped, required, NULL)) {
        free(escaped);
        return NULL;
    }
    return escaped;
}

int ld_emit_result_event(FILE *stream, const char *operation,
                         const char *status, const char *message)
{
    if (stream == NULL || operation == NULL || status == NULL) return -1;
    if (message == NULL) message = "";

    char *escaped_operation = escape_json_text(operation);
    char *escaped_status = escape_json_text(status);
    char *escaped_message = escape_json_text(message);
    if (escaped_operation == NULL || escaped_status == NULL ||
        escaped_message == NULL) {
        free(escaped_operation);
        free(escaped_status);
        free(escaped_message);
        return -1;
    }

    const int written = fprintf(
        stream,
        "@@RESULT {\"operation\":\"%s\",\"status\":\"%s\",\"message\":\"%s\"}\n",
        escaped_operation, escaped_status, escaped_message);
    const int flushed = fflush(stream);

    free(escaped_operation);
    free(escaped_status);
    free(escaped_message);
    return written < 0 || flushed != 0 ? -1 : 0;
}
