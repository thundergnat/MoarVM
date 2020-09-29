#include "moar.h"

#define UNI_LOAD_FACTOR 0.75
#define UNI_MIN_SIZE_BASE_2 3

MVM_STATIC_INLINE MVMuint32 hash_true_size(const struct MVMUniHashTableControl *control) {
    return control->official_size + control->probe_overflow_size;
}

MVM_STATIC_INLINE void hash_demolish_internal(MVMThreadContext *tc,
                                              struct MVMUniHashTableControl *control) {
    size_t actual_items = hash_true_size(control);
    size_t entries_size = sizeof(struct MVMUniHashEntry) * actual_items;
    char *start = (char *)control - entries_size;
    MVM_free(start);
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_uni_hash_demolish(MVMThreadContext *tc, MVMUniHashTable *hashtable) {
    struct MVMUniHashTableControl *control = hashtable->table;
    if (!control)
        return;
    hash_demolish_internal(tc, control);
    hashtable->table = NULL;
}
/* and then free memory if you allocated it */


MVM_STATIC_INLINE struct MVMUniHashTableControl *hash_allocate_common(MVMThreadContext *tc,
                                                                      MVMuint8 key_right_shift,
                                                                      MVMuint32 official_size) {
    MVMuint32 max_items = official_size * UNI_LOAD_FACTOR;
    MVMuint32 overflow_size = max_items - 1;
    /* -1 because...
     * probe distance of 1 is the correct bucket.
     * hence for a value whose ideal slot is the last bucket, it's *in* the
     * official allocation.
     * probe distance of 2 is the first extra bucket beyond the official
     * allocation
     * probe distance of 255 is the 254th beyond the official allocation.
     */
    MVMuint8 probe_overflow_size;
    if (MVM_HASH_MAX_PROBE_DISTANCE < overflow_size) {
        probe_overflow_size = MVM_HASH_MAX_PROBE_DISTANCE - 1;
    } else {
        probe_overflow_size = overflow_size;
    }
    size_t actual_items = official_size + probe_overflow_size;
    size_t entries_size = sizeof(struct MVMUniHashEntry) * actual_items;
    size_t metadata_size = 1 + actual_items + 1;
    size_t total_size
        = entries_size + sizeof (struct MVMUniHashTableControl) + metadata_size;

    struct MVMUniHashTableControl *control =
        (struct MVMUniHashTableControl *) ((char *)MVM_malloc(total_size) + entries_size);

    control->official_size = official_size;
    control->max_items = max_items;
    control->probe_overflow_size = probe_overflow_size;
    control->key_right_shift = key_right_shift;

    MVMuint8 *metadata = (MVMuint8 *)(control + 1);
    memset(metadata, 0, metadata_size);

    /* A sentinel. This marks an occupied slot, at its ideal position. */
    metadata[actual_items + 1] = 1;
    /* A sentinel at the other end. Again, occupited, ideal position. */
    metadata[0] = 1;

    return control;
}

void MVM_uni_hash_build(MVMThreadContext *tc,
                        MVMUniHashTable *hashtable,
                        MVMuint32 entries) {
    MVMuint32 initial_size_base2;
    if (!entries) {
        initial_size_base2 = UNI_MIN_SIZE_BASE_2;
    } else {
        /* Minimum size we need to allocate, given the load factor. */
        MVMuint32 min_needed = entries * (1.0 / UNI_LOAD_FACTOR);
        initial_size_base2 = MVM_round_up_log_base2(min_needed);
        if (initial_size_base2 < UNI_MIN_SIZE_BASE_2) {
            /* "Too small" - use our original defaults. */
            initial_size_base2 = UNI_MIN_SIZE_BASE_2;
        }
    }

    struct MVMUniHashTableControl *control
        = hash_allocate_common(tc,
                               (8 * sizeof(MVMuint32) - initial_size_base2),
                               1 << initial_size_base2);
    control->cur_items = 0;
    hashtable->table = control;
}

static MVMuint64 uni_hash_fsck_internal(struct MVMUniHashTableControl *control, MVMuint32 mode);

MVM_STATIC_INLINE struct MVMUniHashEntry *hash_insert_internal(MVMThreadContext *tc,
                                                               struct MVMUniHashTableControl *control,
                                                               const char *key,
                                                               MVMuint32 hash_val) {
    if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        uni_hash_fsck_internal(control, 5);
        MVM_oops(tc, "oops, attempt to recursively call grow when adding %s",
                 key);
    }

    unsigned int probe_distance = 1;
    MVMHashNumItems bucket = hash_val >> control->key_right_shift;
    MVMuint8 *entry_raw = MVM_uni_hash_entries(control) - bucket * sizeof(struct MVMUniHashEntry);
    MVMuint8 *metadata = MVM_uni_hash_metadata(control) + bucket;
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
                size_t size_to_move = sizeof(struct MVMUniHashEntry) * entries_to_move;
                /* When we had entries *ascending* this was
                 * memmove(entry_raw + sizeof(struct MVMUniHashEntry), entry_raw,
                 *         sizeof(struct MVMUniHashEntry) * entries_to_move);
                 * because we point to the *start* of the block of memory we
                 * want to move, and we want to move it one "entry" forwards.
                 * `entry_raw` is still a pointer to where we want to make free
                 * space, but what want to do now is move everything at it and
                 * *before* it downwards. */
                MVMuint8 *dest = entry_raw - size_to_move;
                memmove(dest, dest + sizeof(struct MVMUniHashEntry), size_to_move);
            }

            /* The same test and optimisation as in the "make room" loop - we're
             * about to insert something at the (current) max_probe_distance, so
             * signal to the next insertion that it needs to take action first.
             */
            if (probe_distance == MVM_HASH_MAX_PROBE_DISTANCE) {
                control->max_items = 0;
            }

            *metadata = probe_distance;
            struct MVMUniHashEntry *entry = (struct MVMUniHashEntry *) entry_raw;
            entry->key = NULL;
            return entry;
        }

        if (*metadata == probe_distance) {
            struct MVMUniHashEntry *entry = (struct MVMUniHashEntry *) entry_raw;
            if (entry->hash_val == hash_val && 0 == strcmp(entry->key, key)) {
                return entry;
            }
        }
        ++probe_distance;
        ++metadata;
        entry_raw -= sizeof(struct MVMUniHashEntry);
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < MVM_uni_hash_metadata(control) + control->official_size + control->max_items);
        assert(metadata < MVM_uni_hash_metadata(control) + control->official_size + 256);
    }
}

