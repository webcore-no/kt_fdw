/*-------------------------------------------------------------------------
 *
 * Kyoto Tycoon Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2013 CloudFlare
 *
 * This software is released under the MIT Licence
 *
 * Author: Matvey Arye <mat@cloudflare.com>
 *
 * IDENTIFICATION
 *        kt_fdw/src/kt_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include <stdint.h>
#include <sys/time.h>
#include "postgres.h"
#if PG_VERSION_NUM >= 130000
	#include "common/hashfn.h"
#endif
#include "ktlangc.h"

#include "access/reloptions.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"

#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "access/xact.h"

PG_MODULE_MAGIC;

// taken from redis_fdw
#define PROCID_TEXTEQ 67
//#define DEBUG 1

/*
 * SQL functions
 */
extern Datum kt_fdw_handler(PG_FUNCTION_ARGS);
extern Datum kt_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(kt_fdw_handler);
PG_FUNCTION_INFO_V1(kt_fdw_validator);

extern HeapTuple heap_form_tuple(TupleDesc tupleDescriptor,
                                 Datum *values,
                                 bool *isnull);

/* callback functions */
static void ktGetForeignRelSize(PlannerInfo *root,
                                RelOptInfo *baserel,
                                Oid foreigntableid);

static void ktGetForeignPaths(PlannerInfo *root,
                              RelOptInfo *baserel,
                              Oid foreigntableid);

static ForeignScan *ktGetForeignPlan(PlannerInfo *root,
                                     RelOptInfo *baserel,
                                     Oid foreigntableid,
                                     ForeignPath *best_path,
                                     List *tlist,
                                     List *scan_clauses,
                                     Plan *plan);

static void ktBeginForeignScan(ForeignScanState *node, int eflags);

static TupleTableSlot *ktIterateForeignScan(ForeignScanState *node);

static void ktReScanForeignScan(ForeignScanState *node);

static void ktEndForeignScan(ForeignScanState *node);

static void ktAddForeignUpdateTargets(Query *parsetree,
                                      RangeTblEntry *target_rte,
                                      Relation target_relation);

static List *ktPlanForeignModify(PlannerInfo *root,
                                 ModifyTable *plan,
                                 Index resultRelation,
                                 int subplan_index);

static void ktBeginForeignModify(ModifyTableState *mtstate,
                                 ResultRelInfo *rinfo,
                                 List *fdw_private,
                                 int subplan_index,
                                 int eflags);

static TupleTableSlot *ktExecForeignInsert(EState *estate,
                                           ResultRelInfo *rinfo,
                                           TupleTableSlot *slot,
                                           TupleTableSlot *planSlot);

static TupleTableSlot *ktExecForeignUpdate(EState *estate,
                                           ResultRelInfo *rinfo,
                                           TupleTableSlot *slot,
                                           TupleTableSlot *planSlot);

static TupleTableSlot *ktExecForeignDelete(EState *estate,
                                           ResultRelInfo *rinfo,
                                           TupleTableSlot *slot,
                                           TupleTableSlot *planSlot);

static void ktEndForeignModify(EState *estate, ResultRelInfo *rinfo);

static void ktExplainForeignScan(ForeignScanState *node,
                                 struct ExplainState *es);

static void ktExplainForeignModify(ModifyTableState *mtstate,
                                   ResultRelInfo *rinfo,
                                   List *fdw_private,
                                   int subplan_index,
                                   struct ExplainState *es);

static bool ktAnalyzeForeignTable(Relation relation,
                                  AcquireSampleRowsFunc *func,
                                  BlockNumber *totalpages);

/*
 * structures used by the FDW
 *
 * These next two are not actualkly used by kt, but something like this
 * will be needed by anything more complicated that does actual work.
 *
 */

/*
 * Describes the valid options for objects that use this wrapper.
 */
struct ktFdwOption {
	const char *optname;
	Oid optcontext; /* Oid of catalog in which option may appear */
};

static struct ktFdwOption valid_options[] = {
        /* Connection options */
        {"host", ForeignServerRelationId},
        {"port", ForeignServerRelationId},
        {"timeout", ForeignServerRelationId},
        {"reconnect_timeout", ForeignServerRelationId},
        /* Sentinel */
        {NULL, InvalidOid}};

typedef struct ktTableOptions {
	char *host;
	int32_t port;
	double timeout;
	int64_t reconnect_timeout;
	Oid serverId;
	Oid userId;
} ktTableOptions;
/*
 * This is what will be set and stashed away in fdw_private and fetched
 * for subsequent routines.
 */
typedef struct {
	ktTableOptions opt;
} KtFdwPlanState;

typedef struct {
	KtFdwPlanState plan;
	KTCUR *cur;
	KTDB *db;
	bool key_based_qual;
	char *key_based_qual_value;
	bool key_based_qual_sent;
	AttInMetadata *attinmeta;
} KtFdwExecState;

typedef struct {
	ktTableOptions opt;
	KTDB *db;
	Relation rel;
	FmgrInfo *key_info;
	FmgrInfo *value_info;
	FmgrInfo *flags_info;
	AttrNumber key_junk_no;
} KtFdwModifyState;

typedef struct {
	Oid serverId;
	Oid userId;
} KtConnCacheKey;

typedef struct {
	KtConnCacheKey key;
	KTDB *db;
	int xact_depth;
	int64_t creation_time;
} KtConnCacheEntry;

static HTAB *ConnectionHash = NULL;

#ifdef USE_TRANSACTIONS
/* needed to shortcut end of transaction logic*/
static bool xact_got_connection = false;
#endif

void initTableOptions(struct ktTableOptions *table_options);
KTDB *GetKtConnection(struct ktTableOptions *table_options);
void ReleaseKtConnection(KTDB *db);

Datum kt_fdw_handler(PG_FUNCTION_ARGS UNUSED)
{
	FdwRoutine *fdwroutine = makeNode(FdwRoutine);

	elog(DEBUG1, "entering function %s", __func__);

	/* assign the handlers for the FDW */

	/* these are required */
	fdwroutine->GetForeignRelSize  = ktGetForeignRelSize;
	fdwroutine->GetForeignPaths    = ktGetForeignPaths;
	fdwroutine->GetForeignPlan     = ktGetForeignPlan;
	fdwroutine->BeginForeignScan   = ktBeginForeignScan;
	fdwroutine->IterateForeignScan = ktIterateForeignScan;
	fdwroutine->ReScanForeignScan  = ktReScanForeignScan;
	fdwroutine->EndForeignScan     = ktEndForeignScan;

	/* remainder are optional - use NULL if not required */
	/* support for insert / update / delete */
	fdwroutine->AddForeignUpdateTargets = ktAddForeignUpdateTargets;
	fdwroutine->PlanForeignModify       = ktPlanForeignModify;
	fdwroutine->BeginForeignModify      = ktBeginForeignModify;
	fdwroutine->ExecForeignInsert       = ktExecForeignInsert;
	fdwroutine->ExecForeignUpdate       = ktExecForeignUpdate;
	fdwroutine->ExecForeignDelete       = ktExecForeignDelete;
	fdwroutine->EndForeignModify        = ktEndForeignModify;

	/* support for EXPLAIN */
	fdwroutine->ExplainForeignScan   = ktExplainForeignScan;
	fdwroutine->ExplainForeignModify = ktExplainForeignModify;

	/* support for ANALYSE */
	fdwroutine->AnalyzeForeignTable = ktAnalyzeForeignTable;

	PG_RETURN_POINTER(fdwroutine);
}

