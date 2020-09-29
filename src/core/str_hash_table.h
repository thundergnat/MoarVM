/* A Better Hash.

A C implementation of https://github.com/martinus/robin-hood-hashing
by Martin Ankerl <http://martin.ankerl.com>

Better than what we had. Not better than his. His is hard to beat.

His design is for a Robin Hood hash (ie open addressing, Robin Hood probing)
with:

* a contiguous block of memory
* hash into 2**n slots
* instead of wrapping round from the end to the start of the array when
  probing, *actually* allocate some extra slots at the end, sufficient to cover
  the maximum permitted probe length
* store metadata for free/used (with the offset from the ideal slot) in a byte
  array immediately after the data slots
* store the offset in the top n bits of the byte, use the lower 8-n bits
  (possibly 0) to store (more) bits of the key's hash in the rest.
  (where n might be 0 - n is updated dynamically whenever a probe would overflow
   the currently permitted maximum)
  (so m bits of the hash are used to pick the ideal slot, and a different n are
   in the metadata, meaning that misses can be rejected more often)
* sentinel byte at the end of the metadata to cause the iterator to efficiently
  terminate
* setting max_items to 0 to force a resize before even trying another allocation
* when inserting and stealing a slot, move the next items up in bulk
  (ie don't implement it as "swap the new element with the evicted element and
  carry on inserting - the rest of the elements are already in a valid probe
  order, so just update *all* their metadata bytes, and then memmove them)

it's incredibly flexible (up to, automatically choosing whether to allocate
the value object inline in the hash, or indrected via a pointer), but
implemented as a C++ template.

Whereas we need something in C. Only for small structures, so they can always
go inline. And it turns out, our keys are always pointers, and easily "hashed"
(either because they are, because they point to something that caches its
hash value, or because we fake it and explicitly store the hash value.)

Not all the optimisations described above are in place yet. Starting with
"minimum viable product", with a design that should support adding them.

*/

/* As to hash randomisation.
 *
 * The key thing about "Denial of Service via Algorithmic Complexity Attacks"
 * https://www.usenix.org/conference/12th-usenix-security-symposium/denial-service-algorithmic-complexity-attacks
 *
 * is that the attacker wins if she is able to cause the same effects as a brute
 * force attack but for much less work. So "all" we have to do is to ensure that
 * it takes as much effort to figure out how to bypass our mitigations as it
 * does just to brute force attack us. ie - deny the *shortcut*.
 *
 * The weakness of "classic" hash tables prior to this paper is that the
 * function that maps keys to buckets is constant, so the attacker can
 * pre-compute keys that will induce linear behaviour.
 *
 * The "classic" fix to that was to introduce some sort of randomisation of the
 * hash mapping (often described as a "seed", but probably better described as a
 * "salt"), so that pre-computing keys no longer works.
 *
 * The problem with implementations of this (including ours) is that for
 * efficiency reasons implementations like to have the hash function be the same
 * for all hash tables (within a process). Implying that there is one process
 * global salt.
 * In turn, this meant that it became possible for a more determined attacker
 * to remotely probe and determine the salt through information leakage -
 * typically by the iteration order of hash tables, and this being exposed.
 * In particular, bucket chains and bucket splitting meant that one bit of the
 * hash value would determine how keys on a single chain were split, and hence
 * which order they would appear in when a hash doubled in size.
 *
 * So in turn, the fix for *that* was often to obfuscate the iteration order.
 * Usually on a per-hash table basis.
 *
 * Previously we did this by generating a pseudo random number for each hash,
 * and using it to perturb the order of bucket chains. As explained:
 *
 * Get a pseudo-random bucket. This works because XORing a random x bit integer
 * with 0..(2**x)-1 will give you 0..(2**x)-1 in a pseudo random order (not *really*
 * random but random enough for our purposes. Example with 0..(2**3)-1 and the rand int is 3
 * 0 ^ 3 = 3; 1 ^ 3 = 2;
 * 2 ^ 3 = 1; 3 ^ 3 = 0;
 * 4 ^ 3 = 7; 5 ^ 3 = 6;
 * 6 ^ 3 = 5; 7 ^ 3 = 4;
 *
 *
 * I realise that can actually take a different approach. Instead of perturbing
 * the bucket order on *iteration*, we can perturb it on insertion. This ends up
 * with the same end result - effectively we store the buckets in a shuffled
 * order and iterate them linearly, instead of storing the buckets in a linear
 * order and iterating them shuffled.
 * We then also pick a new salt for each size doubling, meaning that *all* bits
 * of the hash value contribute to the order change on size doubling (which can
 * be observed by an attacker), not just one.
 *
 * This should be just as secure, but faster. Now prove me wrong with a working
 * attack, and preferably suggest a better solution. :-)
 * (As you can see from "in turn", people were wrong before.)
 */

