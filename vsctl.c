#include "compiler.h"
#include "ovsdb-idl.h"
#include "vswitch-idl.h"
#include "util.h"
#include "dirs.h"
#include "poll-loop.h"
#include "openvswitch/types.h"

/* --db: The database server to contact. */
static const char *db;

bool wait_for_reload = true;

static char *
default_db(void)
{
    static char *def;
    if (!def) {
        def = xasprintf("unix:%s/db.sock", ovs_rundir());
    }
    return def;
}

static void
ovs_insert_bridge(const struct ovsrec_open_vswitch *ovs,
                  struct ovsrec_bridge *bridge)
{
    struct ovsrec_bridge **bridges;
    size_t i;

    bridges = xmalloc(sizeof *ovs->bridges * (ovs->n_bridges + 1));
    for (i = 0; i < ovs->n_bridges; i++) {
        bridges[i] = ovs->bridges[i];
    }
    bridges[ovs->n_bridges] = bridge;
    ovsrec_open_vswitch_set_bridges(ovs, bridges, ovs->n_bridges + 1);
    free(bridges);
}

void
_add_br(struct ovsdb_idl_txn *txn, const struct ovsrec_open_vswitch *ovs,
        const char *br_name)
{
    struct ovsrec_port *port;
    struct ovsrec_bridge *br;
    struct ovsrec_interface *iface;

    iface = ovsrec_interface_insert(txn);
    ovsrec_interface_set_name(iface, br_name);
    ovsrec_interface_set_type(iface, "internal");

    port = ovsrec_port_insert(txn);
    ovsrec_port_set_name(port, br_name);
    ovsrec_port_set_interfaces(port, &iface, 1);

    br = ovsrec_bridge_insert(txn);
    ovsrec_bridge_set_name(br, br_name);
    ovsrec_bridge_set_ports(br, &port, 1);

    ovs_insert_bridge(ovs, br);

    //post_db_reload_expect_iface(iface);
}

int
do_br_op(struct ovsdb_idl *idl, const char *cmd, const char *br_name)
{
    struct ovsdb_idl_txn *txn;
    const struct ovsrec_open_vswitch *ovs;
    struct ovsdb_symbol_table *symtab;
    enum ovsdb_idl_txn_status status;
    struct shash_node *node;
    int64_t next_cfg = 0;
    char *error = NULL;

    txn = ovsdb_idl_txn_create(idl);

    ovsdb_idl_txn_add_comment(txn, "%s:%s", __func__, br_name);

    ovs = ovsrec_open_vswitch_first(idl);
    if (!ovs) {
        /* XXX add verification that table is empty */
        ovs = ovsrec_open_vswitch_insert(txn);
    }

    /* Wait for vswitchd to reload it's config. In ovs-vsctl, this is performed
     * if wait_for_reload is set (true by default).
     */
    if (wait_for_reload)
        ovsdb_idl_txn_increment(txn, &ovs->header_,
                                &ovsrec_open_vswitch_col_next_cfg);

    symtab = ovsdb_symbol_table_create();

    if (strcmp(cmd, "add-br") == 0) {
        _add_br(txn, ovs, br_name);
    }

    SHASH_FOR_EACH (node, &symtab->sh) {
        struct ovsdb_symbol *symbol = node->data;
        if (!symbol->created) {
            /* fatal */
            printf("row id \"%s\" is referenced but never created (e.g. "
                   "with \"-- --id=%s create ...\")\n",
                   node->name, node->name);
            goto error_out;
        }
        if (!symbol->strong_ref) {
            if (!symbol->weak_ref) {
                /* warning */
                printf("row id \"%s\" was created but no reference to it "
                       "was inserted, so it will not actually appear in "
                       "the database\n", node->name);
            } else {
                /* warning */
                printf("row id \"%s\" was created but only a weak "
                       "reference to it was inserted, so it will not "
                       "actually appear in the database\n", node->name);
            }
        }
    }

    status = ovsdb_idl_txn_commit_block(txn);
    if (wait_for_reload && status == TXN_SUCCESS) {
        next_cfg = ovsdb_idl_txn_get_increment_new_value(txn);
    }
    if (status == TXN_UNCHANGED || status == TXN_SUCCESS) {
        /* Perform whatever postprocess work is depicted for the command. For
         * adding and deleting bridges and ports, no postprocess work is needed
         * according to ovs-vsctl.c.
         */
        ;
    }
    error = xstrdup(ovsdb_idl_txn_get_error(txn));

    switch (status) {
    case TXN_UNCOMMITTED:
    case TXN_INCOMPLETE:
        /* OVS_NOT_REACHED() == abort() - we will just return an error. */
        goto error_out;

    case TXN_ABORTED:
        /* Should not happen--we never call ovsdb_idl_txn_abort(). */
        printf("transaction aborted");
        goto error_out;

    case TXN_UNCHANGED:
    case TXN_SUCCESS:
        break;

    case TXN_TRY_AGAIN:
        goto try_again;

    case TXN_ERROR:
        printf("transaction error: %s\n", error);
        goto error_out;

    default:
        /* OVS_NOT_REACHED() == abort() - we will just return an error. */
        goto error_out;
    }
    free(error);

    if (wait_for_reload && status != TXN_UNCHANGED) {
        /* Even, if --retry flag was not specified, ovs-vsctl still
         * has to retry to establish OVSDB connection, if wait_for_reload
         * was set.  Otherwise, ovs-vsctl would end up waiting forever
         * until cur_cfg would be updated. */
        ovsdb_idl_enable_reconnect(idl);
        for (;;) {
            ovsdb_idl_run(idl);
            OVSREC_OPEN_VSWITCH_FOR_EACH (ovs, idl) {
                if (ovs->cur_cfg >= next_cfg) {
                    /* TODO: look into this functions.
                     * post_db_reload_do_checks(&ctx);
                     */
                    goto done;
                }
            }
            ovsdb_idl_wait(idl);
            poll_block();
        }
    done: ;
    }

    ovsdb_idl_txn_destroy(txn);
    ovsdb_idl_destroy(idl);
    return 0;

error_out:
    free(error);
    ovsdb_idl_txn_destroy(txn);
    ovsdb_idl_destroy(idl);
    return -1;

try_again:
    /* Our transaction needs to be rerun, or a prerequisite was not met.  Free
     * resources and return so that the caller can try again. */
    if (txn) {
        ovsdb_idl_txn_abort(txn);
        ovsdb_idl_txn_destroy(txn);
        /* TODO: we don't have a global idl_txn struct - need it?
         * the_idl_txn = NULL;
         */
    }
    ovsdb_symbol_table_destroy(symtab);
    free(error);

    printf("returning ovsdb_idl_txn status (%d)\n", status);
    return status;
}

