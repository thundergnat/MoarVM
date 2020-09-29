#include "moar.h"

#define PTR_LOAD_FACTOR 0.75
#define PTR_INITIAL_SIZE 8
#define PTR_INITIAL_KEY_RIGHT_SHIFT (8 * sizeof(uintptr_t) - 3)

MVM_STATIC_INLINE MVMuint32 hash_true_size(const struct MVMPtrHashTableControl *control) {
    MVMuint32 true_size = control->official_size + control->max_items - 1;
    if (control->official_size + MVM_HASH_MAX_PROBE_DISTANCE < true_size) {
        true_size = control->official_size + MVM_HASH_MAX_PROBE_DISTANCE;
    }
    return true_size;
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_ptr_hash_demolish(MVMThreadContext *tc, MVMPtrHashTable *hashtable) {
    struct MVMPtrHashTableControl *control = hashtable->table;
    if (!control)
        return;
    if (control->entries) {
        MVM_free(control->entries
                 - sizeof(struct MVMPtrHashEntry) * (hash_true_size(control) - 1));
    }
    MVM_free(control);
    hashtable->table = NULL;
}
/* and then free memory if you allocated it */


MVM_STATIC_INLINE void hash_allocate_common(struct MVMPtrHashTableControl *control) {
    control->max_items = control->official_size * PTR_LOAD_FACTOR;
    size_t actual_items = hash_true_size(control);
    size_t entries_size = sizeof(struct MVMPtrHashEntry) * actual_items;
    size_t metadata_size = 1 + actual_items + 1;
    control->metadata
        = (MVMuint8 *) MVM_malloc(entries_size + metadata_size) + entries_size;
    memset(control->metadata, 0, metadata_size);
    /* We point to the *last* entry in the array, not the one-after-the end. */
    control->entries = control->metadata - sizeof(struct MVMPtrHashEntry);
    /* A sentinel. This marks an occupied slot, at its ideal position. */
    *control->metadata = 1;
    ++control->metadata;
    /* A sentinel at the other end. Again, occupied, ideal position. */
    control->metadata[actual_items] = 1;
}

MVM_STATIC_INLINE void hash_initial_allocate(struct MVMPtrHashTableControl *control) {
    control->key_right_shift = PTR_INITIAL_KEY_RIGHT_SHIFT;
    control->official_size = PTR_INITIAL_SIZE;

    hash_allocate_common(control);
}

/* make sure you still have your copies of entries and metadata before you
   call this. */
MVM_STATIC_INLINE void hash_grow(struct MVMPtrHashTableControl *control) {
    --control->key_right_shift;
    control->official_size *= 2;

    hash_allocate_common(control);
}

MVM_STATIC_INLINE struct MVMPtrHashEntry *hash_insert_internal(MVMThreadContext *tc,
                                                               struct MVMPtrHashTableControl *control,
                                                               const void *key) {
    if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        MVM_oops(tc, "oops, attempt to recursively call grow when adding %p",
                 key);
    }

    unsigned int probe_distance = 1;
    MVMHashNumItems bucket = MVM_ptr_hash_code(key) >> control->key_right_shift;
    MVMuint8 *entry_raw = MVM_ptr_hash_entries(control) - bucket * sizeof(struct MVMPtrHashEntry);
    MVMuint8 *metadata = MVM_ptr_hash_metadata(control) + bucket;
    while (1) {
        if (*metadata < probe_distance) {
            /* this is our slot. occupied or not, it is our rightful place. */

            if (*metadata == 0) {
                /* Open goal. Score! */
            } else {
                /* make room. */

                /* Optimisation first seen in Martin Ankerl's implementation -
                   we don't need actually implement the "stealing" by swapping
                   elements and carrying on with insert. The invariant of the
                   hash is that probe distances are never out of order, and as
                   all the following elements have probe distances in order, we
                   can maintain the invariant just as well by moving everything
                   along by one. */
                MVMuint8 *find_me_a_gap = metadata;
                MVMuint8 old_probe_distance = *metadata;
                do {
                    MVMuint8 new_probe_distance = 1 + old_probe_distance;
                    if (new_probe_distance == MVM_HASH_MAX_PROBE_DISTANCE) {
                        /* Optimisation from Martin Ankerl's implementation:
                           setting this to zero forces a resize on any insert,
                           *before* the actual insert, so that we never end up
                           having to handle overflow *during* this loop. This
                           loop can always complete. */
                        control->max_items = 0;
                    }
                    /* a swap: */
                    old_probe_distance = *++find_me_a_gap;
                    *find_me_a_gap = new_probe_distance;
                } while (old_probe_distance);

                MVMuint32 entries_to_move = find_me_a_gap - metadata;
                size_t size_to_move = sizeof(struct MVMPtrHashEntry) * entries_to_move;
                /* When we had entries *ascending* this was
                 * memmove(entry_raw + sizeof(struct MVMPtrHashEntry), entry_raw,
                 *         sizeof(struct MVMPtrHashEntry) * entries_to_move);
                 * because we point to the *start* of the block of memory we
                 * want to move, and we want to move it one "entry" forwards.
                 * `entry_raw` is still a pointer to where we want to make free
                 * space, but what want to do now is move everything at it and
                 * *before* it downwards. */
                MVMuint8 *dest = entry_raw - size_to_move;
                memmove(dest, dest + sizeof(struct MVMPtrHashEntry), size_to_move);
            }

            /* The same test and optimisation as in the "make room" loop - we're
             * about to insert something at the (current) max_probe_distance, so
             * signal to the next insertion that it needs to take action first.
             */
            if (probe_distance == MVM_HASH_MAX_PROBE_DISTANCE) {
                control->max_items = 0;
            }

            *metadata = probe_distance;
            struct MVMPtrHashEntry *entry = (struct MVMPtrHashEntry *) entry_raw;
            entry->key = NULL;
            return entry;
        }

        if (*metadata == probe_distance) {
            struct MVMPtrHashEntry *entry = (struct MVMPtrHashEntry *) entry_raw;
            if (entry->key == key) {
                return entry;
            }
        }
        ++probe_distance;
        ++metadata;
        entry_raw -= sizeof(struct MVMPtrHashEntry);
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < MVM_ptr_hash_metadata(control) + control->official_size + control->max_items);
        assert(metadata < MVM_ptr_hash_metadata(control) + control->official_size + 256);
    }
}

