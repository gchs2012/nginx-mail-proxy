
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>
#include <ngx_mail.h>


typedef struct {
    ngx_flag_t  enable;
    ngx_flag_t  pass_error_message;
    ngx_flag_t  xclient;
    size_t      buffer_size;
    ngx_msec_t  timeout;
} ngx_mail_proxy_conf_t;


static void ngx_mail_proxy_block_read(ngx_event_t *rev);
static void ngx_mail_proxy_pop3_handler(ngx_event_t *rev);
static void ngx_mail_proxy_imap_handler(ngx_event_t *rev);
static void ngx_mail_proxy_smtp_handler(ngx_event_t *rev);
static void ngx_mail_proxy_dummy_handler(ngx_event_t *ev);
static ngx_int_t ngx_mail_proxy_read_response(ngx_mail_session_t *s,
    ngx_uint_t state);
static void ngx_mail_proxy_handler(ngx_event_t *ev);
static void ngx_mail_proxy_upstream_error(ngx_mail_session_t *s);
static void ngx_mail_proxy_internal_server_error(ngx_mail_session_t *s);
static void ngx_mail_proxy_close_session(ngx_mail_session_t *s);
static void *ngx_mail_proxy_create_conf(ngx_conf_t *cf);
static char *ngx_mail_proxy_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);


static ngx_command_t  ngx_mail_proxy_commands[] = {

    { ngx_string("proxy"),
      NGX_MAIL_MAIN_CONF|NGX_MAIL_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_MAIL_SRV_CONF_OFFSET,
      offsetof(ngx_mail_proxy_conf_t, enable),
      NULL },

    { ngx_string("proxy_buffer"),
      NGX_MAIL_MAIN_CONF|NGX_MAIL_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_MAIL_SRV_CONF_OFFSET,
      offsetof(ngx_mail_proxy_conf_t, buffer_size),
      NULL },

    { ngx_string("proxy_timeout"),
      NGX_MAIL_MAIN_CONF|NGX_MAIL_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_MAIL_SRV_CONF_OFFSET,
      offsetof(ngx_mail_proxy_conf_t, timeout),
      NULL },

    { ngx_string("proxy_pass_error_message"),
      NGX_MAIL_MAIN_CONF|NGX_MAIL_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_MAIL_SRV_CONF_OFFSET,
      offsetof(ngx_mail_proxy_conf_t, pass_error_message),
      NULL },

    { ngx_string("xclient"),
      NGX_MAIL_MAIN_CONF|NGX_MAIL_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_MAIL_SRV_CONF_OFFSET,
      offsetof(ngx_mail_proxy_conf_t, xclient),
      NULL },

      ngx_null_command
};


static ngx_mail_module_t  ngx_mail_proxy_module_ctx = {
    NULL,                                  /* protocol */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    ngx_mail_proxy_create_conf,            /* create server configuration */
    ngx_mail_proxy_merge_conf              /* merge server configuration */
};


ngx_module_t  ngx_mail_proxy_module = {
    NGX_MODULE_V1,
    &ngx_mail_proxy_module_ctx,            /* module context */
    ngx_mail_proxy_commands,               /* module directives */
    NGX_MAIL_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static u_char  smtp_auth_ok[] = "235 2.0.0 OK" CRLF;


/*****************************************************************************
    函 数 名 : ngx_mail_proxy_protocol_handler
    功能描述 : 根据用户请求的协议设置实际的邮件认证处理函数
    输入参数 : ngx_mail_session_t *s
               ngx_connection_t *c
    输出参数 : 无
    返 回 值 : 无
    作    者 : zc
    日    期 : 2018年9月26日
*****************************************************************************/
static void
ngx_mail_proxy_protocol_handler(ngx_mail_session_t *s,
    ngx_connection_t *c)
{
    switch (s->protocol) {

    case NGX_MAIL_POP3_PROTOCOL:
        c->read->handler = ngx_mail_proxy_pop3_handler;
        s->mail_state = ngx_pop3_start;
        break;

    case NGX_MAIL_IMAP_PROTOCOL:
        c->read->handler = ngx_mail_proxy_imap_handler;
        s->mail_state = ngx_imap_start;
        break;

    default: /* NGX_MAIL_SMTP_PROTOCOL */
        c->read->handler = ngx_mail_proxy_smtp_handler;
        if (s->proxy_starttls == ngx_smtp_proxy_starttls_finish) {
            s->mail_state = ngx_smtp_helo;

        } else {
            s->mail_state = ngx_smtp_start;
        }
        break;
    }
}


#if (NGX_MAIL_SSL)
/*****************************************************************************
    函 数 名 : ngx_mail_proxy_ssl_handshake_handler
    功能描述 : nginx与上游邮件服务器SSL握手
    输入参数 : ngx_connection_t *c
    输出参数 : 无
    返 回 值 : 无
    作    者 : zc
    日    期 : 2018年9月26日
*****************************************************************************/
static void
ngx_mail_proxy_ssl_handshake_handler(ngx_connection_t *c)
{
    u_char                    *p;
    ngx_str_t                  line;
    ngx_mail_session_t        *s;
    ngx_mail_core_srv_conf_t  *cscf;

    s = c->data;

    cscf = ngx_mail_get_module_srv_conf(s, ngx_mail_core_module);

    if (c->ssl->handshaked) {
        c->write->handler = ngx_mail_proxy_dummy_handler;

        /* STARTTLS方式且协议为SMTP */
        if (cscf->mail_proxy_starttls &&
            s->protocol == NGX_MAIL_SMTP_PROTOCOL) {

            /* STARTTLS交互完成后，需要向上游发送EHLO命令 */
            line.len = sizeof("HELO ")	- 1 + s->smtp_helo.len + 2;
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }

            p = ngx_cpymem(line.data,
                           (s->proxy_esmtp ? "EHLO " : "HELO "),
                           sizeof("HELO ") - 1);
            p = ngx_cpymem(p, s->smtp_helo.data, s->smtp_helo.len);
            *p++ = CR; *p = LF;

            if (c->send(c, line.data, line.len) < (ssize_t) line.len) {
                /*
                 * we treat the incomplete sending as NGX_ERROR
                 * because it is very strange here
                 */
                ngx_mail_proxy_internal_server_error(s);
                return;
            }
        }

        /* 根据用户请求的协议设置实际的邮件认证处理函数 */
        ngx_mail_proxy_protocol_handler(s, s->proxy->upstream.connection);
    }
}


