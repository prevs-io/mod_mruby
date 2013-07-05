/*
// mod_mruby.c - mod_mruby core module
//
// See Copyright Notice in mod_mruby.h
*/

/*
// -------------------------------------------------------------------
// mod_mruby
//      This is a mruby module for Apache HTTP Server.
//
//      By matsumoto_r (MATSUMOTO, Ryosuke) Sep 2012 in Japan
//          Academic Center for Computing and Media Studies, Kyoto University
//          Graduate School of Informatics, Kyoto University
//          email: matsumoto_r at net.ist.i.kyoto-u.ac.jp
//
// Date     2012/04/21
//
// change log
//  2012/04/21 1.00 matsumoto_r first release
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// How To Compile
// [Use DSO]
// make install
//
// <add to httpd.conf or conf.d/mruby.conf>
// LoadModule mruby_module   modules/mod_mruby.so
//
// -------------------------------------------------------------------
*/
#define CORE_PRIVATE

#include "apr_strings.h"
#include "ap_config.h"
#include "ap_provider.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"

#include "apr_thread_proc.h"

/*
#include "apr_global_mutex.h"
#ifdef AP_NEED_SET_MUTEX_PERMS
#include "unixd.h"
#if (AP_SERVER_MINORVERSION_NUMBER > 2)
#define unixd_set_global_mutex_perms ap_unixd_set_global_mutex_perms
#endif
#endif
*/

#include <mruby.h>
#include <mruby/proc.h>
#include <mruby/compile.h>

#include <unistd.h>
#include <sys/stat.h>
#include <setjmp.h>
//#include <sys/prctl.h>

#include "mod_mruby.h"
#include "ap_mrb_request.h"
#include "ap_mrb_authnprovider.h"
#include "ap_mrb_core.h"
#include "ap_mrb_filter.h"

mrb_state *mod_mruby_share_state = NULL;
apr_thread_mutex_t *mod_mruby_mutex;

module AP_MODULE_DECLARE_DATA mruby_module;

int ap_mruby_class_init(mrb_state *mrb);
static int mod_mruby_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s);
static int ap_mruby_run(mrb_state *mrb, request_rec *r, mruby_config_t *conf, const char *mruby_code_file, int module_status);

static void *mod_mruby_create_dir_config(apr_pool_t *p, char *dummy)
{
    mruby_dir_config_t *dir_conf = 
        (mruby_dir_config_t *)apr_pcalloc(p, sizeof(*dir_conf));
    dir_conf->mod_mruby_authn_check_password_code   = NULL;
    dir_conf->mod_mruby_authn_get_realm_hash_code   = NULL;
    dir_conf->mod_mruby_output_filter_code          = NULL;

    return dir_conf;
}

static void *mod_mruby_create_config(apr_pool_t *p, server_rec *s)
{
    mruby_config_t *conf = 
        (mruby_config_t *)apr_pcalloc(p, sizeof(mruby_config_t));

    // inlinde core in httpd.conf
    conf->mod_mruby_handler_inline_code                 = NULL;
    conf->mod_mruby_translate_name_first_inline_code    = NULL;

    // hook script file
    conf->mod_mruby_handler_code                    = NULL;
    conf->mod_mruby_post_config_first_code          = NULL;
    conf->mod_mruby_post_config_middle_code         = NULL;
    conf->mod_mruby_post_config_last_code           = NULL;
    conf->mod_mruby_child_init_first_code           = NULL;
    conf->mod_mruby_child_init_middle_code          = NULL;
    conf->mod_mruby_child_init_last_code            = NULL;
    conf->mod_mruby_handler_first_code              = NULL;
    conf->mod_mruby_handler_middle_code             = NULL;
    conf->mod_mruby_handler_last_code               = NULL;
    conf->mod_mruby_post_read_request_first_code    = NULL;
    conf->mod_mruby_post_read_request_middle_code   = NULL;
    conf->mod_mruby_post_read_request_last_code     = NULL;
    conf->mod_mruby_quick_handler_first_code        = NULL;
    conf->mod_mruby_quick_handler_middle_code       = NULL;
    conf->mod_mruby_quick_handler_last_code         = NULL;
    conf->mod_mruby_translate_name_first_code       = NULL;
    conf->mod_mruby_translate_name_middle_code      = NULL;
    conf->mod_mruby_translate_name_last_code        = NULL;
    conf->mod_mruby_map_to_storage_first_code       = NULL;
    conf->mod_mruby_map_to_storage_middle_code      = NULL;
    conf->mod_mruby_map_to_storage_last_code        = NULL;
    conf->mod_mruby_access_checker_first_code       = NULL;
    conf->mod_mruby_access_checker_middle_code      = NULL;
    conf->mod_mruby_access_checker_last_code        = NULL;
    conf->mod_mruby_check_user_id_first_code        = NULL;
    conf->mod_mruby_check_user_id_middle_code       = NULL;
    conf->mod_mruby_check_user_id_last_code         = NULL;
    conf->mod_mruby_auth_checker_first_code         = NULL;
    conf->mod_mruby_auth_checker_middle_code        = NULL;
    conf->mod_mruby_auth_checker_last_code          = NULL;
    conf->mod_mruby_fixups_first_code               = NULL;
    conf->mod_mruby_fixups_middle_code              = NULL;
    conf->mod_mruby_fixups_last_code                = NULL;
    conf->mod_mruby_insert_filter_first_code        = NULL;
    conf->mod_mruby_insert_filter_middle_code       = NULL;
    conf->mod_mruby_insert_filter_last_code         = NULL;
    conf->mod_mruby_log_transaction_first_code      = NULL;
    conf->mod_mruby_log_transaction_middle_code     = NULL;
    conf->mod_mruby_log_transaction_last_code       = NULL;

    conf->mruby_cache_table_size                    = 0;

    return conf;
}

