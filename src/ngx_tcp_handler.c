
/*
 * Copyright (C) Ngwsx
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_tcp.h>


#if (NGX_TCP_SSL)
static void ngx_tcp_ssl_init_connection(ngx_ssl_t *ssl, ngx_connection_t *c);
static void ngx_tcp_ssl_handshake_handler(ngx_connection_t *c);
#endif
static void ngx_tcp_init_session(ngx_connection_t *c);
static void ngx_tcp_dummy_handler(ngx_event_t *ev);


void
ngx_tcp_init_connection(ngx_connection_t *c)
{
    ngx_uint_t            i;
    ngx_tcp_port_t       *port;
    struct sockaddr      *sa;
    ngx_tcp_log_ctx_t    *ctx;
    ngx_tcp_session_t    *s;
    ngx_tcp_in_addr_t    *addr;
    struct sockaddr_in   *sin;
    ngx_tcp_addr_conf_t  *addr_conf;
#if (NGX_HAVE_INET6)
    ngx_tcp_in6_addr_t   *addr6;
    struct sockaddr_in6  *sin6;
#endif


    /* find the server configuration for the address:port */

    /* AF_INET only */

    port = c->listening->servers;

    if (port->naddrs > 1) {

        /*
         * There are several addresses on this port and one of them
         * is the "*:port" wildcard so getsockname() is needed to determine
         * the server address.
         *
         * AcceptEx() already gave this address.
         */

        if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
            ngx_tcp_close_connection(c);
            return;
        }

        sa = c->local_sockaddr;

        switch (sa->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) sa;

            addr6 = port->addrs;

            /* the last address is "*" */

            for (i = 0; i < port->naddrs - 1; i++) {
                if (ngx_memcmp(&addr6[i].addr6, &sin6->sin6_addr, 16) == 0) {
                    break;
                }
            }

            addr_conf = &addr6[i].conf;

            break;
#endif

        default: /* AF_INET */
            sin = (struct sockaddr_in *) sa;

            addr = port->addrs;

            /* the last address is "*" */

            for (i = 0; i < port->naddrs - 1; i++) {
                if (addr[i].addr == sin->sin_addr.s_addr) {
                    break;
                }
            }

            addr_conf = &addr[i].conf;

            break;
        }

    } else {
        switch (c->local_sockaddr->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            addr6 = port->addrs;
            addr_conf = &addr6[0].conf;
            break;
#endif

        default: /* AF_INET */
            addr = port->addrs;
            addr_conf = &addr[0].conf;
            break;
        }
    }

    s = ngx_pcalloc(c->pool, sizeof(ngx_tcp_session_t));
    if (s == NULL) {
        ngx_tcp_close_connection(c);
        return;
    }

    s->main_conf = addr_conf->ctx->main_conf;
    s->srv_conf = addr_conf->ctx->srv_conf;

    s->addr_text = &addr_conf->addr_text;

    c->data = s;
    s->connection = c;

    ngx_log_error(NGX_LOG_INFO, c->log, 0, "*%ui client %V connected to %V",
                  c->number, &c->addr_text, s->addr_text);

    ctx = ngx_palloc(c->pool, sizeof(ngx_tcp_log_ctx_t));
    if (ctx == NULL) {
        ngx_tcp_close_connection(c);
        return;
    }

    ctx->client = &c->addr_text;
    ctx->session = s;

    c->log->connection = c->number;
    c->log->handler = ngx_tcp_log_error;
    c->log->data = ctx;
    c->log->action = "initializing connection";

    c->log_error = NGX_ERROR_INFO;

#if (NGX_TCP_SSL)
    {
    ngx_tcp_ssl_conf_t  *sslcf;

    sslcf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_ssl_module);

    if (sslcf->enable) {
        c->log->action = "SSL handshaking";

        ngx_tcp_ssl_init_connection(&sslcf->ssl, c);
        return;
    }

    if (addr_conf->ssl) {

        c->log->action = "SSL handshaking";

        if (sslcf->ssl.ctx == NULL) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "no \"ssl_certificate\" is defined "
                          "in server listening on SSL port");
            ngx_tcp_close_connection(c);
            return;
        }

        ngx_tcp_ssl_init_connection(&sslcf->ssl, c);
        return;
    }

    }
#endif

    ngx_tcp_init_session(c);
}


#if (NGX_TCP_SSL)

void
ngx_tcp_starttls_handler(ngx_event_t *rev)
{
    ngx_connection_t    *c;
    ngx_tcp_session_t   *s;
    ngx_tcp_ssl_conf_t  *sslcf;

    c = rev->data;
    s = c->data;
    s->starttls = 1;

    c->log->action = "in starttls state";

    sslcf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_ssl_module);

    ngx_tcp_ssl_init_connection(&sslcf->ssl, c);
}