/*****************************************************************************
    函 数 名 : ngx_mail_proxy_ssl_init_connection
    功能描述 : nginx与上游邮件服务器连接SSL初始化
    输入参数 : ngx_mail_session_t *s
    输出参数 : 无
    返 回 值 : ngx_int_t
    作    者 : zc
    日    期 : 2018年9月26日
*****************************************************************************/
static ngx_int_t
ngx_mail_proxy_ssl_init_connection(ngx_mail_session_t *s)
{
    ngx_int_t          rc;
    ngx_connection_t  *c = s->proxy->upstream.connection;

    /* 创建SSL连接 */
    if (ngx_ssl_create_connection(s->proxy->ssl, c,
                                  NGX_SSL_BUFFER|NGX_SSL_CLIENT)
                                  != NGX_OK) {
        if (c->ssl) {
            if (ngx_ssl_shutdown(c) == NGX_AGAIN) {
                c->ssl->handler = ngx_mail_close_connection;
            }
        }
        return NGX_ERROR;
    }

    /* SSL握手 */
    rc = ngx_ssl_handshake(c);
    if (rc == NGX_AGAIN) {
        c->ssl->handler = ngx_mail_proxy_ssl_handshake_handler;
    }

    ngx_mail_proxy_ssl_handshake_handler(c);

    return NGX_OK;	
}


/*****************************************************************************
    函 数 名 : ngx_mail_proxy_ssl_set
    功能描述 : 设置加密方法
    输入参数 : ngx_mail_session_t *s
               ngx_mail_core_srv_conf_t *cscf
    输出参数 : 无
    返 回 值 : ngx_int_t
    作    者 : zc
    日    期 : 2018年9月26日
*****************************************************************************/
static ngx_int_t
ngx_mail_proxy_ssl_set(ngx_mail_session_t *s,
    ngx_mail_core_srv_conf_t *cscf)
{
    ngx_pool_cleanup_t    *cln;
    ngx_mail_proxy_ctx_t  *p = s->proxy;

    p->ssl = ngx_pcalloc(s->connection->pool, sizeof(ngx_ssl_t));
    if (p->ssl == NULL) {
        return NGX_ERROR;
    }

    p->ssl->log = s->connection->log;

    if (ngx_ssl_create(p->ssl,
                       cscf->mail_proxy_ssl_protocols,
                       NULL) != NGX_OK) {
        goto failed;
    }

    if (SSL_CTX_set_cipher_list(p->ssl->ctx,
                               (const char *) cscf->mail_proxy_ssl_ciphers.data)
                               == 0) {
        ngx_ssl_error(NGX_LOG_ERR, s->connection->log, 0,
                      "SSL_CTX_set_cipher_list(\"%V\") failed",
                      &cscf->mail_proxy_ssl_ciphers);
        goto failed;
    }

    cln = ngx_pool_cleanup_add(s->connection->pool, 0);
    if (cln == NULL) {
        goto failed;
    }

    cln->handler = ngx_ssl_cleanup_ctx;
    cln->data = p->ssl;

    return NGX_OK;

failed:
    if (p->ssl->ctx) {
        ngx_ssl_cleanup_ctx(p->ssl->ctx);
    }
    ngx_pfree(s->connection->pool, p->ssl);

    return NGX_ERROR;
}


