#include <postgres.h>

//#include <access/printtup.h>
#include <access/xact.h>
//#include <catalog/heap.h>
//#include <catalog/namespace.h>
//#include <catalog/pg_type.h>
#include <commands/async.h>
//#include <commands/dbcommands.h>
#include <commands/extension.h>
#include <commands/prepare.h>
//#include <commands/user.h>
#include <common/ip.h>
#include <executor/spi.h>
#include <fe_utils/recovery_gen.h>
//#include <fe_utils/string_utils.h>
#include <funcapi.h>
//#include <jit/jit.h>
#include <libpq-fe.h>
#include <libpq/libpq-be.h>
#include <libpq/pqformat.h>
#include <libpq/libpq.h>
//#include <miscadmin.h>
//#include <nodes/makefuncs.h>
//#include <parser/analyze.h>
#include <parser/parse_func.h>
#include <parser/parse_type.h>
#include <pgstat.h>
//#include <postgresql/internal/pqexpbuffer.h>
#include <postmaster/bgworker.h>
#include <postmaster/interrupt.h>
#include <replication/slot.h>
#include <replication/syncrep.h>
//#include <replication/syncrep.h>
#include <replication/walreceiver.h>
#include <replication/walsender_private.h>
#include <storage/ipc.h>
#include <sys/utsname.h>
//#include <tcop/pquery.h>
#include <tcop/utility.h>
//#include <utils/acl.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
//#include <utils/ps_status.h>
#include <utils/regproc.h>
//#include <utils/snapmgr.h>
#include <utils/timeout.h>
//#include <utils/typcache.h>

#define EXTENSION(function) Datum (function)(PG_FUNCTION_ARGS); PG_FUNCTION_INFO_V1(function); Datum (function)(PG_FUNCTION_ARGS)

#define FORMAT_0(fmt, ...) "%s(%s:%d): %s", __func__, __FILE__, __LINE__, fmt
#define FORMAT_1(fmt, ...) "%s(%s:%d): " fmt,  __func__, __FILE__, __LINE__
#define GET_FORMAT(fmt, ...) GET_FORMAT_PRIVATE(fmt, 0, ##__VA_ARGS__, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
#define GET_FORMAT_PRIVATE(fmt, \
      _0,  _1,  _2,  _3,  _4,  _5,  _6,  _7,  _8,  _9, \
     _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, \
     _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, \
     _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, \
     _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, \
     _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, \
     _60, _61, _62, _63, _64, _65, _66, _67, _68, _69, \
     _70, format, ...) FORMAT_ ## format(fmt)

#define D1(fmt, ...) ereport(DEBUG1, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D2(fmt, ...) ereport(DEBUG2, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D3(fmt, ...) ereport(DEBUG3, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D4(fmt, ...) ereport(DEBUG4, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define D5(fmt, ...) ereport(DEBUG5, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define E(fmt, ...) ereport(ERROR, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define F(fmt, ...) ereport(FATAL, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define I(fmt, ...) ereport(INFO, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define L(fmt, ...) ereport(LOG, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define N(fmt, ...) ereport(NOTICE, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))
#define W(fmt, ...) ereport(WARNING, (errmsg(GET_FORMAT(fmt, ##__VA_ARGS__), ##__VA_ARGS__)))

#define countof(array) (sizeof(array)/sizeof(array[0]))

PG_MODULE_MAGIC;

#define NOTIFY_PAYLOAD_MAX_LENGTH (BLCKSZ - NAMEDATALEN - 128)
typedef struct pg_queue_shmem_t {
    char channel[NAMEDATALEN];
    char payload[NOTIFY_PAYLOAD_MAX_LENGTH];
    int32 pid;
    slock_t mutex;
} pg_queue_shmem_t;

static int pg_queue_size;
static pg_queue_shmem_t *pg_queue_shmem;
static pqsigfunc pg_queue_signal_original = NULL;
static shmem_startup_hook_type pg_queue_shmem_startup_hook_original = NULL;

static void pg_queue_signal(SIGNAL_ARGS) {
    char *channel;
    char *payload;
    int32 pid;
    SpinLockAcquire(&pg_queue_shmem->mutex);
    channel = pstrdup(pg_queue_shmem->channel);
    payload = pstrdup(pg_queue_shmem->payload);
    pid = pg_queue_shmem->pid;
    SpinLockRelease(&pg_queue_shmem->mutex);
    D1("channel = %s, payload = %s, pid = %i", channel, payload, pid);
    NotifyMyFrontEnd(channel, payload, pid);
    pq_flush();
    pg_queue_signal_original(postgres_signal_arg);
    pfree(channel);
    pfree(payload);
}

static void pg_queue_shmem_startup_hook(void) {
    bool found;
    if (pg_queue_shmem_startup_hook_original) pg_queue_shmem_startup_hook_original();
    pg_queue_shmem = NULL;
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    pg_queue_shmem = ShmemInitStruct("pg_queue", sizeof(*pg_queue_shmem), &found);
    if (!found) {
        MemSet(pg_queue_shmem, 0, sizeof(pg_queue_shmem));
        SpinLockInit(&pg_queue_shmem->mutex);
    }
    LWLockRelease(AddinShmemInitLock);
}

void _PG_fini(void); void _PG_fini(void) {
    shmem_startup_hook = pg_queue_shmem_startup_hook_original;
}

void _PG_init(void); void _PG_init(void) {
    if (!process_shared_preload_libraries_in_progress) return;
    D1("hi");
    DefineCustomIntVariable("pg_queue.size", "pg_queue size", NULL, &pg_queue_size, 1024, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
    pg_queue_shmem_startup_hook_original = shmem_startup_hook;
    shmem_startup_hook = pg_queue_shmem_startup_hook;
    RequestAddinShmemSpace(MAXALIGN(sizeof(*pg_queue_shmem)));
}

EXTENSION(pg_queue_listen) {
    const char *channel = PG_ARGISNULL(0) ? "" : text_to_cstring(PG_GETARG_TEXT_PP(0));
    D1("channel = %s", channel);
    if (!pg_queue_signal_original) pg_queue_signal_original = pqsignal(SIGUSR1, pg_queue_signal);
    PG_RETURN_VOID();
}

static void pg_queue_kill(void) {
    int num_backends = pgstat_fetch_stat_numbackends();
    for (int curr_backend = 1; curr_backend <= num_backends; curr_backend++) {
        LocalPgBackendStatus *local_beentry;
        PgBackendStatus *beentry;
        if (!(local_beentry = pgstat_fetch_stat_local_beentry(curr_backend))) continue;
        beentry = &local_beentry->backendStatus;
        if (kill(beentry->st_procpid, SIGUSR1)) W("kill");
    }
}

EXTENSION(pg_queue_notify) {
    const char *channel = PG_ARGISNULL(0) ? "" : text_to_cstring(PG_GETARG_TEXT_PP(0));
    const char *payload = PG_ARGISNULL(1) ? "" : text_to_cstring(PG_GETARG_TEXT_PP(1));
    D1("channel = %s, payload = %s", channel, payload);
    SpinLockAcquire(&pg_queue_shmem->mutex);
    memcpy(pg_queue_shmem->channel, channel, strlen(channel));
    memcpy(pg_queue_shmem->payload, payload, strlen(payload));
    pg_queue_shmem->pid = MyProcPid;
    SpinLockRelease(&pg_queue_shmem->mutex);
    pg_queue_kill();
    PG_RETURN_VOID();
}