//
// Set code functions
// 
static mod_mruby_code_t *ap_mrb_set_file(apr_pool_t *p, const char *arg)
{
    mod_mruby_code_t *c = 
        (mod_mruby_code_t *)apr_pcalloc(p, sizeof(mod_mruby_code_t));

    c->type = MOD_MRUBY_FILE;
    c->path = apr_pstrdup(p, arg);

    return c;
}

static mod_mruby_code_t *ap_mrb_set_string(apr_pool_t *p, const char *arg)
{
    mod_mruby_code_t *c = 
        (mod_mruby_code_t *)apr_pcalloc(p, sizeof(mod_mruby_code_t));

    c->type = MOD_MRUBY_STRING;
    c->code = apr_pstrdup(p, arg);

    return c;
}

//
// set cmds functions (for Ruby inline code)
//
static const char *set_mod_mruby_handler_inline(cmd_parms *cmd, void *mconfig, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, NOT_IN_FILES | NOT_IN_LIMIT);
    mruby_config_t *conf = 
        (mruby_config_t *) ap_get_module_config(cmd->server->module_config, &mruby_module);

    if (err != NULL)
        return err;

    conf->mod_mruby_handler_inline_code = ap_mrb_set_string(cmd->pool, arg);

    return NULL;
}

static const char *set_mod_mruby_translate_name_first_inline(cmd_parms *cmd, void *mconfig, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, NOT_IN_FILES | NOT_IN_LIMIT);
    mruby_config_t *conf = 
        (mruby_config_t *) ap_get_module_config(cmd->server->module_config, &mruby_module);

    if (err != NULL)
        return err;

    conf->mod_mruby_translate_name_first_inline_code = ap_mrb_set_string(cmd->pool, arg);

    return NULL;
}

//
// set cmds functions (for Ruby file path)
//
/*
handler
handler_first
handler_middle
handler_last
post_config_first
post_config_middle
post_config_last
child_init_first
child_init_middle
child_init_last
post_read_request_first
post_read_request_middle
post_read_request_last
quick_handler_first
quick_handler_middle
quick_handler_last
translate_name_first
translate_name_middle
translate_name_last
map_to_storage_first
map_to_storage_middle
map_to_storage_last
access_checker_first
access_checker_middle
access_checker_last
check_user_id_first
check_user_id_middle
check_user_id_last
auth_checker_first
auth_checker_middle
auth_checker_last
fixups_first
fixups_middle
fixups_last
insert_filter_first
insert_filter_middle
insert_filter_last
log_transaction_first
log_transaction_middle
log_transaction_last
*/
#define SET_MOD_MRUBY_SERVER_CMDS(hook) \
static const char *set_mod_mruby_##hook(cmd_parms *cmd, void *mconfig, const char *arg);                          \
static const char *set_mod_mruby_##hook(cmd_parms *cmd, void *mconfig, const char *arg)                           \
{                                                                                                                 \
    const char *err = ap_check_cmd_context(cmd, NOT_IN_FILES | NOT_IN_LIMIT);                                     \
    mruby_config_t *conf = (mruby_config_t *) ap_get_module_config(cmd->server->module_config, &mruby_module);    \
                                                                                                                  \
    if (err != NULL)                                                                                              \
        return err;                                                                                               \
                                                                                                                  \
    conf->mod_mruby_##hook##_code = ap_mrb_set_file(cmd->pool, arg);                                              \
                                                                                                                  \
    return NULL;                                                                                                  \
}

