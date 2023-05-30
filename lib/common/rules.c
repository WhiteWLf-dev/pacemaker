/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>                          // NULL, size_t
#include <stdlib.h>                         // calloc()
#include <ctype.h>                          // isdigit()
#include <regex.h>                          // regmatch_t
#include <stdint.h>                         // uint32_t
#include <inttypes.h>                       // PRIu32
#include <glib.h>                           // gboolean, FALSE
#include <libxml/tree.h>                    // xmlNode

#include <crm/common/scheduler.h>

#include <crm/common/iso8601_internal.h>
#include <crm/common/nvpair_internal.h>
#include <crm/common/scheduler_internal.h>
#include "crmcommon_private.h"

/*!
 * \internal
 * \brief Get the expression type corresponding to given expression XML
 *
 * \param[in] expr  Rule expression XML
 *
 * \return Expression type corresponding to \p expr
 */
enum expression_type
pcmk__expression_type(const xmlNode *expr)
{
    const char *name = NULL;

    // Expression types based on element name

    if (pcmk__xe_is(expr, PCMK_XE_DATE_EXPRESSION)) {
        return pcmk__subexpr_datetime;

    } else if (pcmk__xe_is(expr, PCMK_XE_RSC_EXPRESSION)) {
        return pcmk__subexpr_resource;

    } else if (pcmk__xe_is(expr, PCMK_XE_OP_EXPRESSION)) {
        return pcmk__subexpr_operation;

    } else if (pcmk__xe_is(expr, PCMK_XE_RULE)) {
        return pcmk__subexpr_rule;

    } else if (!pcmk__xe_is(expr, PCMK_XE_EXPRESSION)) {
        return pcmk__subexpr_unknown;
    }

    // Expression types based on node attribute name

    name = crm_element_value(expr, PCMK_XA_ATTRIBUTE);

    if (pcmk__str_any_of(name, CRM_ATTR_UNAME, CRM_ATTR_KIND, CRM_ATTR_ID,
                         NULL)) {
        return pcmk__subexpr_location;
    }

    return pcmk__subexpr_attribute;
}

/*!
 * \internal
 * \brief Get parent XML element's ID for logging purposes
 *
 * \param[in] xml  XML of a subelement
 *
 * \return ID of \p xml's parent for logging purposes (guaranteed non-NULL)
 */
static const char *
loggable_parent_id(const xmlNode *xml)
{
    // Default if called without parent (likely for unit testing)
    const char *parent_id = "implied";

    if ((xml != NULL) && (xml->parent != NULL)) {
        parent_id = pcmk__xe_id(xml->parent);
        if (parent_id == NULL) { // Not possible with schema validation enabled
            parent_id = "without ID";
        }
    }
    return parent_id;
}

/*!
 * \internal
 * \brief Get the moon phase corresponding to a given date/time
 *
 * \param[in] now  Date/time to get moon phase for
 *
 * \return Phase of the moon corresponding to \p now, where 0 is the new moon
 *         and 7 is the full moon
 * \deprecated This feature has been deprecated since 2.1.6.
 */
static int
phase_of_the_moon(const crm_time_t *now)
{
    /* As per the nethack rules:
     * - A moon period is 29.53058 days ~= 30
     * - A year is 365.2422 days
     * - Number of days moon phase advances on first day of year compared to
     *   preceding year is (365.2422 - 12 * 29.53058) ~= 11
     * - Number of years until same phases fall on the same days of the month
     *   is 18.6 ~= 19
     * - Moon phase on first day of year (epact) ~= (11 * (year%19) + 29) % 30
     *   (29 as initial condition)
     * - Current phase in days = first day phase + days elapsed in year
     * - 6 moons ~= 177 days ~= 8 reported phases * 22 (+ 11/22 for rounding)
     */
    uint32_t epact, diy, goldn;
    uint32_t y;

    crm_time_get_ordinal(now, &y, &diy);
    goldn = (y % 19) + 1;
    epact = (11 * goldn + 18) % 30;
    if (((epact == 25) && (goldn > 11)) || (epact == 24)) {
        epact++;
    }
    return (((((diy + epact) * 6) + 11) % 177) / 22) & 7;
}