/* Conceptually, the design is this
 *
 *  Control            Entries
 *  structure
 *
 * +-------------+    +----------------+----------------+----------------+----
 * |             |    | probe distance | probe distance | probe distance |
 * | entries     | -> | key            | key            | key            | ...
 * | other stuff |    | value          | value          | value          |
 * +-------------+    +----------------+----------------+----------------+----
 *
 * but as probe distance is one byte, and key is a pointer, this would waste a
 * lot of memory due to alignment padding, so the design actually ends up as
 * this, with probe distance stored separately a byte array, `metadata`.
 *
 * Also, "conceptually"...
 * We are using open addressing. What this means is that, whilst each hash entry
 * has an "ideal" location in the array, if that location is already in use, we
 * put the entry "nearby". With "Robin Hood" hashing, "nearby" is a location
 * "soon" after (with logic for re-ordering entries), but the upshot of this is
 * that the "actual" position is "ideal" + "probe distance", and that value has
 * to be wrapped (modulo the array size) to find the actual bucket location.
 *
 * So modulo approach means that a hash of size 8 looks like this:
 *
 * +-------------+    +---+---+---+---+---+---+---+---+
 * | metadata    | -> | a | b | c | d | e | f | g | h |  probe distances
 * |             |    +---+---+---+---+---+---+---+---+
 * |             |
 * |             |    +---+---+---+---+---+---+---+---+
 * | entries     | -> | A | B | C | D | E | F | G | H |  key, value
 * | other stuff |    +---+---+---+---+---+---+---+---+
 * +-------------+
 *
 * whereas what we actually do is "unwrap" the modulo, and allocate the worst
 * case extra array slots at the end (longest possible probe distance, starting
 * at the last "official" entry). So for an array of size 8, load factor of
 * 0.75 the longest probe distance is 5 (when all 6 entries would ideally be in
 * the last bucket), so what we actually have is this
 *
 * +----------+    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 * | metadata | -> | 1 | a | b | c | d | e | f | g | h | i | j | k | l | m | 1 |
 * |          |    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 * | (other)  |
 * |          |        +---+---+---+---+---+---+---+---+---+---+---+---+---+
 * | entries  | ->     | A | B | C | D | E | F | G | H | I | J | K | L | M |
 * +----------+        +---+---+---+---+---+---+---+---+---+---+---+---+---+
 *
 *                     <-- official bucket positions --><--   overflow   -->
 *
 * We include sentinel values at each end of the metadata to make iteration
 * easier.
 *
 * Finally, to reduce allocations keep things in the same CPU cache lines, what
 * we allocate in memory actually looks like this:
 *
 * ---+---+---+---+---+---+---+---+---------+---+---+---+---+---+---+---+---+---
 * ...| G | F | E | D | C | B | A | control | 1 | a | b | c | d | e | f | g |...
 * ---+---+---+---+---+---+---+---+---------+---+---+---+---+---+---+---+---+---
 *                                ^
 *                                |
 *                              +---+
 * the public MVMStrHashTable   |   |
 *                              +---+
 *
 * is just a pointer to the dynamically allocated structure.
 *
 * This layout means that a hash clone is
 * 1) malloc
 * 2) memcpy
 * 3) fix up the GC invariants
 */

struct MVMStrHashTableControl {
    MVMuint64 salt;
#if HASH_DEBUG_ITER
    MVMuint64 ht_id;
    MVMuint32 serial;
    MVMuint32 last_delete_at;
#endif
    MVMHashNumItems cur_items;
    MVMHashNumItems max_items; /* hit this and we grow */
    MVMHashNumItems official_size;
    MVMuint8 key_right_shift;
    MVMuint8 entry_size;
    MVMuint8 probe_overflow_size;
};

struct MVMStrHashTable {
    struct MVMStrHashTableControl *table;
};

struct MVMStrHashHandle {
    MVMString *key;
};

typedef struct {
    MVMuint32 pos;
#if HASH_DEBUG_ITER
    MVMuint32 serial;
    MVMuint64 owner;
#endif
}  MVMStrHashIterator;

MVMuint32 MVM_round_up_log_base2(MVMuint32 v);

#if HASH_DEBUG_ITER
MVM_STATIC_INLINE int MVM_str_hash_iterator_target_deleted(MVMThreadContext *tc,
                                                           MVMStrHashTable *hashtable,
                                                           MVMStrHashIterator iterator) {
    /* Returns true if the hash entry that the iterator points to has been
     * deleted (and this is the only action on the hash since the iterator was
     * created) */
    struct MVMStrHashTableControl *control = hashtable->table;
    return control && iterator.serial == control->serial - 1 &&
        iterator.pos == control->last_delete_at;
}
#endif

/* So why is this here, instead of _funcs?
 * Because it is needed in MVM_iter_istrue_hash, which is inline in MVMIter.h
 * So this definition has to be before that definition.
 * In turn, various other inline functions in the reprs are used in
 * str_hash_table_funcs.h, so those declarations have to be seen already, and
 * as the reprs headers are included as one block, *most* of the MVMStrHashTable
 * functions need to be later. */

MVM_STATIC_INLINE int MVM_str_hash_at_end(MVMThreadContext *tc,
                                           MVMStrHashTable *hashtable,
                                           MVMStrHashIterator iterator) {
#if HASH_DEBUG_ITER
    struct MVMStrHashTableControl *control = hashtable->table;
    MVMuint64 ht_id = control ? control->ht_id : 0;
    if (iterator.owner != ht_id) {
        MVM_oops(tc, "MVM_str_hash_at_end called with an iterator from a different hash table: %016" PRIx64 " != %016" PRIx64,
                 iterator.owner, ht_id);
    }
    MVMuint32 serial = control ? control->serial : 0;
    if (iterator.serial != serial
        || MVM_str_hash_iterator_target_deleted(tc, hashtable, iterator)) {
        MVM_oops(tc, "MVM_str_hash_at_end called with an iterator with the wrong serial number: %u != %u",
                 iterator.serial, serial);
    }
#endif
    return iterator.pos == 0;
}