static void
ngx_tcp_ssl_init_connection(ngx_ssl_t *ssl, ngx_connection_t *c)
{
    ngx_tcp_session_t        *s;
    ngx_tcp_core_srv_conf_t  *cscf;

    if (ngx_ssl_create_connection(ssl, c, 0) == NGX_ERROR) {
        ngx_tcp_close_connection(c);
        return;
    }

    if (ngx_ssl_handshake(c) == NGX_AGAIN) {

        s = c->data;

        cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

        ngx_add_timer(c->read, cscf->timeout);

        c->ssl->handler = ngx_tcp_ssl_handshake_handler;

        return;
    }

    ngx_tcp_ssl_handshake_handler(c);
}


static void
ngx_tcp_ssl_handshake_handler(ngx_connection_t *c)
{
    ngx_tcp_session_t        *s;
    ngx_tcp_core_srv_conf_t  *cscf;

    if (c->ssl->handshaked) {

        s = c->data;

        if (s->starttls) {
            cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

            c->read->handler = cscf->protocol->init_protocol;
            c->write->handler = ngx_tcp_send;

            cscf->protocol->init_protocol(c->read);

            return;
        }

        c->read->ready = 0;

        ngx_tcp_init_session(c);
        return;
    }

    ngx_tcp_close_connection(c);
}

#endif


static void
ngx_tcp_init_session(ngx_connection_t *c)
{
    ngx_tcp_session_t        *s;
    ngx_tcp_core_srv_conf_t  *cscf;

    c->read->handler = ngx_tcp_dummy_handler;
    c->write->handler = ngx_tcp_dummy_handler;

    c->log->action = "initializing session";

    s = c->data;

    s->ctx = ngx_pcalloc(c->pool, sizeof(void *) * ngx_tcp_max_module);
    if (s->ctx == NULL) {
        ngx_tcp_close_connection(c);
        return;
    }

    cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

    if (cscf->protocol->init_session(s) != NGX_OK) {
        ngx_tcp_close_connection(c);
        return;
    }

    c->log->action = "processing session";

    cscf->protocol->process_session(s);
}


static void
ngx_tcp_dummy_handler(ngx_event_t *ev)
{
    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ev->log, 0, "tcp dummy handler");
}


#if (NGX_TCP_SSL)

ngx_int_t
ngx_tcp_starttls_only(ngx_tcp_session_t *s, ngx_connection_t *c)
{
    ngx_tcp_ssl_conf_t  *sslcf;

    if (c->ssl) {
        return 0;
    }

    sslcf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_ssl_module);

    if (sslcf->starttls == NGX_TCP_STARTTLS_ONLY) {
        return 1;
    }

    return 0;
}

#endif


void
ngx_tcp_internal_server_error(ngx_tcp_session_t *s)
{
    ngx_tcp_core_srv_conf_t  *cscf;

    cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

    if (cscf->protocol->internal_server_error) {
        cscf->protocol->internal_server_error(s);
    }

    ngx_tcp_close_connection(s->connection);
}


void
ngx_tcp_close_connection(ngx_connection_t *c)
{
    ngx_pool_t               *pool;
    ngx_tcp_session_t        *s;
    ngx_tcp_core_srv_conf_t  *cscf;

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, c->log, 0,
                   "close tcp connection: %d", c->fd);

#if (NGX_TCP_SSL)

    if (c->ssl) {
        if (ngx_ssl_shutdown(c) == NGX_AGAIN) {
            c->ssl->handler = ngx_tcp_close_connection;
            return;
        }
    }

#endif

    s = c->data;

    if (s != NULL) {
        c->log->action = "closing session";

        cscf = ngx_tcp_get_module_srv_conf(s, ngx_tcp_core_module);

        if (cscf->protocol->close_session) {
            cscf->protocol->close_session(s);
        }
    }

#if (NGX_STAT_STUB)
    (void) ngx_atomic_fetch_add(ngx_stat_active, -1);
#endif

    c->destroyed = 1;

    pool = c->pool;

    ngx_close_connection(c);

    ngx_destroy_pool(pool);
}


u_char *
ngx_tcp_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    u_char             *p;
    ngx_tcp_session_t  *s;
    ngx_tcp_log_ctx_t  *ctx;

    if (log->action) {
        p = ngx_snprintf(buf, len, " while %s", log->action);
        len -= p - buf;
        buf = p;
    }

    ctx = log->data;

    p = ngx_snprintf(buf, len, ", client: %V", ctx->client);
    len -= p - buf;
    buf = p;

    s = ctx->session;

    if (s == NULL) {
        return p;
    }

    p = ngx_snprintf(buf, len, "%s, server: %V",
                     s->starttls ? " using starttls" : "",
                     s->addr_text);
    len -= p - buf;
    buf = p;

    if (s->proxy == NULL) {
        return p;
    }

    p = ngx_snprintf(buf, len, ", upstream: %V", s->proxy->upstream.name);

    return p;
}
