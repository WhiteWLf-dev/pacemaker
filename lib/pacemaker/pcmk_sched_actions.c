/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <sys/param.h>
#include <glib.h>

#include <pacemaker-internal.h>
#include "libpacemaker_private.h"

/*!
 * \internal
 * \brief Get the action flags relevant to ordering constraints
 *
 * \param[in] action  Action to check
 * \param[in] node    Node that *other* action in the ordering is on
 *                    (used only for clone resource actions)
 *
 * \return Action flags that should be used for orderings
 */
static enum pe_action_flags
action_flags_for_ordering(pe_action_t *action, pe_node_t *node)
{
    bool runnable = false;
    enum pe_action_flags flags;

    // For non-resource actions, return the action flags
    if (action->rsc == NULL) {
        return action->flags;
    }

    /* For non-clone resources, or a clone action not assigned to a node,
     * return the flags as determined by the resource method without a node
     * specified.
     */
    flags = action->rsc->cmds->action_flags(action, NULL);
    if ((node == NULL) || !pe_rsc_is_clone(action->rsc)) {
        return flags;
    }

    /* Otherwise (i.e., for clone resource actions on a specific node), first
     * remember whether the non-node-specific action is runnable.
     */
    runnable = pcmk_is_set(flags, pe_action_runnable);

    // Then recheck the resource method with the node
    flags = action->rsc->cmds->action_flags(action, node);

    /* For clones in ordering constraints, the node-specific "runnable" doesn't
     * matter, just the non-node-specific setting (i.e., is the action runnable
     * anywhere).
     *
     * This applies only to runnable, and only for ordering constraints. This
     * function shouldn't be used for other types of constraints without
     * changes. Not very satisfying, but it's logical and appears to work well.
     */
    if (runnable && !pcmk_is_set(flags, pe_action_runnable)) {
        pe__set_raw_action_flags(flags, action->rsc->id,
                                 pe_action_runnable);
    }
    return flags;
}

static char *
convert_non_atomic_uuid(char *old_uuid, pe_resource_t * rsc, gboolean allow_notify,
                        gboolean free_original)
{
    guint interval_ms = 0;
    char *uuid = NULL;
    char *rid = NULL;
    char *raw_task = NULL;
    int task = no_action;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "Processing %s", old_uuid);
    if (old_uuid == NULL) {
        return NULL;

    } else if (strstr(old_uuid, "notify") != NULL) {
        goto done;              /* no conversion */

    } else if (rsc->variant < pe_group) {
        goto done;              /* no conversion */
    }

    CRM_ASSERT(parse_op_key(old_uuid, &rid, &raw_task, &interval_ms));
    if (interval_ms > 0) {
        goto done;              /* no conversion */
    }

    task = text2task(raw_task);
    switch (task) {
        case stop_rsc:
        case start_rsc:
        case action_notify:
        case action_promote:
        case action_demote:
            break;
        case stopped_rsc:
        case started_rsc:
        case action_notified:
        case action_promoted:
        case action_demoted:
            task--;
            break;
        case monitor_rsc:
        case shutdown_crm:
        case stonith_node:
            task = no_action;
            break;
        default:
            crm_err("Unknown action: %s", raw_task);
            task = no_action;
            break;
    }

    if (task != no_action) {
        if (pcmk_is_set(rsc->flags, pe_rsc_notify) && allow_notify) {
            uuid = pcmk__notify_key(rid, "confirmed-post", task2text(task + 1));

        } else {
            uuid = pcmk__op_key(rid, task2text(task + 1), 0);
        }
        pe_rsc_trace(rsc, "Converted %s -> %s", old_uuid, uuid);
    }

  done:
    if (uuid == NULL) {
        uuid = strdup(old_uuid);
    }

    if (free_original) {
        free(old_uuid);
    }

    free(raw_task);
    free(rid);
    return uuid;
}

static pe_action_t *
rsc_expand_action(pe_action_t * action)
{
    gboolean notify = FALSE;
    pe_action_t *result = action;
    pe_resource_t *rsc = action->rsc;

    if (rsc == NULL) {
        return action;
    }

    if ((rsc->parent == NULL)
        || (pe_rsc_is_clone(rsc) && (rsc->parent->variant == pe_container))) {
        /* Only outermost resources have notification actions.
         * The exception is those in bundles.
         */
        notify = pcmk_is_set(rsc->flags, pe_rsc_notify);
    }

    if (rsc->variant >= pe_group) {
        /* Expand 'start' -> 'started' */
        char *uuid = NULL;

        uuid = convert_non_atomic_uuid(action->uuid, rsc, notify, FALSE);
        if (uuid) {
            pe_rsc_trace(rsc, "Converting %s to %s %d", action->uuid, uuid,
                         pcmk_is_set(rsc->flags, pe_rsc_notify));
            result = find_first_action(rsc->actions, uuid, NULL, NULL);
            if (result == NULL) {
                crm_err("Couldn't expand %s to %s in %s", action->uuid, uuid, rsc->id);
                result = action;
            }
            free(uuid);
        }
    }
    return result;
}