struct MVMPtrHashEntry *MVM_ptr_hash_lvalue_fetch(MVMThreadContext *tc,
                                                  MVMPtrHashTable *hashtable,
                                                  const void *key) {
    struct MVMPtrHashTableControl *control = hashtable->table;
    if (MVM_UNLIKELY(!control)) {
        control = MVM_calloc(1,sizeof(struct MVMPtrHashTableControl));
        hashtable->table = control;
        hash_initial_allocate(control);
    }
    else if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        /* We should avoid growing the hash if we don't need to.
         * It's expensive, and for hashes with iterators, growing the hash
         * invalidates iterators. Which is buggy behaviour if the fetch doesn't
         * need to create a key. */
        struct MVMPtrHashEntry *entry = MVM_ptr_hash_fetch(tc, hashtable, key);
        if (entry) {
            return entry;
        }

        MVMuint32 true_size =  hash_true_size(control);
        MVMuint8 *entry_raw_orig = MVM_ptr_hash_entries(control);
        MVMuint8 *metadata_orig = MVM_ptr_hash_metadata(control);

        hash_grow(control);

        MVMuint8 *entry_raw = entry_raw_orig;
        MVMuint8 *metadata = metadata_orig;
        MVMHashNumItems bucket = 0;
        while (bucket < true_size) {
            if (*metadata) {
                struct MVMPtrHashEntry *old_entry = (struct MVMPtrHashEntry *) entry_raw;
                struct MVMPtrHashEntry *new_entry =
                    hash_insert_internal(tc, control, old_entry->key);
                assert(new_entry->key == NULL);
                *new_entry = *old_entry;
            }
            ++bucket;
            ++metadata;
            entry_raw -= sizeof(struct MVMPtrHashEntry);
        }
        MVM_free(entry_raw_orig - sizeof(struct MVMPtrHashEntry) * (true_size - 1));
    }
    struct MVMPtrHashEntry *new_entry
        = hash_insert_internal(tc, control, key);
    if (!new_entry->key) {
        ++control->cur_items;
    }
    return new_entry;
}