typedef struct {
	const char *name;
	const char *message;
} ktErrorMessage;

static bool KtOpenConnection(KtConnCacheEntry *entry,
                             struct ktTableOptions *table_options);
static KtConnCacheEntry *GetConnCacheEntry(
        struct ktTableOptions *table_options);

#define ktelog(type, args...) _ktelog(type, __FILE__, __func__, __LINE__, args)

#ifdef KTLOGVERBOSE
	#define _ktelog(type, file, func, line, fmt, ...) \
		elog(type, "%s:%s():%d " fmt, file, func, line, ##__VA_ARGS__)
#else
	#define _ktelog(type, file, func, line, fmt, ...) \
		elog(type, fmt, ##__VA_ARGS__)
#endif

#define handleErrors(db, table_options) \
	_handleErrors(__FILE__, __func__, __LINE__, db, table_options)

#define ktelogdb(type, db) _ktelogdb(type, __FILE__, __func__, __LINE__, db)
static void _ktelogdb(int type,
                      const char *file UNUSED,
                      const char *func UNUSED,
                      int line UNUSED,
                      KTDB *db)
{
	const int err_num   = ktgeterrornum(db);
	const char *name    = ktgeterror(db);
	const char *message = ktgeterrormsg(db);
	_ktelog(type, file, func, line, "[%s(%d)]:%s", name, err_num, message);
}

static bool _handleErrors(const char *file,
                          const char *func,
                          const int line,
                          KTDB *db,
                          struct ktTableOptions *table_options)
{
	KtConnCacheEntry *entry;
	const int err_num = ktgeterrornum(db);
	// Always log non-fatal errors

	switch(err_num) {
		case 6:// Network error
			_ktelogdb(NOTICE, file, func, line, db);
			entry = GetConnCacheEntry(table_options);
			ktdbdel(entry->db);
			entry->db = NULL;
			if(KtOpenConnection(entry, table_options)) {
				return true;
			}
			_ktelog(ERROR,
			        file,
			        func,
			        line,
			        "Failed to reconnect to server %s:%d",
			        table_options->host,
			        table_options->port);
			break;
		default: _ktelogdb(ERROR,file, func, line, db); break;
	}
	return false;
}

static bool isValidOption(const char *option, Oid context)
{
	struct ktFdwOption *opt;

#ifdef DEBUG
	ktelog(NOTICE, "isValidOption %s", option);
#endif
	for(opt = valid_options; opt->optname; opt++) {
		if(context == opt->optcontext &&
		   strcmp(opt->optname, option) == 0) {
			return true;
		}
	}
	return false;
}

#ifdef USE_TRANSACTIONS
static void ktSubXactCallback(XactEvent event, void *arg UNUSED)
{
	HASH_SEQ_STATUS scan;
	KtConnCacheEntry *entry;

	if(!xact_got_connection) return;

	/* xact ended so end transaction on all connections, transaction are
	 * "global" to all tables and fdws */
	hash_seq_init(&scan, ConnectionHash);
	while((entry = (KtConnCacheEntry *)hash_seq_search(&scan))) {
		if(entry->db == NULL || entry->xact_depth == 0) continue;
		switch(event) {
			case XACT_EVENT_PRE_COMMIT:
			case XACT_EVENT_PARALLEL_PRE_COMMIT:
				if(!ktcommit(entry->db)) {
					ktelogdb(ERROR, entry->db);
				}
				break;
			case XACT_EVENT_PRE_PREPARE:
				ereport(ERROR,
				        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				         errmsg("cannot prepare a transaction "
				                "that modified remote "
				                "tables")));
				break;
			case XACT_EVENT_COMMIT:
			case XACT_EVENT_PREPARE:
			case XACT_EVENT_PARALLEL_COMMIT:
				/* Should not get here -- pre-commit should have
				 * handled it */
				elog(ERROR,
				     "missed cleaning up connection during "
				     "pre-commit");
				break;
			case XACT_EVENT_ABORT:
			case XACT_EVENT_PARALLEL_ABORT:
				if(!ktabort(entry->db)) {
					ktelogdb(ERROR, entry->db);
				}
				break;
		}
		entry->xact_depth = 0;
	}

	xact_got_connection = false;
}

static void KtBeginTransactionIfNeeded(KtConnCacheEntry *entry,
                                       struct ktTableOptions *table_options)
{
	int curlevel =
	        GetCurrentTransactionNestLevel();// I dont think it should ever
	                                         // be less than 1;

	if(curlevel < 1)// I dont get something
	{
		elog(ERROR,
		     "Transaction level should not be less than one");// I don't
		                                                      // understand
	}

	if(curlevel > 1)// kt does not support savepoints
	{
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		         errmsg("Kyoto Tycoon does not support savepoints")));
	}

	if(entry->xact_depth < curlevel) {
	RETRY:
		if(!ktbegin_transaction(entry->db)) {
			if(handleErrors(entry->db, table_options)) {
				// Replay
				goto RETRY;
			}
		}
		entry->xact_depth   = curlevel;// 1
		xact_got_connection = true;
	}
}
#endif

static KtConnCacheEntry *GetConnCacheEntry(struct ktTableOptions *table_options)
{
	bool found;
	KtConnCacheKey key;
	KtConnCacheEntry *entry;

	/* First time through, initialize connection cache hashtable */
	if(ConnectionHash == NULL) {
		HASHCTL ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize   = sizeof(KtConnCacheKey);
		ctl.entrysize = sizeof(KtConnCacheEntry);
		ctl.hash      = tag_hash;
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash =
		        hash_create("kt_fdw connections",
		                    8,
		                    &ctl,
		                    HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

#ifdef USE_TRANSACTIONS
		RegisterXactCallback(ktSubXactCallback, NULL);
#endif
	}

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key.serverId = table_options->serverId;
	key.userId   = table_options->userId;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);

	if(!found) {
		/* initialize new hashtable entry (key is already filled in) */
		entry->db            = NULL;
		entry->creation_time = 0;
		entry->xact_depth    = 0;
	}
	return entry;
}

