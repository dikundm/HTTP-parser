/*
 *  HTTP parser internals.
 *  Based on http parser API from Node.js project. 
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nodejs_http_parser/http_parser.h"
#include "parser.h"

#include "../zlib/zlib.h"

/*
 *  Debug helpers:
 */
#define DEBUG 0

#if !DEBUG
#   define DBG_PARSER_ERROR
#   define DBG_HTTP_CALLBACK
#   define DBG_HTTP_CALLBACK_DATA
#   define DBG_PARSER_TYPE
#else
#   define DBG_PARSER_ERROR                                                 \
        printf ("DEBUG: Parser error: %s\n",                                \
                http_errno_name(parser->http_errno));

#   define DBG_HTTP_CALLBACK                                                \
        printf ("DEBUG: %s\n", __FUNCTION__);

#   define DBG_HTTP_CALLBACK_DATA                                           \
        printf ("DEBUG: %s: ", __FUNCTION__);                               \
        char *out = malloc ((length + 1) * sizeof(char));                   \
        memset (out, 0, length + 1);                                        \
        memcpy (out, at, length);                                           \
        printf("%s\n", out);                                                \
        free(out); 

#   define DBG_PARSER_TYPE                                                  \
        printf ("DEBUG: Parser type: %d\n", parser->type);

#   define DBG_PARSER_METHOD                                                \
        printf ("DEBUG: Parser type: %d\n", parser->method);
#endif

/*
 *  Goods:
 */
#define MIN(a,b) a < b ? a : b
#define MAX(a,b) a > b ? a : b

#define CREATE_HTTP_HEADER(header)                                          \
    header = malloc(sizeof(http_header));                                   \
    memset(header, 0, sizeof(http_header));

#define CREATE_HTTP_MESSAGE(message)                                        \
    message = malloc(sizeof(http_message));                                 \
    memset(message, 0, sizeof(http_message));                               \
    CREATE_HTTP_HEADER(message->header);

#define CREATE_HTTP_MESSAGE_NO_HEADER(message)                              \
    message = malloc(sizeof(http_message));                                 \
    memset(message, 0, sizeof(http_message));

#define DESTROY_HTTP_HEADER(header)                                         \
    if (header->url) free(header->url);                                     \
    if (header->status) free(header->status);                               \
    if (header->method) free(header->method);                               \
    for (int i = 0; i < header->paramc; i++) {                              \
        if (header->paramv[i].field) free(header->paramv[i].field);         \
        if (header->paramv[i].value) free(header->paramv[i].value);         \
    }                                                                       \
    if (header->paramv) free(header->paramv);                               \
    free (header);                                                          \
    header=NULL;

#define DESTROY_HTTP_MESSAGE(message)                                       \
    if (message) {                                                          \
        DESTROY_HTTP_HEADER(message->header);                               \
        if (message->body) free(message->body);                             \
        message->body = NULL;                                               \
        if (message->chunkv) free(message->chunkv);                         \
        free(message);                                                      \
        message = NULL;                                                     \
    }

#define APPEND_HTTP_HEADER_PARAM(paramc, paramv)                            \
    paramc++;                                                               \
    if (paramv == NULL) {                                                   \
        paramv = malloc(sizeof(http_header_parameter));                     \
        memset(paramv, 0, sizeof(http_header_parameter));                   \
    } else {                                                                \
        paramv = realloc(paramv, paramc * sizeof(http_header_parameter));   \
        memset(&(paramv[paramc - 1]), 0, sizeof(http_header_parameter));    \
    }

#define APPEND_HTTP_CHUNK(chunkc, chunkv, value)                            \
    chunkc++;                                                               \
    if (chunkv == NULL) {                                                   \
        chunkv = malloc(sizeof(size_t));                              \
    } else {                                                                \
        chunkv = realloc(chunkv, chunkc * sizeof(size_t));            \
    }                                                                       \
    chunkv[chunkc - 1] = value;

#define APPEND_CHARS(dst, src, len)                                         \
    size_t old_len;                                                         \
    if (dst == NULL) {                                                      \
        dst = malloc ((len + 1) * sizeof(char));                            \
        memset(dst, 0 , len + 1);                                           \
        memcpy(dst, src, len);                                              \
    } else {                                                                \
        old_len = strlen(dst);                                              \
        dst = realloc (dst, (old_len + len + 1) * sizeof(char));            \
        memset(dst + old_len, 0, len + 1);                                  \
        memcpy(dst + old_len, src, len);                                    \
    }

