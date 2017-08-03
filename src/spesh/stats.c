#include "moar.h"

/* We associate recoded type tuples in callees with their caller's callsites.
 * This is kept as a flat view, and then folded in when the caller's sim
 * frame (see next) is popped. */
typedef struct SimCallType {
    MVMuint32 bytecode_offset;
    MVMCallsite *cs;
    MVMSpeshStatsType *arg_types;
} SimCallType;

/* Logs are linear recordings marked with frame correlation IDs. We need to
 * simulate the call stack as part of the analysis. This models a frame on
 * the call stack and the stack respectively. */
typedef struct SimStackFrame {
    /* The static frame. */
    MVMStaticFrame *sf;

    /* Spesh stats for the stack frame. */
    MVMSpeshStats *ss;

    /* Correlation ID. */
    MVMuint32 cid;

    /* Callsite stats index (not pointer in case of realloc). */
    MVMuint32 callsite_idx;

    /* Argument types logged. Sized by number of callsite flags. */
    MVMSpeshStatsType *arg_types;

    /* Spesh log entries for types and values, for later processing. */
    MVMSpeshLogEntry **offset_logs;
    MVMuint32 offset_logs_used;
    MVMuint32 offset_logs_limit;

    /* Type tuples observed at a given callsite offset, for later
     * processing. */
    SimCallType *call_type_info;
    MVMuint32 call_type_info_used;
    MVMuint32 call_type_info_limit;

    /* Number of types we crossed an OSR point. */
    MVMuint32 osr_hits;

    /* The last bytecode offset and code object seen in an invoke recording;
     * used for producing callsite type stats based on callee type tuples. */
    MVMuint32 last_invoke_offset;
    MVMObject *last_invoke_code;
} SimStackFrame;
typedef struct SimStack {
    /* Array of frames. */
    SimStackFrame *frames;

    /* Current frame index and allocated space. */
    MVMuint32 used;
    MVMuint32 limit;

    /* Current stack depth. */
    MVMuint32 depth;
} SimStack;

/* Gets the statistics for a static frame, creating them if needed. */
MVMSpeshStats * stats_for(MVMThreadContext *tc, MVMStaticFrame *sf) {
    MVMStaticFrameSpesh *spesh = sf->body.spesh;
    if (!spesh->body.spesh_stats)
        spesh->body.spesh_stats = MVM_calloc(1, sizeof(MVMSpeshStats));
    return spesh->body.spesh_stats;
}

/* Gets the stats by callsite, adding it if it's missing. */
MVMuint32 by_callsite_idx(MVMThreadContext *tc, MVMSpeshStats *ss, MVMCallsite *cs) {
    /* See if we already have it. */
    MVMuint32 found;
    MVMuint32 n = ss->num_by_callsite;
    for (found = 0; found < n; found++)
        if (ss->by_callsite[found].cs == cs)
            return found;

    /* If not, we need a new record. */
    found = ss->num_by_callsite;
    ss->num_by_callsite++;
    ss->by_callsite = MVM_realloc(ss->by_callsite,
        ss->num_by_callsite * sizeof(MVMSpeshStatsByCallsite));
    memset(&(ss->by_callsite[found]), 0, sizeof(MVMSpeshStatsByCallsite));
    ss->by_callsite[found].cs = cs;
    return found;
}

/* Checks if a type tuple is incomplete (no types logged for some passed
 * objects, or no decont type logged for a container type). */
MVMint32 incomplete_type_tuple(MVMThreadContext *tc, MVMCallsite *cs,
                               MVMSpeshStatsType *arg_types) {
    MVMuint32 i;
    for (i = 0; i < cs->flag_count; i++) {
        if (cs->arg_flags[i] & MVM_CALLSITE_ARG_OBJ) {
            MVMObject *type = arg_types[i].type;
            if (!type)
                return 1;
            if (arg_types[i].type_concrete && type->st->container_spec)
                if (!arg_types[i].decont_type)
                    return 1;
        }
    }
    return 0;
}