/* UNCONDITIONALLY creates a new hash entry with the given key and value.
 * Doesn't check if the key already exists. Use with care.
 * (well that's the official line. As you can see, the XXX suggests we currently
 * don't exploit the documented freedom.) */
void MVM_ptr_hash_insert(MVMThreadContext *tc,
                         MVMPtrHashTable *hashtable,
                         const void *key,
                         uintptr_t value) {
    struct MVMPtrHashEntry *new_entry = MVM_ptr_hash_lvalue_fetch(tc, hashtable, key);
    if (new_entry->key) {
        if (value != new_entry->value) {
            MVMHashNumItems bucket = MVM_ptr_hash_code(key) >> hashtable->table->key_right_shift;
            /* definately XXX - what should we do here? */
            MVM_oops(tc, "insert conflict, %p is %u, %"PRIu64" != %"PRIu64,
                     key, bucket, (MVMuint64) value, (MVMuint64) new_entry->value);
        }
    } else {
        new_entry->key = key;
        new_entry->value = value;
    }
}

uintptr_t MVM_ptr_hash_fetch_and_delete(MVMThreadContext *tc,
                                        MVMPtrHashTable *hashtable,
                                        const void *key) {
    struct MVMPtrHashTableControl *control = hashtable->table;
    if (MVM_UNLIKELY(!control || !MVM_ptr_hash_entries(control))) {
        /* Should this be an oops? */
        return 0;
    }
    unsigned int probe_distance = 1;
    MVMHashNumItems bucket = MVM_ptr_hash_code(key) >> control->key_right_shift;
    MVMuint8 *entry_raw = MVM_ptr_hash_entries(control) - bucket * sizeof(struct MVMPtrHashEntry);
    uint8_t *metadata = MVM_ptr_hash_metadata(control) + bucket;
    while (1) {
        if (*metadata == probe_distance) {
            struct MVMPtrHashEntry *entry = (struct MVMPtrHashEntry *) entry_raw;
            if (entry->key == key) {
                /* Target acquired. */
                uintptr_t retval = entry->value;

                uint8_t *metadata_target = metadata;
                /* Look at the next slot */
                uint8_t old_probe_distance = metadata_target[1];
                while (old_probe_distance > 1) {
                    /* OK, we can move this one. */
                    *metadata_target = old_probe_distance - 1;
                    /* Try the next one, etc */
                    ++metadata_target;
                    old_probe_distance = metadata_target[1];
                }
                /* metadata_target now points to the metadata for the last thing
                   we did move. (possibly still our target). */

                uint32_t entries_to_move = metadata_target - metadata;
                if (entries_to_move) {
                    size_t size_to_move = sizeof(struct MVMPtrHashEntry) * entries_to_move;
                    /* When we had entries *ascending* in memory, this was
                     * memmove(entry_raw, entry_raw + sizeof(struct MVMPtrHashEntry),
                     *         sizeof(struct MVMPtrHashEntry) * entries_to_move);
                     * because we point to the *start* of the block of memory we
                     * want to move, and we want to move the block one "entry"
                     * backwards.
                     * `entry_raw` is still a pointer to the entry that we need
                     * to ovewrite, but now we need to move everything *before*
                     * it upwards to close the gap. */
                    memmove(entry_raw - size_to_move + sizeof(struct MVMPtrHashEntry),
                            entry_raw - size_to_move,
                            size_to_move);
                }
                /* and this slot is now emtpy. */
                *metadata_target = 0;
                --control->cur_items;

                /* Job's a good 'un. */
                return retval;
            }
        }
        /* There's a sentinel at the end. This will terminate: */
        if (*metadata < probe_distance) {
            /* So, if we hit 0, the bucket is empty. "Not found".
               If we hit something with a lower probe distance then...
               consider what would have happened had this key been inserted into
               the hash table - it would have stolen this slot, and the key we
               find here now would have been displaced futher on. Hence, the key
               we seek can't be in the hash table. */
            /* Strange. Not in the hash. Should this be an oops? */
            return 0;
        }
        ++probe_distance;
        ++metadata;
        entry_raw -= sizeof(struct MVMPtrHashEntry);
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < MVM_ptr_hash_metadata(control) + control->official_size + control->max_items);
        assert(metadata < MVM_ptr_hash_metadata(control) + control->official_size + 256);
    }
}
