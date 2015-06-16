/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2015 by Delphix. All rights reserved.
 */

#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/spa.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_indirect_mapping.h>
#include <util/bsearch.h>
#include <sys/zfeature.h>
#include <sys/dmu_objset.h>

static boolean_t
vdev_indirect_mapping_verify(vdev_indirect_mapping_t *vim)
{
	ASSERT(vim != NULL);

	ASSERT(vim->vim_object != 0);
	ASSERT(vim->vim_objset != NULL);
	ASSERT(vim->vim_phys != NULL);
	ASSERT(vim->vim_dbuf != NULL);

	EQUIV(vim->vim_phys->vimp_num_entries > 0,
	    vim->vim_entries != NULL);
	if (vim->vim_phys->vimp_num_entries > 0) {
		vdev_indirect_mapping_entry_phys_t *last_entry =
		    &vim->vim_entries[vim->vim_phys->vimp_num_entries - 1];
		uint64_t offset = DVA_MAPPING_GET_SRC_OFFSET(last_entry);
		uint64_t size = DVA_GET_ASIZE(&last_entry->vimep_dst);

		ASSERT3U(vim->vim_phys->vimp_max_offset, >=, offset + size);
	}
	if (vim->vim_havecounts) {
		ASSERT(vim->vim_phys->vimp_counts_object != 0);
	}

	return (B_TRUE);
}

uint64_t
vdev_indirect_mapping_num_entries(vdev_indirect_mapping_t *vim)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	return (vim->vim_phys->vimp_num_entries);
}

uint64_t
vdev_indirect_mapping_max_offset(vdev_indirect_mapping_t *vim)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	return (vim->vim_phys->vimp_max_offset);
}

uint64_t
vdev_indirect_mapping_object(vdev_indirect_mapping_t *vim)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	return (vim->vim_object);
}

uint64_t
vdev_indirect_mapping_bytes_mapped(vdev_indirect_mapping_t *vim)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	return (vim->vim_phys->vimp_bytes_mapped);
}

/*
 * The length (in bytes) of the mapping object array in memory and
 * (logically) on disk.
 *
 * Note that unlike most of our accessor functions,
 * we don't assert that the struct is consistent; therefore it can be
 * called while there may be concurrent changes, if we don't care about
 * the value being immediately stale (e.g. from spa_removal_get_stats()).
 */
uint64_t
vdev_indirect_mapping_size(vdev_indirect_mapping_t *vim)
{
	return (vim->vim_phys->vimp_num_entries * sizeof (*vim->vim_entries));
}

static int
dva_mapping_overlap_compare(const void *v_key, const void *v_array_elem)
{
	const uint64_t const *key = v_key;
	const vdev_indirect_mapping_entry_phys_t const *array_elem =
	    v_array_elem;
	uint64_t src_offset = DVA_MAPPING_GET_SRC_OFFSET(array_elem);

	if (*key < src_offset) {
		return (-1);
	} else if (*key < src_offset + DVA_GET_ASIZE(&array_elem->vimep_dst)) {
		return (0);
	} else {
		return (1);
	}
}

/*
 * Returns the mapping entry for the given offset.
 *
 * The offset must be present in the mapping.
 */
vdev_indirect_mapping_entry_phys_t *
vdev_indirect_mapping_entry_for_offset(vdev_indirect_mapping_t *vim,
    uint64_t offset)
{
	ASSERT(vdev_indirect_mapping_verify(vim));
	ASSERT(vim->vim_phys->vimp_num_entries > 0);

	vdev_indirect_mapping_entry_phys_t *entry =
	    bsearch(&offset, vim->vim_entries, vim->vim_phys->vimp_num_entries,
	    sizeof (vdev_indirect_mapping_entry_phys_t),
	    dva_mapping_overlap_compare);

	ASSERT(entry != NULL);

	return (entry);
}