/*!
 * \internal
 * \brief Check an integer value against a range from a date specification
 *
 * \param[in] date_spec  XML of PCMK_XE_DATE_SPEC element to check
 * \param[in] id         XML ID for logging purposes
 * \param[in] attr       Name of XML attribute with range to check against
 * \param[in] value      Value to compare against range
 *
 * \return Standard Pacemaker return code (specifically, pcmk_rc_before_range,
 *         pcmk_rc_after_range, or pcmk_rc_ok to indicate that result is either
 *         within range or undetermined)
 * \note We return pcmk_rc_ok for an undetermined result so we can continue
 *       checking the next range attribute.
 */
static int
check_range(const xmlNode *date_spec, const char *id, const char *attr,
            uint32_t value)
{
    int rc = pcmk_rc_ok;
    const char *range = crm_element_value(date_spec, attr);
    long long low, high;

    if (range == NULL) { // Attribute not present
        goto bail;
    }

    if (pcmk__parse_ll_range(range, &low, &high) != pcmk_rc_ok) {
        // Invalid range
        /* @COMPAT When we can break behavioral backward compatibility, treat
         * the entire rule as not passing.
         */
        pcmk__config_err("Ignoring " PCMK_XE_DATE_SPEC
                         " %s attribute %s because '%s' is not a valid range",
                         id, attr, range);

    } else if ((low != -1) && (value < low)) {
        rc = pcmk_rc_before_range;

    } else if ((high != -1) && (value > high)) {
        rc = pcmk_rc_after_range;
    }

bail:
    crm_trace("Checked " PCMK_XE_DATE_SPEC " %s %s='%s' for %" PRIu32 ": %s",
              id, attr, pcmk__s(range, ""), value, pcmk_rc_str(rc));
    return rc;
}

/*!
 * \internal
 * \brief Evaluate a date specification for a given date/time
 *
 * \param[in] date_spec  XML of PCMK_XE_DATE_SPEC element to evaluate
 * \param[in] now        Time to check
 *
 * \return Standard Pacemaker return code (specifically, EINVAL for NULL
 *         arguments, pcmk_rc_ok if time matches specification, or
 *         pcmk_rc_before_range, pcmk_rc_after_range, or pcmk_rc_op_unsatisfied
 *         as appropriate to how time relates to specification)
 */
int
pcmk__evaluate_date_spec(const xmlNode *date_spec, const crm_time_t *now)
{
    const char *id = NULL;
    const char *parent_id = loggable_parent_id(date_spec);

    // Range attributes that can be specified for a PCMK_XE_DATE_SPEC element
    struct range {
        const char *attr;
        uint32_t value;
    } ranges[] = {
        { PCMK_XA_YEARS, 0U },
        { PCMK_XA_MONTHS, 0U },
        { PCMK_XA_MONTHDAYS, 0U },
        { PCMK_XA_HOURS, 0U },
        { PCMK_XA_MINUTES, 0U },
        { PCMK_XA_SECONDS, 0U },
        { PCMK_XA_YEARDAYS, 0U },
        { PCMK_XA_WEEKYEARS, 0U },
        { PCMK_XA_WEEKS, 0U },
        { PCMK_XA_WEEKDAYS, 0U },
        { PCMK__XA_MOON, 0U },
    };

    if ((date_spec == NULL) || (now == NULL)) {
        return EINVAL;
    }

    // Get specification ID (for logging)
    id = pcmk__xe_id(date_spec);
    if (pcmk__str_empty(id)) { // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * fail the specification
         */
        pcmk__config_warn(PCMK_XE_DATE_SPEC " subelement of "
                          PCMK_XE_DATE_EXPRESSION " %s has no " PCMK_XA_ID,
                          parent_id);
        id = "without ID"; // for logging
    }

    // Year, month, day
    crm_time_get_gregorian(now, &(ranges[0].value), &(ranges[1].value),
                           &(ranges[2].value));

    // Hour, minute, second
    crm_time_get_timeofday(now, &(ranges[3].value), &(ranges[4].value),
                           &(ranges[5].value));

    // Year (redundant) and day of year
    crm_time_get_ordinal(now, &(ranges[0].value), &(ranges[6].value));

    // Week year, week of week year, day of week
    crm_time_get_isoweek(now, &(ranges[7].value), &(ranges[8].value),
                         &(ranges[9].value));

    // Moon phase (deprecated)
    ranges[10].value = phase_of_the_moon(now);
    if (crm_element_value(date_spec, PCMK__XA_MOON) != NULL) {
        pcmk__config_warn("Support for '" PCMK__XA_MOON "' in "
                          PCMK_XE_DATE_SPEC " elements (such as %s) is "
                          "deprecated and will be removed in a future release "
                          "of Pacemaker", id);
    }

    for (int i = 0; i < PCMK__NELEM(ranges); ++i) {
        int rc = check_range(date_spec, id, ranges[i].attr, ranges[i].value);

        if (rc != pcmk_rc_ok) {
            return rc;
        }
    }

    // All specified ranges passed, or none were given (also considered a pass)
    return pcmk_rc_ok;
}