/* Returns true if the callsite has no object arguments, false otherwise. */
MVMint32 cs_without_object_args(MVMThreadContext *tc, MVMCallsite *cs) {
    MVMuint32 i;
    for (i = 0; i < cs->flag_count; i++)
        if (cs->arg_flags[i] & MVM_CALLSITE_ARG_OBJ)
            return 0;
    return 1;
}

/* Gets the stats by type, adding it if it's missing. Frees arg_types. */
MVMSpeshStatsByType * by_type(MVMThreadContext *tc, MVMSpeshStats *ss, MVMuint32 callsite_idx,
                              MVMSpeshStatsType *arg_types) {
    /* Resolve type by callsite level info. If this is the no-callsite
     * specialization or this callsite has no object arguments, there is
     * nothing further to do. */
    MVMSpeshStatsByCallsite *css = &(ss->by_callsite[callsite_idx]);
    MVMCallsite *cs = css->cs;
    if (!cs || cs_without_object_args(tc, cs)) {
        MVM_free(arg_types);
        return NULL;
    }
    else if (incomplete_type_tuple(tc, cs, arg_types)) {
        /* Type tuple is incomplete, maybe because the log buffer ended prior
         * to having all the type information. Discard. */
        MVM_free(arg_types);
        return NULL;
    }
    else {
        /* See if we already have it. */
        size_t args_length = cs->flag_count * sizeof(MVMSpeshStatsType);
        MVMuint32 found;
        MVMuint32 n = css->num_by_type;
        for (found = 0; found < n; found++) {
            if (memcmp(css->by_type[found].arg_types, arg_types, args_length) == 0) {
                MVM_free(arg_types);
                return &(css->by_type[found]);
            }
        }

        /* If not, we need a new record. */
        found = css->num_by_type;
        css->num_by_type++;
        css->by_type = MVM_realloc(css->by_type,
            css->num_by_type * sizeof(MVMSpeshStatsByType));
        memset(&(css->by_type[found]), 0, sizeof(MVMSpeshStatsByType));
        css->by_type[found].arg_types = arg_types;
        return &(css->by_type[found]);
    }
}

/* Get the stats by offset entry, adding it if it's missing. */
MVMSpeshStatsByOffset * by_offset(MVMThreadContext *tc, MVMSpeshStatsByType *tss,
                                  MVMuint32 bytecode_offset) {
    /* See if we already have it. */
    MVMuint32 found;
    MVMuint32 n = tss->num_by_offset;
    for (found = 0; found < n; found++)
        if (tss->by_offset[found].bytecode_offset == bytecode_offset)
            return &(tss->by_offset[found]);

    /* If not, we need a new record. */
    found = tss->num_by_offset;
    tss->num_by_offset++;
    tss->by_offset = MVM_realloc(tss->by_offset,
        tss->num_by_offset * sizeof(MVMSpeshStatsByOffset));
    memset(&(tss->by_offset[found]), 0, sizeof(MVMSpeshStatsByOffset));
    tss->by_offset[found].bytecode_offset = bytecode_offset;
    return &(tss->by_offset[found]);
}

/* Adds/increments the count of a certain type seen at the given offset. */
void add_type_at_offset(MVMThreadContext *tc, MVMSpeshStatsByOffset *oss,
                        MVMStaticFrame *sf, MVMObject *type, MVMuint8 concrete) {
    /* If we have it already, increment the count. */
    MVMuint32 found;
    MVMuint32 n = oss->num_types;
    for (found = 0; found < n; found++) {
        if (oss->types[found].type == type && oss->types[found].type_concrete == concrete) {
            oss->types[found].count++;
            return;
        }
    }

    /* Otherwise, add it to the list. */
    found = oss->num_types;
    oss->num_types++;
    oss->types = MVM_realloc(oss->types, oss->num_types * sizeof(MVMSpeshStatsTypeCount));
    MVM_ASSIGN_REF(tc, &(sf->body.spesh->common.header), oss->types[found].type, type);
    oss->types[found].type_concrete = concrete;
    oss->types[found].count = 1;
}

