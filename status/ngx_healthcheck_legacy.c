/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 * Copyright (C) 2010-2014 Weibin Yao (yaoweibin@gmail.com)
 * Copyright (C) 2010-2014 Alibaba Group Holding Limited
 */

#include "ngx_healthcheck_snapshot.h"

ngx_int_t
ngx_healthcheck_legacy_output(ngx_healthcheck_status_writer_t *writer,
    ngx_healthcheck_snapshot_t *snapshot, ngx_uint_t format)
{
    ngx_healthcheck_snapshot_peer_t *peer = snapshot->peers.elts;
    ngx_uint_t                      i, count = snapshot->peers.nelts;

    if (format == 0) {
        if (ngx_healthcheck_status_writer_printf(writer,
                "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\n"
                "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
                "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
                "<head>\n"
                "  <title>Nginx http upstream check status</title>\n"
                "</head>\n"
                "<body>\n"
                "<h1>Nginx http upstream check status</h1>\n"
                "<h2>Check upstream server number: %ui, generation: %ui</h2>\n"
                "<table style=\"background-color:white\" cellspacing=\"0\" "
                "       cellpadding=\"3\" border=\"1\">\n"
                "  <tr bgcolor=\"#C0C0C0\">\n"
                "    <th>Index</th>\n"
                "    <th>Upstream</th>\n"
                "    <th>Name</th>\n"
                "    <th>Status</th>\n"
                "    <th>Rise counts</th>\n"
                "    <th>Fall counts</th>\n"
                "    <th>Check type</th>\n"
                "    <th>Check port</th>\n"
                "  </tr>\n", count, snapshot->generation) != NGX_OK)
        {
            return NGX_ERROR;
        }
    } else if (format == 2) {
        if (ngx_healthcheck_status_writer_printf(writer,
                "{\"servers\": {\n"
                "  \"total\": %ui,\n"
                "  \"generation\": %ui,\n"
                "  \"server\": [\n", count, snapshot->generation) != NGX_OK)
        {
            return NGX_ERROR;
        }
    }
    for (i = 0; i < count; i++) {
        if (format == 0) {
            if (ngx_healthcheck_status_writer_printf(writer,
                    "  <tr%s>\n"
                    "    <td>%ui</td>\n"
                    "    <td>%V</td>\n"
                    "    <td>%V</td>\n"
                    "    <td>%s</td>\n"
                    "    <td>%ui</td>\n"
                    "    <td>%ui</td>\n"
                    "    <td>%V</td>\n"
                    "    <td>%ui</td>\n"
                    "  </tr>\n",
                    peer[i].health.down ? " bgcolor=\"#FF0000\"" : "",
                    peer[i].index, peer[i].upstream_name, &peer[i].peer_addr->name,
                    peer[i].health.down ? "down" : "up", peer[i].health.rise_count,
                    peer[i].health.fall_count, &peer[i].conf->check_type_conf->name,
                    peer[i].conf->port) != NGX_OK)
            {
                return NGX_ERROR;
            }
        } else if (format == 1) {
            if (ngx_healthcheck_status_writer_printf(writer,
                    "%ui,%V,%V,%s,%ui,%ui,%V,%ui\n",
                    peer[i].index, peer[i].upstream_name, &peer[i].peer_addr->name,
                    peer[i].health.down ? "down" : "up", peer[i].health.rise_count,
                    peer[i].health.fall_count, &peer[i].conf->check_type_conf->name,
                    peer[i].conf->port) != NGX_OK)
            {
                return NGX_ERROR;
            }
        } else {
            if (ngx_healthcheck_status_writer_printf(writer,
                    "    {\"index\": %ui, "
                    "\"upstream\": \"%V\", "
                    "\"name\": \"%V\", "
                    "\"status\": \"%s\", "
                    "\"rise\": %ui, "
                    "\"fall\": %ui, "
                    "\"type\": \"%V\", "
                    "\"port\": %ui}%s\n",
                    peer[i].index, peer[i].upstream_name, &peer[i].peer_addr->name,
                    peer[i].health.down ? "down" : "up", peer[i].health.rise_count,
                    peer[i].health.fall_count, &peer[i].conf->check_type_conf->name,
                    peer[i].conf->port, i + 1 == count ? "" : ",") != NGX_OK)
            {
                return NGX_ERROR;
            }
        }
    }
    if (format == 0) {
        return ngx_healthcheck_status_writer_printf(writer,
                                                    "</table>\n</body>\n</html>\n");
    }
    if (format == 2) {
        return ngx_healthcheck_status_writer_printf(writer, "  ]\n}}\n");
    }
    return NGX_OK;
}
