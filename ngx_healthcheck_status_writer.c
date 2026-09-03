#include "ngx_healthcheck_status_writer.h"


static ngx_int_t ngx_healthcheck_status_writer_add_block(
    ngx_healthcheck_status_writer_t *writer);
static ngx_int_t ngx_healthcheck_status_writer_append_uint(
    ngx_healthcheck_status_writer_t *writer, uint64_t value);


void
ngx_healthcheck_status_writer_init(ngx_healthcheck_status_writer_t *writer,
    ngx_pool_t *pool)
{
    ngx_memzero(writer, sizeof(*writer));
    writer->pool = pool;
}


static ngx_int_t
ngx_healthcheck_status_writer_add_block(
    ngx_healthcheck_status_writer_t *writer)
{
    ngx_buf_t    *buffer;
    ngx_chain_t  *chain;
    u_char       *data;

    chain = ngx_pcalloc(writer->pool, sizeof(ngx_chain_t));
    buffer = ngx_pcalloc(writer->pool, sizeof(ngx_buf_t));
    data = ngx_palloc(writer->pool, NGX_HEALTHCHECK_STATUS_BLOCK_SIZE);

    if (chain == NULL || buffer == NULL || data == NULL) {
        return NGX_ERROR;
    }

    buffer->start = data;
    buffer->pos = data;
    buffer->last = data;
    buffer->end = data + NGX_HEALTHCHECK_STATUS_BLOCK_SIZE;
    buffer->temporary = 1;
    chain->buf = buffer;

    if (writer->tail == NULL) {
        writer->head = chain;
    } else {
        writer->tail->next = chain;
    }

    writer->tail = chain;
    writer->blocks++;

    return NGX_OK;
}


ngx_int_t
ngx_healthcheck_status_writer_append(ngx_healthcheck_status_writer_t *writer,
    const u_char *data, size_t len)
{
    size_t  available, copy;

    while (len != 0) {
        if (writer->tail == NULL
            || writer->tail->buf->last == writer->tail->buf->end)
        {
            if (ngx_healthcheck_status_writer_add_block(writer) != NGX_OK) {
                return NGX_ERROR;
            }
        }

        available = writer->tail->buf->end - writer->tail->buf->last;
        copy = ngx_min(available, len);

        if ((uint64_t) writer->length > INT64_MAX - (uint64_t) copy) {
            return NGX_ERROR;
        }

        writer->tail->buf->last = ngx_cpymem(writer->tail->buf->last, data,
                                             copy);
        writer->length += copy;
        data += copy;
        len -= copy;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_healthcheck_status_writer_append_uint(
    ngx_healthcheck_status_writer_t *writer, uint64_t value)
{
    u_char  number[NGX_INT64_LEN];
    u_char *last;

    last = number + sizeof(number);

    do {
        *--last = (u_char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);

    return ngx_healthcheck_status_writer_append(writer, last,
                                                 number + sizeof(number) - last);
}


ngx_int_t
ngx_healthcheck_status_writer_printf(ngx_healthcheck_status_writer_t *writer,
    const char *fmt, ...)
{
    const char  *literal, *string;
    ngx_str_t   *value;
    va_list      args;
    ngx_int_t    rc;

    va_start(args, fmt);
    rc = NGX_OK;
    literal = fmt;

    while (*fmt != '\0') {
        if (*fmt++ != '%') {
            continue;
        }

        if (fmt - 1 != literal
            && ngx_healthcheck_status_writer_append(writer,
                   (const u_char *) literal, (fmt - 1) - literal) != NGX_OK)
        {
            rc = NGX_ERROR;
            break;
        }

        switch (*fmt) {
        case '%':
            rc = ngx_healthcheck_status_writer_append(writer,
                                                       (u_char *) "%", 1);
            fmt++;
            break;

        case 'V':
            value = va_arg(args, ngx_str_t *);
            rc = ngx_healthcheck_status_writer_append(writer, value->data,
                                                       value->len);
            fmt++;
            break;

        case 's':
            string = va_arg(args, const char *);
            rc = ngx_healthcheck_status_writer_append(writer,
                (const u_char *) string, ngx_strlen(string));
            fmt++;
            break;

        case 'u':
            if (fmt[1] != 'i') {
                rc = NGX_ERROR;
                break;
            }

            rc = ngx_healthcheck_status_writer_append_uint(writer,
                (uint64_t) va_arg(args, ngx_uint_t));
            fmt += 2;
            break;

        case 'M':
        {
            ngx_msec_t milliseconds;

            milliseconds = va_arg(args, ngx_msec_t);
            if ((ngx_msec_int_t) milliseconds == -1) {
                rc = ngx_healthcheck_status_writer_append(writer,
                                                           (u_char *) "-", 1);
                if (rc == NGX_OK) {
                    rc = ngx_healthcheck_status_writer_append_uint(writer, 1);
                }
            } else {
                rc = ngx_healthcheck_status_writer_append_uint(writer,
                    (uint64_t) milliseconds);
            }
            fmt++;
            break;
        }

        default:
            rc = NGX_ERROR;
            break;
        }

        if (rc != NGX_OK) {
            break;
        }

        literal = fmt;
    }

    if (rc == NGX_OK && fmt != literal) {
        rc = ngx_healthcheck_status_writer_append(writer,
            (const u_char *) literal, fmt - literal);
    }

    va_end(args);
    return rc;
}


ngx_int_t
ngx_healthcheck_status_writer_send(ngx_http_request_t *r,
    ngx_healthcheck_status_writer_t *writer)
{
    ngx_int_t  rc;

    if (writer->tail != NULL) {
        writer->tail->buf->last_buf = (r->main == NULL || r == r->main);
        writer->tail->buf->last_in_chain = 1;
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = writer->length;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only
        || r->method == NGX_HTTP_HEAD || writer->head == NULL)
    {
        return rc;
    }

    return ngx_http_output_filter(r, writer->head);
}