/* Adds/increments the count of a certain value seen at the given offset. */
void add_value_at_offset(MVMThreadContext *tc, MVMSpeshStatsByOffset *oss,
                         MVMStaticFrame *sf, MVMObject *value) {
    /* If we have it already, increment the count. */
    MVMuint32 found;
    MVMuint32 n = oss->num_values;
    for (found = 0; found < n; found++) {
        if (oss->values[found].value == value) {
            oss->values[found].count++;
            return;
        }
    }

    /* Otherwise, add it to the list. */
    found = oss->num_values;
    oss->num_values++;
    oss->values = MVM_realloc(oss->values, oss->num_values * sizeof(MVMSpeshStatsValueCount));
    MVM_ASSIGN_REF(tc, &(sf->body.spesh->common.header), oss->values[found].value, value);
    oss->values[found].count = 1;
}

/* Adds/increments the count of a type tuple seen at the given offset. */
void add_type_tuple_at_offset(MVMThreadContext *tc, MVMSpeshStatsByOffset *oss,
                              MVMStaticFrame *sf, SimCallType *info) {
    /* Compute type tuple size. */
    size_t tt_size = info->cs->flag_count * sizeof(MVMSpeshStatsType);

    /* If we have it already, increment the count. */
    MVMuint32 found, i;
    MVMuint32 n = oss->num_type_tuples;
    for (found = 0; found < n; found++) {
        if (oss->type_tuples[found].cs == info->cs) {
            if (memcmp(oss->type_tuples[found].arg_types, info->arg_types, tt_size) == 0) {
                oss->type_tuples[found].count++;
                return;
            }
        }
    }

    /* Otherwise, add it to the list; copy type tuple to ease memory
     * management, but also need to write barrier any types. */
    found = oss->num_type_tuples;
    oss->num_type_tuples++;
    oss->type_tuples = MVM_realloc(oss->type_tuples,
        oss->num_type_tuples * sizeof(MVMSpeshStatsTypeTupleCount));
    oss->type_tuples[found].cs = info->cs;
    oss->type_tuples[found].arg_types = MVM_malloc(tt_size);
    memcpy(oss->type_tuples[found].arg_types, info->arg_types, tt_size);
    for (i = 0; i < info->cs->flag_count; i++) {
        if (info->arg_types[i].type)
            MVM_gc_write_barrier(tc, &(sf->body.spesh->common.header),
                &(info->arg_types[i].type->header));
        if (info->arg_types[i].decont_type)
            MVM_gc_write_barrier(tc, &(sf->body.spesh->common.header),
                &(info->arg_types[i].decont_type->header));
    }
    oss->type_tuples[found].count = 1;
}

/* Initializes the stack simulation. */
void sim_stack_init(MVMThreadContext *tc, SimStack *sims) {
    sims->used = 0;
    sims->limit = 32;
    sims->frames = MVM_malloc(sims->limit * sizeof(SimStackFrame));
    sims->depth = 0;
}

/* Pushes an entry onto the stack frame model. */
void sim_stack_push(MVMThreadContext *tc, SimStack *sims, MVMStaticFrame *sf,
                    MVMSpeshStats *ss, MVMuint32 cid, MVMuint32 callsite_idx) {
    SimStackFrame *frame;
    MVMCallsite *cs;
    if (sims->used == sims->limit) {
        sims->limit *= 2;
        sims->frames = MVM_realloc(sims->frames, sims->limit * sizeof(SimStackFrame));
    }
    frame = &(sims->frames[sims->used++]);
    frame->sf = sf;
    frame->ss = ss;
    frame->cid = cid;
    frame->callsite_idx = callsite_idx;
    frame->arg_types = (cs = ss->by_callsite[callsite_idx].cs)
        ? MVM_calloc(cs->flag_count, sizeof(MVMSpeshStatsType))
        : NULL;
    frame->offset_logs = NULL;
    frame->offset_logs_used = frame->offset_logs_limit = 0;
    frame->osr_hits = 0;
    frame->call_type_info = NULL;
    frame->call_type_info_used = frame->call_type_info_limit = 0;
    frame->last_invoke_offset = 0;
    frame->last_invoke_code = NULL;
    sims->depth++;
}

