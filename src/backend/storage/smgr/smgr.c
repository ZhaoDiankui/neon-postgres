/*-------------------------------------------------------------------------
 *
 * smgr.c
 *	  public interface routines to storage manager switch.
 *
 *	  All file system operations in POSTGRES dispatch through these
 *	  routines.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogutils.h"
#include "catalog/pg_tablespace.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"

static const f_smgr smgr_md = {
	/* magnetic disk */
		.smgr_init = mdinit,
		.smgr_shutdown = NULL,
		.smgr_open = mdopen,
		.smgr_close = mdclose,
		.smgr_create = mdcreate,
		.smgr_exists = mdexists,
		.smgr_unlink = mdunlink,
		.smgr_extend = mdextend,
		.smgr_prefetch = mdprefetch,
		.smgr_read = mdread,
		.smgr_write = mdwrite,
		.smgr_writeback = mdwriteback,
		.smgr_nblocks = mdnblocks,
		.smgr_truncate = mdtruncate,
		.smgr_immedsync = mdimmedsync,
};

/*
 * Each backend has a hashtable that stores all extant SMgrRelation objects.
 * In addition, "unowned" SMgrRelation objects are chained together in a list.
 */
static HTAB *SMgrRelationHash = NULL;

static dlist_head unowned_relns;

/* local function prototypes */
//static void smgrshutdown(int code, Datum arg);


/*
 *	smgrinit(), smgrshutdown() -- Initialize or shut down storage
 *								  managers.
 *
 * Note: smgrinit is called during backend startup (normal or standalone
 * case), *not* during postmaster start.  Therefore, any resources created
 * here or destroyed in smgrshutdown are backend-local.
 */
void
smgrinit(void)
{
	(*smgr_init_hook)();

	/*
	 * ZENITH XXX
	 * This doesn't work with inmem_smgr, so temporarily disable.
	 * Anyway, we don't have any real smgrshutdown function.
	 */
	// /* register the shutdown proc */
	// on_proc_exit(smgrshutdown, 0);
}

//ZENITH XXX See comment above. Silence compiler warning.
// /*
//  * on_proc_exit hook for smgr cleanup during backend shutdown
//  */
// static void
// smgrshutdown(int code, Datum arg)
// {
// 	if (smgr_shutdown_hook)
// 		(*smgr_shutdown_hook)();

// 	smgr_shutdown_standard();
// }

/* Hook for plugins to get control in smgr */
smgr_hook_type smgr_hook = NULL;
smgr_init_hook_type smgr_init_hook = smgr_init_standard;
smgr_shutdown_hook_type smgr_shutdown_hook = NULL;

const f_smgr *
smgr_standard(BackendId backend, RelFileNode rnode)
{
	return &smgr_md;
}

void
smgr_init_standard(void)
{
	mdinit();
}

void
smgr_shutdown_standard(void)
{
}

const f_smgr *
smgr(BackendId backend, RelFileNode rnode)
{
	const f_smgr *result;

	if (smgr_hook)
	{
		result = (*smgr_hook)(backend, rnode);
	}
	else
		result = smgr_standard(backend, rnode);

	return result;
}


/*
 *	smgropen() -- Return an SMgrRelation object, creating it if need be.
 *
 *		This does not attempt to actually open the underlying file.
 *
 * The caller should pass the value of pg_class.relpersistence, if they know
 * it, or 0 if unknown. Some operations, like smgrwrite() and smgrunlink()
 * are allowed when relpersistence is not known, but others like smgrread()
 * require it.
 */