#define ADD_COMPONENT(component) do {                                       \
        int sub_rc = pcmk__add_time_from_xml(*end, component, duration);    \
        if (sub_rc != pcmk_rc_ok) {                                         \
            /* @COMPAT return sub_rc when we can break compatibility */     \
            pcmk__config_warn("Ignoring %s in " PCMK_XE_DURATION " %s "     \
                              "because it is invalid",                      \
                              pcmk__time_component_attr(component), id);    \
            rc = sub_rc;                                                    \
        }                                                                   \
    } while (0)

/*!
 * \internal
 * \brief Given a duration and a start time, calculate the end time
 *
 * \param[in]  duration  XML of PCMK_XE_DURATION element
 * \param[in]  start     Start time
 * \param[out] end       Where to store end time (\p *end must be NULL
 *                       initially)
 *
 * \return Standard Pacemaker return code
 * \note The caller is responsible for freeing \p *end using crm_time_free().
 */
int
pcmk__unpack_duration(const xmlNode *duration, const crm_time_t *start,
                      crm_time_t **end)
{
    int rc = pcmk_rc_ok;
    const char *id = NULL;
    const char *parent_id = loggable_parent_id(duration);

    if ((start == NULL) || (duration == NULL)
        || (end == NULL) || (*end != NULL)) {
        return EINVAL;
    }

    // Get duration ID (for logging)
    id = pcmk__xe_id(duration);
    if (pcmk__str_empty(id)) { // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error instead
         */
        pcmk__config_warn(PCMK_XE_DURATION " subelement of "
                          PCMK_XE_DATE_EXPRESSION " %s has no " PCMK_XA_ID,
                          parent_id);
        id = "without ID";
    }

    *end = pcmk_copy_time(start);

    ADD_COMPONENT(pcmk__time_years);
    ADD_COMPONENT(pcmk__time_months);
    ADD_COMPONENT(pcmk__time_weeks);
    ADD_COMPONENT(pcmk__time_days);
    ADD_COMPONENT(pcmk__time_hours);
    ADD_COMPONENT(pcmk__time_minutes);
    ADD_COMPONENT(pcmk__time_seconds);

    return rc;
}

/*!
 * \internal
 * \brief Evaluate a range check for a given date/time
 *
 * \param[in]     date_expression  XML of PCMK_XE_DATE_EXPRESSION element
 * \param[in]     id               Expression ID for logging purposes
 * \param[in]     now              Date/time to compare
 * \param[in,out] next_change      If not NULL, set this to when the evaluation
 *                                 will change, if known and earlier than the
 *                                 original value
 *
 * \return Standard Pacemaker return code
 */
static int
evaluate_in_range(const xmlNode *date_expression, const char *id,
                  const crm_time_t *now, crm_time_t *next_change)
{
    crm_time_t *start = NULL;
    crm_time_t *end = NULL;

    if (pcmk__xe_get_datetime(date_expression, PCMK_XA_START,
                              &start) != pcmk_rc_ok) {
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Ignoring " PCMK_XA_START " in "
                          PCMK_XE_DATE_EXPRESSION " %s because it is invalid",
                          id);
    }

    if (pcmk__xe_get_datetime(date_expression, PCMK_XA_END,
                              &end) != pcmk_rc_ok) {
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Ignoring " PCMK_XA_END " in "
                          PCMK_XE_DATE_EXPRESSION " %s because it is invalid",
                          id);
    }

    if ((start == NULL) && (end == NULL)) {
        // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION " %s as not "
                          "passing because in_range requires at least one of "
                          PCMK_XA_START " or " PCMK_XA_END, id);
        return pcmk_rc_undetermined;
    }

    if (end == NULL) {
        xmlNode *duration = first_named_child(date_expression,
                                              PCMK_XE_DURATION);

        if (duration != NULL) {
            /* @COMPAT When we can break behavioral backward compatibility,
             * return the result of this if not OK
             */
            pcmk__unpack_duration(duration, start, &end);
        }
    }

    if ((start != NULL) && (crm_time_compare(now, start) < 0)) {
        pcmk__set_time_if_earlier(next_change, start);
        crm_time_free(start);
        crm_time_free(end);
        return pcmk_rc_before_range;
    }

    if (end != NULL) {
        if (crm_time_compare(now, end) > 0) {
            crm_time_free(start);
            crm_time_free(end);
            return pcmk_rc_after_range;
        }

        // Evaluation doesn't change until second after end
        if (next_change != NULL) {
            crm_time_add_seconds(end, 1);
            pcmk__set_time_if_earlier(next_change, end);
        }
    }

    crm_time_free(start);
    crm_time_free(end);
    return pcmk_rc_within_range;
}