static bool KtOpenConnection(KtConnCacheEntry *entry,
                             struct ktTableOptions *table_options)
{
	if(entry->db == NULL) {
		int64_t now;
		struct timeval tv;
		gettimeofday(&tv, NULL);
		now = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;

		if(entry->creation_time == 0 ||
		   (table_options->reconnect_timeout != -1 &&
		    entry->creation_time + table_options->reconnect_timeout <
		            now)) {
			entry->creation_time = now;
			entry->db            = ktdbnew();

			if(!entry->db) {
				ktelog(ERROR,
				       "could not allocate memory for ktdb");
			}

			if(!ktdbopen(entry->db,
			             table_options->host,
			             table_options->port,
			             table_options->timeout)) {
				ktdbdel(entry->db);
				entry->db = NULL;
				return false;
			}
		} else {
			return false;
		}
	}
	return true;
}

KTDB *GetKtConnection(struct ktTableOptions *table_options)
{
	KtConnCacheEntry *entry = GetConnCacheEntry(table_options);
	if(!KtOpenConnection(entry, table_options)) {
		ktelog(ERROR,
		       "Failed to connect to %s:%d",
		       table_options->host,
		       table_options->port);
	}

#ifdef USE_TRANSACTIONS
	KtBeginTransactionIfNeeded(entry, table_options);
#endif

	return entry->db;
}

void ReleaseKtConnection(KTDB *db UNUSED)
{
	/* keep arround for next connection */

	// Do we know that transactions are completed?
	/*if(!ktdbclose(db))
	    elog(ERROR, "Error could not close connection when counting");
	ktdbdel(db);*/
}

void initTableOptions(struct ktTableOptions *table_options)
{
	table_options->host              = NULL;
	table_options->port              = 0;
	table_options->timeout           = 0;
	table_options->reconnect_timeout = 0;
}

static void getTableOptions(Oid foreigntableid,
                            struct ktTableOptions *table_options)
{
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *mapping;
	List *options;
	ListCell *lc;

#ifdef DEBUG
	elog(NOTICE, "getTableOptions");
#endif

	/*
	 * Extract options from FDW objects. We only need to worry about server
	 * options for Redis
	 *
	 */
	table   = GetForeignTable(foreigntableid);
	server  = GetForeignServer(table->serverid);
	mapping = GetUserMapping(GetUserId(), table->serverid);

	table_options->userId   = mapping->userid;
	table_options->serverId = server->serverid;

	options = NIL;
	options = list_concat(options, table->options);
	options = list_concat(options, server->options);
	options = list_concat(options, mapping->options);

	//	table_options->table_type = PG_REDIS_SCALAR_TABLE;

	/* Loop through the options, and get the server/port */
	foreach(lc, options) {
		DefElem *def = (DefElem *)lfirst(lc);

		if(strcmp(def->defname, "host") == 0)
			table_options->host = defGetString(def);

		if(strcmp(def->defname, "port") == 0)
			table_options->port = atoi(defGetString(def));

		if(strcmp(def->defname, "timeout") == 0)
			table_options->timeout = atoi(defGetString(def));

		if(strcmp(def->defname, "reconnect_timeout") == 0)
			table_options->reconnect_timeout =
			        atoi(defGetString(def));
	}

	/* Default values, if required */
	if(!table_options->host) { table_options->host = "127.0.0.1"; }
	if(!table_options->port) { table_options->port = 1978; }

	if(!table_options->timeout) {
		table_options->timeout = -1;// no timeout
	}

	if(!table_options->reconnect_timeout) {
		table_options->reconnect_timeout = -1;// One secound timeout
	}
}

Datum kt_fdw_validator(PG_FUNCTION_ARGS UNUSED)
{
	List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid catalog        = PG_GETARG_OID(1);
	ListCell *cell;

	/* used for detecting duplicates; does not remember vals */
	struct ktTableOptions table_options;

	elog(DEBUG1, "entering function %s", __func__);

	initTableOptions(&table_options);

	foreach(cell, options_list) {
		DefElem *def = (DefElem *)lfirst(cell);

		/*check that this is in the list of known options*/
		if(!isValidOption(def->defname, catalog)) {
			struct ktFdwOption *opt;
			StringInfoData buf;
			/*
			 * Unknown option specified, complain about it. Provide
			 * a hint with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for(opt = valid_options; opt->optname; opt++) {
				if(catalog == opt->optcontext)
					appendStringInfo(
					        &buf,
					        "%s%s",
					        (buf.len > 0) ? ", " : "",
					        opt->optname);
			}

			ereport(ERROR,
			        (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
			         errmsg("invalid option \"%s\"", def->defname),
			         errhint("Valid options in this context are: "
			                 "%s",
			                 buf.len ? buf.data : "<none>")));
		}

		/*make sure options don't repeat */

		if(strcmp(def->defname, "host") == 0) {
			if(table_options.host)
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant "
				                "options: "
				                "host (%s)",
				                defGetString(def))));

			table_options.host = defGetString(def);
		}

		if(strcmp(def->defname, "port") == 0) {
			if(table_options.port)
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant "
				                "options: "
				                "port (%s)",
				                defGetString(def))));

			table_options.port = atoi(defGetString(def));
		}

		if(strcmp(def->defname, "timeout") == 0) {
			if(table_options.timeout)
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant "
				                "options: "
				                "timeout (%s)",
				                defGetString(def))));

			table_options.timeout = atoi(defGetString(def));
		}
		if(strcmp(def->defname, "reconnect_timeout") == 0) {
			if(table_options.reconnect_timeout)
				ereport(ERROR,
				        (errcode(ERRCODE_SYNTAX_ERROR),
				         errmsg("conflicting or redundant "
				                "options: "
				                "reconnect_timeout (%s)",
				                defGetString(def))));

			table_options.reconnect_timeout =
			        atoi(defGetString(def));
		}
	}

	PG_RETURN_VOID();
}