SMgrRelation
smgropen(RelFileNode rnode, BackendId backend, char relpersistence)
{
	RelFileNodeBackend brnode;
	SMgrRelation reln;
	bool		found;

	if (SMgrRelationHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileNodeBackend);
		ctl.entrysize = sizeof(SMgrRelationData);
		SMgrRelationHash = hash_create("smgr relation table", 400,
									   &ctl, HASH_ELEM | HASH_BLOBS);
		dlist_init(&unowned_relns);
	}

	/* Look up or create an entry */
	brnode.node = rnode;
	brnode.backend = backend;
	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &brnode,
									  HASH_ENTER, &found);

	/* Initialize it if not present before */
	if (!found)
	{
		/* hash_search already filled in the lookup key */
		reln->smgr_owner = NULL;
		reln->smgr_targblock = InvalidBlockNumber;
		reln->smgr_relpersistence = relpersistence;
		for (int i = 0; i <= MAX_FORKNUM; ++i)
			reln->smgr_cached_nblocks[i] = InvalidBlockNumber;

		reln->smgr = smgr(backend, rnode);

		/* implementation-specific initialization */
		(*reln->smgr).smgr_open(reln);

		/* it has no owner yet */
		dlist_push_tail(&unowned_relns, &reln->node);
	}
	else
	{
		/*
		 * If the caller passed a valid 'relpersistence', and it was unknown
		 * before, update it.
		 */
		if (reln->smgr_relpersistence == 0)
			reln->smgr_relpersistence = relpersistence;
		else
		{
			if (!(relpersistence == 0 || reln->smgr_relpersistence == relpersistence))
				elog(ERROR, "relpersistence mismatch: smgropen %c vs SmgrRelation %c",
					 relpersistence, reln->smgr_relpersistence);
		}
	}

	return reln;
}

/*
 * smgrsetowner() -- Establish a long-lived reference to an SMgrRelation object
 *
 * There can be only one owner at a time; this is sufficient since currently
 * the only such owners exist in the relcache.
 */
void
smgrsetowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* We don't support "disowning" an SMgrRelation here, use smgrclearowner */
	Assert(owner != NULL);

	/*
	 * First, unhook any old owner.  (Normally there shouldn't be any, but it
	 * seems possible that this can happen during swap_relation_files()
	 * depending on the order of processing.  It's ok to close the old
	 * relcache entry early in that case.)
	 *
	 * If there isn't an old owner, then the reln should be in the unowned
	 * list, and we need to remove it.
	 */
	if (reln->smgr_owner)
		*(reln->smgr_owner) = NULL;
	else
		dlist_delete(&reln->node);

	/* Now establish the ownership relationship. */
	reln->smgr_owner = owner;
	*owner = reln;
}

/*
 * smgrclearowner() -- Remove long-lived reference to an SMgrRelation object
 *					   if one exists
 */
void
smgrclearowner(SMgrRelation *owner, SMgrRelation reln)
{
	/* Do nothing if the SMgrRelation object is not owned by the owner */
	if (reln->smgr_owner != owner)
		return;

	/* unset the owner's reference */
	*owner = NULL;

	/* unset our reference to the owner */
	reln->smgr_owner = NULL;

	/* add to list of unowned relations */
	dlist_push_tail(&unowned_relns, &reln->node);
}

/*
 *	smgrexists() -- Does the underlying file for a fork exist?
 */
bool
smgrexists(SMgrRelation reln, ForkNumber forknum)
{
	return (*reln->smgr).smgr_exists(reln, forknum);
}

/*
 *	smgrclose() -- Close and delete an SMgrRelation object.
 */
void
smgrclose(SMgrRelation reln)
{
	SMgrRelation *owner;
	ForkNumber	forknum;

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		(*reln->smgr).smgr_close(reln, forknum);

	owner = reln->smgr_owner;

	if (!owner)
		dlist_delete(&reln->node);

	if (hash_search(SMgrRelationHash,
					(void *) &(reln->smgr_rnode),
					HASH_REMOVE, NULL) == NULL)
		elog(ERROR, "SMgrRelation hashtable corrupted");

	/*
	 * Unhook the owner pointer, if any.  We do this last since in the remote
	 * possibility of failure above, the SMgrRelation object will still exist.
	 */
	if (owner)
		*owner = NULL;
}

/*
 *	smgrrelease() -- Release all resources used by this object.
 *
 *	The object remains valid.
 */
void
smgrrelease(SMgrRelation reln)
{
	for (ForkNumber forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		(*reln->smgr).smgr_close(reln, forknum);
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
	}
}

/*
 *	smgrreleaseall() -- Release resources used by all objects.
 *
 *	This is called for PROCSIGNAL_BARRIER_SMGRRELEASE.
 */
void
smgrreleaseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrrelease(reln);
}

/*
 *	smgrcloseall() -- Close all existing SMgrRelation objects.
 */