/*****************************************************************************
    函 数 名 : ngx_mail_proxy_ssl_connection
    功能描述 : 采用ssl方式与上游建立连接
    输入参数 : ngx_mail_session_t *s
    输出参数 : 无
    返 回 值 : ngx_int_t
    作    者 : zc
    日    期 : 2018年9月26日
*****************************************************************************/
static ngx_int_t
ngx_mail_proxy_ssl_connection(ngx_mail_session_t *s)
{
    ngx_mail_core_srv_conf_t  *cscf;

    s->connection->log->action = "ssl connecting to upstream";

    cscf = ngx_mail_get_module_srv_conf(s, ngx_mail_core_module);

    /* 设置加密方法 */
    if (ngx_mail_proxy_ssl_set(s, cscf) != NGX_OK) {
        return NGX_ERROR;
    }

    if (s->proxy->upstream.connection->ssl == NULL) {

        /* SSL连接初始化 */
        if (ngx_mail_proxy_ssl_init_connection(s) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}
#endif


void
ngx_mail_proxy_init(ngx_mail_session_t *s, ngx_addr_t *peer)
{
    ngx_int_t                  rc;
    ngx_mail_proxy_ctx_t      *p;
    ngx_mail_proxy_conf_t     *pcf;
    ngx_mail_core_srv_conf_t  *cscf;

    s->connection->log->action = "connecting to upstream";

    cscf = ngx_mail_get_module_srv_conf(s, ngx_mail_core_module);

    p = ngx_pcalloc(s->connection->pool, sizeof(ngx_mail_proxy_ctx_t));
    if (p == NULL) {
        ngx_mail_session_internal_server_error(s);
        return;
    }

    s->proxy = p;

    p->upstream.sockaddr = peer->sockaddr;
    p->upstream.socklen = peer->socklen;
    p->upstream.name = &peer->name;
    p->upstream.get = ngx_event_get_peer;
    p->upstream.log = s->connection->log;
    p->upstream.log_error = NGX_ERROR_ERR;

    rc = ngx_event_connect_peer(&p->upstream);

    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    ngx_add_timer(p->upstream.connection->read, cscf->timeout);

    p->upstream.connection->data = s;
    p->upstream.connection->pool = s->connection->pool;

    s->connection->read->handler = ngx_mail_proxy_block_read;
    p->upstream.connection->write->handler = ngx_mail_proxy_dummy_handler;

    pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);

    s->proxy->buffer = ngx_create_temp_buf(s->connection->pool,
                                           pcf->buffer_size);
    if (s->proxy->buffer == NULL) {
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    s->out.len = 0;

    /* BEGIN: Added by zc, 2018/9/26 */
#if (NGX_MAIL_SSL)
    if (cscf->mail_proxy_ssl) {
        if (ngx_mail_proxy_ssl_connection(s) != NGX_OK) {
            ngx_mail_proxy_internal_server_error(s);
        }
        return;
    }
#endif

    /* 根据协议进行具体处理 */
    ngx_mail_proxy_protocol_handler(s, s->proxy->upstream.connection);
    /* END:   Added by zc, 2018/9/26 */
}


static void
ngx_mail_proxy_block_read(ngx_event_t *rev)
{
    ngx_connection_t    *c;
    ngx_mail_session_t  *s;

    ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0, "mail proxy block read");

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        c = rev->data;
        s = c->data;

        ngx_mail_proxy_close_session(s);
    }
}


static void
ngx_mail_proxy_pop3_handler(ngx_event_t *rev)
{
    u_char                 *p;
    ngx_int_t               rc;
    ngx_str_t               line;
    ngx_connection_t       *c;
    ngx_mail_session_t     *s;
    ngx_mail_proxy_conf_t  *pcf;

    ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                   "mail proxy pop3 auth handler");

    c = rev->data;
    s = c->data;

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "upstream timed out");
        c->timedout = 1;
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    rc = ngx_mail_proxy_read_response(s, 0);

    if (rc == NGX_AGAIN) {
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_mail_proxy_upstream_error(s);
        return;
    }

    switch (s->mail_state) {

    case ngx_pop3_start:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0, "mail proxy send user");

        s->connection->log->action = "sending user name to upstream";

        line.len = sizeof("USER ")  - 1 + s->login.len + 2;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        p = ngx_cpymem(line.data, "USER ", sizeof("USER ") - 1);
        p = ngx_cpymem(p, s->login.data, s->login.len);
        *p++ = CR; *p = LF;

        s->mail_state = ngx_pop3_user;
        break;

    case ngx_pop3_user:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0, "mail proxy send pass");

        s->connection->log->action = "sending password to upstream";

        line.len = sizeof("PASS ")  - 1 + s->passwd.len + 2;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        p = ngx_cpymem(line.data, "PASS ", sizeof("PASS ") - 1);
        p = ngx_cpymem(p, s->passwd.data, s->passwd.len);
        *p++ = CR; *p = LF;

        s->mail_state = ngx_pop3_passwd;
        break;

    case ngx_pop3_passwd:
        s->connection->read->handler = ngx_mail_proxy_handler;
        s->connection->write->handler = ngx_mail_proxy_handler;
        rev->handler = ngx_mail_proxy_handler;
        c->write->handler = ngx_mail_proxy_handler;

        pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);
        ngx_add_timer(s->connection->read, pcf->timeout);
        ngx_del_timer(c->read);

        c->log->action = NULL;
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "client logged in");

        ngx_mail_proxy_handler(s->connection->write);

        return;

    default:
#if (NGX_SUPPRESS_WARN)
        ngx_str_null(&line);
#endif
        break;
    }

    if (c->send(c, line.data, line.len) < (ssize_t) line.len) {
        /*
         * we treat the incomplete sending as NGX_ERROR
         * because it is very strange here
         */
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    s->proxy->buffer->pos = s->proxy->buffer->start;
    s->proxy->buffer->last = s->proxy->buffer->start;
}