#define SET_CHARS(dst, src, len)                                            \
    if (dst == NULL) {                                                      \
        dst = malloc ((len + 1) * sizeof(char));                            \
        memset(dst, 0 , len + 1);                                           \
        memcpy(dst, src, len);                                              \
    }


/*
 *  Private stuff:
 */
typedef struct {
    connection_id           id;
    connection_info         *info;
    parser_callbacks        *callbacks;
    http_parser             *parser;
    http_parser_settings    *settings;
    http_message            *message;
    size_t                  done;
    unsigned int            in_field;
    unsigned int            have_body;
    unsigned int            body_started;
    unsigned int            need_decode;
    unsigned int            need_decompress;
} connection_context;


/*
 *  Node.js http_parser's callbacks (parser->settings):
 */
/* For in-callback using only! */
#define CONTEXT         ((connection_context*)parser->data)
#define ID              (CONTEXT->id)
#define CALLBACKS       (CONTEXT->callbacks)
#define IN_FIELD        (CONTEXT->in_field)
#define HAVE_BODY       (CONTEXT->have_body)
#define BODY_STARTED    (CONTEXT->body_started)
#define NEED_DECODE     (CONTEXT->need_decode)
#define NEED_DECOMPRESS (CONTEXT->need_decompress)

#define MESSAGE         (CONTEXT->message)
#define HEADER          (MESSAGE->header)
#define BODY            (MESSAGE->body)
#define BODY_LENGTH     (MESSAGE->body_length)
#define CHUNKC          (MESSAGE->chunkc)
#define CHUNKV          (MESSAGE->chunkv)
#define METHOD          (HEADER->method)
#define URL             (HEADER->url)
#define STATUS          (HEADER->status)
#define STATUS_CODE     (HEADER->status_code)
#define PARAMC          (HEADER->paramc)
#define PARAMV          (HEADER->paramv)


/*
 *  Internal callbacks:
 */
int _on_message_begin(http_parser *parser) {
    DBG_HTTP_CALLBACK
    CREATE_HTTP_MESSAGE (MESSAGE);
    return 0;
}

int _on_url(http_parser *parser, const char *at, size_t length) {
    DBG_HTTP_CALLBACK_DATA
    if (at != NULL && length > 0) {
        APPEND_CHARS(URL, at, length);
    }
    return 0;
}

int _on_status(http_parser *parser, const char *at, size_t length) {
    DBG_HTTP_CALLBACK_DATA
    if (at != NULL && length > 0) {
        APPEND_CHARS(STATUS, at, length);
        STATUS_CODE = parser->status_code;
    }
    return 0;
}

int _on_header_field(http_parser *parser, const char *at, size_t length) {
    DBG_HTTP_CALLBACK
    if (at != NULL && length > 0) {
        if (!IN_FIELD) {
            IN_FIELD = 1;
            APPEND_HTTP_HEADER_PARAM(PARAMC, PARAMV);
        }
        APPEND_CHARS(PARAMV[PARAMC - 1].field, at, length);
    }
    return 0;
}

int _on_header_value(http_parser *parser, const char *at, size_t length) {
    DBG_HTTP_CALLBACK
    IN_FIELD = 0;
    if (at != NULL && length > 0) {
        APPEND_CHARS(PARAMV[PARAMC - 1].value, at, length);
    }
    return 0;
}

int _on_headers_complete(http_parser *parser) {
    DBG_HTTP_CALLBACK
    int skip = 0;
    switch (parser->type) {
        case HTTP_REQUEST:
            SET_CHARS (METHOD, http_method_str(parser->method), 
                       strlen(http_method_str(parser->method)));
            skip = CALLBACKS->http_request_received(ID, MESSAGE, 
                                                    sizeof(MESSAGE));
            break;
        case HTTP_RESPONSE:
            skip = CALLBACKS->http_response_received(ID, MESSAGE, 
                                                     sizeof(MESSAGE));
            break;
        default:
            break;
    }

    return skip;
}

int _on_body(http_parser *parser, const char *at, size_t length) {
    DBG_HTTP_CALLBACK_DATA
    HAVE_BODY = 1;
    APPEND_CHARS(BODY, at, length);
    BODY_LENGTH+=length;
    switch (parser->type) {
        case HTTP_REQUEST:
            if (BODY_STARTED == 0) {
                CALLBACKS->http_request_body_started(ID, NULL, 0);
            }
            CALLBACKS->http_request_body_data(ID, NULL, 0);
            break;
        case HTTP_RESPONSE:
            if (BODY_STARTED == 0) {
                CALLBACKS->http_response_body_started(ID, NULL, 0);
            }
            CALLBACKS->http_response_body_data(ID, NULL, 0);
            break;
        default:
            break;
    }

    BODY_STARTED = 1;
    return 0;
}