static void getKeyBasedQual(Node *node,
                            TupleDesc tupdesc,
                            char **value,
                            bool *key_based_qual)
{
	char *key       = NULL;
	*value          = NULL;
	*key_based_qual = false;

	if(!node) return;

	if(IsA(node, OpExpr)) {
		OpExpr *op = (OpExpr *)node;
		Node *left, *right;
		Index varattno;

		if(list_length(op->args) != 2) return;

		left = list_nth(op->args, 0);

		if(!IsA(left, Var)) return;

		varattno = ((Var *)left)->varattno;

		right = list_nth(op->args, 1);

		if(IsA(right, Const)) {
			StringInfoData buf;

			initStringInfo(&buf);

			/* And get the column and value... */
#if PG_VERSION_NUM >= 110000
			key = NameStr(tupdesc->attrs[varattno - 1].attname);
#else
			key = NameStr(tupdesc->attrs[varattno - 1]->attname);
#endif
			*value = TextDatumGetCString(
			        ((Const *)right)->constvalue);

			/*
			 * We can push down this qual if:
			 * - The operatory is TEXTEQ
			 * - The qual is on the key column
			 */
			if(op->opfuncid == PROCID_TEXTEQ &&
			   strcmp(key, "key") == 0)
				*key_based_qual = true;

			return;
		}
	}

	return;
}

static void ktGetForeignRelSize(PlannerInfo *root UNUSED,
                                RelOptInfo *baserel,
                                Oid foreigntableid)
{
	/*
	 * Obtain relation size estimates for a foreign table. This is called at
	 * the beginning of planning for a query that scans a foreign table.
	 * root is the planner's global information about the query; baserel is
	 * the planner's information about this table; and foreigntableid is the
	 * pg_class OID of the foreign table. (foreigntableid could be obtained
	 * from the planner data structures, but it's passed explicitly to save
	 * effort.)
	 *
	 * This function should update baserel->rows to be the expected number
	 * of rows returned by the table scan, after accounting for the
	 * filtering done by the restriction quals. The initial value of
	 * baserel->rows is just a constant default estimate, which should be
	 * replaced if at all possible. The function may also choose to update
	 * baserel->width if it can compute a better estimate of the average
	 * result row width.
	 */

	ListCell *cell;

	KtFdwPlanState *fdw_private;
	KTDB *db;
	// ktTableOptions table_options;

	elog(DEBUG1, "entering function %s", __func__);

	baserel->rows = 0;

	fdw_private          = palloc(sizeof(KtFdwPlanState));
	baserel->fdw_private = (void *)fdw_private;

	initTableOptions(&(fdw_private->opt));
	getTableOptions(foreigntableid, &(fdw_private->opt));

	/* initialize reuired state in fdw_private */

	foreach(cell, baserel->baserestrictinfo) {
		RestrictInfo *info = (RestrictInfo *)lfirst(cell);
		if(IsA(info->clause, OpExpr)) {
			OpExpr *op = (OpExpr *)info->clause;
			Node *left, *right;

			if(list_length(op->args) != 2) break;

			left = list_nth(op->args, 0);

			if(!IsA(left, Var)) break;

			right = list_nth(op->args, 1);

			if(IsA(right, Const)) {
				/* todo: how to check key name? */
				baserel->rows = 1;
				return;
			}
		}
	}

	db = GetKtConnection(&(fdw_private->opt));

	baserel->rows = ktdbcount(db);

	ReleaseKtConnection(db);
}

static void ktGetForeignPaths(PlannerInfo *root,
                              RelOptInfo *baserel,
                              Oid foreigntableid UNUSED)
{
	/*
	 * Create possible access paths for a scan on a foreign table. This is
	 * called during query planning. The parameters are the same as for
	 * GetForeignRelSize, which has already been called.
	 *
	 * This function must generate at least one access path (ForeignPath
	 * node) for a scan on the foreign table and must call add_path to add
	 * each such path to baserel->pathlist. It's recommended to use
	 * create_foreignscan_path to build the ForeignPath nodes. The function
	 * can generate multiple access paths, e.g., a path which has valid
	 * pathkeys to represent a pre-sorted result. Each access path must
	 * contain cost estimates, and can contain any FDW-private information
	 * that is needed to identify the specific scan method intended.
	 */

	KtFdwPlanState *fdw_private = baserel->fdw_private;

	Cost startup_cost, total_cost;

	elog(DEBUG1, "entering function %s", __func__);

	if(strcmp(fdw_private->opt.host, "127.0.0.1") == 0 ||
	   strcmp(fdw_private->opt.host, "localhost") == 0)
		startup_cost = 10;
	else
		startup_cost = 25;

	total_cost = startup_cost + baserel->rows;

	/* Create a ForeignPath node and add it as only possible path */
	add_path(
	        baserel,
	        (Path *)create_foreignscan_path(root,
	                                        baserel,
#if PG_VERSION_NUM >= 100000
	                                        NULL,// PathTarget
#endif
	                                        baserel->rows,
	                                        startup_cost,
	                                        total_cost,
	                                        NIL,  /* no pathkeys */
	                                        NULL, /* no outer rel either */
#if PG_VERSION_NUM >= 90500
	                                        NULL,
#endif
	                                        NIL)); /* no fdw_private data */
}

static ForeignScan *ktGetForeignPlan(PlannerInfo *root UNUSED,
                                     RelOptInfo *baserel,
                                     Oid foreigntableid UNUSED,
                                     ForeignPath *best_path UNUSED,
                                     List *tlist,
                                     List *scan_clauses,
                                     Plan *plan UNUSED)
{
	/*
	 * Create a ForeignScan plan node from the selected foreign access path.
	 * This is called at the end of query planning. The parameters are as
	 * for GetForeignRelSize, plus the selected ForeignPath (previously
	 * produced by GetForeignPaths), the target list to be emitted by the
	 * plan node, and the restriction clauses to be enforced by the plan
	 * node.
	 *
	 * This function must create and return a ForeignScan plan node; it's
	 * recommended to use make_foreignscan to build the ForeignScan node.
	 *
	 */

	Index scan_relid = baserel->relid;

	/*
	 * We have no native ability to evaluate restriction clauses, so we just
	 * put all the scan_clauses into the plan node's qual list for the
	 * executor to check. So all we have to do here is strip RestrictInfo
	 * nodes from the clauses and ignore pseudoconstants (which will be
	 * handled elsewhere).
	 */

	elog(DEBUG1, "entering function %s", __func__);

	scan_clauses = extract_actual_clauses(scan_clauses, false);

	/* Create the ForeignScan node */
	return make_foreignscan(tlist,
	                        scan_clauses,
	                        scan_relid,
	                        NIL, /* no expressions to evaluate */
	                        NIL
#if PG_VERSION_NUM >= 90500
	                        ,
	                        NULL,
	                        NULL,
	                        NULL
#endif
	); /* no private state either */
}