static void
ngx_mail_proxy_imap_handler(ngx_event_t *rev)
{
    u_char                 *p;
    ngx_int_t               rc;
    ngx_str_t               line;
    ngx_connection_t       *c;
    ngx_mail_session_t     *s;
    ngx_mail_proxy_conf_t  *pcf;

    ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                   "mail proxy imap auth handler");

    c = rev->data;
    s = c->data;

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "upstream timed out");
        c->timedout = 1;
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    rc = ngx_mail_proxy_read_response(s, s->mail_state);

    if (rc == NGX_AGAIN) {
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_mail_proxy_upstream_error(s);
        return;
    }

    switch (s->mail_state) {

    case ngx_imap_start:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy send login");

        s->connection->log->action = "sending LOGIN command to upstream";

        line.len = s->tag.len + sizeof("LOGIN ") - 1
                   + 1 + NGX_SIZE_T_LEN + 1 + 2;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        line.len = ngx_sprintf(line.data, "%VLOGIN {%uz}" CRLF,
                               &s->tag, s->login.len)
                   - line.data;

        s->mail_state = ngx_imap_login;
        break;

    case ngx_imap_login:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0, "mail proxy send user");

        s->connection->log->action = "sending user name to upstream";

        line.len = s->login.len + 1 + 1 + NGX_SIZE_T_LEN + 1 + 2;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        line.len = ngx_sprintf(line.data, "%V {%uz}" CRLF,
                               &s->login, s->passwd.len)
                   - line.data;

        s->mail_state = ngx_imap_user;
        break;

    case ngx_imap_user:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy send passwd");

        s->connection->log->action = "sending password to upstream";

        line.len = s->passwd.len + 2;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        p = ngx_cpymem(line.data, s->passwd.data, s->passwd.len);
        *p++ = CR; *p = LF;

        s->mail_state = ngx_imap_passwd;
        break;

    case ngx_imap_passwd:
        s->connection->read->handler = ngx_mail_proxy_handler;
        s->connection->write->handler = ngx_mail_proxy_handler;
        rev->handler = ngx_mail_proxy_handler;
        c->write->handler = ngx_mail_proxy_handler;

        pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);
        ngx_add_timer(s->connection->read, pcf->timeout);
        ngx_del_timer(c->read);

        c->log->action = NULL;
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "client logged in");

        ngx_mail_proxy_handler(s->connection->write);

        return;

    default:
#if (NGX_SUPPRESS_WARN)
        ngx_str_null(&line);
#endif
        break;
    }

    if (c->send(c, line.data, line.len) < (ssize_t) line.len) {
        /*
         * we treat the incomplete sending as NGX_ERROR
         * because it is very strange here
         */
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    s->proxy->buffer->pos = s->proxy->buffer->start;
    s->proxy->buffer->last = s->proxy->buffer->start;
}


static void
ngx_mail_proxy_smtp_handler(ngx_event_t *rev)
{
    u_char                    *p;
    ngx_int_t                  rc;
    ngx_str_t                  line;
    ngx_buf_t                 *b;
    ngx_connection_t          *c;
    ngx_mail_session_t        *s;
    ngx_mail_proxy_conf_t     *pcf;
    ngx_mail_core_srv_conf_t  *cscf;

    ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                   "mail proxy smtp auth handler");

    c = rev->data;
    s = c->data;

    /* BEGIN: Added by zc, 2018/10/8 */
    cscf = ngx_mail_get_module_srv_conf(s, ngx_mail_core_module);    
    /* END:   Added by zc, 2018/10/8 */

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "upstream timed out");
        c->timedout = 1;
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    rc = ngx_mail_proxy_read_response(s, s->mail_state);

    if (rc == NGX_AGAIN) {
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_mail_proxy_upstream_error(s);
        return;
    }

    /* BEGIN: Modified by zc, 2018/10/8 */
    /* 修改nginx与上游服务器认证交互过程 */