/* I think that we are going to expose this soon. */
MVM_STATIC_INLINE void *MVM_uni_hash_lvalue_fetch(MVMThreadContext *tc,
                                                  MVMUniHashTable *hashtable,
                                                  const char *key) {
    struct MVMUniHashTableControl *control = hashtable->table;
    if (!control) {
        MVM_uni_hash_build(tc, hashtable, 0);
        control = hashtable->table;
    }
    else if (MVM_UNLIKELY(control->cur_items >= control->max_items)) {
        /* We should avoid growing the hash if we don't need to.
         * It's expensive, and for hashes with iterators, growing the hash
         * invalidates iterators. Which is buggy behaviour if the fetch doesn't
         * need to create a key. */
        struct MVMUniHashEntry *entry = MVM_uni_hash_fetch(tc, hashtable, key);
        if (entry) {
            return entry;
        }

        MVMuint32 true_size =  hash_true_size(control);
        MVMuint8 *entry_raw_orig = MVM_uni_hash_entries(control);
        MVMuint8 *metadata_orig = MVM_uni_hash_metadata(control);

        struct MVMUniHashTableControl *control_orig = control;

        control = hash_allocate_common(tc,
                                       control_orig->key_right_shift - 1,
                                       control_orig->official_size * 2);

        control->cur_items = control_orig->cur_items;
        hashtable->table = control;

        MVMuint8 *entry_raw = entry_raw_orig;
        MVMuint8 *metadata = metadata_orig;
        MVMHashNumItems bucket = 0;
        while (bucket < true_size) {
            if (*metadata) {
                struct MVMUniHashEntry *old_entry = (struct MVMUniHashEntry *) entry_raw;
                struct MVMUniHashEntry *new_entry =
                    hash_insert_internal(tc, control, old_entry->key, old_entry->hash_val);
                assert(new_entry->key == NULL);
                *new_entry = *old_entry;
            }
            ++bucket;
            ++metadata;
            entry_raw -= sizeof(struct MVMUniHashEntry);
        }
        hash_demolish_internal(tc, control_orig);
    }
    MVMuint32 hash_val = MVM_uni_hash_code(key, strlen(key));
    struct MVMUniHashEntry *new_entry
        = hash_insert_internal(tc, control, key, hash_val);
    if (!new_entry->key) {
        new_entry->hash_val = hash_val;
        ++control->cur_items;
    }
    return new_entry;
}