int _on_message_complete(http_parser *parser) {
    DBG_HTTP_CALLBACK
    if (HAVE_BODY) {
        switch (parser->type) {
            case HTTP_REQUEST:
                CALLBACKS->http_request_body_finished(ID, MESSAGE,
                                                      sizeof(MESSAGE));
                break;
            case HTTP_RESPONSE:
                CALLBACKS->http_response_body_finished(ID, MESSAGE,
                                                       sizeof(MESSAGE));
                break;
            default:
                break;
        }
    }

    /* Re-init parser before next message. */
    http_parser_init(parser, HTTP_BOTH);

    DESTROY_HTTP_MESSAGE(MESSAGE);
    HAVE_BODY = 0;
    BODY_STARTED = 0;
    NEED_DECODE = 0;
    NEED_DECOMPRESS = 0;

    return 0;
}

int _on_chunk_header(http_parser *parser) {
    DBG_HTTP_CALLBACK
    APPEND_HTTP_CHUNK(CHUNKC, CHUNKV, parser->content_length);
    if (BODY_STARTED == 0) {
        switch (parser->type) {
            case HTTP_REQUEST:
                NEED_DECODE = 
                    CALLBACKS->http_request_body_started(ID, NULL, 0);
                break;
            case HTTP_RESPONSE:
                NEED_DECODE = 
                    CALLBACKS->http_response_body_started(ID, NULL, 0);
                break;
            default:
                break;
        }
        BODY_STARTED = 1;
    }
    return 0;
}

int _on_chunk_complete(http_parser *parser) {
    DBG_HTTP_CALLBACK
    switch (parser->type) {
        case HTTP_REQUEST:
            CALLBACKS->http_request_body_data(ID, NULL, 0);
            break;
        case HTTP_RESPONSE:
            CALLBACKS->http_response_body_data(ID, NULL, 0);
            break;
        default:
            break;
    }
    return 0;
}


http_parser_settings _settings = {
    .on_message_begin       = _on_message_begin,
    .on_url                 = _on_url,
    .on_status              = _on_status,
    .on_header_field        = _on_header_field,
    .on_header_value        = _on_header_value,
    .on_headers_complete    = _on_headers_complete,
    .on_body                = _on_body,
    .on_message_complete    = _on_message_complete,
    .on_chunk_header        = _on_chunk_header,
    .on_chunk_complete      = _on_chunk_complete
};

connection_context *context = NULL;

/*
 *  API implementaion:
 */
int parser_connect(connection_id id, connection_info *info,
            parser_callbacks *callbacks) {
    context = malloc(sizeof(connection_context));
    memset (context, 0, sizeof(connection_context));
    
    context->id = id;
    context->info = info;
    context->callbacks = callbacks;

    context->settings = &_settings;
    context->parser = malloc(sizeof(http_parser));

    context->parser->data = context;
    
    http_parser_init(context->parser, HTTP_BOTH);

    return 0;
}

int parser_disconnect(connection_id id, transfer_direction direction) {
    return 0;
}

#define INPUT_LENGTH_AT_ERROR 1

int parser_input(connection_id id, transfer_direction direction, const char *data,
          size_t length) {
    length = strnlen(data, length);
    context->done = 0;

    if (HTTP_PARSER_ERRNO(context->parser) != HPE_OK) {
        http_parser_init(context->parser, HTTP_BOTH);    
    }

    context->done = http_parser_execute (context->parser, context->settings,
                                         data, length);

    while (context->done < length)
    {
        if (HTTP_PARSER_ERRNO(context->parser) != HPE_OK) {
            http_parser_init(context->parser, HTTP_BOTH);
        }
        http_parser_execute (context->parser, context->settings,
                             data + context->done, INPUT_LENGTH_AT_ERROR);
        context->done+=INPUT_LENGTH_AT_ERROR;
    }

    return 0;
}

int parser_connection_close(connection_id id) {
    return 0;
}

/*
 *  Utility methods definition:
 */
http_message *http_message_struct(void) {
    http_message *message;
    CREATE_HTTP_MESSAGE(message);
    return message; 
}