#if 1
    switch (s->mail_state) {

    case ngx_smtp_start:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy send ehlo");

        s->connection->log->action = "sending HELO/EHLO to upstream";

        line.len = sizeof("HELO ") - 1 + s->smtp_helo.len + 2;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        p = ngx_cpymem(line.data,
                       (s->proxy_esmtp ? "EHLO " : "HELO "),
                       sizeof("HELO ") - 1);
        p = ngx_cpymem(p, s->smtp_helo.data, s->smtp_helo.len);
        *p++ = CR; *p = LF;

        s->mail_state = ngx_smtp_helo;
        break;

    case ngx_smtp_helo:
        if (cscf->mail_proxy_starttls &&
            s->proxy_starttls == ngx_smtp_proxy_starttls_yes) {
            ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                           "mail proxy send starttls");

            s->connection->log->action = "sending STARTTLS to upstream";

            line.len = sizeof("STARTTLS") + 1;
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }

            p = ngx_cpymem(line.data, "STARTTLS", sizeof("STARTTLS") - 1);
            *p++ = CR; *p = LF;

            s->mail_state = ngx_smtp_auth_cram_md5;
            s->proxy_starttls = ngx_smtp_proxy_starttls_ready;
            break;
        }

        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy send auth login");

        line.len = sizeof("AUTH LOGIN") - 1 + sizeof(CRLF) - 1;
        line.data = ngx_pnalloc(c->pool, line.len);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        p = ngx_cpymem(line.data, "AUTH LOGIN", sizeof("AUTH LOGIN") - 1);
        *p++ = CR; *p = LF;

        s->mail_state = ngx_smtp_auth_plain;
        s->proxy_starttls = ngx_smtp_proxy_starttls_no;
        break;

    case ngx_smtp_auth_cram_md5:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy start starttls");

        if (s->proxy_starttls == ngx_smtp_proxy_starttls_start) {
            s->mail_state = ngx_smtp_start;
            s->proxy_starttls = ngx_smtp_proxy_starttls_finish;
            s->proxy->buffer->pos = s->proxy->buffer->start;
            s->proxy->buffer->last = s->proxy->buffer->start;

            /* 采用starttls方式与上游建立连接 */
            if (ngx_mail_proxy_ssl_connection(s) != NGX_OK) {
                ngx_mail_proxy_internal_server_error(s);
            }
            return;

        } else {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "smtp proxy starttls states error: \"%d\"",
                          s->proxy_starttls);
            ngx_mail_proxy_internal_server_error(s);
            return;
        }
        break;

    case ngx_smtp_auth_plain:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy send user name");

        s->connection->log->action = "sending USER NAME to upstream";

        line.data = ngx_pnalloc(c->pool,
                                ngx_base64_encoded_length(s->login.len) + 2);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        /* 用户名进行base64加密 */
        ngx_encode_base64(&line, &s->login);
        p = line.data + line.len;
        *p++ = CR; *p = LF;
        line.len += 2;

        s->mail_state = ngx_smtp_auth_login_username;
        break;

    case ngx_smtp_auth_login_username:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy send uesr password");

        s->connection->log->action = "sending USER PASSWORD to upstream";        

        line.data = ngx_pnalloc(c->pool,
                                ngx_base64_encoded_length(s->passwd.len) + 2);
        if (line.data == NULL) {
            ngx_mail_proxy_internal_server_error(s);
            return;
        }

        /* 密码进行base64加密 */
        ngx_encode_base64(&line, &s->passwd);
        p = line.data + line.len;
        *p++ = CR; *p = LF;
        line.len += 2;

        s->mail_state = ngx_smtp_auth_login_password;
        break;

    case ngx_smtp_auth_login_password:
        ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                       "mail proxy auth finish");

        /* 用户认证完成，进行上下游数据透传 */
        b = s->proxy->buffer;

        ngx_memcpy(b->start, smtp_auth_ok, sizeof(smtp_auth_ok) - 1);
        b->last = b->start + sizeof(smtp_auth_ok) - 1;

        s->connection->read->handler = ngx_mail_proxy_handler;
        s->connection->write->handler = ngx_mail_proxy_handler;
        rev->handler = ngx_mail_proxy_handler;
        c->write->handler = ngx_mail_proxy_handler;

        pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);
        ngx_add_timer(s->connection->read, pcf->timeout);
        ngx_del_timer(c->read);

        c->log->action = NULL;
        if (s->buffer->pos == s->buffer->last) {
            ngx_mail_proxy_handler(s->connection->write);

        } else {
            ngx_mail_proxy_handler(c->write);
        }
        return;

    default:
#if (NGX_SUPPRESS_WARN)
        ngx_str_null(&line);
#endif
        break;
    }
#else
        switch (s->mail_state) {
    
        case ngx_smtp_start:
            ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0, "mail proxy send ehlo");
    
            s->connection->log->action = "sending HELO/EHLO to upstream";
    
            cscf = ngx_mail_get_module_srv_conf(s, ngx_mail_core_module);
    
            line.len = sizeof("HELO ")  - 1 + cscf->server_name.len + 2;
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }
    
            pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);
    
            p = ngx_cpymem(line.data,
                           ((s->esmtp || pcf->xclient) ? "EHLO " : "HELO "),
                           sizeof("HELO ") - 1);
    
            p = ngx_cpymem(p, cscf->server_name.data, cscf->server_name.len);
            *p++ = CR; *p = LF;
    
            if (pcf->xclient) {
                s->mail_state = ngx_smtp_helo_xclient;
    
            } else if (s->auth_method == NGX_MAIL_AUTH_NONE) {
                s->mail_state = ngx_smtp_helo_from;
    
            } else {
                s->mail_state = ngx_smtp_helo;
            }
    
            break;
    
        case ngx_smtp_helo_xclient:
            ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                           "mail proxy send xclient");
    
            s->connection->log->action = "sending XCLIENT to upstream";
    
            line.len = sizeof("XCLIENT ADDR= LOGIN= NAME="
                              CRLF) - 1
                       + s->connection->addr_text.len + s->login.len + s->host.len;
    
#if (NGX_HAVE_INET6)
            if (s->connection->sockaddr->sa_family == AF_INET6) {
                line.len += sizeof("IPV6:") - 1;
            }
#endif
    
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }
    
            p = ngx_cpymem(line.data, "XCLIENT ADDR=", sizeof("XCLIENT ADDR=") - 1);
    
#if (NGX_HAVE_INET6)
            if (s->connection->sockaddr->sa_family == AF_INET6) {
                p = ngx_cpymem(p, "IPV6:", sizeof("IPV6:") - 1);
            }