void
vdev_indirect_mapping_close(vdev_indirect_mapping_t *vim)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	if (vim->vim_phys->vimp_num_entries > 0) {
		uint64_t map_size = vdev_indirect_mapping_size(vim);
		kmem_free(vim->vim_entries, map_size);
		vim->vim_entries = NULL;
	}

	dmu_buf_rele(vim->vim_dbuf, vim);

	vim->vim_objset = NULL;
	vim->vim_object = 0;
	vim->vim_dbuf = NULL;
	vim->vim_phys = NULL;

	kmem_free(vim, sizeof (*vim));
}

uint64_t
vdev_indirect_mapping_alloc(objset_t *os, dmu_tx_t *tx)
{
	uint64_t object;
	ASSERT(dmu_tx_is_syncing(tx));
	uint64_t bonus_size = VDEV_INDIRECT_MAPPING_SIZE_V0;

	if (spa_feature_is_enabled(os->os_spa, SPA_FEATURE_OBSOLETE_COUNTS)) {
		bonus_size = sizeof (vdev_indirect_mapping_phys_t);
	}

	object = dmu_object_alloc(os,
	    DMU_OTN_UINT64_METADATA, SPA_OLD_MAXBLOCKSIZE,
	    DMU_OTN_UINT64_METADATA, bonus_size,
	    tx);

	if (spa_feature_is_enabled(os->os_spa, SPA_FEATURE_OBSOLETE_COUNTS)) {
		dmu_buf_t *dbuf;
		vdev_indirect_mapping_phys_t *vimp;

		VERIFY0(dmu_bonus_hold(os, object, FTAG, &dbuf));
		dmu_buf_will_dirty(dbuf, tx);
		vimp = dbuf->db_data;
		vimp->vimp_counts_object = dmu_object_alloc(os,
		    DMU_OTN_UINT32_METADATA, SPA_OLD_MAXBLOCKSIZE,
		    DMU_OT_NONE, 0, tx);
		spa_feature_incr(os->os_spa, SPA_FEATURE_OBSOLETE_COUNTS, tx);
		dmu_buf_rele(dbuf, FTAG);
	}

	return (object);
}


vdev_indirect_mapping_t *
vdev_indirect_mapping_open(objset_t *os, uint64_t mapping_object)
{
	vdev_indirect_mapping_t *vim = kmem_zalloc(sizeof (*vim), KM_SLEEP);
	dmu_object_info_t doi;
	VERIFY0(dmu_object_info(os, mapping_object, &doi));

	vim->vim_objset = os;
	vim->vim_object = mapping_object;

	VERIFY0(dmu_bonus_hold(os, vim->vim_object, vim,
	    &vim->vim_dbuf));
	vim->vim_phys = vim->vim_dbuf->db_data;

	vim->vim_havecounts =
	    (doi.doi_bonus_size > VDEV_INDIRECT_MAPPING_SIZE_V0);

	if (vim->vim_phys->vimp_num_entries > 0) {
		uint64_t map_size = vdev_indirect_mapping_size(vim);
		vim->vim_entries = kmem_alloc(map_size, KM_SLEEP);
		VERIFY0(dmu_read(os, vim->vim_object, 0, map_size,
		    vim->vim_entries, DMU_READ_PREFETCH));
	}

	ASSERT(vdev_indirect_mapping_verify(vim));

	return (vim);
}

void
vdev_indirect_mapping_free(objset_t *os, uint64_t object, dmu_tx_t *tx)
{
	vdev_indirect_mapping_t *vim = vdev_indirect_mapping_open(os, object);
	if (vim->vim_havecounts) {
		VERIFY0(dmu_object_free(os, vim->vim_phys->vimp_counts_object,
		    tx));
		spa_feature_decr(os->os_spa, SPA_FEATURE_OBSOLETE_COUNTS, tx);
	}
	vdev_indirect_mapping_close(vim);

	VERIFY0(dmu_object_free(os, object, tx));
}

/*
 * Append the list of vdev_indirect_mapping_entry_t's to the on-disk
 * mapping object.  Also remove the entries from the list and free them.
 * This also implicitly extends the max_offset of the mapping (to the end
 * of the last entry).
 */