void
smgrcloseall(void)
{
	HASH_SEQ_STATUS status;
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	hash_seq_init(&status, SMgrRelationHash);

	while ((reln = (SMgrRelation) hash_seq_search(&status)) != NULL)
		smgrclose(reln);
}

/*
 *	smgrclosenode() -- Close SMgrRelation object for given RelFileNode,
 *					   if one exists.
 *
 * This has the same effects as smgrclose(smgropen(rnode)), but it avoids
 * uselessly creating a hashtable entry only to drop it again when no
 * such entry exists already.
 */
void
smgrclosenode(RelFileNodeBackend rnode)
{
	SMgrRelation reln;

	/* Nothing to do if hashtable not set up */
	if (SMgrRelationHash == NULL)
		return;

	reln = (SMgrRelation) hash_search(SMgrRelationHash,
									  (void *) &rnode,
									  HASH_FIND, NULL);
	if (reln != NULL)
		smgrclose(reln);
}

/*
 *	smgrcreate() -- Create a new relation.
 *
 *		Given an already-created (but presumably unused) SMgrRelation,
 *		cause the underlying disk file or other storage for the fork
 *		to be created.
 */
void
smgrcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	(*reln->smgr).smgr_create(reln, forknum, isRedo);
}

/*
 *	smgrdosyncall() -- Immediately sync all forks of all given relations
 *
 *		All forks of all given relations are synced out to the store.
 *
 *		This is equivalent to FlushRelationBuffers() for each smgr relation,
 *		then calling smgrimmedsync() for all forks of each relation, but it's
 *		significantly quicker so should be preferred when possible.
 */
void
smgrdosyncall(SMgrRelation *rels, int nrels)
{
	int			i = 0;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	FlushRelationsAllBuffers(rels, nrels);

	/*
	 * Sync the physical file(s).
	 */
	for (i = 0; i < nrels; i++)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		{
			if ((*rels[i]->smgr).smgr_exists(rels[i], forknum))
				(*rels[i]->smgr).smgr_immedsync(rels[i], forknum);
		}
	}
}

/*
 *	smgrdounlinkall() -- Immediately unlink all forks of all given relations
 *
 *		All forks of all given relations are removed from the store.  This
 *		should not be used during transactional operations, since it can't be
 *		undone.
 *
 *		If isRedo is true, it is okay for the underlying file(s) to be gone
 *		already.
 */
void
smgrdounlinkall(SMgrRelation *rels, int nrels, bool isRedo)
{
	int			i = 0;
	RelFileNodeBackend *rnodes;
	ForkNumber	forknum;

	if (nrels == 0)
		return;

	/*
	 * Get rid of any remaining buffers for the relations.  bufmgr will just
	 * drop them without bothering to write the contents.
	 */
	DropRelFileNodesAllBuffers(rels, nrels);

	/*
	 * create an array which contains all relations to be dropped, and close
	 * each relation's forks at the smgr level while at it
	 */
	rnodes = palloc(sizeof(RelFileNodeBackend) * nrels);
	for (i = 0; i < nrels; i++)
	{
		RelFileNodeBackend rnode = rels[i]->smgr_rnode;

		rnodes[i] = rnode;

		/* Close the forks at smgr level */
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			(*rels[i]->smgr).smgr_close(rels[i], forknum);
	}

	/*
	 * Send a shared-inval message to force other backends to close any
	 * dangling smgr references they may have for these rels.  We should do
	 * this before starting the actual unlinking, in case we fail partway
	 * through that step.  Note that the sinval messages will eventually come
	 * back to this backend, too, and thereby provide a backstop that we
	 * closed our own smgr rel.
	 */
	for (i = 0; i < nrels; i++)
		CacheInvalidateSmgr(rnodes[i]);

	/*
	 * Delete the physical file(s).
	 *
	 * Note: smgr_unlink must treat deletion failure as a WARNING, not an
	 * ERROR, because we've already decided to commit or abort the current
	 * xact.
	 */

	for (i = 0; i < nrels; i++)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			(*rels[i]->smgr).smgr_unlink(rnodes[i], forknum, isRedo);
	}

	pfree(rnodes);
}