SET_MOD_MRUBY_SERVER_CMDS(handler);
SET_MOD_MRUBY_SERVER_CMDS(handler_first);
SET_MOD_MRUBY_SERVER_CMDS(handler_middle);
SET_MOD_MRUBY_SERVER_CMDS(handler_last);
SET_MOD_MRUBY_SERVER_CMDS(post_config_first);
SET_MOD_MRUBY_SERVER_CMDS(post_config_middle);
SET_MOD_MRUBY_SERVER_CMDS(post_config_last);
SET_MOD_MRUBY_SERVER_CMDS(child_init_first);
SET_MOD_MRUBY_SERVER_CMDS(child_init_middle);
SET_MOD_MRUBY_SERVER_CMDS(child_init_last);
SET_MOD_MRUBY_SERVER_CMDS(post_read_request_first);
SET_MOD_MRUBY_SERVER_CMDS(post_read_request_middle);
SET_MOD_MRUBY_SERVER_CMDS(post_read_request_last);
SET_MOD_MRUBY_SERVER_CMDS(quick_handler_first);
SET_MOD_MRUBY_SERVER_CMDS(quick_handler_middle);
SET_MOD_MRUBY_SERVER_CMDS(quick_handler_last);
SET_MOD_MRUBY_SERVER_CMDS(translate_name_first);
SET_MOD_MRUBY_SERVER_CMDS(translate_name_middle);
SET_MOD_MRUBY_SERVER_CMDS(translate_name_last);
SET_MOD_MRUBY_SERVER_CMDS(map_to_storage_first);
SET_MOD_MRUBY_SERVER_CMDS(map_to_storage_middle);
SET_MOD_MRUBY_SERVER_CMDS(map_to_storage_last);
SET_MOD_MRUBY_SERVER_CMDS(access_checker_first);
SET_MOD_MRUBY_SERVER_CMDS(access_checker_middle);
SET_MOD_MRUBY_SERVER_CMDS(access_checker_last);
SET_MOD_MRUBY_SERVER_CMDS(check_user_id_first);
SET_MOD_MRUBY_SERVER_CMDS(check_user_id_middle);
SET_MOD_MRUBY_SERVER_CMDS(check_user_id_last);
SET_MOD_MRUBY_SERVER_CMDS(auth_checker_first);
SET_MOD_MRUBY_SERVER_CMDS(auth_checker_middle);
SET_MOD_MRUBY_SERVER_CMDS(auth_checker_last);
SET_MOD_MRUBY_SERVER_CMDS(fixups_first);
SET_MOD_MRUBY_SERVER_CMDS(fixups_middle);
SET_MOD_MRUBY_SERVER_CMDS(fixups_last);
SET_MOD_MRUBY_SERVER_CMDS(insert_filter_first);
SET_MOD_MRUBY_SERVER_CMDS(insert_filter_middle);
SET_MOD_MRUBY_SERVER_CMDS(insert_filter_last);
SET_MOD_MRUBY_SERVER_CMDS(log_transaction_first);
SET_MOD_MRUBY_SERVER_CMDS(log_transaction_middle);
SET_MOD_MRUBY_SERVER_CMDS(log_transaction_last);

#define SET_MOD_MRUBY_DIR_CMDS(hook) \
static const char *set_mod_mruby_##hook(cmd_parms *cmd, void *mconfig, const char *arg);                          \
static const char *set_mod_mruby_##hook(cmd_parms *cmd, void *mconfig, const char *arg)                           \
{                                                                                                                 \
    const char *err = ap_check_cmd_context(cmd, NOT_IN_FILES | NOT_IN_LIMIT);                                     \
    mruby_dir_config_t *dir_conf = (mruby_dir_config_t *)mconfig;                                                 \
                                                                                                                  \
    if (err != NULL)                                                                                              \
        return err;                                                                                               \
                                                                                                                  \
    dir_conf->mod_mruby_##hook##_code = ap_mrb_set_file(cmd->pool, arg);                                          \
                                                                                                                  \
    return NULL;                                                                                                  \
}

SET_MOD_MRUBY_DIR_CMDS(authn_check_password);
SET_MOD_MRUBY_DIR_CMDS(authn_get_realm_hash);
SET_MOD_MRUBY_DIR_CMDS(output_filter);

//
// run mruby core functions
//
static void ap_mruby_irep_clean(mrb_state *mrb, struct mrb_irep *irep, request_rec *r)
{
    mrb_irep_free(mrb, irep);
    mrb->exc = 0;
}

// mruby_run for not request phase.
static int ap_mruby_run_nr(const char *mruby_code_file)
{

    int n;
    struct mrb_parser_state* p;
    FILE *mrb_file;
    mrb_state *mrb = mrb_open();
    ap_mruby_class_init(mrb);

    if ((mrb_file = fopen(mruby_code_file, "r")) == NULL) {
        ap_log_error(APLOG_MARK
            , APLOG_ERR
            , 0
            , NULL
            , "%s ERROR %s: mrb file oepn failed: %s"
            , MODULE_NAME
            , __func__
            , mruby_code_file
        );
        mrb_close(mrb);
        return -1;
    }

   ap_log_error(APLOG_MARK
       , APLOG_DEBUG
       , 0
       , NULL
       , "%s DEBUG %s: cache nothing on pid %d, compile code: %s"
       , MODULE_NAME
       , __func__
       , getpid()
       , mruby_code_file
   );

    p = mrb_parse_file(mrb, mrb_file, NULL);
    fclose(mrb_file);
    n = mrb_generate_code(mrb, p);

    mrb_pool_close(p->pool);

    ap_log_error(APLOG_MARK
        , APLOG_DEBUG
        , 0
        , NULL
        , "%s DEBUG %s: run mruby code: %s"
        , MODULE_NAME
        , __func__
        , mruby_code_file
    );

    ap_mrb_set_status_code(OK);
    mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[n]), mrb_top_self(mrb));

    if (mrb->exc)
        ap_mrb_raise_file_error_nr(mrb, mrb_obj_value(mrb->exc), mruby_code_file);

    mrb_close(mrb);

    return APR_SUCCESS;
}