static void ktBeginForeignScan(ForeignScanState *node, int eflags)
{
	/*
	 * Begin executing a foreign scan. This is called during executor
	 * startup. It should perform any initialization needed before the scan
	 * can start, but not start executing the actual scan (that should be
	 * done upon the first call to IterateForeignScan). The ForeignScanState
	 * node has already been created, but its fdw_state field is still NULL.
	 * Information about the table to scan is accessible through the
	 * ForeignScanState node (in particular, from the underlying ForeignScan
	 * plan node, which contains any FDW-private information provided by
	 * GetForeignPlan). eflags contains flag bits describing the executor's
	 * operating mode for this plan node.
	 *
	 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this
	 * function should not perform any externally-visible actions; it should
	 * only do the minimum required to make the node state valid for
	 * ExplainForeignScan and EndForeignScan.
	 *
	 */

	KtFdwExecState *estate;

	elog(DEBUG1, "entering function %s", __func__);

	estate      = (KtFdwExecState *)palloc(sizeof(KtFdwExecState));
	estate->cur = NULL;
	estate->db  = NULL;
	estate->key_based_qual      = false;
	estate->key_based_qual_sent = false;
	node->fdw_state             = (void *)estate;

	initTableOptions(&(estate->plan.opt));
	getTableOptions(RelationGetRelid(node->ss.ss_currentRelation),
	                &(estate->plan.opt));

	/* initialize required state in fdw_private */

	estate->db = GetKtConnection(&(estate->plan.opt));

	/* OK, we connected. If this is an EXPLAIN, bail out now */
	if(eflags & EXEC_FLAG_EXPLAIN_ONLY) return;

	estate->attinmeta =
	        TupleDescGetAttInMetadata(node->ss.ss_currentRelation->rd_att);

	if(node->ss.ps.plan->qual) {
		ListCell *lc;

		foreach(lc, node->ss.ps.plan->qual) {
			/* Only the first qual can be pushed down to Redis */
			ExprState *state = lfirst(lc);

			getKeyBasedQual((Node *)state,
			                node->ss.ss_currentRelation->rd_att,
			                &estate->key_based_qual_value,
			                &estate->key_based_qual);

			if(estate->key_based_qual) break;
		}
	}

	if(!estate->key_based_qual) estate->cur = get_cursor(estate->db);
}

static TupleTableSlot *ktIterateForeignScan(ForeignScanState *node)
{
	/*
	 * Fetch one row from the foreign source, returning it in a tuple table
	 * slot (the node's ScanTupleSlot should be used for this purpose).
	 * Return NULL if no more rows are available. The tuple table slot
	 * infrastructure allows either a physical or virtual tuple to be
	 * returned; in most cases the latter choice is preferable from a
	 * performance standpoint. Note that this is called in a short-lived
	 * memory context that will be reset between invocations. Create a
	 * memory context in BeginForeignScan if you need longer-lived storage,
	 * or use the es_query_cxt of the node's EState.
	 *
	 * The rows returned must match the column signature of the foreign
	 * table being scanned. If you choose to optimize away fetching columns
	 * that are not needed, you should insert nulls in those column
	 * positions.
	 *
	 * Note that PostgreSQL's executor doesn't care whether the rows
	 * returned violate any NOT NULL constraints that were defined on the
	 * foreign table columns — but the planner does care, and may optimize
	 * queries incorrectly if NULL values are present in a column declared
	 * not to contain them. If a NULL value is encountered when the user has
	 * declared that none should be present, it may be appropriate to raise
	 * an error (just as you would need to do in the case of a data type
	 * mismatch).
	 */

	KtFdwExecState *estate = (KtFdwExecState *)node->fdw_state;

	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	TupleDesc desc       = slot->tts_tupleDescriptor;

	bool found = false;
	bool *nulls;
	HeapTuple tuple;
	Datum *dvalues;
	bytea *b;
	text *t;
	int i;

	size_t len[2];
	char **val = palloc(sizeof(char *) * 2);

	elog(DEBUG1, "entering function %s", __func__);

	ExecClearTuple(slot);

	/* get the next record, if any, and fill in the slot */
	/* Build the tuple */

	if(estate->key_based_qual) {
		if(!estate->key_based_qual_sent) {
			estate->key_based_qual_sent = true;
			found                       = ktgetl(estate->db,
                                       estate->key_based_qual_value,
                                       &len[0],
                                       &val[1],
                                       &len[1]);
			len[0] = strlen(estate->key_based_qual_value);
			val[0] = estate->key_based_qual_value;
		}
	} else {
		found = nextl(estate->db,
		              estate->cur,
		              &val[0],
		              &len[0],
		              &val[1],
		              &len[1]);
	}

	if(found) {
		dvalues = (Datum *)palloc(3 * sizeof(Datum));
		nulls   = (bool *)palloc(3 * sizeof(bool));
		for(i = 0; i < 2; i++) {
			nulls[i] = false;

			switch(TupleDescAttr(desc, i)->atttypid) {
				case TEXTOID:
				case VARCHAROID:
					if(len[i] != strlen(val[i])) {
						elog(ERROR,
						     "null characters not "
						     "allowed in text values");
						nulls[i] = true;
					} else {
						t = palloc(len[i] + VARHDRSZ);
						memcpy(VARDATA(t),
						       val[i],
						       len[i]);
						SET_VARSIZE(t,
						            len[i] + VARHDRSZ);
						dvalues[i] = PointerGetDatum(t);
					}

					break;
				case BYTEAOID:
					b = palloc(len[i] + VARHDRSZ);
					memcpy(VARDATA(b), val[i], len[i]);
					SET_VARSIZE(b, len[i] + VARHDRSZ);
					dvalues[i] = PointerGetDatum(b);
					break;
				default:
					elog(ERROR, "type not supported");
					nulls[i] = true;
			}
		}
		/* set flags to null */
		nulls[2] = true;

		tuple = heap_form_tuple(
		        estate->attinmeta->tupdesc, dvalues, nulls);
		ExecStoreHeapTuple(tuple, slot, false);

		pfree(dvalues);
		pfree(nulls);
	}

	pfree(val);

	/* then return the slot */
	return slot;
}

static void ktReScanForeignScan(ForeignScanState *node UNUSED)
{
	/*
	 * Restart the scan from the beginning. Note that any parameters the
	 * scan depends on may have changed value, so the new scan does not
	 * necessarily return exactly the same rows.
	 */

	elog(DEBUG1, "entering function %s", __func__);
}

static void ktEndForeignScan(ForeignScanState *node)
{
	/*
	 * End the scan and release resources. It is normally not important to
	 * release palloc'd memory, but for example open files and connections
	 * to remote servers should be cleaned up.
	 */

	KtFdwExecState *estate = (KtFdwExecState *)node->fdw_state;

	elog(DEBUG1, "entering function %s", __func__);

	if(estate) {
		if(estate->cur) {
			ktcurdel(estate->cur);
			estate->cur = NULL;
		}

		if(estate->db) {
			ReleaseKtConnection(estate->db);
			estate->db = NULL;
		}
	}
}