void
vdev_indirect_mapping_add_entries(vdev_indirect_mapping_t *vim,
    list_t *list, dmu_tx_t *tx)
{
	vdev_indirect_mapping_entry_phys_t *mapbuf;
	uint64_t old_size;
	uint32_t *countbuf = NULL;
	vdev_indirect_mapping_entry_phys_t *old_entries;
	uint64_t old_count;
	uint64_t entries_written = 0;

	ASSERT(vdev_indirect_mapping_verify(vim));
	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(dsl_pool_sync_context(dmu_tx_pool(tx)));
	ASSERT(!list_is_empty(list));

	old_size = vdev_indirect_mapping_size(vim);
	old_entries = vim->vim_entries;
	old_count = vim->vim_phys->vimp_num_entries;

	dmu_buf_will_dirty(vim->vim_dbuf, tx);

	mapbuf = zio_buf_alloc(SPA_OLD_MAXBLOCKSIZE);
	if (vim->vim_havecounts) {
		countbuf = zio_buf_alloc(SPA_OLD_MAXBLOCKSIZE);
		ASSERT(spa_feature_is_active(vim->vim_objset->os_spa,
		    SPA_FEATURE_OBSOLETE_COUNTS));
	}
	while (!list_is_empty(list)) {
		uint64_t i;
		/*
		 * Write entries from the list to the
		 * vdev_im_object in batches of size SPA_OLD_MAXBLOCKSIZE.
		 */
		for (i = 0; i < SPA_OLD_MAXBLOCKSIZE / sizeof (*mapbuf); i++) {
			vdev_indirect_mapping_entry_t *entry =
			    list_remove_head(list);
			if (entry == NULL)
				break;

			uint64_t size =
			    DVA_GET_ASIZE(&entry->vime_mapping.vimep_dst);
			uint64_t src_offset =
			    DVA_MAPPING_GET_SRC_OFFSET(&entry->vime_mapping);

			/*
			 * We shouldn't be adding an entry which is fully
			 * obsolete.
			 */
			ASSERT3U(entry->vime_obsolete_count, <, size);
			IMPLY(entry->vime_obsolete_count != 0,
			    vim->vim_havecounts);

			mapbuf[i] = entry->vime_mapping;
			if (vim->vim_havecounts)
				countbuf[i] = entry->vime_obsolete_count;

			vim->vim_phys->vimp_bytes_mapped += size;
			ASSERT3U(src_offset, >=,
			    vim->vim_phys->vimp_max_offset);
			vim->vim_phys->vimp_max_offset = src_offset + size;

			entries_written++;

			kmem_free(entry, sizeof (*entry));
		}
		dmu_write(vim->vim_objset, vim->vim_object,
		    vim->vim_phys->vimp_num_entries * sizeof (*mapbuf),
		    i * sizeof (*mapbuf),
		    mapbuf, tx);
		if (vim->vim_havecounts) {
			dmu_write(vim->vim_objset,
			    vim->vim_phys->vimp_counts_object,
			    vim->vim_phys->vimp_num_entries *
			    sizeof (*countbuf),
			    i * sizeof (*countbuf), countbuf, tx);
		}
		vim->vim_phys->vimp_num_entries += i;
	}
	zio_buf_free(mapbuf, SPA_OLD_MAXBLOCKSIZE);
	if (vim->vim_havecounts)
		zio_buf_free(countbuf, SPA_OLD_MAXBLOCKSIZE);

	/*
	 * Update the entry array to reflect the new entries. First, copy
	 * over any old entries then read back the new entries we just wrote.
	 */
	uint64_t new_size = vdev_indirect_mapping_size(vim);
	ASSERT3U(new_size, >, old_size);
	ASSERT3U(new_size - old_size, ==,
	    entries_written * sizeof (vdev_indirect_mapping_entry_phys_t));
	vim->vim_entries = kmem_alloc(new_size, KM_SLEEP);
	if (old_size > 0) {
		bcopy(old_entries, vim->vim_entries, old_size);
		kmem_free(old_entries, old_size);
	}
	VERIFY0(dmu_read(vim->vim_objset, vim->vim_object, old_size,
	    new_size - old_size, &vim->vim_entries[old_count],
	    DMU_READ_PREFETCH));

	zfs_dbgmsg("txg %llu: wrote %llu entries to "
	    "indirect mapping obj %llu; max offset=0x%llx",
	    (u_longlong_t)dmu_tx_get_txg(tx),
	    (u_longlong_t)entries_written,
	    (u_longlong_t)vim->vim_object,
	    (u_longlong_t)vim->vim_phys->vimp_max_offset);
}

