
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


typedef struct {
    ngx_flag_t  pcre_jit;
} ngx_regex_conf_t;


#if (NGX_HAVE_PCRE_JIT)
static void ngx_pcre_free_studies(void *data);
#endif

static ngx_int_t ngx_regex_module_init(ngx_cycle_t *cycle);

static void *ngx_regex_create_conf(ngx_cycle_t *cycle);
static char *ngx_regex_init_conf(ngx_cycle_t *cycle, void *conf);

static char *ngx_regex_pcre_jit(ngx_conf_t *cf, void *post, void *data);
static ngx_conf_post_t  ngx_regex_pcre_jit_post = { ngx_regex_pcre_jit };


static ngx_command_t  ngx_regex_commands[] = {

    { ngx_string("pcre_jit"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_regex_conf_t, pcre_jit),
      &ngx_regex_pcre_jit_post },

      ngx_null_command
};


static ngx_core_module_t  ngx_regex_module_ctx = {
    ngx_string("regex"),
    ngx_regex_create_conf,
    ngx_regex_init_conf
};


ngx_module_t  ngx_regex_module = {
    NGX_MODULE_V1,
    &ngx_regex_module_ctx,                 /* module context */
    ngx_regex_commands,                    /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    ngx_regex_module_init,                 /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_list_t  *ngx_pcre_studies;


ngx_int_t
ngx_regex_compile(ngx_regex_compile_t *rc)
{
    int               n;
    char             *p;
    pcre2_code       *re;
    int              errcode;
    PCRE2_SIZE       erroff;
    ngx_regex_elt_t  *elt;

    re = pcre2_compile(rc->pattern.data, PCRE2_ZERO_TERMINATED,
                      (int) rc->options, &errcode, &erroff, NULL);

    if (re == NULL) {
        u_char errstr[128];
        pcre2_get_error_message(errcode, errstr, sizeof errstr);

        if ((size_t) erroff == rc->pattern.len) {
           rc->err.len = ngx_snprintf(rc->err.data, rc->err.len,
                              "pcre2_compile() failed: %s in \"%V\"",
                               errstr, &rc->pattern)
                      - rc->err.data;

        } else {
           rc->err.len = ngx_snprintf(rc->err.data, rc->err.len,
                              "pcre2_compile() failed: %s in \"%V\" at \"%s\"",
                               errstr, &rc->pattern, rc->pattern.data + erroff)
                      - rc->err.data;
        }

        return NGX_ERROR;
    }

    rc->regex = ngx_pcalloc(rc->pool, sizeof(ngx_regex_t));
    if (rc->regex == NULL) {
        goto nomem;
    }

    rc->regex->code = re;


    if (ngx_pcre_studies != NULL) {
        elt = ngx_list_push(ngx_pcre_studies);
        if (elt == NULL) {
            goto nomem;
        }

        elt->regex = rc->regex;
        elt->name = rc->pattern.data;
    }

    n = pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &rc->captures);
    if (n < 0) {
        p = "pcre2_pattern_info(\"%V\", PCRE2_INFO_CAPTURECOUNT) failed: %d";
        goto failed;
    }

    if (rc->captures == 0) {
        return NGX_OK;
    }

    n = pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &rc->named_captures);
    if (n < 0) {
        p = "pcre2_pattern_info(\"%V\", PCRE2_INFO_NAMECOUNT) failed: %d";
        goto failed;
    }

    if (rc->named_captures == 0) {
        return NGX_OK;
    }

    n = pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &rc->name_size);
    if (n < 0) {
        p = "pcre2_pattern_info(\"%V\", PCRE2_INFO_NAMEENTRYSIZE) failed: %d";
        goto failed;
    }

    n = pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &rc->names);
    if (n < 0) {
        p = "pcre2_pattern_info(\"%V\", PCRE2_INFO_NAMETABLE) failed: %d";
        goto failed;
    }

    return NGX_OK;

failed:

    rc->err.len = ngx_snprintf(rc->err.data, rc->err.len, p, &rc->pattern, n)
                  - rc->err.data;
    return NGX_ERROR;

