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
    idl = ovsdb_idl_create(db, &ovsrec_idl_class, false, true);

    seqno = ovsdb_idl_get_seqno(idl);
    for (;;) {
        ovsdb_idl_run(idl);
        if (!ovsdb_idl_is_alive(idl)) {
            int retval = ovsdb_idl_get_last_error(idl);
            printf("%s: database connection failed (%s)",
                   db, ovs_retval_to_string(retval));
            return retval;
        }

        if (seqno != ovsdb_idl_get_seqno(idl)) {
            seqno = ovsdb_idl_get_seqno(idl);
            printf("now do something with it... \n");
        }

        if (seqno == ovsdb_idl_get_seqno(idl)) {
            ovsdb_idl_wait(idl);
            poll_block();
        }
    }
    
    return 1;
}
