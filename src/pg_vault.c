#include "postgres.h"
#include "miscadmin.h"
#include "storage/ipc.h"

#include "utils/guc.h"
#include "storage/lwlock.h"
#include "access/htup_details.h"

#include "utils/builtins.h"

#include "funcapi.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* private functions */
static void pgvault_shmem_startup(void);

#define SEGMENT_NAME	"pgvault"

#define	MAX_ID_LENGTH		64
#define	MAX_COMMENT_LENGTH	255
#define	MAX_KEY_LENGTH		1024

static int  pgvault_mem_max_size  = (1024*1024); /* 1MB by default, plenty of space for passwords */

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

void		_PG_init(void);
void		_PG_fini(void);

/*
 * This is a fixed-length structure, as it makes it easier to allocate and copy.
 * It might be made more space efficient by using regular pointers etc. The other
 * benefit is that this makes the capacity of the vault deterministic.
 */
typedef struct VaultItemData
{
	char	id[MAX_ID_LENGTH];				/* ID of the key (\0-terminated string) */
	char	comment[MAX_COMMENT_LENGTH];	/* comment of the key (\0-terminated string, may be NULL) */
	char	key[MAX_KEY_LENGTH];			/* key data */
} VaultItemData;

typedef VaultItemData* VaultItem;

/* used to allocate memory in the shared segment */
typedef struct VaultInfoData {

	LWLockId		lock;		/* LWLock guarding the vault */

	int				maxitems;	/* max number of items we can keep */
	int				nitems;		/* number of items in the vault */
	VaultItemData	items[1];	/* pointer to the first item */

} VaultInfoData;

typedef VaultInfoData*	VaultInfo;

/* pointer to the vault structure */
static VaultInfo vault_info = NULL;

static VaultInfo vault_copy(bool strip_keys);

/*
 * Module load callback
 */
void
_PG_init(void)
{

	/* check that we can initialize the shared segment */
	if (! process_shared_preload_libraries_in_progress) {
		elog(ERROR, "pg_vault needs to be loaded using shared_preload_libraries");
		return;
	}

	/* Define custom GUC variables. */

	/* How much memory should we preallocate for the dictionaries (limits how many
	 * dictionaries you can load into the shared segment). */
	DefineCustomIntVariable("pg_vault.max_size",
							"amount of memory to pre-allocate for pg_vault",
							NULL,
							&pgvault_mem_max_size,
							(1024*1024),
							(1024*1024), INT_MAX,
							PGC_POSTMASTER,
							GUC_UNIT_BLOCKS,
#if (PG_VERSION_NUM >= 90100)
							NULL,
#endif
							NULL,
							NULL);

	EmitWarningsOnPlaceholders("pg_vault");

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in ispell_shmem_startup().
	 */
	RequestAddinShmemSpace(pgvault_mem_max_size);
	RequestAddinLWLocks(1);

	/* Install hooks. */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgvault_shmem_startup;

}


/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
}


/* 
 * Probably the most important part of the startup - initializes the
 * memory in shared memory segment (creates and initializes the
 * VaultInfo data structure).
 * 
 * This is called from a shmem_startup_hook (see _PG_init). */
static
void pgvault_shmem_startup() {

	bool		found = FALSE;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	elog(DEBUG1, "initializing pg_vault segment (size: %d B)",
				 pgvault_mem_max_size);

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	vault_info = (VaultInfo)ShmemInitStruct(SEGMENT_NAME,
											pgvault_mem_max_size,
											&found);

	/* Was the shared memory segment already initialized? */
	if (! found) {

		/* nope - first time through, so initialize */
		memset(vault_info, 0, pgvault_mem_max_size);

		vault_info->lock  = LWLockAssign();

		/* how many items fit into the vault */
		vault_info->maxitems = (pgvault_mem_max_size - offsetof(VaultInfoData, items)) / sizeof(VaultItemData);

		elog(DEBUG1, "shared memory segment for pg_vault successfully created");

	}

	LWLockRelease(AddinShmemInitLock);

}

