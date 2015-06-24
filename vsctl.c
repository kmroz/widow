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