/*
 *	smgrextend() -- Add a new block to a file.
 *
 *		The semantics are nearly the same as smgrwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
void
smgrextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   char *buffer, bool skipFsync)
{
	(*reln->smgr).smgr_extend(reln, forknum, blocknum,
										 buffer, skipFsync);

	/*
	 * Normally we expect this to increase nblocks by one, but if the cached
	 * value isn't as expected, just invalidate it so the next call asks the
	 * kernel.
	 */
	if (reln->smgr_cached_nblocks[forknum] == blocknum)
		reln->smgr_cached_nblocks[forknum] = blocknum + 1;
	else
		reln->smgr_cached_nblocks[forknum] = InvalidBlockNumber;
}

/*
 *	smgrprefetch() -- Initiate asynchronous read of the specified block of a relation.
 *
 *		In recovery only, this can return false to indicate that a file
 *		doesn't	exist (presumably it has been dropped by a later WAL
 *		record).
 */
bool
smgrprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return (*reln->smgr).smgr_prefetch(reln, forknum, blocknum);
}

/*
 *	smgrread() -- read a particular block from a relation into the supplied
 *				  buffer.
 *
 *		This routine is called from the buffer manager in order to
 *		instantiate pages in the shared buffer cache.  All storage managers
 *		return pages in the format that POSTGRES expects.
 */
void
smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 char *buffer)
{
	(*reln->smgr).smgr_read(reln, forknum, blocknum, buffer);
}

/*
 *	smgrwrite() -- Write the supplied buffer out.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use smgrextend().
 *
 *		This is not a synchronous write -- the block is not necessarily
 *		on disk at return, only dumped out to the kernel.  However,
 *		provisions will be made to fsync the write before the next checkpoint.
 *
 *		skipFsync indicates that the caller will make other provisions to
 *		fsync the relation, so we needn't bother.  Temporary relations also
 *		do not require fsync.
 */
void
smgrwrite(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		  char *buffer, bool skipFsync)
{
	(*reln->smgr).smgr_write(reln, forknum, blocknum,
										buffer, skipFsync);
}


/*
 *	smgrwriteback() -- Trigger kernel writeback for the supplied range of
 *					   blocks.
 */
void
smgrwriteback(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			  BlockNumber nblocks)
{
	(*reln->smgr).smgr_writeback(reln, forknum, blocknum,
											nblocks);
}

/*
 *	smgrnblocks() -- Calculate the number of blocks in the
 *					 supplied relation.
 */