http_header *http_header_clone(const http_header *source) {
    http_header *header;
    CREATE_HTTP_HEADER(header);
    if (source->url != NULL)
        SET_CHARS(header->url, source->url, strlen(source->url));
    if (source->status != NULL)
        SET_CHARS(header->status, source->status, strlen(source->status));
    if (source->method != NULL)
        SET_CHARS(header->method, source->method, strlen(source->method));
    header->status_code = source->status_code;
    for (int i = 0; i < source->paramc; i++) {
        APPEND_HTTP_HEADER_PARAM(header->paramc, header->paramv);
        SET_CHARS(header->paramv[i].field, source->paramv[i].field,
                     strlen(source->paramv[i].field));
        SET_CHARS(header->paramv[i].value, source->paramv[i].value,
                     strlen(source->paramv[i].value));
    }
    return header;
}

http_message *http_message_clone(const http_message *source) {
    http_message *message;
    CREATE_HTTP_MESSAGE_NO_HEADER(message);
    message->header = http_header_clone(source->header);
    if (source->body) {
        SET_CHARS(message->body, source->body, source->body_length);
        message->body_length = source->body_length;
    }
    if (source->chunkc) {
        message->chunkc = source->chunkc;
        message->chunkv = malloc(message->chunkc * sizeof(size_t));
        memcpy (message->chunkv, source->chunkv,
                message->chunkc * sizeof(size_t));
    }
    return message;
}

int http_message_set_method(http_message *message,
                            const char *method, size_t length) {
    if (message == NULL || method == NULL) return 1;
    if (message->header == NULL) return 1;
    if (message->header->method) free(message->header->method);
    SET_CHARS(message->header->method, method, length);
    return 0;
}

int http_message_set_url(http_message *message,
                         const char *url, size_t length) {
    if (message == NULL || url == NULL) return 1;
    if (message->header == NULL) return 1;
    if (message->header->url) free(message->header->url);
    SET_CHARS(message->header->url, url, length);
    return 0;
}

int http_message_set_status(http_message *message,
                            const char *status, size_t length) {
    if (message == NULL || status == NULL) return 1;
    if (message->header == NULL) return 1;
    if (message->header->status) free(message->header->status);
    SET_CHARS(message->header->status, status, length);
    return 0;
}

int http_message_set_status_code(http_message *message, int status_code) {
    if (message == NULL) return 1;
    if (message->header == NULL) return 1;
    message->header->status_code = status_code;
    return 0;
}

int http_message_set_field(http_message *message, 
                           const char *field, size_t f_length,
                           const char *value, size_t v_length) {
    if (message == NULL || field == NULL || f_length == 0 ||
        value == NULL || v_length == 0 ) return 1;
    if (message->header == NULL) return 1;
    for (int i = 0; i < message->header->paramc; i++) {
        if (strncmp(message->header->paramv[i].field, field, f_length) == 0) {
            if (message->header->paramv[i].value != NULL) {
                free (message->header->paramv[i].value);
                message->header->paramv[i].value = NULL;
            }
            SET_CHARS(message->header->paramv[i].value, value, v_length);
            return 0;
        }
    }
    return  1;
}

char *http_message_get_field(const http_message *message,
                             const char *field, size_t length) {
    char *value;
    if (message == NULL || field == NULL || length == 0) return NULL;
    if (message->header == NULL) return NULL;
    for (int i = 0; i < message->header->paramc; i++) {
        if (strncmp(message->header->paramv[i].field, field, length) == 0) {
            SET_CHARS(value, message->header->paramv[i].value,
                      strlen(message->header->paramv[i].value));
            return value;
        }
    }
    return NULL;
}

int http_message_add_field(http_message *message,
                           const char *field, size_t length) {
    if (message == NULL || field == NULL || length == 0) return 1;
    if (message->header == NULL) return 1;
    for (int i = 0; i < message->header->paramc; i++) {
        if (strncmp(message->header->paramv[i].field, field, length) == 0)
            return 1;
    }
    APPEND_HTTP_HEADER_PARAM(message->header->paramc, message->header->paramv);
    APPEND_CHARS(message->header->paramv[message->header->paramc - 1].field,
                 field, length);
    return 0;
}