#endif
    
            p = ngx_copy(p, s->connection->addr_text.data,
                         s->connection->addr_text.len);
    
            if (s->login.len) {
                p = ngx_cpymem(p, " LOGIN=", sizeof(" LOGIN=") - 1);
                p = ngx_copy(p, s->login.data, s->login.len);
            }
    
            p = ngx_cpymem(p, " NAME=", sizeof(" NAME=") - 1);
            p = ngx_copy(p, s->host.data, s->host.len);
    
            *p++ = CR; *p++ = LF;
    
            line.len = p - line.data;
    
            if (s->smtp_helo.len) {
                s->mail_state = ngx_smtp_xclient_helo;
    
            } else if (s->auth_method == NGX_MAIL_AUTH_NONE) {
                s->mail_state = ngx_smtp_xclient_from;
    
            } else {
                s->mail_state = ngx_smtp_xclient;
            }
    
            break;
    
        case ngx_smtp_xclient_helo:
            ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                           "mail proxy send client ehlo");
    
            s->connection->log->action = "sending client HELO/EHLO to upstream";
    
            line.len = sizeof("HELO " CRLF) - 1 + s->smtp_helo.len;
    
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }
    
            line.len = ngx_sprintf(line.data,
                           ((s->esmtp) ? "EHLO %V" CRLF : "HELO %V" CRLF),
                           &s->smtp_helo)
                       - line.data;
    
            s->mail_state = (s->auth_method == NGX_MAIL_AUTH_NONE) ?
                                ngx_smtp_helo_from : ngx_smtp_helo;
    
            break;
    
        case ngx_smtp_helo_from:
        case ngx_smtp_xclient_from:
            ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                           "mail proxy send mail from");
    
            s->connection->log->action = "sending MAIL FROM to upstream";
    
            line.len = s->smtp_from.len + sizeof(CRLF) - 1;
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }
    
            p = ngx_cpymem(line.data, s->smtp_from.data, s->smtp_from.len);
            *p++ = CR; *p = LF;
    
            s->mail_state = ngx_smtp_from;
    
            break;
    
        case ngx_smtp_from:
            ngx_log_debug0(NGX_LOG_DEBUG_MAIL, rev->log, 0,
                           "mail proxy send rcpt to");
    
            s->connection->log->action = "sending RCPT TO to upstream";
    
            line.len = s->smtp_to.len + sizeof(CRLF) - 1;
            line.data = ngx_pnalloc(c->pool, line.len);
            if (line.data == NULL) {
                ngx_mail_proxy_internal_server_error(s);
                return;
            }
    
            p = ngx_cpymem(line.data, s->smtp_to.data, s->smtp_to.len);
            *p++ = CR; *p = LF;
    
            s->mail_state = ngx_smtp_to;
    
            break;
    
        case ngx_smtp_helo:
        case ngx_smtp_xclient:
        case ngx_smtp_to:
    
            b = s->proxy->buffer;
    
            if (s->auth_method == NGX_MAIL_AUTH_NONE) {
                b->pos = b->start;
    
            } else {
                ngx_memcpy(b->start, smtp_auth_ok, sizeof(smtp_auth_ok) - 1);
                b->last = b->start + sizeof(smtp_auth_ok) - 1;
            }
    
            s->connection->read->handler = ngx_mail_proxy_handler;
            s->connection->write->handler = ngx_mail_proxy_handler;
            rev->handler = ngx_mail_proxy_handler;
            c->write->handler = ngx_mail_proxy_handler;
    
            pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);
            ngx_add_timer(s->connection->read, pcf->timeout);
            ngx_del_timer(c->read);
    
            c->log->action = NULL;
            ngx_log_error(NGX_LOG_INFO, c->log, 0, "client logged in");
    
            if (s->buffer->pos == s->buffer->last) {
                ngx_mail_proxy_handler(s->connection->write);
    
            } else {
                ngx_mail_proxy_handler(c->write);
            }
    
            return;
    
        default:
#if (NGX_SUPPRESS_WARN)
            ngx_str_null(&line);
#endif
            break;
        }
#endif    
    /* END:   Modified by zc, 2018/10/8 */

    if (c->send(c, line.data, line.len) < (ssize_t) line.len) {
        /*
         * we treat the incomplete sending as NGX_ERROR
         * because it is very strange here
         */
        ngx_mail_proxy_internal_server_error(s);
        return;
    }

    s->proxy->buffer->pos = s->proxy->buffer->start;
    s->proxy->buffer->last = s->proxy->buffer->start;
}


static void
ngx_mail_proxy_dummy_handler(ngx_event_t *wev)
{
    ngx_connection_t    *c;
    ngx_mail_session_t  *s;

    ngx_log_debug0(NGX_LOG_DEBUG_MAIL, wev->log, 0, "mail proxy dummy handler");

    if (ngx_handle_write_event(wev, 0) != NGX_OK) {
        c = wev->data;
        s = c->data;

        ngx_mail_proxy_close_session(s);
    }
}