Datum add_key(PG_FUNCTION_ARGS);
Datum delete_key(PG_FUNCTION_ARGS);
Datum lookup_key(PG_FUNCTION_ARGS);
Datum list_keys(PG_FUNCTION_ARGS);
Datum delete_keys(PG_FUNCTION_ARGS);
Datum save_keys(PG_FUNCTION_ARGS);
Datum load_keys(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(add_key);
PG_FUNCTION_INFO_V1(delete_key);
PG_FUNCTION_INFO_V1(lookup_key);
PG_FUNCTION_INFO_V1(list_keys);
PG_FUNCTION_INFO_V1(delete_keys);
PG_FUNCTION_INFO_V1(save_keys);
PG_FUNCTION_INFO_V1(load_keys);

/*
 * add a key to the vault
 *
 * TODO maybe use a 'replace' option to replace existing keys
 *
 * - id  (TEXT)
 * - key (BYTEA)
 * - comment (TEXT)
 */
Datum
add_key(PG_FUNCTION_ARGS)
{
	int		i;
	char	*id			= NULL,
			*comment	= NULL;
	bytea	*key		= NULL;

	bool	vault_is_full	= FALSE;
	bool	key_id_unique	= TRUE;

	if (PG_ARGISNULL(0))
		elog(ERROR, "key ID must not be NULL");

	if (PG_ARGISNULL(1))
		elog(ERROR, "key data must not be NULL");

	id	= text_to_cstring(PG_GETARG_TEXT_P(0));
	key	= PG_GETARG_BYTEA_P(1);

	if (! PG_ARGISNULL(2))
		comment = text_to_cstring(PG_GETARG_TEXT_P(2));

	/* check size limits */
	if (strlen(id) >= MAX_ID_LENGTH)
		elog(ERROR, "key ID too long (max=%d len=%ld)", MAX_ID_LENGTH, strlen(id));

	if ((comment != NULL) && (strlen(comment) >= MAX_COMMENT_LENGTH))
		elog(ERROR, "comment too long (max=%d len=%ld)", MAX_COMMENT_LENGTH, strlen(comment));

	if (VARSIZE_ANY_EXHDR(key) >= MAX_KEY_LENGTH)
		elog(ERROR, "key too long (max=%d len=%ld)", MAX_KEY_LENGTH, VARSIZE_ANY_EXHDR(key));

	LWLockAcquire(vault_info->lock, LW_EXCLUSIVE);

	/* do the checks here, but report the errors outside the locked section */

	/* is the key ID unique? */
	for (i = 0; i < vault_info->nitems; i++)
	{
		if (strcmp(vault_info->items[i].id, id) == 0)
		{
			key_id_unique = FALSE;
			break;
		}
	}

	/* is there space for another item? */
	if (vault_info->nitems >= vault_info->maxitems)
		vault_is_full = TRUE;

	/* the key can be added only if the ID is unique and there's enough space */
	if ((!vault_is_full) && key_id_unique)
	{
		/* copy the fields into the structure */
		memcpy(vault_info->items[vault_info->nitems].id, id, strlen(id));
		memcpy(vault_info->items[vault_info->nitems].key, key, VARSIZE_ANY(key));

		if (comment != NULL)
			memcpy(vault_info->items[vault_info->nitems].comment, comment, strlen(comment));

		vault_info->nitems++;
	}

	LWLockRelease(vault_info->lock);

	if (! key_id_unique)
		elog(ERROR, "the supplied key ID '%s' is not unique", id);

	if (vault_is_full)
		elog(ERROR, "cannot add a key - the vault is full");

	PG_RETURN_VOID();
}


/*
 * delete a key from the vault
 *
 * - id  (TEXT)
 */
Datum
delete_key(PG_FUNCTION_ARGS)
{
	int		i;
	char	*id = NULL;

	if (PG_ARGISNULL(0))
		elog(ERROR, "key ID must not be NULL");

	id	= text_to_cstring(PG_GETARG_TEXT_P(0));

	LWLockAcquire(vault_info->lock, LW_EXCLUSIVE);

	/* find the matching item and copy the last item to this place */
	for (i = 0; i < vault_info->nitems; i++)
	{
		if (strcmp(vault_info->items[i].id, id) == 0)
		{
			/* consider the last item already deleted */
			vault_info->nitems--;

			memcpy(&vault_info->items[i], &vault_info->items[vault_info->nitems],
				   sizeof(VaultItemData));

			break;
		}
	}

	LWLockRelease(vault_info->lock);

	/* FIXME Maybe this should report error if the key was not found? */

	PG_RETURN_VOID();
}


/*
 * lookup a key in the vault
 */
Datum
lookup_key(PG_FUNCTION_ARGS)
{
	int		i;
	char	*id = NULL;
	bytea	*key = NULL;

	if (PG_ARGISNULL(0))
		elog(ERROR, "key ID must not be NULL");

	id	= text_to_cstring(PG_GETARG_TEXT_P(0));

	LWLockAcquire(vault_info->lock, LW_SHARED);

	/* find the matching item and copy the last item to this place */
	for (i = 0; i < vault_info->nitems; i++)
	{
		if (strcmp(vault_info->items[i].id, id) == 0)
		{
			key = (bytea*)palloc(VARSIZE_ANY(vault_info->items[i].key));
			memcpy(key, vault_info->items[i].key, VARSIZE_ANY(vault_info->items[i].key));
			break;
		}
	}

	LWLockRelease(vault_info->lock);

	if (key != NULL)
		PG_RETURN_BYTEA_P(key);

	PG_RETURN_NULL();
}


/*
 * list all keys from a vault (without the key data)
 */
Datum
list_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	TupleDesc	   tupdesc;
	AttInMetadata   *attinmeta;

	/* init on the first call */
	if (SRF_IS_FIRSTCALL())
	{

		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		LWLockAcquire(vault_info->lock, LW_EXCLUSIVE);

		/* number of items in the vault */
		funcctx->max_calls = vault_info->nitems;
		funcctx->user_fctx = vault_copy(true);

		LWLockRelease(vault_info->lock);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;
		funcctx->tuple_desc = tupdesc;

		/* switch back to the old context */
		MemoryContextSwitchTo(oldcontext);

	}

	/* init the context */
	funcctx = SRF_PERCALL_SETUP();

	/* check if we have more data */
	if (funcctx->max_calls > funcctx->call_cntr)
	{
		HeapTuple	   tuple;
		Datum		   result;
		Datum		   values[3];
		bool			nulls[3];

		VaultInfo		vault = (VaultInfo)funcctx->user_fctx;
		VaultItem		item = &(vault->items[funcctx->call_cntr]);

		memset(nulls, 0, sizeof(nulls));

		/* key ID */
		values[0] = CStringGetTextDatum(item->id);
		// values[1] = Int32GetDatum(VARSIZE_ANY_EXHDR(item->key));
		values[2] = CStringGetTextDatum(item->comment);

		nulls[1] = true;

		/* Build and return the tuple. */
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		/* Here we want to return another item: */
		SRF_RETURN_NEXT(funcctx, result);

	}
	else
	{
		/* Here we are done returning items and just need to clean up: */
		SRF_RETURN_DONE(funcctx);
	}
}


/*
 * delete all the keys from a vault
 */
Datum
delete_keys(PG_FUNCTION_ARGS)
{
	LWLockAcquire(vault_info->lock, LW_EXCLUSIVE);

	/* just zero all the memory after 'nitems' (keep lock / maxitems) */
	memset((char*)vault_info + offsetof(VaultInfoData, nitems), 0,
		   pgvault_mem_max_size - offsetof(VaultInfoData, nitems));

	LWLockRelease(vault_info->lock);

	PG_RETURN_VOID();
}


static VaultInfo
vault_copy(bool strip_keys)
{
	int i;
	VaultInfo copy = palloc0(pgvault_mem_max_size);
	memcpy(copy, vault_info, pgvault_mem_max_size);

	for (i = 0; i < copy->nitems; i++)
		memset(copy->items[i].key, 0, MAX_KEY_LENGTH);

	return copy;
}