/* UNCONDITIONALLY creates a new hash entry with the given key and value.
 * Doesn't check if the key already exists. Use with care.
 * (well that's the official line. As you can see, the XXX suggests we currently
 * don't exploit the documented freedom.) */
void MVM_uni_hash_insert(MVMThreadContext *tc,
                         MVMUniHashTable *hashtable,
                         const char *key,
                         MVMint32 value) {
    struct MVMUniHashEntry *new_entry = MVM_uni_hash_lvalue_fetch(tc, hashtable, key);
    if (new_entry->key) {
        if (value != new_entry->value) {
            MVMuint32 hash_val = MVM_uni_hash_code(key, strlen(key));
            /* definately XXX - what should we do here? */
            MVM_oops(tc, "insert conflict, %s is %u, %i != %i", key, hash_val, value, new_entry->value);
        }
    } else {
        new_entry->key = key;
        new_entry->value = value;
    }
}

/* This is not part of the public API, and subject to change at any point.
   (possibly in ways that are actually incompatible but won't generate compiler
   warnings.) */
MVMuint64 MVM_uni_hash_fsck(MVMUniHashTable *hashtable, MVMuint32 mode) {
    return uni_hash_fsck_internal(hashtable->table, mode);
}

static MVMuint64 uni_hash_fsck_internal(struct MVMUniHashTableControl *control, MVMuint32 mode) {
    const char *prefix_hashes = mode & 1 ? "# " : "";
    MVMuint32 display = (mode >> 1) & 3;
    MVMuint64 errors = 0;
    MVMuint64 seen = 0;

    if (control == NULL) {
        return 0;
    }

    MVMuint32 true_size = hash_true_size(control);
    MVMuint8 *entry_raw = MVM_uni_hash_entries(control);
    MVMuint8 *metadata = MVM_uni_hash_metadata(control);
    MVMuint32 bucket = 0;
    MVMint64 prev_offset = 0;
    while (bucket < true_size) {
        if (!*metadata) {
            /* empty slot. */
            prev_offset = 0;
            if (display == 2) {
                fprintf(stderr, "%s%3X\n", prefix_hashes, bucket);
            }
        } else {
            ++seen;

            struct MVMUniHashEntry *entry = (struct MVMUniHashEntry *) entry_raw;
            MVMuint32 ideal_bucket = entry->hash_val >> control->key_right_shift;
            MVMint64 offset = 1 + bucket - ideal_bucket;
            int wrong_bucket = offset != *metadata;
            int wrong_order = offset < 1 || offset > prev_offset + 1;

            if (display == 2 || wrong_bucket || wrong_order) {
                fprintf(stderr, "%s%3X%c%3"PRIx64"%c%08X %s\n", prefix_hashes,
                        bucket, wrong_bucket ? '!' : ' ', offset,
                        wrong_order ? '!' : ' ', entry->hash_val, entry->key);
                errors += wrong_bucket + wrong_order;
            }
            prev_offset = offset;
        }
        ++bucket;
        ++metadata;
        entry_raw -= sizeof(struct MVMUniHashEntry);
    }
    if (*metadata != 1) {
        ++errors;
        if (display) {
            fprintf(stderr, "%s    %02x!\n", prefix_hashes, *metadata);
        }
    }
    if (seen != control->cur_items) {
        ++errors;
        if (display) {
            fprintf(stderr, "%s %"PRIx64"u != %"PRIx32"u \n",
                    prefix_hashes, seen, control->cur_items);
        }
    }

    return errors;
}