void
vdev_indirect_mapping_extend_max_offset(vdev_indirect_mapping_t *vim,
    uint64_t offset, dmu_tx_t *tx)
{
	ASSERT(vdev_indirect_mapping_verify(vim));
	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(dsl_pool_sync_context(dmu_tx_pool(tx)));
	ASSERT3U(offset, >=, vim->vim_phys->vimp_max_offset);

	dmu_buf_will_dirty(vim->vim_dbuf, tx);
	vim->vim_phys->vimp_max_offset = offset;
}

/*
 * Increment the relevant counts for the specified offset and length.
 * The counts array must be obtained from
 * vdev_indirect_mapping_load_obsolete_counts().
 */
void
vdev_indirect_mapping_increment_obsolete_count(vdev_indirect_mapping_t *vim,
    uint64_t offset, uint64_t length, uint32_t *counts)
{
	vdev_indirect_mapping_entry_phys_t *mapping;
	uint64_t index;

	mapping = vdev_indirect_mapping_entry_for_offset(vim,  offset);

	ASSERT(length > 0);
	ASSERT3P(mapping, !=, NULL);

	index = mapping - vim->vim_entries;

	while (length > 0) {
		ASSERT3U(index, <, vdev_indirect_mapping_num_entries(vim));

		uint64_t size = DVA_GET_ASIZE(&mapping->vimep_dst);
		uint64_t inner_offset = offset -
		    DVA_MAPPING_GET_SRC_OFFSET(mapping);
		VERIFY3U(inner_offset, <, size);
		uint64_t inner_size = MIN(length, size - inner_offset);

		VERIFY3U(counts[index] + inner_size, <=, size);
		counts[index] += inner_size;

		offset += inner_size;
		length -= inner_size;
		mapping++;
		index++;
	}
}

typedef struct load_obsolete_space_map_arg {
	vdev_indirect_mapping_t	*losma_vim;
	uint32_t		*losma_counts;
} load_obsolete_space_map_arg_t;

static int
load_obsolete_sm_callback(maptype_t type, uint64_t offset, uint64_t size,
    void *arg)
{
	load_obsolete_space_map_arg_t *losma = arg;
	ASSERT3S(type, ==, SM_ALLOC);

	vdev_indirect_mapping_increment_obsolete_count(losma->losma_vim,
	    offset, size, losma->losma_counts);

	return (0);
}

/*
 * Modify the counts (increment them) based on the spacemap.
 */
void
vdev_indirect_mapping_load_obsolete_spacemap(vdev_indirect_mapping_t *vim,
    uint32_t *counts, space_map_t *obsolete_space_sm)
{
	load_obsolete_space_map_arg_t losma;
	losma.losma_counts = counts;
	losma.losma_vim = vim;
	VERIFY0(space_map_iterate(obsolete_space_sm,
	    load_obsolete_sm_callback, &losma));
}

/*
 * Read the obsolete counts from disk, returning them in an array.
 */
uint32_t *
vdev_indirect_mapping_load_obsolete_counts(vdev_indirect_mapping_t *vim)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	uint64_t counts_size =
	    vim->vim_phys->vimp_num_entries * sizeof (uint32_t);
	uint32_t *counts = kmem_alloc(counts_size, KM_SLEEP);
	if (vim->vim_havecounts) {
		VERIFY0(dmu_read(vim->vim_objset,
		    vim->vim_phys->vimp_counts_object,
		    0, counts_size,
		    counts, DMU_READ_PREFETCH));
	} else {
		bzero(counts, counts_size);
	}
	return (counts);
}

extern void
vdev_indirect_mapping_free_obsolete_counts(vdev_indirect_mapping_t *vim,
    uint32_t *counts)
{
	ASSERT(vdev_indirect_mapping_verify(vim));

	kmem_free(counts, vim->vim_phys->vimp_num_entries * sizeof (uint32_t));
}