int http_message_del_field(http_message *message,
                           const char *field, size_t length) {
    if (message == NULL || field == NULL || length == 0) return 1;
    if (message->header == NULL) return 1;
    for (int i = 0; i < message->header->paramc; i++) {
        if (strncmp(message->header->paramv[i].field, field, length) == 0) {
            free (message->header->paramv[i].field);
            free (message->header->paramv[i].value);
            for (int j = i + 1; j < message->header->paramc; j++) {
                message->header->paramv[j -1 ].field = 
                    message->header->paramv[j].field;
                message->header->paramv[j -1 ].value = 
                    message->header->paramv[j].value;
            }
            message->header->paramc--;
            message->header->paramv = 
                realloc(message->header->paramv,
                        message->header->paramc * sizeof(http_header_parameter));
            return 0;
        }
    }
    return 1;
}

#define HEX_ULONG_CSTRING_SIZE 17

char *http_message_raw(const http_message *message) {
    char *out_buffer, chunk_length_hex[HEX_ULONG_CSTRING_SIZE], *chunk;
    int length, line_length, chunk_offset = 0;
    
    if (message == NULL) return NULL;
    if (message->header == NULL) return NULL;

    if (message->header->status && message->header->status_code) {
        length = strlen(HTTP_VERSION) + strlen(message->header->status) + 7;
        out_buffer = malloc((length + 1)* sizeof(char));
        memset (out_buffer, 0, (length + 1) * sizeof(char));
        sprintf(out_buffer, "%s %d %s\r\n",
                HTTP_VERSION, message->header->status_code,
                message->header->status);
    } else {
        length = strlen(HTTP_VERSION) + strlen(message->header->url) + 
                 strlen(message->header->method) + 4;
        out_buffer = malloc((length + 1)* sizeof(char));
        memset (out_buffer, 0, (length + 1) * sizeof(char));
        sprintf(out_buffer, "%s %s %s\r\n",
                message->header->method, message->header->url, HTTP_VERSION);
    }

    for (int i = 0; i < message->header->paramc; i++) {
        line_length = strlen(message->header->paramv[i].field) + 
                      strlen(message->header->paramv[i].value) + 4;
        out_buffer = realloc (out_buffer,
                              (length + line_length + 1) * sizeof(char));
        sprintf (out_buffer + length, "%s: %s\r\n", 
                 message->header->paramv[i].field,
                 message->header->paramv[i].value);
        length += line_length;
    }

    out_buffer = realloc (out_buffer, (length + 3) * sizeof(char));
    sprintf (out_buffer + length, "\r\n");
    length += 2;

    if (!message->chunkv && !message->chunkc) {
        line_length = message->body_length + 4;
        out_buffer = realloc (out_buffer,
                              (length + line_length + 1) * sizeof(char));
        sprintf (out_buffer + length, "%s\r\n\r\n", message->body);
        length += line_length;
    } else {
        for (int i = 0; i < message->chunkc; i++) {
            memset(chunk_length_hex, 0, HEX_ULONG_CSTRING_SIZE * sizeof(char));
            sprintf(chunk_length_hex, "%lX", message->chunkv[i]);

            chunk = malloc((message->chunkv[i] + 1) * sizeof(char));
            memset(chunk, 0, message->chunkv[i] + 1);
            memcpy(chunk, message->body + chunk_offset, 
                   message->chunkv[i] * sizeof(char));

            line_length = strlen(chunk_length_hex) + message->chunkv[i] + 4;
            out_buffer = realloc (out_buffer,
                                  (length + line_length + 1) * sizeof(char));
            sprintf (out_buffer + length, "%s\r\n%s\r\n",
                     chunk_length_hex, chunk);

            chunk_offset += message->chunkv[i];
            length += line_length;
        }
    }

    return out_buffer;
} 


int http_message_decompress(http_message *message) {
    int uncompress_result;
    if (message == NULL) return 1;
    if (message->body == NULL || message->body_length == 0) return 1;
    if (message->dec_body != NULL) free(message->dec_body);
    
    message->dec_body_length = 0;
    message->dec_body = malloc (sizeof(char));
    uncompress_result = uncompress(message->dec_body,
                                   &(message->dec_body_length),
                                   message->body, message->body_length);

    if (uncompress_result == Z_BUF_ERROR) {
        message->dec_body = 
            realloc (message->dec_body,
                     message->dec_body_length + 1);
            message->dec_body[message->dec_body_length] = 0;
            uncompress_result = uncompress(message->dec_body,
                                           &(message->dec_body_length),
                                           message->body, message->body_length);
            if (uncompress_result == Z_OK) return 0;
    }
    return 1;
}