static enum pe_graph_flags
graph_update_action(pe_action_t * first, pe_action_t * then, pe_node_t * node,
                    enum pe_action_flags first_flags, enum pe_action_flags then_flags,
                    pe_action_wrapper_t *order, pe_working_set_t *data_set)
{
    enum pe_graph_flags changed = pe_graph_none;
    enum pe_ordering type = order->type;

    /* TODO: Do as many of these in parallel as possible */

    if (pcmk_is_set(type, pe_order_implies_then_on_node)) {
        /* Normally we want the _whole_ 'then' clone to
         * restart if 'first' is restarted, so then->node is
         * needed.
         *
         * However for unfencing, we want to limit this to
         * instances on the same node as 'first' (the
         * unfencing operation), so first->node is supplied.
         *
         * Swap the node, from then on we can can treat it
         * like any other 'pe_order_implies_then'
         */

        pe__clear_order_flags(type, pe_order_implies_then_on_node);
        pe__set_order_flags(type, pe_order_implies_then);
        node = first->node;
        pe_rsc_trace(then->rsc,
                     "%s then %s: mapped pe_order_implies_then_on_node to "
                     "pe_order_implies_then on %s",
                     first->uuid, then->uuid, node->details->uname);
    }

    if (type & pe_order_implies_then) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags & pe_action_optional, pe_action_optional,
                pe_order_implies_then, data_set);

        } else if (!pcmk_is_set(first_flags, pe_action_optional)
                   && pcmk_is_set(then->flags, pe_action_optional)) {
            pe__clear_action_flags(then, pe_action_optional);
            pe__set_graph_flags(changed, first, pe_graph_updated_then);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_implies_then",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if ((type & pe_order_restart) && then->rsc) {
        enum pe_action_flags restart = (pe_action_optional | pe_action_runnable);

        changed |= then->rsc->cmds->update_actions(first, then, node,
                                                   first_flags, restart,
                                                   pe_order_restart, data_set);
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_restart",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_implies_first) {
        if (first->rsc) {
            changed |= first->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_optional, pe_order_implies_first,
                data_set);

        } else if (!pcmk_is_set(first_flags, pe_action_optional)
                   && pcmk_is_set(first->flags, pe_action_runnable)) {
            pe__clear_action_flags(first, pe_action_runnable);
            pe__set_graph_flags(changed, first, pe_graph_updated_first);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_implies_first",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_promoted_implies_first) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags & pe_action_optional, pe_action_optional,
                pe_order_promoted_implies_first, data_set);
        }
        pe_rsc_trace(then->rsc,
                     "%s then %s: %s after pe_order_promoted_implies_first",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_one_or_more) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_runnable, pe_order_one_or_more,
                data_set);

        } else if (pcmk_is_set(first_flags, pe_action_runnable)) {
            // We have another runnable instance of "first"
            then->runnable_before++;

            /* Mark "then" as runnable if it requires a certain number of
             * "before" instances to be runnable, and they now are.
             */
            if ((then->runnable_before >= then->required_runnable_before)
                && !pcmk_is_set(then->flags, pe_action_runnable)) {

                pe__set_action_flags(then, pe_action_runnable);
                pe__set_graph_flags(changed, first, pe_graph_updated_then);
            }
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_one_or_more",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (then->rsc && pcmk_is_set(type, pe_order_probe)) {
        if (!pcmk_is_set(first_flags, pe_action_runnable)
            && (first->rsc->running_on != NULL)) {

            pe_rsc_trace(then->rsc,
                         "%s then %s: ignoring because first is stopping",
                         first->uuid, then->uuid);
            type = pe_order_none;
            order->type = pe_order_none;

        } else {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_runnable, pe_order_runnable_left,
                data_set);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_probe",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_runnable_left) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_runnable, pe_order_runnable_left,
                data_set);

        } else if (!pcmk_is_set(first_flags, pe_action_runnable)
                   && pcmk_is_set(then->flags, pe_action_runnable)) {

            pe__clear_action_flags(then, pe_action_runnable);
            pe__set_graph_flags(changed, first, pe_graph_updated_then);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_runnable_left",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_implies_first_migratable) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_optional,
                pe_order_implies_first_migratable, data_set);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after "
                     "pe_order_implies_first_migratable",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_pseudo_left) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_optional, pe_order_pseudo_left,
                data_set);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_pseudo_left",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_optional) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_runnable, pe_order_optional, data_set);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_optional",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if (type & pe_order_asymmetrical) {
        if (then->rsc) {
            changed |= then->rsc->cmds->update_actions(first, then, node,
                first_flags, pe_action_runnable, pe_order_asymmetrical,
                data_set);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after pe_order_asymmetrical",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    if ((first->flags & pe_action_runnable) && (type & pe_order_implies_then_printed)
        && (first_flags & pe_action_optional) == 0) {
        pe_rsc_trace(then->rsc, "%s will be in graph because %s is required",
                     then->uuid, first->uuid);
        pe__set_action_flags(then, pe_action_print_always);
        // Don't bother marking 'then' as changed just for this
    }

    if (pcmk_is_set(type, pe_order_implies_first_printed)
        && !pcmk_is_set(then_flags, pe_action_optional)) {

        pe_rsc_trace(then->rsc, "%s will be in graph because %s is required",
                     first->uuid, then->uuid);
        pe__set_action_flags(first, pe_action_print_always);
        // Don't bother marking 'first' as changed just for this
    }

    if ((type & pe_order_implies_then
         || type & pe_order_implies_first
         || type & pe_order_restart)
        && first->rsc
        && pcmk__str_eq(first->task, RSC_STOP, pcmk__str_casei)
        && !pcmk_is_set(first->rsc->flags, pe_rsc_managed)
        && pcmk_is_set(first->rsc->flags, pe_rsc_block)
        && !pcmk_is_set(first->flags, pe_action_runnable)) {

        if (pcmk_is_set(then->flags, pe_action_runnable)) {
            pe__clear_action_flags(then, pe_action_runnable);
            pe__set_graph_flags(changed, first, pe_graph_updated_then);
        }
        pe_rsc_trace(then->rsc, "%s then %s: %s after checking whether first "
                     "is blocked, unmanaged, unrunnable stop",
                     first->uuid, then->uuid,
                     (changed? "changed" : "unchanged"));
    }

    return changed;
}