static ngx_int_t
ngx_mail_proxy_read_response(ngx_mail_session_t *s, ngx_uint_t state)
{
    u_char                 *p, *m;
    ssize_t                 n;
    ngx_buf_t              *b;
    ngx_mail_proxy_conf_t  *pcf;

    s->connection->log->action = "reading response from upstream";

    b = s->proxy->buffer;

    n = s->proxy->upstream.connection->recv(s->proxy->upstream.connection,
                                            b->last, b->end - b->last);

    if (n == NGX_ERROR || n == 0) {
        return NGX_ERROR;
    }

    if (n == NGX_AGAIN) {
        return NGX_AGAIN;
    }

    b->last += n;

    if (b->last - b->pos < 4) {
        return NGX_AGAIN;
    }

    if (*(b->last - 2) != CR || *(b->last - 1) != LF) {
        if (b->last == b->end) {
            *(b->last - 1) = '\0';
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "upstream sent too long response line: \"%s\"",
                          b->pos);
            return NGX_ERROR;
        }

        return NGX_AGAIN;
    }

    p = b->pos;

    switch (s->protocol) {

    case NGX_MAIL_POP3_PROTOCOL:
        if (p[0] == '+' && p[1] == 'O' && p[2] == 'K') {
            return NGX_OK;
        }
        break;

    case NGX_MAIL_IMAP_PROTOCOL:
        switch (state) {

        case ngx_imap_start:
            if (p[0] == '*' && p[1] == ' ' && p[2] == 'O' && p[3] == 'K') {
                return NGX_OK;
            }
            break;

        case ngx_imap_login:
        case ngx_imap_user:
            if (p[0] == '+') {
                return NGX_OK;
            }
            break;

        case ngx_imap_passwd:
            if (ngx_strncmp(p, s->tag.data, s->tag.len) == 0) {
                p += s->tag.len;
                if (p[0] == 'O' && p[1] == 'K') {
                    return NGX_OK;
                }
            }
            break;
        }

        break;

    default: /* NGX_MAIL_SMTP_PROTOCOL */

        /* BEGIN: Modified by zc, 2018/10/8 */
        /* 修改认证命令交互过程 */
#if 1
        if (p[3] == '-') {
            m = p;
            /* multiline reply, check if we got last line */
            if (ngx_strnstr(m, CRLF "250 ", b->last - b->pos) == NULL) {
                return NGX_AGAIN;
            }
        }

        switch (state) {

        case ngx_smtp_start:
            if (p[0] == '2' && p[1] == '2' && p[2] == '0') {
                if (ngx_strnstr(p, "ESMTP", b->last - b->pos) != NULL) {
                    s->proxy_esmtp = 1;
                }
                return NGX_OK;
            }
            break;

        case ngx_smtp_helo:
            if (p[0] == '2' && p[1] == '5' && p[2] == '0') {
                if (ngx_strnstr(p, "250-STARTTLS" CRLF,
                                b->last - b->pos) != NULL) {
                    if (s->proxy_starttls == ngx_smtp_proxy_starttls_no) {
                        s->proxy_starttls = ngx_smtp_proxy_starttls_yes;
                    }
                }
                return NGX_OK;
            }
            break;

        case ngx_smtp_auth_cram_md5:
            if (p[0] == '2' && p[1] == '2' && p[2] == '0') {
                if (s->proxy_starttls == ngx_smtp_proxy_starttls_ready) {
                    s->proxy_starttls = ngx_smtp_proxy_starttls_start;
                    return NGX_OK;
                } else {
                    ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                                  "smtp proxy starttls states error: \"%d\"",
                                  s->proxy_starttls);
                    return NGX_ERROR;
                }
            }
            break;

        case ngx_smtp_auth_plain:
        case ngx_smtp_auth_login_username:
            if (p[0] == '3' && p[1] == '3' && p[2] == '4') {
                return NGX_OK;
            }
            break;

        case ngx_smtp_auth_login_password:
            if (p[0] == '2' && p[1] == '3' && p[2] == '5') {
                return NGX_OK;
            }
            break;
        }
#else
        if (p[3] == '-') {
            /* multiline reply, check if we got last line */

            m = b->last - (sizeof(CRLF "200" CRLF) - 1);

            while (m > p) {
                if (m[0] == CR && m[1] == LF) {
                    break;
                }

                m--;
            }

            if (m <= p || m[5] == '-') {
                return NGX_AGAIN;
            }
        }

        switch (state) {

        case ngx_smtp_start:
            if (p[0] == '2' && p[1] == '2' && p[2] == '0') {
                return NGX_OK;
            }
            break;

        case ngx_smtp_helo:
        case ngx_smtp_helo_xclient:
        case ngx_smtp_helo_from:
        case ngx_smtp_from:
            if (p[0] == '2' && p[1] == '5' && p[2] == '0') {
                return NGX_OK;
            }
            break;

        case ngx_smtp_xclient:
        case ngx_smtp_xclient_from:
        case ngx_smtp_xclient_helo:
            if (p[0] == '2' && (p[1] == '2' || p[1] == '5') && p[2] == '0') {
                return NGX_OK;
            }
            break;

        case ngx_smtp_to:
            return NGX_OK;
        }
#endif
        /* END:   Modified by zc, 2018/10/8 */

        break;
    }

    pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);

    if (pcf->pass_error_message == 0) {
        *(b->last - 2) = '\0';
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "upstream sent invalid response: \"%s\"", p);
        return NGX_ERROR;
    }

    s->out.len = b->last - p - 2;
    s->out.data = p;

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "upstream sent invalid response: \"%V\"", &s->out);

    s->out.len = b->last - b->pos;
    s->out.data = b->pos;

    return NGX_ERROR;
}


