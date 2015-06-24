#include "compiler.h"
#include "ovsdb-idl.h"
#include "vswitch-idl.h"
#include "util.h"
#include "dirs.h"
#include "poll-loop.h"
#include "openvswitch/types.h"

/* --db: The database server to contact. */
static const char *db;

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
add_br(struct ovsdb_idl *idl, const char *br_name)
{
    struct ovsdb_idl_txn *txn;
    const struct ovsrec_open_vswitch *ovs;
    __attribute__((unused)) struct ovsdb_symbol_table *symtab;
    enum ovsdb_idl_txn_status status;
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
    ovsdb_idl_txn_increment(txn, &ovs->header_,
                            &ovsrec_open_vswitch_col_next_cfg);

    symtab = ovsdb_symbol_table_create();

    /* TODO: do the actual bridge addition steps */
    _add_br(txn, ovs, br_name);

    status = ovsdb_idl_txn_commit_block(txn);
    error = xstrdup(ovsdb_idl_txn_get_error(txn));

    switch (status) {
    case TXN_ERROR:
        printf("transaction error: %s", error);
        break;
    default:
        break;
    }

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
    bool wait_for_reload = true;

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
            if (strcmp(cmd, "add-br") == 0) {
                ret = add_br(idl, br_name);
            } else {
                printf("unsupported command\n");
                ret = -1;
            }
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