nomem:

    rc->err.len = ngx_snprintf(rc->err.data, rc->err.len,
                               "regex \"%V\" compilation failed: no memory",
                               &rc->pattern)
                  - rc->err.data;
    return NGX_ERROR;
}


ngx_int_t
ngx_regex_exec_array(ngx_array_t *a, ngx_str_t *s, ngx_log_t *log)
{
    ngx_int_t         n;
    ngx_uint_t        i;
    ngx_regex_elt_t  *re;

    re = a->elts;

    for (i = 0; i < a->nelts; i++) {

        n = ngx_regex_exec(re[i].regex, s, NULL, 0);

        if (n == NGX_REGEX_NO_MATCHED) {
            continue;
        }

        if (n < 0) {
            ngx_log_error(NGX_LOG_ALERT, log, 0,
                          ngx_regex_exec_n " failed: %i on \"%V\" using \"%s\"",
                          n, s, re[i].name);
            return NGX_ERROR;
        }

        /* match */

        return NGX_OK;
    }

    return NGX_DECLINED;
}


static void
ngx_pcre_free_studies(void *data)
{
    ngx_list_t *studies = data;

    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_regex_elt_t  *elts;

    part = &studies->part;
    elts = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            elts = part->elts;
            i = 0;
        }

        pcre2_code_free(elts[i].regex->code);
    }
}


static ngx_int_t
ngx_regex_module_init(ngx_cycle_t *cycle)
{
    int               pcre_jit;
    ngx_uint_t        i;
    ngx_list_part_t  *part;
    ngx_regex_elt_t  *elts;
    ngx_pool_cleanup_t  *cln;

    pcre_jit = 0;

#if (NGX_HAVE_PCRE_JIT)
    {
    ngx_regex_conf_t    *rcf;

    rcf = (ngx_regex_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_regex_module);

    pcre_jit = rcf->pcre_jit;
    }
#endif

    cln = ngx_pool_cleanup_add(cycle->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    cln->handler = ngx_pcre_free_studies;
    cln->data = ngx_pcre_studies;

    part = &ngx_pcre_studies->part;
    elts = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            elts = part->elts;
            i = 0;
        }

#if (NGX_HAVE_PCRE_JIT)
        if (pcre_jit) {
            int n;

            n = pcre2_jit_compile(elts[i].regex->code, PCRE2_JIT_COMPLETE);

            if (n != 0) {
                ngx_log_error(NGX_LOG_INFO, cycle->log, 0,
                              "JIT compiler does not support pattern: \"%s\"",
                              elts[i].name);
            }
        }
#endif
    }

    ngx_pcre_studies = NULL;

    return NGX_OK;
}


static void *
ngx_regex_create_conf(ngx_cycle_t *cycle)
{
    ngx_regex_conf_t  *rcf;

    rcf = ngx_pcalloc(cycle->pool, sizeof(ngx_regex_conf_t));
    if (rcf == NULL) {
        return NULL;
    }

    rcf->pcre_jit = NGX_CONF_UNSET;

    ngx_pcre_studies = ngx_list_create(cycle->pool, 8, sizeof(ngx_regex_elt_t));
    if (ngx_pcre_studies == NULL) {
        return NULL;
    }

    return rcf;
}


static char *
ngx_regex_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_regex_conf_t *rcf = conf;

    ngx_conf_init_value(rcf->pcre_jit, 0);

    return NGX_CONF_OK;
}


static char *
ngx_regex_pcre_jit(ngx_conf_t *cf, void *post, void *data)
{
    ngx_flag_t  *fp = data;

    if (*fp == 0) {
        return NGX_CONF_OK;
    }

#if (NGX_HAVE_PCRE_JIT)
    {
    int  jit, r;

    jit = 0;
    r = pcre2_config(PCRE2_CONFIG_JIT, &jit);

    if (r != 0 || jit != 1) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "PCRE library does not support JIT");
        *fp = 0;
    }
    }
#else
    ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                       "nginx was built without PCRE JIT support");
    *fp = 0;
#endif

    return NGX_CONF_OK;
}
