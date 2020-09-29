/* These are private. We need them out here for the inline functions. Use those.
 */
MVM_STATIC_INLINE MVMuint32 MVM_index_hash_kompromat(const struct MVMIndexHashTableControl *control) {
    return control->official_size + control->probe_overflow_size;
}
MVM_STATIC_INLINE MVMuint8 *MVM_index_hash_metadata(const struct MVMIndexHashTableControl *control) {
    return (MVMuint8 *) control + sizeof(struct MVMIndexHashTableControl);
}
MVM_STATIC_INLINE MVMuint8 *MVM_index_hash_entries(const struct MVMIndexHashTableControl *control) {
    return (MVMuint8 *) control - sizeof(struct MVMIndexHashEntry);
}

/* Frees the entire contents of the hash, leaving you just the hashtable itself,
   which you allocated (heap, stack, inside another struct, wherever) */
void MVM_index_hash_demolish(MVMThreadContext *tc, MVMIndexHashTable *hashtable);
/* and then free memory if you allocated it */

/* Call this before you use the hashtable, to initialise it.
 * Doesn't allocate memory for the hashtable struct itself - you can embed the
 * struct within a larger struct if you wish.
 */
void MVM_index_hash_build(MVMThreadContext *tc,
                          MVMIndexHashTable *hashtable,
                          MVMuint32 entries);

/* This code assumes that the destination hash is uninitialised - ie not even
 * MVM_index_hash_build has been called upon it. */
MVM_STATIC_INLINE void MVM_index_hash_shallow_copy(MVMThreadContext *tc,
                                                   MVMIndexHashTable *source,
                                                   MVMIndexHashTable *dest) {
    const struct MVMIndexHashTableControl *control = source->table;
    if (!control)
        return;
    size_t actual_items = MVM_index_hash_kompromat(control);
    size_t entries_size = sizeof(struct MVMIndexHashEntry) * actual_items;
    size_t metadata_size = actual_items + 1;
    const char *start = (const char *)control - entries_size;
    size_t total_size
        = entries_size + sizeof (struct MVMIndexHashTableControl) + metadata_size;
    char *target = MVM_malloc(total_size);
    memcpy(target, start, total_size);
    dest->table = (struct MVMIndexHashTableControl *)(target + entries_size);
}

/* UNCONDITIONALLY creates a new hash entry with the given key and value.
 * Doesn't check if the key already exists. Use with care. */
void MVM_index_hash_insert_nocheck(MVMThreadContext *tc,
                                   MVMIndexHashTable *hashtable,
                                   MVMString **list,
                                   MVMuint32 idx);

MVM_STATIC_INLINE MVMuint32 MVM_index_hash_fetch_nocheck(MVMThreadContext *tc,
                                                         MVMIndexHashTable *hashtable,
                                                         MVMString **list,
                                                         MVMString *want) {
    struct MVMIndexHashTableControl *control = hashtable->table;
    if (MVM_UNLIKELY(!control || !MVM_index_hash_entries(control))) {
        return MVM_INDEX_HASH_NOT_FOUND;
    }
    unsigned int probe_distance = 1;
    MVMuint64 hash_val = MVM_string_hash_code(tc, want);
    MVMHashNumItems bucket = hash_val >> control->key_right_shift;
    MVMuint8 *entry_raw = MVM_index_hash_entries(control) - bucket * sizeof(struct MVMIndexHashEntry);
    MVMuint8 *metadata = MVM_index_hash_metadata(control) + bucket;
    while (1) {
        if (*metadata == probe_distance) {
            struct MVMIndexHashEntry *entry = (struct MVMIndexHashEntry *) entry_raw;
            MVMString *key = list[entry->index];
            if (key == want
                || (MVM_string_graphs_nocheck(tc, want) == MVM_string_graphs_nocheck(tc, key)
                    && MVM_string_substrings_equal_nocheck(tc, want, 0,
                                                           MVM_string_graphs_nocheck(tc, want),
                                                           key, 0))) {
                return entry->index;
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
            return MVM_INDEX_HASH_NOT_FOUND;
        }
        ++probe_distance;
        ++metadata;
        entry_raw -= sizeof(struct MVMIndexHashEntry);
        assert(probe_distance <= MVM_HASH_MAX_PROBE_DISTANCE);
        assert(metadata < MVM_index_hash_metadata(control) + control->official_size + control->max_items);
        assert(metadata < MVM_index_hash_metadata(control) + control->official_size + 256);
    }
}

MVM_STATIC_INLINE MVMuint32 MVM_index_hash_fetch(MVMThreadContext *tc,
                                                 MVMIndexHashTable *hashtable,
                                                 MVMString **list,
                                                 MVMString *want) {
    if (!MVM_str_hash_key_is_valid(tc, want)) {
        MVM_str_hash_key_throw_invalid(tc, want);
    }
    return MVM_index_hash_fetch_nocheck(tc, hashtable, list, want);
}

MVM_STATIC_INLINE int MVM_index_hash_built(MVMThreadContext *tc,
                                           MVMIndexHashTable *hashtable) {
    return !!hashtable->table;
}