/* Adds an entry to a sim frame's callsite type info list, for later
 * inclusion in the callsite stats. */
void add_sim_call_type_info(MVMThreadContext *tc, SimStackFrame *simf,
                            MVMuint32 bytecode_offset, MVMCallsite *cs,
                            MVMSpeshStatsType *arg_types) {
    SimCallType *info;
    if (simf->call_type_info_used == simf->call_type_info_limit) {
        simf->call_type_info_limit += 32;
        simf->call_type_info = MVM_realloc(simf->call_type_info,
            simf->call_type_info_limit * sizeof(SimCallType));
    }
    info = &(simf->call_type_info[simf->call_type_info_used++]);
    info->bytecode_offset = bytecode_offset;
    info->cs = cs;
    info->arg_types = arg_types;
}


/* Pops the top frame from the sim stack. */
void sim_stack_pop(MVMThreadContext *tc, SimStack *sims) {
    SimStackFrame *simf;
    MVMSpeshStatsByType *tss;
    MVMuint32 frame_depth;

    /* Pop off the simulated frame. */
    if (sims->used == 0)
        MVM_panic(1, "Spesh stats: cannot pop an empty simulation stack");
    sims->used--;
    simf = &(sims->frames[sims->used]);
    frame_depth = sims->depth--;

    /* Add OSR hits at callsite level and update depth. */
    if (simf->osr_hits) {
        simf->ss->osr_hits += simf->osr_hits;
        simf->ss->by_callsite[simf->callsite_idx].osr_hits += simf->osr_hits;
    }
    if (frame_depth > simf->ss->by_callsite[simf->callsite_idx].max_depth)
        simf->ss->by_callsite[simf->callsite_idx].max_depth = frame_depth;

    /* See if there's a type tuple to attach type-based stats to. */
    tss = by_type(tc, simf->ss, simf->callsite_idx, simf->arg_types);
    if (tss) {
        /* Incorporate data logged at offsets. */
        MVMuint32 i;
        for (i = 0; i < simf->offset_logs_used; i++) {
            MVMSpeshLogEntry *e = simf->offset_logs[i];
            switch (e->kind) {
                case MVM_SPESH_LOG_TYPE:
                case MVM_SPESH_LOG_RETURN: {
                    MVMSpeshStatsByOffset *oss = by_offset(tc, tss,
                        e->type.bytecode_offset);
                    add_type_at_offset(tc, oss, simf->sf, e->type.type,
                        e->type.flags & MVM_SPESH_LOG_TYPE_FLAG_CONCRETE);
                    break;
                }
                case MVM_SPESH_LOG_INVOKE: {
                    MVMSpeshStatsByOffset *oss = by_offset(tc, tss,
                        e->value.bytecode_offset);
                    add_value_at_offset(tc, oss, simf->sf, e->value.value);
                    break;
                }
            }
        }

        /* Incorporate callsite type stats (what type tuples did we make a
         * call with). */
        for (i = 0; i < simf->call_type_info_used; i++) {
            SimCallType *info = &(simf->call_type_info[i]);
            MVMSpeshStatsByOffset *oss = by_offset(tc, tss, info->bytecode_offset);
            add_type_tuple_at_offset(tc, oss, simf->sf, info);
        }

        /* Incorporate OSR hits and bump max depth. */
        tss->hits++;
        tss->osr_hits += simf->osr_hits;
        if (frame_depth > tss->max_depth)
            tss->max_depth = frame_depth;

        /* If the callee's last incovation matches the frame just invoked,
         * then log the type tuple against the callsite. */
        if (sims->used) {
            SimStackFrame *caller = &(sims->frames[sims->used - 1]);
            MVMObject *lic = caller->last_invoke_code;
            if (lic && IS_CONCRETE(lic) && REPR(lic)->ID == MVM_REPR_ID_MVMCode) {
                MVMStaticFrame *called_sf = ((MVMCode *)lic)->body.sf;
                if (called_sf == simf->sf)
                    add_sim_call_type_info(tc, caller, caller->last_invoke_offset,
                        simf->ss->by_callsite[simf->callsite_idx].cs,
                        tss->arg_types);
            }
        }
    }

    /* Clear up offset logs and call type info; they're either incorproated or
     * to be tossed. */
    MVM_free(simf->offset_logs);
    MVM_free(simf->call_type_info);
}