static int ap_mruby_run(mrb_state *mrb, request_rec *r, mruby_config_t *conf, const char *mruby_code_file, int module_status)
{

    //int i, n;
    int n;
    struct mrb_parser_state* p;
    //struct stat st;
    FILE *mrb_file;
    int ai = 0;
    jmp_buf mod_mruby_jmp;

    // mutex lock
    if (apr_thread_mutex_lock(mod_mruby_mutex) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK
            , APLOG_ERR
            , 0
            , NULL
            , "%s ERROR %s: mod_mruby_mutex lock failed"
            , MODULE_NAME
            , __func__
        );
        return DECLINED;
    }
    ap_mrb_push_request(r);

    if ((mrb_file = fopen(mruby_code_file, "r")) == NULL) {
        ap_log_error(APLOG_MARK
            , APLOG_ERR
            , 0
            , NULL
            , "%s ERROR %s: mrb file oepn failed: %s"
            , MODULE_NAME
            , __func__
            , mruby_code_file
        );
        return DECLINED;
    }

   ap_log_rerror(APLOG_MARK
       , APLOG_DEBUG
       , 0
       , r
       , "%s DEBUG %s: cache nothing on pid %d, compile code: %s"
       , MODULE_NAME
       , __func__
       , getpid()
       , mruby_code_file
   );

    ai = mrb_gc_arena_save(mrb);
    p = mrb_parse_file(mrb, mrb_file, NULL);
    fclose(mrb_file);
    n = mrb_generate_code(mrb, p);

    mrb_pool_close(p->pool);

    ap_log_rerror(APLOG_MARK
        , APLOG_DEBUG
        , 0
        , r
        , "%s DEBUG %s: run mruby code: %s"
        , MODULE_NAME
        , __func__
        , mruby_code_file
    );

    ap_mrb_set_status_code(OK);
    if (!setjmp(mod_mruby_jmp)) {
        mrb->jmp = &mod_mruby_jmp;
        mrb_run(mrb, mrb_proc_new(mrb, mrb->irep[n]), mrb_top_self(mrb));
    }

    mrb->jmp = 0;
    mrb_gc_arena_restore(mrb, ai);

    if (mrb->exc)
        ap_mrb_raise_file_error(mrb, mrb_obj_value(mrb->exc), r, mruby_code_file);

    ap_log_rerror(APLOG_MARK
        , APLOG_DEBUG
        , 0
        , r
        , "%s DEBUG %s: n=%d return mruby code(%d): %s"
        , MODULE_NAME
        , __func__
        , n
        , ap_mrb_get_status_code()
        , mruby_code_file
    );

    mrb->irep_len = n;
    ap_log_rerror(APLOG_MARK
        , APLOG_DEBUG
        , 0
        , r
        , "%s DEBUG %s: irep[%d] cleaning"
        , MODULE_NAME
        , __func__
        , n
    );
    ap_mruby_irep_clean(mrb, mrb->irep[n], r);

    // mutex unlock
    if (apr_thread_mutex_unlock(mod_mruby_mutex) != APR_SUCCESS){
        ap_log_error(APLOG_MARK
            , APLOG_ERR
            , 0
            , NULL
            , "%s ERROR %s: mod_mruby_mutex unlock failed"
            , MODULE_NAME
            , __func__
        );
        return OK;
    }

    return ap_mrb_get_status_code();
}

static int ap_mruby_run_inline(mrb_state *mrb, request_rec *r, mod_mruby_code_t *c)
{
    int ai;
    mrb_value ret;

    // mutex lock
    if (apr_thread_mutex_lock(mod_mruby_mutex) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK
            , APLOG_ERR
            , 0
            , NULL
            , "%s ERROR %s: mod_mruby_mutex lock failed"
            , MODULE_NAME
            , __func__
        );
        return OK;
    }
    ap_mrb_push_request(r);
    ai = mrb_gc_arena_save(mrb);
    ret = mrb_run(mrb
        , mrb_proc_new(mrb, mrb->irep[c->irep_n])
        , mrb_top_self(mrb)
    );
    mrb_gc_arena_restore(mrb, ai);
    // mutex unlock
    if (apr_thread_mutex_unlock(mod_mruby_mutex) != APR_SUCCESS){
        ap_log_error(APLOG_MARK
            , APLOG_ERR
            , 0
            , NULL
            , "%s ERROR %s: mod_mruby_mutex unlock failed"
            , MODULE_NAME
            , __func__
        );
        return OK;
    }

    ap_log_perror(APLOG_MARK
        , APLOG_DEBUG
        , 0
        , r->pool
        , "%s NOTICE %s: naitve code execed."
        , MODULE_NAME
        , __func__
    );

    return OK;
}

static apr_status_t mod_mruby_hook_term(void *data)
{
    mrb_close(mod_mruby_share_state);
    return APR_SUCCESS;
}

//
// hook functions (hook Ruby inline code in httpd.conf)
//
static int mod_mruby_handler_inline(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);

    if (strcmp(r->handler, "mruby-native-script") != 0)
        return DECLINED;

    return ap_mruby_run_inline(mod_mruby_share_state, r, conf->mod_mruby_handler_inline_code);
}

static int mod_mruby_translate_name_first_inline(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_translate_name_first_inline_code == NULL)
        return DECLINED;
    return ap_mruby_run_inline(mod_mruby_share_state, r, conf->mod_mruby_translate_name_first_inline_code);
}