// Convenience macros for logging action properties

#define action_type_str(flags) \
    (pcmk_is_set((flags), pe_action_pseudo)? "pseudo-action" : "action")

#define action_optional_str(flags) \
    (pcmk_is_set((flags), pe_action_optional)? "optional" : "required")

#define action_runnable_str(flags) \
    (pcmk_is_set((flags), pe_action_runnable)? "runnable" : "unrunnable")

#define action_node_str(a) \
    (((a)->node == NULL)? "no node" : (a)->node->details->uname)

gboolean
update_action(pe_action_t *then, pe_working_set_t *data_set)
{
    GList *lpc = NULL;
    enum pe_graph_flags changed = pe_graph_none;
    int last_flags = then->flags;

    pe_rsc_trace(then->rsc, "Updating %s %s (%s %s) on %s",
                 action_type_str(then->flags), then->uuid,
                 action_optional_str(then->flags),
                 action_runnable_str(then->flags), action_node_str(then));

    if (pcmk_is_set(then->flags, pe_action_requires_any)) {
        /* initialize current known runnable before actions to 0
         * from here as graph_update_action is called for each of
         * then's before actions, this number will increment as
         * runnable 'first' actions are encountered */
        then->runnable_before = 0;

        /* for backwards compatibility with previous options that use
         * the 'requires_any' flag, initialize required to 1 if it is
         * not set. */ 
        if (then->required_runnable_before == 0) {
            then->required_runnable_before = 1;
        }
        pe__clear_action_flags(then, pe_action_runnable);
        /* We are relying on the pe_order_one_or_more clause of
         * graph_update_action(), called as part of the:
         *
         *    'if (first == other->action)'
         *
         * block below, to set this back if appropriate
         */
    }

    for (lpc = then->actions_before; lpc != NULL; lpc = lpc->next) {
        pe_action_wrapper_t *other = (pe_action_wrapper_t *) lpc->data;
        pe_action_t *first = other->action;

        pe_node_t *then_node = then->node;
        pe_node_t *first_node = first->node;

        enum pe_action_flags then_flags = 0;
        enum pe_action_flags first_flags = 0;

        if (first->rsc && first->rsc->variant == pe_group && pcmk__str_eq(first->task, RSC_START, pcmk__str_casei)) {
            first_node = first->rsc->fns->location(first->rsc, NULL, FALSE);
            if (first_node) {
                pe_rsc_trace(first->rsc, "Found node %s for 'first' %s",
                             first_node->details->uname, first->uuid);
            }
        }

        if (then->rsc && then->rsc->variant == pe_group && pcmk__str_eq(then->task, RSC_START, pcmk__str_casei)) {
            then_node = then->rsc->fns->location(then->rsc, NULL, FALSE);
            if (then_node) {
                pe_rsc_trace(then->rsc, "Found node %s for 'then' %s",
                             then_node->details->uname, then->uuid);
            }
        }
        /* Disable constraint if it only applies when on same node, but isn't */
        if (pcmk_is_set(other->type, pe_order_same_node)
            && (first_node != NULL) && (then_node != NULL)
            && (first_node->details != then_node->details)) {

            pe_rsc_trace(then->rsc,
                         "Disabled ordering %s on %s then %s on %s: not same node",
                         other->action->uuid, first_node->details->uname,
                         then->uuid, then_node->details->uname);
            other->type = pe_order_none;
            continue;
        }

        pe__clear_graph_flags(changed, then, pe_graph_updated_first);

        if (first->rsc && pcmk_is_set(other->type, pe_order_then_cancels_first)
            && !pcmk_is_set(then->flags, pe_action_optional)) {

            /* 'then' is required, so we must abandon 'first'
             * (e.g. a required stop cancels any agent reload).
             */
            pe__set_action_flags(other->action, pe_action_optional);
            if (!strcmp(first->task, CRMD_ACTION_RELOAD_AGENT)) {
                pe__clear_resource_flags(first->rsc, pe_rsc_reload);
            }
        }

        if (first->rsc && then->rsc && (first->rsc != then->rsc)
            && (is_parent(then->rsc, first->rsc) == FALSE)) {
            first = rsc_expand_action(first);
        }
        if (first != other->action) {
            pe_rsc_trace(then->rsc, "Ordering %s after %s instead of %s",
                         then->uuid, first->uuid, other->action->uuid);
        }

        first_flags = action_flags_for_ordering(first, then_node);
        then_flags = action_flags_for_ordering(then, first_node);

        pe_rsc_trace(then->rsc,
                     "%s then %s: type=0x%.6x filter=0x%.6x "
                     "(%s %s %s on %s 0x%.6x then 0x%.6x)",
                     first->uuid, then->uuid, other->type, first_flags,
                     action_optional_str(first_flags),
                     action_runnable_str(first_flags),
                     action_type_str(first_flags), action_node_str(first),
                     first->flags, then->flags);

        if (first == other->action) {
            /*
             * 'first' was not expanded (e.g. from 'start' to 'running'), which could mean it:
             * - has no associated resource,
             * - was a primitive,
             * - was pre-expanded (e.g. 'running' instead of 'start')
             *
             * The third argument here to graph_update_action() is a node which is used under two conditions:
             * - Interleaving, in which case first->node and
             *   then->node are equal (and NULL)
             * - If 'then' is a clone, to limit the scope of the
             *   constraint to instances on the supplied node
             *
             */
            pe_node_t *node = then->node;
            changed |= graph_update_action(first, then, node, first_flags,
                                           then_flags, other, data_set);

            /* 'first' was for a complex resource (clone, group, etc),
             * create a new dependency if necessary
             */
        } else if (order_actions(first, then, other->type)) {
            /* This was the first time 'first' and 'then' were associated,
             * start again to get the new actions_before list
             */
            pe__set_graph_flags(changed, then,
                                pe_graph_updated_then|pe_graph_disable);
        }

        if (changed & pe_graph_disable) {
            pe_rsc_trace(then->rsc,
                         "Disabled ordering %s then %s in favor of %s then %s",
                         other->action->uuid, then->uuid, first->uuid,
                         then->uuid);
            pe__clear_graph_flags(changed, then, pe_graph_disable);
            other->type = pe_order_none;
        }

        if (changed & pe_graph_updated_first) {
            GList *lpc2 = NULL;

            crm_trace("Re-processing %s and its 'after' actions since it changed",
                      first->uuid);
            for (lpc2 = first->actions_after; lpc2 != NULL; lpc2 = lpc2->next) {
                pe_action_wrapper_t *other = (pe_action_wrapper_t *) lpc2->data;

                update_action(other->action, data_set);
            }
            update_action(first, data_set);
        }
    }

    if (pcmk_is_set(then->flags, pe_action_requires_any)) {
        if (last_flags != then->flags) {
            pe__set_graph_flags(changed, then, pe_graph_updated_then);
        } else {
            pe__clear_graph_flags(changed, then, pe_graph_updated_then);
        }
    }

    if (changed & pe_graph_updated_then) {
        crm_trace("Re-processing %s and its 'after' actions since it changed",
                  then->uuid);
        if (pcmk_is_set(last_flags, pe_action_runnable)
            && !pcmk_is_set(then->flags, pe_action_runnable)) {
            pcmk__block_colocated_starts(then, data_set);
        }
        update_action(then, data_set);
        for (lpc = then->actions_after; lpc != NULL; lpc = lpc->next) {
            pe_action_wrapper_t *other = (pe_action_wrapper_t *) lpc->data;

            update_action(other->action, data_set);
        }
    }

    return FALSE;
}