static void ktAddForeignUpdateTargets(Query *parsetree,
                                      RangeTblEntry *target_rte UNUSED,
                                      Relation target_relation)
{
	/*
	 * UPDATE and DELETE operations are performed against rows previously
	 * fetched by the table-scanning functions. The FDW may need extra
	 * information, such as a row ID or the values of primary-key columns,
	 * to ensure that it can identify the exact row to update or delete. To
	 * support that, this function can add extra hidden, or "junk", target
	 * columns to the list of columns that are to be retrieved from the
	 * foreign table during an UPDATE or DELETE.
	 *
	 * To do that, add TargetEntry items to parsetree->targetList,
	 * containing expressions for the extra values to be fetched. Each such
	 * entry must be marked resjunk = true, and must have a distinct resname
	 * that will identify it at execution time. Avoid using names matching
	 * ctidN or wholerowN, as the core system can generate junk columns of
	 * these names.
	 *
	 * This function is called in the rewriter, not the planner, so the
	 * information available is a bit different from that available to the
	 * planning routines. parsetree is the parse tree for the UPDATE or
	 * DELETE command, while target_rte and target_relation describe the
	 * target foreign table.
	 *
	 * If the AddForeignUpdateTargets pointer is set to NULL, no extra
	 * target expressions are added. (This will make it impossible to
	 * implement DELETE operations, though UPDATE may still be feasible if
	 * the FDW relies on an unchanging primary key to identify rows.)
	 */

	Form_pg_attribute attr;
	Var *varnode;
	const char *attrname;
	TargetEntry *tle;

	elog(DEBUG1, "entering function %s", __func__);

#if PG_VERSION_NUM >= 110000
	attr = &RelationGetDescr(target_relation)->attrs[0];
#else
	attr = RelationGetDescr(target_relation)->attrs[0];
#endif

	varnode = makeVar(parsetree->resultRelation,
	                  attr->attnum,
	                  attr->atttypid,
	                  attr->atttypmod,
	                  attr->attcollation,
	                  0);
	/* Wrap it in a resjunk TLE with the right name ... */
	attrname = "key_junk";
	tle      = makeTargetEntry((Expr *)varnode,
                              list_length(parsetree->targetList) + 1,
                              pstrdup(attrname),
                              true);

	/* ... and add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}

static List *ktPlanForeignModify(PlannerInfo *root UNUSED,
                                 ModifyTable *plan UNUSED,
                                 Index resultRelation UNUSED,
                                 int subplan_index UNUSED)
{
	/*
	 * Perform any additional planning actions needed for an insert, update,
	 * or delete on a foreign table. This function generates the FDW-private
	 * information that will be attached to the ModifyTable plan node that
	 * performs the update action. This private information must have the
	 * form of a List, and will be delivered to BeginForeignModify during
	 * the execution stage.
	 *
	 * root is the planner's global information about the query. plan is the
	 * ModifyTable plan node, which is complete except for the fdwPrivLists
	 * field. resultRelation identifies the target foreign table by its
	 * rangetable index. subplan_index identifies which target of the
	 * ModifyTable plan node this is, counting from zero; use this if you
	 * want to index into plan->plans or other substructure of the plan
	 * node.
	 *
	 * If the PlanForeignModify pointer is set to NULL, no additional
	 * plan-time actions are taken, and the fdw_private list delivered to
	 * BeginForeignModify will be NIL.
	 */

	elog(DEBUG1, "entering function %s", __func__);

	return NULL;
}

static void ktBeginForeignModify(ModifyTableState *mtstate,
                                 ResultRelInfo *rinfo,
                                 List *fdw_private UNUSED,
                                 int subplan_index,
                                 int eflags)
{
	/*
	 * Begin executing a foreign table modification operation. This routine
	 * is called during executor startup. It should perform any
	 * initialization needed prior to the actual table modifications.
	 * Subsequently, ExecForeignInsert, ExecForeignUpdate or
	 * ExecForeignDelete will be called for each tuple to be inserted,
	 * updated, or deleted.
	 *
	 * mtstate is the overall state of the ModifyTable plan node being
	 * executed; global data about the plan and execution state is available
	 * via this structure. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. (The ri_FdwState field of ResultRelInfo is
	 * available for the FDW to store any private state it needs for this
	 * operation.) fdw_private contains the private data generated by
	 * PlanForeignModify, if any. subplan_index identifies which target of
	 * the ModifyTable plan node this is. eflags contains flag bits
	 * describing the executor's operating mode for this plan node.
	 *
	 * Note that when (eflags & EXEC_FLAG_EXPLAIN_ONLY) is true, this
	 * function should not perform any externally-visible actions; it should
	 * only do the minimum required to make the node state valid for
	 * ExplainForeignModify and EndForeignModify.
	 *
	 * If the BeginForeignModify pointer is set to NULL, no action is taken
	 * during executor startup.
	 */
	Relation rel = rinfo->ri_RelationDesc;
	KtFdwModifyState *fmstate;
	Form_pg_attribute attr;
	Oid typefnoid;
	bool isvarlena;
	CmdType operation = mtstate->operation;

	elog(DEBUG1, "entering function %s", __func__);

	if(eflags & EXEC_FLAG_EXPLAIN_ONLY) return;

	fmstate      = (KtFdwModifyState *)palloc0(sizeof(KtFdwModifyState));
	fmstate->rel = rel;
	fmstate->key_info   = (FmgrInfo *)palloc0(sizeof(FmgrInfo));
	fmstate->value_info = (FmgrInfo *)palloc0(sizeof(FmgrInfo));
	fmstate->flags_info = (FmgrInfo *)palloc0(sizeof(FmgrInfo));

	if(operation == CMD_UPDATE || operation == CMD_DELETE) {
		/* Find the ctid resjunk column in the subplan's result */
		Plan *subplan        = mtstate->mt_plans[subplan_index]->plan;
		fmstate->key_junk_no = ExecFindJunkAttributeInTlist(
		        subplan->targetlist, "key_junk");
		if(!AttributeNumberIsValid(fmstate->key_junk_no))
			elog(ERROR, "could not find key junk column");
	}

#if PG_VERSION_NUM >= 110000
	attr = &RelationGetDescr(rel)->attrs[0];
#else
	attr = RelationGetDescr(rel)->attrs[0];
#endif
	Assert(!attr->attisdropped);
	getTypeBinaryOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
	fmgr_info(typefnoid, fmstate->key_info);

#if PG_VERSION_NUM >= 110000
	attr = &RelationGetDescr(rel)->attrs[1];
#else
	attr = RelationGetDescr(rel)->attrs[1];
#endif
	Assert(!attr->attisdropped);

	getTypeBinaryOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
	fmgr_info(typefnoid, fmstate->value_info);

#if PG_VERSION_NUM >= 110000
	attr = &RelationGetDescr(rel)->attrs[2];
#else
	attr = RelationGetDescr(rel)->attrs[2];
#endif
	Assert(!attr->attisdropped);

	getTypeBinaryOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
	fmgr_info(typefnoid, fmstate->flags_info);

	initTableOptions(&(fmstate->opt));
	getTableOptions(RelationGetRelid(rel), &(fmstate->opt));

	fmstate->db = GetKtConnection(&(fmstate->opt));

	rinfo->ri_FdwState = fmstate;
}