BlockNumber
smgrnblocks(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	/* Check and return if we get the cached value for the number of blocks. */
	result = smgrnblocks_cached(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	result = (*reln->smgr).smgr_nblocks(reln, forknum);

	reln->smgr_cached_nblocks[forknum] = result;

	return result;
}

/*
 *	smgrnblocks_cached() -- Get the cached number of blocks in the supplied
 *							relation.
 *
 * Returns an InvalidBlockNumber when not in recovery and when the relation
 * fork size is not cached.
 */
BlockNumber
smgrnblocks_cached(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * For now, we only use cached values in recovery due to lack of a shared
	 * invalidation mechanism for changes in file size.
	 */
	if (InRecovery && reln->smgr_cached_nblocks[forknum] != InvalidBlockNumber)
		return reln->smgr_cached_nblocks[forknum];

	return InvalidBlockNumber;
}

/*
 *	smgrtruncate() -- Truncate the given forks of supplied relation to
 *					  each specified numbers of blocks
 *
 * The truncation is done immediately, so this can't be rolled back.
 *
 * The caller must hold AccessExclusiveLock on the relation, to ensure that
 * other backends receive the smgr invalidation event that this function sends
 * before they access any forks of the relation again.
 */
void
smgrtruncate(SMgrRelation reln, ForkNumber *forknum, int nforks, BlockNumber *nblocks)
{
	int			i;

	/*
	 * Get rid of any buffers for the about-to-be-deleted blocks. bufmgr will
	 * just drop them without bothering to write the contents.
	 */
	DropRelFileNodeBuffers(reln, forknum, nforks, nblocks);

	/*
	 * Send a shared-inval message to force other backends to close any smgr
	 * references they may have for this rel.  This is useful because they
	 * might have open file pointers to segments that got removed, and/or
	 * smgr_targblock variables pointing past the new rel end.  (The inval
	 * message will come back to our backend, too, causing a
	 * probably-unnecessary local smgr flush.  But we don't expect that this
	 * is a performance-critical path.)  As in the unlink code, we want to be
	 * sure the message is sent before we start changing things on-disk.
	 */
	CacheInvalidateSmgr(reln->smgr_rnode);

	/* Do the truncation */
	for (i = 0; i < nforks; i++)
	{
		/* Make the cached size is invalid if we encounter an error. */
		reln->smgr_cached_nblocks[forknum[i]] = InvalidBlockNumber;

		(*reln->smgr).smgr_truncate(reln, forknum[i], nblocks[i]);

		/*
		 * We might as well update the local smgr_cached_nblocks values. The
		 * smgr cache inval message that this function sent will cause other
		 * backends to invalidate their copies of smgr_fsm_nblocks and
		 * smgr_vm_nblocks, and these ones too at the next command boundary.
		 * But these ensure they aren't outright wrong until then.
		 */
		reln->smgr_cached_nblocks[forknum[i]] = nblocks[i];
	}
}

/*
 *	smgrimmedsync() -- Force the specified relation to stable storage.
 *
 *		Synchronously force all previous writes to the specified relation
 *		down to disk.
 *
 *		This is useful for building completely new relations (eg, new
 *		indexes).  Instead of incrementally WAL-logging the index build
 *		steps, we can just write completed index pages to disk with smgrwrite
 *		or smgrextend, and then fsync the completed index file before
 *		committing the transaction.  (This is sufficient for purposes of
 *		crash recovery, since it effectively duplicates forcing a checkpoint
 *		for the completed index.  But it is *not* sufficient if one wishes
 *		to use the WAL log for PITR or replication purposes: in that case
 *		we have to make WAL entries as well.)
 *
 *		The preceding writes should specify skipFsync = true to avoid
 *		duplicative fsyncs.
 *
 *		Note that you need to do FlushRelationBuffers() first if there is
 *		any possibility that there are dirty buffers for the relation;
 *		otherwise the sync is not very meaningful.
 */
void
smgrimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	(*reln->smgr).smgr_immedsync(reln, forknum);
}

/*
 * Zenith-added functions to mark the phases of an unlogged index build.
 */
void
smgr_start_unlogged_build(SMgrRelation reln)
{
	if ((*reln->smgr).smgr_start_unlogged_build)
		(*reln->smgr).smgr_start_unlogged_build(reln);
}

void
smgr_finish_unlogged_build_phase_1(SMgrRelation reln)
{
	if ((*reln->smgr).smgr_finish_unlogged_build_phase_1)
		(*reln->smgr).smgr_finish_unlogged_build_phase_1(reln);
}

void
smgr_end_unlogged_build(SMgrRelation reln)
{
	if ((*reln->smgr).smgr_end_unlogged_build)
		(*reln->smgr).smgr_end_unlogged_build(reln);
}

/*
 * AtEOXact_SMgr
 *
 * This routine is called during transaction commit or abort (it doesn't
 * particularly care which).  All transient SMgrRelation objects are closed.
 *
 * We do this as a compromise between wanting transient SMgrRelations to
 * live awhile (to amortize the costs of blind writes of multiple blocks)
 * and needing them to not live forever (since we're probably holding open
 * a kernel file descriptor for the underlying file, and we need to ensure
 * that gets closed reasonably soon if the file gets deleted).
 */
void
AtEOXact_SMgr(void)
{
	dlist_mutable_iter iter;

	/*
	 * Zap all unowned SMgrRelations.  We rely on smgrclose() to remove each
	 * one from the list.
	 */
	dlist_foreach_modify(iter, &unowned_relns)
	{
		SMgrRelation rel = dlist_container(SMgrRelationData, node,
										   iter.cur);

		Assert(rel->smgr_owner == NULL);

		smgrclose(rel);
	}
}

/*
 * This routine is called when we are ordered to release all open files by a
 * ProcSignalBarrier.
 */
bool
ProcessBarrierSmgrRelease(void)
{
	smgrreleaseall();
	return true;
}