//
// hook functions (hook Ruby file path)
//
static int mod_mruby_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    void *data = NULL;
    const char *userdata_key = "mruby_init";

    apr_status_t status = apr_thread_mutex_create(&mod_mruby_mutex, APR_THREAD_MUTEX_DEFAULT, p);
    if(status != APR_SUCCESS){
        ap_log_error(APLOG_MARK
            , APLOG_ERR        
            , 0                
            , NULL             
            , "%s ERROR %s: Error creating thread mutex."
            , MODULE_NAME
            , __func__
        );

        return status;
    }   

    ap_log_perror(APLOG_MARK
        , APLOG_INFO
        , 0                
        , p
        , "%s %s: main process / thread (pid=%d) initialized."
        , MODULE_NAME
        , __func__
        , getpid()
    );  

    apr_pool_userdata_get(&data, userdata_key, p);

    if (!data)
        apr_pool_userdata_set((const void *)1, userdata_key, apr_pool_cleanup_null, p);

    ap_log_perror(APLOG_MARK
        , APLOG_NOTICE
        , 0                
        , p
        , "%s %s: %s / %s mechanism enabled."
        , MODULE_NAME
        , __func__
        , MODULE_NAME
        , MODULE_VERSION
    );  
    
    return DECLINED;
}

static void mod_mruby_child_init(apr_pool_t *pool, server_rec *server)
{

    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);
    struct mrb_parser_state* p;

    mod_mruby_share_state = mrb_open();
    ap_mruby_class_init(mod_mruby_share_state);

    //prctl(PR_SET_KEEPCAPS,1);

    if (conf->mod_mruby_handler_inline_code != NULL) {
        p = mrb_parse_string(mod_mruby_share_state, conf->mod_mruby_handler_inline_code->code, NULL);
        conf->mod_mruby_handler_inline_code->irep_n = mrb_generate_code(mod_mruby_share_state, p);
        mrb_pool_close(p->pool);
    }

    if (conf->mod_mruby_translate_name_first_inline_code != NULL) {
        p = mrb_parse_string(mod_mruby_share_state, conf->mod_mruby_translate_name_first_inline_code->code, NULL);
        conf->mod_mruby_translate_name_first_inline_code->irep_n = mrb_generate_code(mod_mruby_share_state, p);
        mrb_pool_close(p->pool);
    }

    apr_pool_cleanup_register(pool, NULL, mod_mruby_hook_term, apr_pool_cleanup_null);
 
    ap_log_perror(APLOG_MARK
        , APLOG_INFO
        , 0
        , pool
        , "%s %s: child process (pid=%d) initialized."
        , MODULE_NAME
        , __func__
        , getpid()
    );
}

static void mod_mruby_child_init_first(apr_pool_t *pool, server_rec *server)
{
    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);

    if (conf->mod_mruby_child_init_first_code == NULL)
        return;

    ap_mruby_run_nr(conf->mod_mruby_child_init_first_code->path);
}


static void mod_mruby_child_init_middle(apr_pool_t *pool, server_rec *server)
{
    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);

    if (conf->mod_mruby_child_init_middle_code == NULL)
        return;

    ap_mruby_run_nr(conf->mod_mruby_child_init_middle_code->path);
}


static void mod_mruby_child_init_last(apr_pool_t *pool, server_rec *server)
{
    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);

    if (conf->mod_mruby_child_init_last_code == NULL)
        return;

    ap_mruby_run_nr(conf->mod_mruby_child_init_last_code->path);
}


static int mod_mruby_post_config_first(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *server)
{
    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);

    if (conf->mod_mruby_post_config_first_code == NULL)
        return DECLINED;

    ap_mruby_run_nr(conf->mod_mruby_post_config_first_code->path);
    return OK;
}


static int mod_mruby_post_config_middle(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *server)
{
    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);

    if (conf->mod_mruby_post_config_middle_code == NULL)
        return DECLINED;

    ap_mruby_run_nr(conf->mod_mruby_post_config_middle_code->path);
    return OK;
}


static int mod_mruby_post_config_last(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *server)
{
    mruby_config_t *conf = ap_get_module_config(server->module_config, &mruby_module);

    if (conf->mod_mruby_post_config_last_code == NULL)
        return DECLINED;

    ap_mruby_run_nr(conf->mod_mruby_post_config_last_code->path);
    return OK;
}

static int mod_mruby_handler(request_rec *r)
{

    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);

    if (!r->handler)
        return DECLINED;

    if (strcmp(r->handler, "mruby-script") == 0)
        conf->mod_mruby_handler_code = ap_mrb_set_file(r->pool, r->filename);
    else
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_handler_code->path, DECLINED);
}


static int mod_mruby_handler_first(request_rec *r)
{

    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);

    if (conf->mod_mruby_handler_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_handler_first_code->path, DECLINED);
}


static int mod_mruby_handler_middle(request_rec *r)
{

    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);

    if (conf->mod_mruby_handler_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_handler_middle_code->path, DECLINED);
}


static int mod_mruby_handler_last(request_rec *r)
{

    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);

    if (conf->mod_mruby_handler_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_handler_last_code->path, DECLINED);
}


static int mod_mruby_post_read_request_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_post_read_request_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_post_read_request_first_code->path, OK);
}


static int mod_mruby_post_read_request_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_post_read_request_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_post_read_request_middle_code->path, OK);
}


static int mod_mruby_post_read_request_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_post_read_request_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_post_read_request_last_code->path, OK);
}