/* Gets the simulation stack frame for the specified correlation ID. If it is
 * not on the top, searches to see if it's further down. If it is, then pops
 * off the top to reach it. If it's not found at all, returns NULL and does
 * nothing to the simulation stack. */
SimStackFrame * sim_stack_find(MVMThreadContext *tc, SimStack *sims, MVMuint32 cid) {
    MVMuint32 found_at = sims->used;
    while (found_at != 0) {
        found_at--;
        if (sims->frames[found_at].cid == cid) {
            MVMint32 pop = (sims->used - found_at) - 1;
            MVMint32 i;
            for (i = 0; i < pop; i++)
                sim_stack_pop(tc, sims);
            return &(sims->frames[found_at]);
        }
    }
    return NULL;
}

/* Destroys the stack simulation. */
void sim_stack_destroy(MVMThreadContext *tc, SimStack *sims) {
    while (sims->used)
        sim_stack_pop(tc, sims);
    MVM_free(sims->frames);
}

/* Gets the parameter type slot from a simulation frame. */
MVMSpeshStatsType * param_type(MVMThreadContext *tc, SimStackFrame *simf, MVMSpeshLogEntry *e) {
    MVMuint16 idx = e->param.arg_idx;
    MVMCallsite *cs = simf->ss->by_callsite[simf->callsite_idx].cs;
    if (cs) {
        MVMint32 flag_idx = idx < cs->num_pos
            ? idx
            : cs->num_pos + (((idx - 1) - cs->num_pos) / 2);
        if (flag_idx >= cs->flag_count)
            MVM_panic(1, "Spesh stats: argument flag index out of bounds");
        if (cs->arg_flags[flag_idx] & MVM_CALLSITE_ARG_OBJ)
            return &(simf->arg_types[flag_idx]);
    }
    return NULL;
}

/* Records a static value for a frame, unless it's already in the log. */
void add_static_value(MVMThreadContext *tc, SimStackFrame *simf, MVMint32 bytecode_offset,
                      MVMObject *value) {
    MVMSpeshStats *ss = simf->ss;
    MVMuint32 i, id;
    for (i = 0; i < ss->num_static_values; i++)
        if (ss->static_values[i].bytecode_offset == bytecode_offset)
            return;
    id = ss->num_static_values++;
    ss->static_values = MVM_realloc(ss->static_values,
        ss->num_static_values * sizeof(MVMSpeshStatsStatic));
    ss->static_values[id].bytecode_offset = bytecode_offset;
    MVM_ASSIGN_REF(tc, &(simf->sf->body.spesh->common.header), ss->static_values[id].value, value);
}

/* Receives a spesh log and updates static frame statistics. Each static frame
 * that is updated is pushed once into sf_updated. */