/*!
 * \internal
 * \brief Evaluate a greater-than check for a given date/time
 *
 * \param[in]     date_expression  XML of PCMK_XE_DATE_EXPRESSION element
 * \param[in]     id               Expression ID for logging purposes
 * \param[in]     now              Date/time to compare
 * \param[in,out] next_change      If not NULL, set this to when the evaluation
 *                                 will change, if known and earlier than the
 *                                 original value
 *
 * \return Standard Pacemaker return code
 */
static int
evaluate_gt(const xmlNode *date_expression, const char *id,
            const crm_time_t *now, crm_time_t *next_change)
{
    crm_time_t *start = NULL;

    if (pcmk__xe_get_datetime(date_expression, PCMK_XA_START,
                              &start) != pcmk_rc_ok) {
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION " %s as not "
                          "passing because " PCMK_XA_START " is invalid",
                          id);
        return pcmk_rc_undetermined;
    }

    if (start == NULL) { // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION " %s as not "
                          "passing because " PCMK_VALUE_GT " requires "
                          PCMK_XA_START, id);
        return pcmk_rc_undetermined;
    }

    if (crm_time_compare(now, start) > 0) {
        crm_time_free(start);
        return pcmk_rc_within_range;
    }

    // Evaluation doesn't change until second after start time
    crm_time_add_seconds(start, 1);
    pcmk__set_time_if_earlier(next_change, start);
    crm_time_free(start);
    return pcmk_rc_before_range;
}

/*!
 * \internal
 * \brief Evaluate a less-than check for a given date/time
 *
 * \param[in]     date_expression  XML of PCMK_XE_DATE_EXPRESSION element
 * \param[in]     id               Expression ID for logging purposes
 * \param[in]     now              Date/time to compare
 * \param[in,out] next_change      If not NULL, set this to when the evaluation
 *                                 will change, if known and earlier than the
 *                                 original value
 *
 * \return Standard Pacemaker return code
 */
static int
evaluate_lt(const xmlNode *date_expression, const char *id,
            const crm_time_t *now, crm_time_t *next_change)
{
    crm_time_t *end = NULL;

    if (pcmk__xe_get_datetime(date_expression, PCMK_XA_END,
                              &end) != pcmk_rc_ok) {
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION " %s as not "
                          "passing because " PCMK_XA_END " is invalid", id);
        return pcmk_rc_undetermined;
    }

    if (end == NULL) { // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION " %s as not "
                          "passing because " PCMK_VALUE_GT " requires "
                          PCMK_XA_END, id);
        return pcmk_rc_undetermined;
    }

    if (crm_time_compare(now, end) < 0) {
        pcmk__set_time_if_earlier(next_change, end);
        crm_time_free(end);
        return pcmk_rc_within_range;
    }

    crm_time_free(end);
    return pcmk_rc_after_range;
}

/*!
 * \internal
 * \brief Evaluate a rule's date expression for a given date/time
 *
 * \param[in]     date_expression  XML of a PCMK_XE_DATE_EXPRESSION element
 * \param[in]     now              Time to use for evaluation
 * \param[in,out] next_change      If not NULL, set this to when the evaluation
 *                                 will change, if known and earlier than the
 *                                 original value
 *
 * \return Standard Pacemaker return code (unlike most other evaluation
 *         functions, this can return either pcmk_rc_ok or pcmk_rc_within_range
 *         on success)
 */