static int mod_mruby_quick_handler_first(request_rec *r, int lookup)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_quick_handler_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_quick_handler_first_code->path, OK);
}


static int mod_mruby_quick_handler_middle(request_rec *r, int lookup)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_quick_handler_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_quick_handler_middle_code->path, OK);
}


static int mod_mruby_quick_handler_last(request_rec *r, int lookup)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_quick_handler_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_quick_handler_last_code->path, OK);
}


static int mod_mruby_translate_name_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_translate_name_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_translate_name_first_code->path, OK);
}


static int mod_mruby_translate_name_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_translate_name_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_translate_name_middle_code->path, OK);
}


static int mod_mruby_translate_name_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_translate_name_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_translate_name_last_code->path, OK);
}


static int mod_mruby_map_to_storage_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_map_to_storage_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_map_to_storage_first_code->path, OK);
}


static int mod_mruby_map_to_storage_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_map_to_storage_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_map_to_storage_middle_code->path, OK);
}


static int mod_mruby_map_to_storage_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_map_to_storage_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_map_to_storage_last_code->path, OK);
}


static int mod_mruby_access_checker_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_access_checker_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_access_checker_first_code->path, OK);
}


static int mod_mruby_access_checker_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_access_checker_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_access_checker_middle_code->path, OK);
}


static int mod_mruby_access_checker_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_access_checker_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_access_checker_last_code->path, OK);
}


static int mod_mruby_check_user_id_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_check_user_id_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_check_user_id_first_code->path, OK);
}


static int mod_mruby_check_user_id_middle(request_rec *r)
{   
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_check_user_id_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_check_user_id_middle_code->path, OK);
}


static int mod_mruby_check_user_id_last(request_rec *r)
{   
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_check_user_id_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_check_user_id_last_code->path, OK);
}


static int mod_mruby_auth_checker_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_auth_checker_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_auth_checker_first_code->path, OK);
}


static int mod_mruby_auth_checker_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_auth_checker_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_auth_checker_middle_code->path, OK);
}


static int mod_mruby_auth_checker_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_auth_checker_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_auth_checker_last_code->path, OK);
}


static int mod_mruby_fixups_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_fixups_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_fixups_first_code->path, OK);
}


static int mod_mruby_fixups_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_fixups_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_fixups_middle_code->path, OK);
}


static int mod_mruby_fixups_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_fixups_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_fixups_last_code->path, OK);
}


static void mod_mruby_insert_filter_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_insert_filter_first_code == NULL)
        return;

    ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_insert_filter_first_code->path, OK);
}


static void mod_mruby_insert_filter_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_insert_filter_middle_code == NULL)
        return;

    ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_insert_filter_middle_code->path, OK);
}


static void mod_mruby_insert_filter_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_insert_filter_last_code == NULL)
        return;

    ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_insert_filter_last_code->path, OK);
}


static int mod_mruby_log_transaction_first(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_log_transaction_first_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_log_transaction_first_code->path, OK);
}


static int mod_mruby_log_transaction_middle(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_log_transaction_middle_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_log_transaction_middle_code->path, OK);
}


static int mod_mruby_log_transaction_last(request_rec *r)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    if (conf->mod_mruby_log_transaction_last_code == NULL)
        return DECLINED;

    return ap_mruby_run(mod_mruby_share_state, r, conf, conf->mod_mruby_log_transaction_last_code->path, OK);
}


static authn_status mod_mruby_authn_check_password(request_rec *r, const char *user, const char *password)
{
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    mruby_dir_config_t *dir_conf = ap_get_module_config(r->per_dir_config, &mruby_module);
    if (dir_conf->mod_mruby_authn_check_password_code == NULL)
        return AUTH_GENERAL_ERROR;

    ap_mrb_init_authnprovider_basic(r, user, password);
    return ap_mruby_run(mod_mruby_share_state, r, conf, dir_conf->mod_mruby_authn_check_password_code->path, OK);
}


static authn_status mod_mruby_authn_get_realm_hash(request_rec *r, const char *user, const char *realm, char **rethash)
{
    authn_status ret;
    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    mruby_dir_config_t *dir_conf = ap_get_module_config(r->per_dir_config, &mruby_module);
    if (dir_conf->mod_mruby_authn_get_realm_hash_code == NULL)
        return AUTH_GENERAL_ERROR;

    ap_mrb_init_authnprovider_digest(r, user, realm);
    ret = ap_mruby_run(mod_mruby_share_state, r, conf, dir_conf->mod_mruby_authn_get_realm_hash_code->path, OK);
    *rethash = ap_mrb_get_authnprovider_digest_rethash();
    return ret;
}


static apr_status_t mod_mruby_output_filter(ap_filter_t* f, apr_bucket_brigade* bb)
{
    apr_status_t rv;
    request_rec *r = f->r;

    mruby_config_t *conf = ap_get_module_config(r->server->module_config, &mruby_module);
    mruby_dir_config_t *dir_conf = ap_get_module_config(r->per_dir_config, &mruby_module);

    if (dir_conf->mod_mruby_output_filter_code == NULL)
        return ap_pass_brigade(f->next, bb);

    ap_mrb_push_filter(f, bb);
    rv = ap_mruby_run(mod_mruby_share_state, r, conf, dir_conf->mod_mruby_output_filter_code->path, OK);
    return ap_pass_brigade(f->next, bb);
}