void MVM_spesh_stats_update(MVMThreadContext *tc, MVMSpeshLog *sl, MVMObject *sf_updated) {
    MVMuint32 i;
    MVMuint32 n = sl->body.used;
    SimStack sims;
#if MVM_GC_DEBUG
    tc->in_spesh = 1;
#endif
    sim_stack_init(tc, &sims);
    for (i = 0; i < n; i++) {
        MVMSpeshLogEntry *e = &(sl->body.entries[i]);
        switch (e->kind) {
            case MVM_SPESH_LOG_ENTRY: {
                MVMSpeshStats *ss = stats_for(tc, e->entry.sf);
                MVMuint32 callsite_idx;
                if (ss->last_update != tc->instance->spesh_stats_version) {
                    ss->last_update = tc->instance->spesh_stats_version;
                    MVM_repr_push_o(tc, sf_updated, (MVMObject *)e->entry.sf);
                }
                ss->hits++;
                callsite_idx = by_callsite_idx(tc, ss, e->entry.cs);
                ss->by_callsite[callsite_idx].hits++;
                sim_stack_push(tc, &sims, e->entry.sf, ss, e->id, callsite_idx);
                break;
            }
            case MVM_SPESH_LOG_PARAMETER: {
                SimStackFrame *simf = sim_stack_find(tc, &sims, e->id);
                if (simf) {
                    MVMSpeshStatsType *type_slot = param_type(tc, simf, e);
                    if (type_slot) {
                        MVM_ASSIGN_REF(tc, &(simf->sf->body.spesh->common.header),
                            type_slot->type, e->param.type);
                        type_slot->type_concrete =
                            e->param.flags & MVM_SPESH_LOG_TYPE_FLAG_CONCRETE;
                    }
                }
                break;
            }
            case MVM_SPESH_LOG_PARAMETER_DECONT: {
                SimStackFrame *simf = sim_stack_find(tc, &sims, e->id);
                if (simf) {
                    MVMSpeshStatsType *type_slot = param_type(tc, simf, e);
                    if (type_slot) {
                        MVM_ASSIGN_REF(tc, &(simf->sf->body.spesh->common.header),
                            type_slot->decont_type, e->param.type);
                        type_slot->decont_type_concrete =
                            e->param.flags & MVM_SPESH_LOG_TYPE_FLAG_CONCRETE;
                    }
                }
                break;
            }
            case MVM_SPESH_LOG_TYPE:
            case MVM_SPESH_LOG_INVOKE: {
                /* We only incorporate these into the model later, and only
                 * then if we need to. For now, just keep references to
                 * them. */
                SimStackFrame *simf = sim_stack_find(tc, &sims, e->id);
                if (simf) {
                    if (simf->offset_logs_used == simf->offset_logs_limit) {
                        simf->offset_logs_limit += 32;
                        simf->offset_logs = MVM_realloc(simf->offset_logs,
                            simf->offset_logs_limit * sizeof(MVMSpeshLogEntry *));
                    }
                    simf->offset_logs[simf->offset_logs_used++] = e;
                    if (e->kind == MVM_SPESH_LOG_INVOKE) {
                        simf->last_invoke_offset = e->value.bytecode_offset;
                        simf->last_invoke_code = e->value.value;
                    }
                }
                break;
            }
            case MVM_SPESH_LOG_OSR: {
                SimStackFrame *simf = sim_stack_find(tc, &sims, e->id);
                if (simf)
                    simf->osr_hits++;
                break;
            }
            case MVM_SPESH_LOG_STATIC: {
                SimStackFrame *simf = sim_stack_find(tc, &sims, e->id);
                if (simf)
                    add_static_value(tc, simf, e->value.bytecode_offset, e->value.value);
                break;
            }
            case MVM_SPESH_LOG_RETURN: {
                SimStackFrame *simf = sim_stack_find(tc, &sims, e->id);
                if (simf) {
                    MVMStaticFrame *called_sf = simf->sf;
                    sim_stack_pop(tc, &sims);
                    if (e->type.type && sims.used) {
                        SimStackFrame *caller = &(sims.frames[sims.used - 1]);
                        MVMObject *lic = caller->last_invoke_code;
                        if (lic && IS_CONCRETE(lic) && REPR(lic)->ID == MVM_REPR_ID_MVMCode) {
                            if (called_sf == ((MVMCode *)lic)->body.sf) {
                                if (caller->offset_logs_used == caller->offset_logs_limit) {
                                    caller->offset_logs_limit += 32;
                                    caller->offset_logs = MVM_realloc(caller->offset_logs,
                                        caller->offset_logs_limit * sizeof(MVMSpeshLogEntry *));
                                }
                                e->type.bytecode_offset = caller->last_invoke_offset;
                                caller->offset_logs[caller->offset_logs_used++] = e;
                            }
                        }
                    }
                }
                break;
            }
        }
    }
    sim_stack_destroy(tc, &sims);
#if MVM_GC_DEBUG
    tc->in_spesh = 0;
#endif
}

/* Takes an array of frames we recently updated the stats in. If they weren't
 * updated in a while, clears them out. */