enum kt_flag { NONE, SET, APPEND };

static TupleTableSlot *ktExecForeignInsert(EState *estate UNUSED,
                                           ResultRelInfo *rinfo,
                                           TupleTableSlot *slot,
                                           TupleTableSlot *planSlot)
{
	/*
	 * Insert one tuple into the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains the tuple to be inserted; it will
	 * match the rowtype definition of the foreign table. planSlot contains
	 * the tuple that was generated by the ModifyTable plan node's subplan;
	 * it differs from slot in possibly containing additional "junk"
	 * columns. (The planSlot is typically of little interest for INSERT
	 * cases, but is provided for completeness.)
	 *
	 * The return value is either a slot containing the data that was
	 * actually inserted (this might differ from the data supplied, for
	 * example as a result of trigger actions), or NULL if no row was
	 * actually inserted (again, typically as a result of triggers). The
	 * passed-in slot can be re-used for this purpose.
	 *
	 * The data in the returned slot is used only if the INSERT query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignInsert pointer is set to NULL, attempts to insert
	 * into the foreign table will fail with an error message.
	 *
	 */

	Datum value;
	bool isnull;
	enum kt_flag flag = NONE;
	bool isDelete     = false;
	bytea *bkey, *bval = NULL, *sval;
	KtFdwModifyState *fmstate = (KtFdwModifyState *)rinfo->ri_FdwState;

	elog(DEBUG1, "entering function %s", __func__);

	value = slot_getattr(planSlot, 1, &isnull);
	if(isnull) { elog(ERROR, "can't get key value"); }

	bkey = SendFunctionCall(fmstate->key_info, value);

	value = slot_getattr(planSlot, 2, &isnull);
	if(isnull) {
		isDelete = true;
	} else {
		bval = SendFunctionCall(fmstate->value_info, value);
	}

	value = slot_getattr(planSlot, 3, &isnull);
	if(!isnull) {
		sval = SendFunctionCall(fmstate->flags_info, value);
		if(memcmp(VARDATA(sval), "kt_set", VARSIZE(sval) - VARHDRSZ) ==
		   0) {
			flag = SET;
		} else if(memcmp(VARDATA(sval),
		                 "kt_append",
		                 VARSIZE(sval) - VARHDRSZ) == 0) {
			flag = APPEND;
		}
	}

#ifndef USE_TRANSACTIONS
RETRY:
#endif
	switch(flag) {
		case SET: {
			if(isDelete) {
				if(!ktremovel(fmstate->db,
				              VARDATA(bkey),
				              VARSIZE(bkey) - VARHDRSZ)) {
					const char *err_msg =
					        ktgeterrormsg(fmstate->db);
					if(strcmp(err_msg,
					          "DB: 7: no record: no "
					          "record") != 0) {
#ifdef USE_TRANSACTIONS
						ktelogdb(ERROR, fmstate->db);
#else
						if(handleErrors(
						           fmstate->db,
						           &fmstate->opt)) {
							// Replay
							goto RETRY;
						}
#endif
					}
				}
			} else if(!ktsetl(fmstate->db,
			                  VARDATA(bkey),
			                  VARSIZE(bkey) - VARHDRSZ,
			                  VARDATA(bval),
			                  VARSIZE(bval) - VARHDRSZ)) {
#ifdef USE_TRANSACTIONS
				ktelogdb(ERROR, fmstate->db);
#else
				if(handleErrors(fmstate->db, &fmstate->opt)) {
					// Replay
					goto RETRY;
				}
#endif
			}
		} break;
		case NONE:
		default: {
			if(isDelete) {
				ktelog(ERROR,
				       "Value is NULL, to overwrite value use "
				       "'kt_set' flag");
			}
			if(!ktaddl(fmstate->db,
			           VARDATA(bkey),
			           VARSIZE(bkey) - VARHDRSZ,
			           VARDATA(bval),
			           VARSIZE(bval) - VARHDRSZ)) {
#ifdef USE_TRANSACTIONS
				ktelogdb(ERROR, fmstate->db);
#else
				if(handleErrors(fmstate->db, &fmstate->opt)) {
					// Replay
					goto RETRY;
				}
#endif
			}
		} break;
	}

	return slot;
}

static TupleTableSlot *ktExecForeignUpdate(EState *estate UNUSED,
                                           ResultRelInfo *rinfo,
                                           TupleTableSlot *slot,
                                           TupleTableSlot *planSlot)
{
	/*
	 * Update one tuple in the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains the new data for the tuple; it
	 * will match the rowtype definition of the foreign table. planSlot
	 * contains the tuple that was generated by the ModifyTable plan node's
	 * subplan; it differs from slot in possibly containing additional
	 * "junk" columns. In particular, any junk columns that were requested
	 * by AddForeignUpdateTargets will be available from this slot.
	 *
	 * The return value is either a slot containing the row as it was
	 * actually updated (this might differ from the data supplied, for
	 * example as a result of trigger actions), or NULL if no row was
	 * actually updated (again, typically as a result of triggers). The
	 * passed-in slot can be re-used for this purpose.
	 *
	 * The data in the returned slot is used only if the UPDATE query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignUpdate pointer is set to NULL, attempts to update
	 * the foreign table will fail with an error message.
	 *
	 */
	Datum value;
	bool isnull;
	bytea *bkey, *bkey_old, *bval;
	KtFdwModifyState *fmstate = (KtFdwModifyState *)rinfo->ri_FdwState;

	elog(DEBUG1, "entering function %s", __func__);

	value = ExecGetJunkAttribute(planSlot, fmstate->key_junk_no, &isnull);
	if(isnull) elog(ERROR, "can't get junk key value");

	bkey = SendFunctionCall(fmstate->key_info, value);

	value = slot_getattr(planSlot, 1, &isnull);
	if(isnull) elog(ERROR, "can't get new key value");

	bkey_old = SendFunctionCall(fmstate->key_info, value);

	if(VARSIZE(bkey) != VARSIZE(bkey_old) ||
	   memcmp(VARDATA(bkey), VARDATA(bkey_old), VARSIZE(bkey) - VARHDRSZ) !=
	           0) {
		elog(ERROR, "You cannot update key values");
		return slot;
	}

	value = slot_getattr(planSlot, 2, &isnull);
	if(isnull) elog(ERROR, "can't get value value");

	bval = SendFunctionCall(fmstate->value_info, value);
#ifndef USE_TRANSACTIONS
RETRY:
#endif
	if(!ktreplacel(fmstate->db,
	               VARDATA(bkey),
	               VARSIZE(bkey) - VARHDRSZ,
	               VARDATA(bval),
	               VARSIZE(bval) - VARHDRSZ)) {
#ifdef USE_TRANSACTIONS
		ktelogdb(ERROR, fmstate->db);
#else
		if(handleErrors(fmstate->db, &fmstate->opt)) {
			// Replay
			goto RETRY;
		}
#endif
	}

	return slot;
}