static const authn_provider authn_mruby_provider = {
    &mod_mruby_authn_check_password,
    &mod_mruby_authn_get_realm_hash
};


static void register_hooks(apr_pool_t *p)
{
    // inline code in httpd.conf
    ap_hook_handler(mod_mruby_handler_inline, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_translate_name(mod_mruby_translate_name_first_inline, NULL, NULL, APR_HOOK_FIRST);

    // hook script file
    ap_hook_post_config(mod_mruby_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(mod_mruby_post_config_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_post_config(mod_mruby_post_config_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(mod_mruby_post_config_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_child_init(mod_mruby_child_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(mod_mruby_child_init_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_child_init(mod_mruby_child_init_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(mod_mruby_child_init_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_handler(mod_mruby_handler, NULL, NULL, APR_HOOK_REALLY_FIRST);
    ap_hook_handler(mod_mruby_handler_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_handler(mod_mruby_handler_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(mod_mruby_handler_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_post_read_request(mod_mruby_post_read_request_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_post_read_request(mod_mruby_post_read_request_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_read_request(mod_mruby_post_read_request_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_quick_handler(mod_mruby_quick_handler_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_quick_handler(mod_mruby_quick_handler_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_quick_handler(mod_mruby_quick_handler_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_translate_name(mod_mruby_translate_name_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_translate_name(mod_mruby_translate_name_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_translate_name(mod_mruby_translate_name_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_map_to_storage(mod_mruby_map_to_storage_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_map_to_storage(mod_mruby_map_to_storage_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_map_to_storage(mod_mruby_map_to_storage_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_access_checker(mod_mruby_access_checker_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_access_checker(mod_mruby_access_checker_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_access_checker(mod_mruby_access_checker_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_check_user_id(mod_mruby_check_user_id_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_check_user_id(mod_mruby_check_user_id_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_check_user_id(mod_mruby_check_user_id_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_auth_checker(mod_mruby_auth_checker_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_auth_checker(mod_mruby_auth_checker_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_auth_checker(mod_mruby_auth_checker_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_fixups(mod_mruby_fixups_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_fixups(mod_mruby_fixups_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_fixups(mod_mruby_fixups_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_insert_filter(mod_mruby_insert_filter_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_insert_filter(mod_mruby_insert_filter_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_insert_filter(mod_mruby_insert_filter_last, NULL, NULL, APR_HOOK_LAST);
    ap_hook_log_transaction(mod_mruby_log_transaction_first, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_log_transaction(mod_mruby_log_transaction_middle, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_log_transaction(mod_mruby_log_transaction_last, NULL, NULL, APR_HOOK_LAST);

    ap_register_provider(p, AUTHN_PROVIDER_GROUP, "mruby", "0", &authn_mruby_provider);

    ap_register_output_filter("mruby", mod_mruby_output_filter, NULL, AP_FTYPE_CONTENT_SET);
    //ap_register_input_filter( "MODMRUBYFILTER", mod_mruby_input_filter,  NULL, AP_FTYPE_CONTENT_SET);
}


static const command_rec mod_mruby_cmds[] = {

    AP_INIT_TAKE1("mrubyHandlerCode", set_mod_mruby_handler_inline, NULL, RSRC_CONF | ACCESS_CONF, "hook inline code for handler phase."),
    AP_INIT_TAKE1("mrubyTranslateNameFirstCode", set_mod_mruby_translate_name_first_inline, NULL, RSRC_CONF | ACCESS_CONF, "hook inline code for translate_name first phase."),

    AP_INIT_TAKE1("mrubyHandler", set_mod_mruby_handler, NULL, RSRC_CONF | ACCESS_CONF, "hook for handler phase."),
    AP_INIT_TAKE1("mrubyHandlerFIrst", set_mod_mruby_handler_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for handler first phase."),
    AP_INIT_TAKE1("mrubyHandlerMiddle", set_mod_mruby_handler_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for handler middle phase."),
    AP_INIT_TAKE1("mrubyHandlerLast", set_mod_mruby_handler_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for handler last phase."),
    AP_INIT_TAKE1("mrubyPostConfigFirst", set_mod_mruby_post_config_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for post_config fast phase."),
    AP_INIT_TAKE1("mrubyPostConfigMiddle", set_mod_mruby_post_config_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for post_config middle phase."),
    AP_INIT_TAKE1("mrubyPostConfigLast", set_mod_mruby_post_config_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for post_config last phase."),
    AP_INIT_TAKE1("mrubyChildInitFirst", set_mod_mruby_child_init_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for child_init first phase."),
    AP_INIT_TAKE1("mrubyChildInitMiddle", set_mod_mruby_child_init_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for child_init middle phase."),
    AP_INIT_TAKE1("mrubyChildInitLast", set_mod_mruby_child_init_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for child_init last phase."),
    AP_INIT_TAKE1("mrubyPostReadRequestFirst", set_mod_mruby_post_read_request_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for post_read_request first phase."),
    AP_INIT_TAKE1("mrubyPostReadRequestMiddle", set_mod_mruby_post_read_request_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for post_read_request middle phase."),
    AP_INIT_TAKE1("mrubyPostReadRequestLast", set_mod_mruby_post_read_request_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for post_read_request last phase."),
    AP_INIT_TAKE1("mrubyQuickHandlerFirst", set_mod_mruby_quick_handler_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for quick_handler first phase."),
    AP_INIT_TAKE1("mrubyQuickHandlerMiddle", set_mod_mruby_quick_handler_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for quick_handler middle phase."),
    AP_INIT_TAKE1("mrubyQuickHandlerLast", set_mod_mruby_quick_handler_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for quick_handler last phase."),
    AP_INIT_TAKE1("mrubyTranslateNameFirst", set_mod_mruby_translate_name_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for translate_name first phase."),
    AP_INIT_TAKE1("mrubyTranslateNameMiddle", set_mod_mruby_translate_name_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for translate_name middle phase."),
    AP_INIT_TAKE1("mrubyTranslateNameLast", set_mod_mruby_translate_name_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for translate_name last phase."),
    AP_INIT_TAKE1("mrubyMapToStorageFirst", set_mod_mruby_map_to_storage_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for map_to_storage first phase."),
    AP_INIT_TAKE1("mrubyMapToStorageMiddle", set_mod_mruby_map_to_storage_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for map_to_storage middle phase."),
    AP_INIT_TAKE1("mrubyMapToStorageLast", set_mod_mruby_map_to_storage_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for map_to_storage last phase."),
    AP_INIT_TAKE1("mrubyAccessCheckerFirst", set_mod_mruby_access_checker_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for access_checker first phase."),
    AP_INIT_TAKE1("mrubyAccessCheckerMiddle", set_mod_mruby_access_checker_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for access_checker middle phase."),
    AP_INIT_TAKE1("mrubyAccessCheckerLast", set_mod_mruby_access_checker_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for access_checker last phase."),
    AP_INIT_TAKE1("mrubyCheckUserIdFirst", set_mod_mruby_check_user_id_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for check_user_id first phase."),
    AP_INIT_TAKE1("mrubyCheckUserIdMiddle", set_mod_mruby_check_user_id_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for check_user_id middle phase."),
    AP_INIT_TAKE1("mrubyCheckUserIdLast", set_mod_mruby_check_user_id_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for check_user_id last phase."),
    AP_INIT_TAKE1("mrubyAuthCheckerFirst", set_mod_mruby_auth_checker_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for auth_checker first phase."),
    AP_INIT_TAKE1("mrubyAuthCheckerMiddle", set_mod_mruby_auth_checker_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for auth_checker middle phase."),
    AP_INIT_TAKE1("mrubyAuthCheckerLast", set_mod_mruby_auth_checker_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for auth_checker last phase."),
    AP_INIT_TAKE1("mrubyFixupsFirst", set_mod_mruby_fixups_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for fixups first phase."),
    AP_INIT_TAKE1("mrubyFixupsMiddle", set_mod_mruby_fixups_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for fixups middle phase."),
    AP_INIT_TAKE1("mrubyFixupsLast", set_mod_mruby_fixups_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for fixups last phase."),
    AP_INIT_TAKE1("mrubyInsertFilterFirst", set_mod_mruby_insert_filter_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for insert_filter first phase."),
    AP_INIT_TAKE1("mrubyInsertFilterMiddle", set_mod_mruby_insert_filter_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for insert_filter middle phase."),
    AP_INIT_TAKE1("mrubyInsertFilterLast", set_mod_mruby_insert_filter_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for insert_filter last phase."),
    AP_INIT_TAKE1("mrubyLogTransactionFirst", set_mod_mruby_log_transaction_first, NULL, RSRC_CONF | ACCESS_CONF, "hook for log_transaction first phase."),
    AP_INIT_TAKE1("mrubyLogTransactionMiddle", set_mod_mruby_log_transaction_middle, NULL, RSRC_CONF | ACCESS_CONF, "hook for log_transaction middle phase."),
    AP_INIT_TAKE1("mrubyLogTransactionLast", set_mod_mruby_log_transaction_last, NULL, RSRC_CONF | ACCESS_CONF, "hook for log_transaction last phase."),
    //AP_INIT_TAKE1("mrubyCacheSize", set_mod_mruby_cache_table_size, NULL, RSRC_CONF | ACCESS_CONF, "set mruby cache table size."),

    AP_INIT_TAKE1("mrubyAuthnCheckPassword", set_mod_mruby_authn_check_password, NULL, OR_AUTHCFG, "hook for authn basic."),
    AP_INIT_TAKE1("mrubyAuthnGetRealmHash", set_mod_mruby_authn_get_realm_hash, NULL, OR_AUTHCFG, "hook for authn digest."),

    AP_INIT_TAKE1("mrubyOutputFilter", set_mod_mruby_output_filter, NULL, OR_OPTIONS, "set mruby output filter script."),

    {NULL}
};


module AP_MODULE_DECLARE_DATA mruby_module = {
    STANDARD20_MODULE_STUFF,
    mod_mruby_create_dir_config,    /* dir config creater */
    NULL,                      /* dir merger */
    mod_mruby_create_config,   /* server config */
    NULL,                      /* merge server config */
    mod_mruby_cmds,            /* command apr_table_t */
    register_hooks             /* register hooks */
};