static void
ngx_mail_proxy_handler(ngx_event_t *ev)
{
    char                   *action, *recv_action, *send_action;
    size_t                  size;
    ssize_t                 n;
    ngx_buf_t              *b;
    ngx_uint_t              do_write;
    ngx_connection_t       *c, *src, *dst;
    ngx_mail_session_t     *s;
    ngx_mail_proxy_conf_t  *pcf;

    c = ev->data;
    s = c->data;

    if (ev->timedout) {
        c->log->action = "proxying";

        if (c == s->connection) {
            ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                          "client timed out");
            c->timedout = 1;

        } else {
            ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                          "upstream timed out");
        }

        ngx_mail_proxy_close_session(s);
        return;
    }

    if (c == s->connection) {
        if (ev->write) {
            recv_action = "proxying and reading from upstream";
            send_action = "proxying and sending to client";
            src = s->proxy->upstream.connection;
            dst = c;
            b = s->proxy->buffer;

        } else {
            recv_action = "proxying and reading from client";
            send_action = "proxying and sending to upstream";
            src = c;
            dst = s->proxy->upstream.connection;
            b = s->buffer;
        }

    } else {
        if (ev->write) {
            recv_action = "proxying and reading from client";
            send_action = "proxying and sending to upstream";
            src = s->connection;
            dst = c;
            b = s->buffer;

        } else {
            recv_action = "proxying and reading from upstream";
            send_action = "proxying and sending to client";
            src = c;
            dst = s->connection;
            b = s->proxy->buffer;
        }
    }

    do_write = ev->write ? 1 : 0;

    ngx_log_debug3(NGX_LOG_DEBUG_MAIL, ev->log, 0,
                   "mail proxy handler: %ui, #%d > #%d",
                   do_write, src->fd, dst->fd);

    for ( ;; ) {

        if (do_write) {

            size = b->last - b->pos;

            if (size && dst->write->ready) {
                c->log->action = send_action;

                n = dst->send(dst, b->pos, size);

                if (n == NGX_ERROR) {
                    ngx_mail_proxy_close_session(s);
                    return;
                }

                if (n > 0) {
                    b->pos += n;

                    if (b->pos == b->last) {
                        b->pos = b->start;
                        b->last = b->start;
                    }
                }
            }
        }

        size = b->end - b->last;

        if (size && src->read->ready) {
            c->log->action = recv_action;

            n = src->recv(src, b->last, size);

            if (n == NGX_AGAIN || n == 0) {
                break;
            }

            if (n > 0) {
                do_write = 1;
                b->last += n;

                continue;
            }

            if (n == NGX_ERROR) {
                src->read->eof = 1;
            }
        }

        break;
    }

    c->log->action = "proxying";

    if ((s->connection->read->eof && s->buffer->pos == s->buffer->last)
        || (s->proxy->upstream.connection->read->eof
            && s->proxy->buffer->pos == s->proxy->buffer->last)
        || (s->connection->read->eof
            && s->proxy->upstream.connection->read->eof))
    {
        action = c->log->action;
        c->log->action = NULL;
        ngx_log_error(NGX_LOG_INFO, c->log, 0, "proxied session done");
        c->log->action = action;

        ngx_mail_proxy_close_session(s);
        return;
    }

    if (ngx_handle_write_event(dst->write, 0) != NGX_OK) {
        ngx_mail_proxy_close_session(s);
        return;
    }

    if (ngx_handle_read_event(dst->read, 0) != NGX_OK) {
        ngx_mail_proxy_close_session(s);
        return;
    }

    if (ngx_handle_write_event(src->write, 0) != NGX_OK) {
        ngx_mail_proxy_close_session(s);
        return;
    }

    if (ngx_handle_read_event(src->read, 0) != NGX_OK) {
        ngx_mail_proxy_close_session(s);
        return;
    }

    if (c == s->connection) {
        pcf = ngx_mail_get_module_srv_conf(s, ngx_mail_proxy_module);
        ngx_add_timer(c->read, pcf->timeout);
    }
}


static void
ngx_mail_proxy_upstream_error(ngx_mail_session_t *s)
{
    if (s->proxy->upstream.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_MAIL, s->connection->log, 0,
                       "close mail proxy connection: %d",
                       s->proxy->upstream.connection->fd);

        ngx_close_connection(s->proxy->upstream.connection);
    }

    if (s->out.len == 0) {
        ngx_mail_session_internal_server_error(s);
        return;
    }

    s->quit = 1;
    ngx_mail_send(s->connection->write);
}


static void
ngx_mail_proxy_internal_server_error(ngx_mail_session_t *s)
{
    if (s->proxy->upstream.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_MAIL, s->connection->log, 0,
                       "close mail proxy connection: %d",
                       s->proxy->upstream.connection->fd);

        ngx_close_connection(s->proxy->upstream.connection);
    }

    ngx_mail_session_internal_server_error(s);
}


static void
ngx_mail_proxy_close_session(ngx_mail_session_t *s)
{
    if (s->proxy->upstream.connection) {
        ngx_log_debug1(NGX_LOG_DEBUG_MAIL, s->connection->log, 0,
                       "close mail proxy connection: %d",
                       s->proxy->upstream.connection->fd);

        ngx_close_connection(s->proxy->upstream.connection);
    }

    ngx_mail_close_connection(s->connection);
}


static void *
ngx_mail_proxy_create_conf(ngx_conf_t *cf)
{
    ngx_mail_proxy_conf_t  *pcf;

    pcf = ngx_pcalloc(cf->pool, sizeof(ngx_mail_proxy_conf_t));
    if (pcf == NULL) {
        return NULL;
    }

    pcf->enable = NGX_CONF_UNSET;
    pcf->pass_error_message = NGX_CONF_UNSET;
    pcf->xclient = NGX_CONF_UNSET;
    pcf->buffer_size = NGX_CONF_UNSET_SIZE;
    pcf->timeout = NGX_CONF_UNSET_MSEC;

    return pcf;
}


static char *
ngx_mail_proxy_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_mail_proxy_conf_t *prev = parent;
    ngx_mail_proxy_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->pass_error_message, prev->pass_error_message, 0);
    ngx_conf_merge_value(conf->xclient, prev->xclient, 1);
    ngx_conf_merge_size_value(conf->buffer_size, prev->buffer_size,
                              (size_t) ngx_pagesize);
    ngx_conf_merge_msec_value(conf->timeout, prev->timeout, 24 * 60 * 60000);

    return NGX_CONF_OK;
}