static TupleTableSlot *ktExecForeignDelete(EState *estate UNUSED,
                                           ResultRelInfo *rinfo,
                                           TupleTableSlot *slot,
                                           TupleTableSlot *planSlot)
{
	/*
	 * Delete one tuple from the foreign table. estate is global execution
	 * state for the query. rinfo is the ResultRelInfo struct describing the
	 * target foreign table. slot contains nothing useful upon call, but can
	 * be used to hold the returned tuple. planSlot contains the tuple that
	 * was generated by the ModifyTable plan node's subplan; in particular,
	 * it will carry any junk columns that were requested by
	 * AddForeignUpdateTargets. The junk column(s) must be used to identify
	 * the tuple to be deleted.
	 *
	 * The return value is either a slot containing the row that was
	 * deleted, or NULL if no row was deleted (typically as a result of
	 * triggers). The passed-in slot can be used to hold the tuple to be
	 * returned.
	 *
	 * The data in the returned slot is used only if the DELETE query has a
	 * RETURNING clause. Hence, the FDW could choose to optimize away
	 * returning some or all columns depending on the contents of the
	 * RETURNING clause. However, some slot must be returned to indicate
	 * success, or the query's reported rowcount will be wrong.
	 *
	 * If the ExecForeignDelete pointer is set to NULL, attempts to delete
	 * from the foreign table will fail with an error message.
	 */

	Datum value;
	bool isnull;
	bytea *bkey;
	KtFdwModifyState *fmstate = (KtFdwModifyState *)rinfo->ri_FdwState;

	elog(DEBUG1, "entering function %s", __func__);

	value = ExecGetJunkAttribute(planSlot, fmstate->key_junk_no, &isnull);
	if(isnull) elog(ERROR, "can't get key value");

	bkey = SendFunctionCall(fmstate->key_info, value);
#ifndef USE_TRANSACTIONS
RETRY:
#endif
	if(!ktremovel(fmstate->db, VARDATA(bkey), VARSIZE(bkey) - VARHDRSZ)) {
#ifdef USE_TRANSACTIONS
		ktelogdb(ERROR, fmstate->db);
#else
		if(handleErrors(fmstate->db, &fmstate->opt)) {
			// Replay
			goto RETRY;
		}
#endif
	}

	return slot;
}

static void ktEndForeignModify(EState *estate UNUSED, ResultRelInfo *rinfo)
{
	/*
	 * End the table update and release resources. It is normally not
	 * important to release palloc'd memory, but for example open files and
	 * connections to remote servers should be cleaned up.
	 *
	 * If the EndForeignModify pointer is set to NULL, no action is taken
	 * during executor shutdown.
	 */

	KtFdwModifyState *fmstate = (KtFdwModifyState *)rinfo->ri_FdwState;

	elog(DEBUG1, "entering function %s", __func__);

	if(fmstate) {
		if(fmstate->db) {
			ReleaseKtConnection(fmstate->db);
			fmstate->db = NULL;
		}
	}
}

static void ktExplainForeignScan(ForeignScanState *node UNUSED,
                                 struct ExplainState *es UNUSED)
{
	/*
	 * Print additional EXPLAIN output for a foreign table scan. This
	 * function can call ExplainPropertyText and related functions to add
	 * fields to the EXPLAIN output. The flag fields in es can be used to
	 * determine what to print, and the state of the ForeignScanState node
	 * can be inspected to provide run-time statistics in the EXPLAIN
	 * ANALYZE case.
	 *
	 * If the ExplainForeignScan pointer is set to NULL, no additional
	 * information is printed during EXPLAIN.
	 */

	elog(DEBUG1, "entering function %s", __func__);
}

static void ktExplainForeignModify(ModifyTableState *mtstate UNUSED,
                                   ResultRelInfo *rinfo UNUSED,
                                   List *fdw_private UNUSED,
                                   int subplan_index UNUSED,
                                   struct ExplainState *es UNUSED)
{
	/*
	 * Print additional EXPLAIN output for a foreign table update. This
	 * function can call ExplainPropertyText and related functions to add
	 * fields to the EXPLAIN output. The flag fields in es can be used to
	 * determine what to print, and the state of the ModifyTableState node
	 * can be inspected to provide run-time statistics in the EXPLAIN
	 * ANALYZE case. The first four arguments are the same as for
	 * BeginForeignModify.
	 *
	 * If the ExplainForeignModify pointer is set to NULL, no additional
	 * information is printed during EXPLAIN.
	 */

	elog(DEBUG1, "entering function %s", __func__);
}

static bool ktAnalyzeForeignTable(Relation relation UNUSED,
                                  AcquireSampleRowsFunc *func UNUSED,
                                  BlockNumber *totalpages UNUSED)
{
	/* ----
	 * This function is called when ANALYZE is executed on a foreign table.
	 *If the FDW can collect statistics for this foreign table, it should
	 *return true, and provide a pointer to a function that will collect
	 *sample rows from the table in func, plus the estimated size of the
	 *table in pages in totalpages. Otherwise, return false.
	 *
	 * If the FDW does not support collecting statistics for any tables, the
	 * AnalyzeForeignTable pointer can be set to NULL.
	 *
	 * If provided, the sample collection function must have the signature:
	 *
	 *	  int
	 *	  AcquireSampleRowsFunc (Relation relation, int elevel,
	 *							 HeapTuple
	 **rows, int targrows, double *totalrows, double *totaldeadrows);
	 *
	 * A random sample of up to targrows rows should be collected from the
	 * table and stored into the caller-provided rows array. The actual
	 *number of rows collected must be returned. In addition, store
	 *estimates of the total numbers of live and dead rows in the table into
	 *the output parameters totalrows and totaldeadrows. (Set totaldeadrows
	 *to zero if the FDW does not have any concept of dead rows.)
	 * ----
	 */
	elog(DEBUG1, "entering function %s", __func__);
	return false;
}