static void
pre_get_info(struct ovsdb_idl *idl)
{
    ovsdb_idl_add_column(idl, &ovsrec_open_vswitch_col_bridges);

    ovsdb_idl_add_column(idl, &ovsrec_bridge_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_bridge_col_controller);
    ovsdb_idl_add_column(idl, &ovsrec_bridge_col_fail_mode);
    ovsdb_idl_add_column(idl, &ovsrec_bridge_col_ports);

    ovsdb_idl_add_column(idl, &ovsrec_port_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_fake_bridge);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_tag);
    ovsdb_idl_add_column(idl, &ovsrec_port_col_interfaces);

    ovsdb_idl_add_column(idl, &ovsrec_interface_col_name);
    ovsdb_idl_add_column(idl, &ovsrec_interface_col_ofport);
}

static void
run_prerequisites(const char *cmd, struct ovsdb_idl *idl)
{
    ovsdb_idl_add_table(idl, &ovsrec_table_open_vswitch);
    if (wait_for_reload) {
        ovsdb_idl_add_column(idl, &ovsrec_open_vswitch_col_cur_cfg);
    }

    if (strcmp(cmd, "add-br") == 0) {
        /* This needs to be run before the first call to ovsdb_idl_run()
         * to ensure that column additions are done first. Otherwise,
         * we will blow an assertion in ovsdb_idl_get_mode().
         */
        pre_get_info(idl);
    }
}

int
main(int argc, char **argv)
{
    struct ovsdb_idl *idl;
    unsigned int seqno;
    int ret;
    char *cmd;
    char *br_name;

    if (argc < 3) {
        printf("not enough arguments\n");
        return -1;
    }

    cmd = argv[1];
    br_name = argv[2];

    ovsrec_init();

    db = default_db();

    /* db, ovsrec_idl_class, false, false (retry) */
    idl = ovsdb_idl_create(db, &ovsrec_idl_class, false, false);

    /* add columns, etc - per ovs-vsctl.c:run_prerequistes() */
    run_prerequisites(cmd, idl);

    seqno = ovsdb_idl_get_seqno(idl);
    for (;;) {
        ovsdb_idl_run(idl);
        if (!ovsdb_idl_is_alive(idl)) {
            int retval = ovsdb_idl_get_last_error(idl);
            printf("%s: database connection failed (%s)\n",
                   db, ovs_retval_to_string(retval));
            /* returns json rpc error code (>0) */
            return retval;
        }

        if (seqno != ovsdb_idl_get_seqno(idl)) {
            seqno = ovsdb_idl_get_seqno(idl);
            if ((strcmp(cmd, "add-br") == 0) ||
                (strcmp(cmd, "del-br") == 0)) {
                ret = do_br_op(idl, cmd, br_name);
            } else {
                printf("unsupported command\n");
                ret = -1;
            }

            if (ret == TXN_TRY_AGAIN)
                continue;
            printf("ret: %d\n", ret);
            return ret;
        }

        if (seqno == ovsdb_idl_get_seqno(idl)) {
            printf("blocking/waiting\n");
            ovsdb_idl_wait(idl);
            poll_block();
        }
    }

    /* shouldn't get here */
    return -1;
}