int
pcmk__evaluate_date_expression(const xmlNode *date_expression,
                               const crm_time_t *now, crm_time_t *next_change)
{
    const char *id = NULL;
    const char *op = NULL;
    int rc = pcmk_rc_undetermined;

    if ((date_expression == NULL) || (now == NULL)) {
        return EINVAL;
    }

    // Get expression ID (for logging)
    id = pcmk__xe_id(date_expression);
    if (pcmk__str_empty(id)) { // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn(PCMK_XE_DATE_EXPRESSION " element has no "
                          PCMK_XA_ID);
        id = "without ID"; // for logging
    }

    op = crm_element_value(date_expression, PCMK_XA_OPERATION);
    if (pcmk__str_eq(op, PCMK_VALUE_IN_RANGE,
                     pcmk__str_null_matches|pcmk__str_casei)) {
        rc = evaluate_in_range(date_expression, id, now, next_change);

    } else if (pcmk__str_eq(op, PCMK_VALUE_DATE_SPEC, pcmk__str_casei)) {
        xmlNode *date_spec = first_named_child(date_expression,
                                               PCMK_XE_DATE_SPEC);

        if (date_spec == NULL) { // Not possible with schema validation enabled
            /* @COMPAT When we can break behavioral backward compatibility,
             * return pcmk_rc_unpack_error
             */
            pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION " %s "
                              "as not passing because " PCMK_VALUE_DATE_SPEC
                              " operations require a " PCMK_XE_DATE_SPEC
                              " subelement", id);
        } else {
            // @TODO set next_change appropriately
            rc = pcmk__evaluate_date_spec(date_spec, now);
        }

    } else if (pcmk__str_eq(op, PCMK_VALUE_GT, pcmk__str_casei)) {
        rc = evaluate_gt(date_expression, id, now, next_change);

    } else if (pcmk__str_eq(op, PCMK_VALUE_LT, pcmk__str_casei)) {
        rc = evaluate_lt(date_expression, id, now, next_change);

    } else { // Not possible with schema validation enabled
        /* @COMPAT When we can break behavioral backward compatibility,
         * return pcmk_rc_unpack_error
         */
        pcmk__config_warn("Treating " PCMK_XE_DATE_EXPRESSION
                          " %s as not passing because '%s' is not a valid "
                          PCMK_XE_OPERATION, op);
    }

    crm_trace(PCMK_XE_DATE_EXPRESSION " %s (%s): %s (%d)",
              id, op, pcmk_rc_str(rc), rc);
    return rc;
}

/*!
 * \internal
 * \brief Expand any regular expression submatches (%0-%9) in a string
 *
 * \param[in] string      String possibly containing submatch variables
 * \param[in] match       String that matched the regular expression
 * \param[in] submatches  Regular expression submatches (as set by regexec())
 * \param[in] nmatches    Number of entries in \p submatches[]
 *
 * \return Newly allocated string identical to \p string with submatches
 *         expanded, or NULL if there were no matches
 */
char *
pcmk__replace_submatches(const char *string, const char *match,
                         const regmatch_t submatches[], int nmatches)
{
    size_t len = 0;
    int i;
    const char *p, *last_match_index;
    char *p_dst, *result = NULL;

    if (pcmk__str_empty(string)) {
        return NULL;
    }

    p = last_match_index = string;

    while (*p) {
        if (*p == '%' && *(p + 1) && isdigit(*(p + 1))) {
            i = *(p + 1) - '0';
            if ((nmatches >= i) && (submatches[i].rm_so != -1)
                && (submatches[i].rm_eo > submatches[i].rm_so)) {
                len += p - last_match_index
                       + (submatches[i].rm_eo - submatches[i].rm_so);
                last_match_index = p + 2;
            }
            p++;
        }
        p++;
    }
    len += p - last_match_index + 1;

    /* FIXME: Excessive? */
    if (len - 1 <= 0) {
        return NULL;
    }

    p_dst = result = calloc(1, len);
    p = string;

    while (*p) {
        if (*p == '%' && *(p + 1) && isdigit(*(p + 1))) {
            i = *(p + 1) - '0';
            if ((nmatches >= i) && (submatches[i].rm_so != -1)
                && (submatches[i].rm_eo > submatches[i].rm_so)) {
                // rm_eo can be equal to rm_so, but then there is nothing to do
                int match_len = submatches[i].rm_eo - submatches[i].rm_so;

                memcpy(p_dst, match + submatches[i].rm_so, match_len);
                p_dst += match_len;
            }
            p++;
        } else {
            *(p_dst) = *(p);
            p_dst++;
        }
        p++;
    }

    return result;
}