void MVM_spesh_stats_cleanup(MVMThreadContext *tc, MVMObject *check_frames) {
    MVMint64 elems = MVM_repr_elems(tc, check_frames);
    MVMint64 insert_pos = 0;
    MVMint64 i;
    for (i = 0; i < elems; i++) {
        MVMStaticFrame *sf = (MVMStaticFrame *)MVM_repr_at_pos_o(tc, check_frames, i);
        MVMStaticFrameSpesh *spesh = sf->body.spesh;
        MVMSpeshStats *ss = spesh->body.spesh_stats;
        if (!ss) {
            /* No stats; already destroyed, don't keep this frame under
             * consideration. */
        }
        else if (tc->instance->spesh_stats_version - ss->last_update > MVM_SPESH_STATS_MAX_AGE) {
            MVM_spesh_stats_destroy(tc, ss);
            spesh->body.spesh_stats = NULL;
        }
        else {
            MVM_repr_bind_pos_o(tc, check_frames, insert_pos++, (MVMObject *)sf);
        }
    }
    MVM_repr_pos_set_elems(tc, check_frames, insert_pos);
}

void MVM_spesh_stats_gc_mark(MVMThreadContext *tc, MVMSpeshStats *ss, MVMGCWorklist *worklist) {
    if (ss) {
        MVMuint32 i, j, k, l, m;
        for (i = 0; i < ss->num_by_callsite; i++) {
            MVMSpeshStatsByCallsite *by_cs = &(ss->by_callsite[i]);
            for (j = 0; j < by_cs->num_by_type; j++) {
                MVMSpeshStatsByType *by_type = &(by_cs->by_type[j]);
                MVMuint32 num_types = by_cs->cs->flag_count;
                for (k = 0; k < num_types; k++) {
                    MVM_gc_worklist_add(tc, worklist, &(by_type->arg_types[k].type));
                    MVM_gc_worklist_add(tc, worklist, &(by_type->arg_types[k].decont_type));
                }
                for (k = 0; k < by_type->num_by_offset; k++) {
                    MVMSpeshStatsByOffset *by_offset = &(by_type->by_offset[k]);
                    for (l = 0; l < by_offset->num_types; l++)
                        MVM_gc_worklist_add(tc, worklist, &(by_offset->types[l].type));
                    for (l = 0; l < by_offset->num_values; l++)
                        MVM_gc_worklist_add(tc, worklist, &(by_offset->values[l].value));
                    for (l = 0; l < by_offset->num_type_tuples; l++) {
                        MVMSpeshStatsType *off_types = by_offset->type_tuples[l].arg_types;
                        MVMuint32 num_off_types = by_offset->type_tuples[l].cs->flag_count;
                        for (m = 0; m < num_off_types; m++) {
                            MVM_gc_worklist_add(tc, worklist, &(off_types[m].type));
                            MVM_gc_worklist_add(tc, worklist, &(off_types[m].decont_type));
                        }
                    }
                }
            }
        }
        for (i = 0; i < ss->num_static_values; i++)
            MVM_gc_worklist_add(tc, worklist, &(ss->static_values[i].value));
    }
}

void MVM_spesh_stats_destroy(MVMThreadContext *tc, MVMSpeshStats *ss) {
    if (ss) {
        MVMuint32 i, j, k, l;
        for (i = 0; i < ss->num_by_callsite; i++) {
            MVMSpeshStatsByCallsite *by_cs = &(ss->by_callsite[i]);
            for (j = 0; j < by_cs->num_by_type; j++) {
                MVMSpeshStatsByType *by_type = &(by_cs->by_type[j]);
                for (k = 0; k < by_type->num_by_offset; k++) {
                    MVMSpeshStatsByOffset *by_offset = &(by_type->by_offset[k]);
                    MVM_free(by_offset->types);
                    MVM_free(by_offset->values);
                    for (l = 0; l < by_offset->num_type_tuples; l++)
                        MVM_free(by_offset->type_tuples[l].arg_types);
                    MVM_free(by_offset->type_tuples);
                }
                MVM_free(by_type->by_offset);
                MVM_free(by_type->arg_types);
            }
            MVM_free(by_cs->by_type);
        }
        MVM_free(ss->by_callsite);
        MVM_free(ss->static_values);
    }
}
