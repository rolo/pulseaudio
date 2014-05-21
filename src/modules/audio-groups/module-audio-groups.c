/***
  This file is part of PulseAudio.

  Copyright 2014 Intel Corporation

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/dynarray.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/conf-parser.h>
#include <pulsecore/hashmap.h>

#include <modules/volume-api/sstream.h>
#include <modules/volume-api/volume-control.h>
#include <modules/volume-api/audio-group.h>

#include "module-audio-groups-symdef.h"

PA_MODULE_AUTHOR("Ismo Puustinen");
PA_MODULE_DESCRIPTION("Create audio groups and classify streams to them");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

#ifndef AUDIO_GROUP_CONFIG
#define AUDIO_GROUP_CONFIG "audio-groups.conf"
#endif

enum match_direction {
    match_direction_unknown = 0,
    match_direction_input,
    match_direction_output,
};

/* logical expressions */

struct literal {

    /* TODO: this might be parsed to some faster-to-check format? */

    char *property_name;
    char *property_value;
    enum match_direction stream_direction;

    bool negation;
    PA_LLIST_FIELDS(struct literal);
};

struct conjunction {
    /* a conjunction of literals */
    PA_LLIST_HEAD(struct literal, literals);
    PA_LLIST_FIELDS(struct conjunction);
};

struct expression {
    /* this is in disjunctive normal form, so a disjunction of conjunctions */
    PA_LLIST_HEAD(struct conjunction, conjunctions);
};

/* data gathered from settings */

enum control_action {
    CONTROL_ACTION_NONE,
    CONTROL_ACTION_CREATE,
    CONTROL_ACTION_BIND,
};

struct audio_group {
    struct userdata *userdata;
    char *id;
    char *description;
    enum control_action volume_control_action;
    enum control_action mute_control_action;
    pa_binding_target_info *volume_control_target_info;
    pa_binding_target_info *mute_control_target_info;

    /* official audio group */
    pa_audio_group *group;

    struct audio_group_control *volume_control;
    struct audio_group_control *mute_control;

    bool unlinked;
};

struct stream {
    struct userdata *userdata;
    char *id;
    enum match_direction direction;
    char *audio_group_name_for_volume;
    char *audio_group_name_for_mute;
    pa_audio_group *audio_group_for_volume;
    pa_audio_group *audio_group_for_mute;
    pa_binding_target_info *volume_control_target_info;
    pa_binding_target_info *mute_control_target_info;
    struct expression *rule;

    bool unlinked;
};

struct userdata {
    pa_hashmap *audio_groups; /* name -> struct audio_group */
    pa_dynarray *streams; /* struct stream */
    pa_hook_slot *new_stream_volume;
    pa_hook_slot *new_stream_mute;

    pa_volume_api *api;

    /* The following fields are only used during initialization. */
    pa_hashmap *audio_group_names; /* name -> name (hashmap-as-a-set) */
    pa_hashmap *unused_audio_groups; /* name -> struct audio_group */
    pa_dynarray *stream_names;
    pa_hashmap *unused_streams; /* name -> struct stream */
};

static const char* const valid_modargs[] = {
    "filename",
    NULL
};

static void audio_group_unlink(struct audio_group *group);

static void print_literal(struct literal *l);
static void print_conjunction(struct conjunction *c);
static void print_expression(struct expression *e);
static void delete_expression(struct expression *e);

static struct audio_group *audio_group_new(struct userdata *u, const char *name) {
    struct audio_group *group;

    pa_assert(u);
    pa_assert(name);

    group = pa_xnew0(struct audio_group, 1);
    group->userdata = u;
    group->id = pa_xstrdup(name);
    group->description = pa_xstrdup(name);
    group->volume_control_action = CONTROL_ACTION_NONE;
    group->mute_control_action = CONTROL_ACTION_NONE;

    return group;
}

static int audio_group_put(struct audio_group *group) {
    int r;

    pa_assert(group);

    r = pa_audio_group_new(group->userdata->api, group->id, group->description, &group->group);
    if (r < 0)
        goto fail;

    switch (group->volume_control_action) {
        case CONTROL_ACTION_NONE:
            break;

        case CONTROL_ACTION_CREATE:
            pa_audio_group_set_have_own_volume_control(group->group, true);
            pa_audio_group_set_volume_control(group->group, group->group->own_volume_control);
            break;

        case CONTROL_ACTION_BIND:
            pa_audio_group_bind_volume_control(group->group, group->volume_control_target_info);
            break;
    }

    switch (group->mute_control_action) {
        case CONTROL_ACTION_NONE:
            break;

        case CONTROL_ACTION_CREATE:
            pa_audio_group_set_have_own_mute_control(group->group, true);
            pa_audio_group_set_mute_control(group->group, group->group->own_mute_control);
            break;

        case CONTROL_ACTION_BIND:
            pa_audio_group_bind_mute_control(group->group, group->mute_control_target_info);
            break;
    }

    pa_audio_group_put(group->group);

    return 0;

fail:
    audio_group_unlink(group);

    return r;
}

static void audio_group_unlink(struct audio_group *group) {
    pa_assert(group);

    if (group->unlinked)
        return;

    group->unlinked = true;

    if (group->group) {
        pa_audio_group_free(group->group);
        group->group = NULL;
    }
}

static void audio_group_free(struct audio_group *group) {
    pa_assert(group);

    if (!group->unlinked)
        audio_group_unlink(group);

    if (group->mute_control_target_info)
        pa_binding_target_info_free(group->mute_control_target_info);

    if (group->volume_control_target_info)
        pa_binding_target_info_free(group->volume_control_target_info);

    pa_xfree(group->description);
    pa_xfree(group->id);
    pa_xfree(group);
}

static void audio_group_set_description(struct audio_group *group, const char *description) {
    pa_assert(group);
    pa_assert(description);

    pa_xfree(group->description);
    group->description = pa_xstrdup(description);
}

static void audio_group_set_volume_control_action(struct audio_group *group, enum control_action action,
                                                  pa_binding_target_info *target_info) {
    pa_assert(group);
    pa_assert((action == CONTROL_ACTION_BIND) ^ !target_info);

    group->volume_control_action = action;

    if (group->volume_control_target_info)
        pa_binding_target_info_free(group->volume_control_target_info);

    if (action == CONTROL_ACTION_BIND)
        group->volume_control_target_info = pa_binding_target_info_copy(target_info);
    else
        group->volume_control_target_info = NULL;
}

static void audio_group_set_mute_control_action(struct audio_group *group, enum control_action action,
                                                pa_binding_target_info *target_info) {
    pa_assert(group);
    pa_assert((action == CONTROL_ACTION_BIND) ^ !target_info);

    group->mute_control_action = action;

    if (group->mute_control_target_info)
        pa_binding_target_info_free(group->mute_control_target_info);

    if (action == CONTROL_ACTION_BIND)
        group->mute_control_target_info = pa_binding_target_info_copy(target_info);
    else
        group->mute_control_target_info = NULL;
}

static struct stream *stream_new(struct userdata *u, const char *name) {
    struct stream *stream;

    pa_assert(u);
    pa_assert(name);

    stream = pa_xnew0(struct stream, 1);
    stream->userdata = u;
    stream->id = pa_xstrdup(name);
    stream->direction = match_direction_unknown;

    return stream;
}

static void stream_put(struct stream *stream) {
    pa_assert(stream);

    if (stream->audio_group_name_for_volume) {
        stream->audio_group_for_volume = pa_hashmap_get(stream->userdata->audio_groups, stream->audio_group_name_for_volume);
        if (stream->audio_group_for_volume)
            stream->volume_control_target_info =
                    pa_binding_target_info_new(PA_AUDIO_GROUP_BINDING_TARGET_TYPE, stream->audio_group_for_volume->name,
                                               PA_AUDIO_GROUP_BINDING_TARGET_FIELD_VOLUME_CONTROL);
        else
            pa_log("Stream %s refers to undefined audio group %s.", stream->id, stream->audio_group_name_for_volume);
    }

    if (stream->audio_group_name_for_mute) {
        stream->audio_group_for_mute = pa_hashmap_get(stream->userdata->audio_groups, stream->audio_group_name_for_mute);
        if (stream->audio_group_for_mute)
            stream->mute_control_target_info =
                    pa_binding_target_info_new(PA_AUDIO_GROUP_BINDING_TARGET_TYPE, stream->audio_group_for_mute->name,
                                               PA_AUDIO_GROUP_BINDING_TARGET_FIELD_MUTE_CONTROL);
        else
            pa_log("Stream %s refers to undefined audio group %s.", stream->id, stream->audio_group_name_for_volume);
    }
}

static void stream_unlink(struct stream *stream) {
    pa_assert(stream);

    if (stream->unlinked)
        return;

    if (stream->mute_control_target_info) {
        pa_binding_target_info_free(stream->mute_control_target_info);
        stream->mute_control_target_info = NULL;
    }

    if (stream->volume_control_target_info) {
        pa_binding_target_info_free(stream->volume_control_target_info);
        stream->volume_control_target_info = NULL;
    }

    stream->unlinked = true;
}

static void stream_free(struct stream *stream) {
    pa_assert(stream);

    if (!stream->unlinked)
        stream_unlink(stream);

    if (stream->rule)
        delete_expression(stream->rule);

    pa_xfree(stream->audio_group_name_for_mute);
    pa_xfree(stream->audio_group_name_for_volume);
    pa_xfree(stream->id);
    pa_xfree(stream);
}

static void stream_set_audio_group_name_for_volume(struct stream *stream, const char *name) {
    pa_assert(stream);

    pa_xfree(stream->audio_group_name_for_volume);
    stream->audio_group_name_for_volume = pa_xstrdup(name);
}

static void stream_set_audio_group_name_for_mute(struct stream *stream, const char *name) {
    pa_assert(stream);

    pa_xfree(stream->audio_group_name_for_mute);
    stream->audio_group_name_for_mute = pa_xstrdup(name);
}

/* stream classification */

static bool match_predicate(struct literal *l, pas_stream *d) {

    if (l->stream_direction != match_direction_unknown) {
        /* check the stream direction; _sink inputs_ are always _outputs_ */

        if ((d->direction == PA_DIRECTION_OUTPUT && l->stream_direction == match_direction_output) ||
            ((d->direction == PA_DIRECTION_INPUT && l->stream_direction == match_direction_input))) {
            return true;
        }
    }
    else if (l->property_name && l->property_value) {
        /* check the property from the property list */

        if (pa_proplist_contains(d->proplist, l->property_name) &&
                strcmp(pa_proplist_gets(d->proplist, l->property_name), l->property_value) == 0)
            return true;
    }

    /* no match */
    return false;
}

static bool match_rule(struct expression *e, pas_stream *d) {

    struct conjunction *c;

    PA_LLIST_FOREACH(c, e->conjunctions) {
        struct literal *l;
        bool and_success = true;
        PA_LLIST_FOREACH(l, c->literals) {
            if (!match_predicate(l, d)) {
                /* at least one fail for conjunction */
                and_success = false;
                break;
            }
        }

        if (and_success) {
            /* at least one match for disjunction */
            return true;
        }
    }

    /* no matches */
    return false;
}

static void classify_stream(struct userdata *u, pas_stream *new_data, bool mute) {
    /* do the classification here */

    struct stream *stream = NULL;
    unsigned idx;

    /* go through the stream match definitions in given order */

    PA_DYNARRAY_FOREACH(stream, u->streams, idx) {
        if (stream->rule && match_rule(stream->rule, new_data)) {
            pa_log_info("stream %s (%s) match with rule %s:", new_data->name, new_data->description, stream->id);
            print_expression(stream->rule);

            if (mute) {
                if (new_data->use_default_mute_control && stream->audio_group_for_mute)
                    pas_stream_bind_mute_control(new_data, stream->mute_control_target_info);
            } else {
                if (new_data->use_default_volume_control && stream->audio_group_for_volume)
                    pas_stream_bind_volume_control(new_data, stream->volume_control_target_info);
            }

            return;
        }
    }

    /* no matches, don't touch the volumes */
}

static pa_hook_result_t set_volume_control_cb(
        void *hook_data,
        pas_stream *new_data,
        struct userdata *u) {

    pa_assert(new_data);
    pa_assert(u);

    classify_stream(u, new_data, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t set_mute_control_cb(
        void *hook_data,
        pas_stream *new_data,
        struct userdata *u) {

    pa_assert(new_data);
    pa_assert(u);

    classify_stream(u, new_data, true);

    return PA_HOOK_OK;
}

/* parser for configuration file */

/*
    Parse the match expression. The syntax is this:

    OPER           := "AND" | "OR"
    OPEN_BRACE     := "("
    CLOSE_BRACE    := ")"
    EXPR           := OPEN_BRACE EXPR OPER EXPR CLOSE_BRACE | VAR
    VAR            := LIT | "NEG" LIT
    LIT            := PREDICATE (defined by rule semantics)

    In addition there is a requirement that the expressions need to be in
    disjunctive normal form. It means that if there is an expression that
    has AND operator, there may not be any OR operators in its subexpressions.

    Example expressions:

    (foo)
    (foo AND bar)
    (foo OR (bar AND xxx))
    (NEG foo OR (bar AND NEG xxx))

    The predicate here is the single rule that is matched against the new sink
    input. The syntax is this:

    PREDICATE      := "direction" DIRECTION  | "property" PROPERTY
    DIRECTION      := "input" | "output"
    PROPERTY       := PROPERTY_NAME "=" PROPERTY_VALUE
    PROPERTY_NAME  := STRING
    PROPERTY_VALUE := STRING

    The allowed characters for STRING are standard ascii characters. Not
    allowed substrings are the reserved words "AND", "OR", "(", ")", "NEG" and
    "=".

    Complete examples:

    (property application.process.binary=paplay)

    (property media.role=music AND direction input)

    (property application.process.binary=paplay OR (direction input OR direction output))
*/

static void print_literal(struct literal *l) {
    if (l->stream_direction != match_direction_unknown) {
        pa_log_info("       %sstream direction %s",
                l->negation ? "NEG " : "",
                l->stream_direction == match_direction_input ? "input" : "output");
    }
    else {
        pa_log_info("       %sproperty %s == %s",
                l->negation ? "NEG " : "",
                l->property_name ? l->property_name : "NULL",
                l->property_value ? l->property_value : "NULL");
    }
}

static void print_conjunction(struct conjunction *c) {
    struct literal *l;
    pa_log_info("   conjunction for literals:");
    PA_LLIST_FOREACH(l, c->literals) {
        print_literal(l);
    }
}

static void print_expression(struct expression *e) {
    struct conjunction *c;
    pa_log_info("disjunction for conjunctions:");
    PA_LLIST_FOREACH(c, e->conjunctions) {
        print_conjunction(c);
    }
}

static void delete_literal(struct literal *l) {

    if (!l)
        return;

    pa_xfree(l->property_name);
    pa_xfree(l->property_value);
    pa_xfree(l);
}

static void delete_conjunction(struct conjunction *c) {
    struct literal *l;

    if (!c)
        return;

    PA_LLIST_FOREACH(l, c->literals) {
        delete_literal(l);
    }

    pa_xfree(c);
}

static void delete_expression(struct expression *e) {
    struct conjunction *c;

    PA_LLIST_FOREACH(c, e->conjunctions) {
        delete_conjunction(c);
    }

    pa_xfree(e);
}

enum logic_operator {
    operator_not_set = 0,
    operator_and,
    operator_or,
};

struct expression_token {
    struct expression_token *left;
    struct expression_token *right;

    enum logic_operator oper;

    struct literal_token *lit;
};

struct literal_token {
    bool negation;
    char *var;
};

static void delete_literal_token(struct literal_token *l) {

    if (!l)
        return;

    pa_xfree(l->var);
    pa_xfree(l);
}

static void delete_expression_token(struct expression_token *e) {

    if (!e)
        return;

    delete_expression_token(e->left);
    delete_expression_token(e->right);
    delete_literal_token(e->lit);

    e->left = NULL;
    e->right = NULL;
    e->lit = NULL;

    pa_xfree(e);
}

static struct expression_token *parse_rule_internal(const char *rule, bool disjunction_allowed) {

    int len = strlen(rule);
    struct expression_token *et;
    char *p;
    int brace_count = 0;
    bool braces_present = false;
    char left_buf[len];
    char right_buf[len];

#if 0
    /* check if the rule is still valid */

    if (len < 2)
        return NULL;

    if (rule[0] != '(' || rule[len-1] != ')')
        return NULL;
#endif

    et = pa_xnew0(struct expression_token, 1);

    if (!et)
        return NULL;

    /* count the braces -- we want to find the case when there is only one brace open */

    p = (char *) rule;

    while (*p) {
        if (*p == '(') {
            braces_present = true;
            brace_count++;
        }
        else if (*p == ')') {
            brace_count--;
        }

        if (brace_count == 1) {

            /* the parser is recursive and just goes down the tree on the
             * topmost level (where the brace count is 1). If there are no
             * braces this is a literal */

            /* find the operator AND or OR */

            if (strncmp(p, "AND", 3) == 0) {

                /* copy parts */
                char *begin_left = (char *) rule+1;
                char *begin_right = p+3;

                int left_len = p - rule - 1; /* minus '(' */
                int right_len = len - 3 - left_len - 2; /* minus AND and '(' and ')'*/

                memcpy(left_buf, begin_left, left_len);
                left_buf[left_len] = '\0';
                memcpy(right_buf, begin_right, right_len);
                right_buf[right_len] = '\0';

                et->left = parse_rule_internal(left_buf, false);
                et->right = parse_rule_internal(right_buf, false);
                et->oper = operator_and;

                if (!et->left || !et->right) {
                    delete_expression_token(et);
                    return NULL;
                }

                return et;
            }
            else if (strncmp(p, "OR", 2) == 0) {

                char *begin_left = (char *) rule+1;
                char *begin_right = p+2;

                int left_len = p - rule - 1; /* minus '(' */
                int right_len = len - 2 - left_len - 2; /* minus OR and '(' and ')'*/

                if (!disjunction_allowed) {
                    pa_log_error("logic expression not in dnf");
                    delete_expression_token(et);
                    return NULL;
                }

                memcpy(left_buf, begin_left, left_len);
                left_buf[left_len] = '\0';
                memcpy(right_buf, begin_right, right_len);
                right_buf[right_len] = '\0';

                et->left = parse_rule_internal(left_buf, true);
                et->right = parse_rule_internal(right_buf, true);
                et->oper = operator_or;

                if (!et->left || !et->right) {
                    delete_expression_token(et);
                    return NULL;
                }

                return et;
            }
            /* else a literal which is inside braces */
        }

        p++;
    }

    if (brace_count != 0) {
        /* the input is not valid */
        pa_log_error("mismatched braces in logic expression");
        delete_expression_token(et);
        return NULL;
    }
    else {
        /* this is a literal */
        char *begin_lit;
        char buf[strlen(rule)+1];

        struct literal_token *lit = pa_xnew0(struct literal_token, 1);
        if (!lit) {
            delete_expression_token(et);
            return NULL;
        }

        if (braces_present) {
            /* remove all braces */
            char *k;
            char *l;

            k = (char *) rule;
            l = buf;

            while (*k) {
                if (*k == '(' || *k == ')') {
                    k++;
                    continue;
                }

                *l = *k;
                l++;
                k++;
            }
            *l = '\0';
        }
        else {
            strncpy(buf, rule, sizeof(buf));
        }

        if (strncmp(buf, "NEG", 3) == 0) {
            begin_lit = (char *) buf + 3;
            lit->negation = true;
        }
        else {
            begin_lit = (char *) buf;
            lit->negation = false;
        }

        lit->var = pa_xstrdup(begin_lit);
        et->lit = lit;
    }

    return et;
}

static bool gather_literal(struct expression_token *et, struct literal *l) {
#define PROPERTY_KEYWORD "property"
#define DIRECTION_KEYWORD "direction"
#define DIRECTION_VALUE_INPUT "input"
#define DIRECTION_VALUE_OUTPUT "output"

    char *p = et->lit->var;
    int len = strlen(et->lit->var);

    l->negation = et->lit->negation;

    if (strncmp(p, PROPERTY_KEYWORD, strlen(PROPERTY_KEYWORD)) == 0) {
        char name[len];
        char value[len];
        int i = 0;

        p += strlen(PROPERTY_KEYWORD);

        /* parse the property pair: name=value */

        while (*p && *p != '=') {
            name[i++] = *p;
            p++;
        }

        /* check if we really found '=' */

        if (*p != '=') {
            pa_log_error("property syntax broken for '%s'", et->lit->var);
            goto error;
        }

        name[i] = '\0';

        p++;
        i = 0;

        while (*p) {
            value[i++] = *p;
            p++;
        }

        value[i] = '\0';

        l->property_name = pa_xstrdup(name);
        l->property_value = pa_xstrdup(value);
    }
    else if (strncmp(p, DIRECTION_KEYWORD, strlen(DIRECTION_KEYWORD)) == 0) {
        p += strlen(DIRECTION_KEYWORD);

        if (strncmp(p, DIRECTION_VALUE_INPUT, strlen(DIRECTION_VALUE_INPUT)) == 0) {
            l->stream_direction = match_direction_input;
        }
        else if (strncmp(p, DIRECTION_VALUE_OUTPUT, strlen(DIRECTION_VALUE_OUTPUT)) == 0) {
            l->stream_direction = match_direction_output;
        }
        else {
            pa_log_error("unknown direction(%s): %s", et->lit->var, p);
            goto error;
        }
    }
    else {
        pa_log_error("not able to parse the value: '%s'", et->lit->var);
        goto error;
    }

    return true;

error:
    return false;

#undef DIRECTION_VALUE_OUTPUT
#undef DIRECTION_VALUE_INPUT
#undef DIRECTION_KEYWORD
#undef PROPERTY_KEYWORD
}

static bool gather_conjunction(struct expression_token *et, struct conjunction *c) {

    if (et->oper == operator_and) {
        if (!gather_conjunction(et->left, c) ||
            !gather_conjunction(et->right, c))
            return false;
    }
    else {
        /* literal */
        struct literal *l = pa_xnew0(struct literal, 1);

        if (!l)
            return false;

        gather_literal(et, l);

        PA_LLIST_PREPEND(struct literal, c->literals, l);
    }

    return true;
}

static bool gather_expression(struct expression *e, struct expression_token *et) {

    if (et->oper == operator_or) {
        if (!gather_expression(e, et->right) ||
            !gather_expression(e, et->left))
            return false;
    }
    else {
        /* conjunction or literal */
        struct conjunction *c = pa_xnew0(struct conjunction, 1);
        if (!gather_conjunction(et, c))
            return false;

        PA_LLIST_PREPEND(struct conjunction, e->conjunctions, c);
    }

    return true;
}

static struct expression *parse_rule(const char *rule_string) {
    char *k, *l;
    struct expression *e = NULL;
    int len;
    char *buf = NULL;
    struct expression_token *et = NULL;

    if (!rule_string)
        goto error;

    len = strlen(rule_string);

    buf = (char *) pa_xmalloc0(len);

    if (!buf)
        goto error;

    /* remove whitespace */

    k = (char *) rule_string;
    l = buf;

    while (*k) {
        if (*k == ' ') {
            k++;
            continue;
        }

        *l = *k;
        l++;
        k++;
    }

    /* et is the root of an expression tree */
    et = parse_rule_internal(buf, true);

    if (!et)
        goto error;

    e = pa_xnew0(struct expression, 1);

    if (!e)
        goto error;

    /* gather expressions to actual match format */
    gather_expression(e, et);

#if 1
    print_expression(e);
#endif

    /* free memory */
    delete_expression_token(et);
    pa_xfree(buf);

    return e;

error:
    delete_expression_token(et);
    pa_xfree(buf);
    pa_xfree(e);
    return NULL;
}

static int parse_audio_groups(pa_config_parser_state *state) {
    struct userdata *u;
    char *name;
    const char *split_state = NULL;

    pa_assert(state);

    u = state->userdata;

    pa_hashmap_remove_all(u->audio_group_names);

    while ((name = pa_split_spaces(state->rvalue, &split_state)))
        pa_hashmap_put(u->audio_group_names, name, name);

    return 0;
}

static int parse_streams(pa_config_parser_state *state) {
    struct userdata *u;
    char *name;
    const char *split_state = NULL;

    pa_assert(state);

    u = state->userdata;

    pa_dynarray_remove_all(u->stream_names);

    while ((name = pa_split_spaces(state->rvalue, &split_state))) {
        const char *name2;
        unsigned idx;
        bool duplicate = false;

        /* Avoid adding duplicates in u->stream_names. */
        PA_DYNARRAY_FOREACH(name2, u->stream_names, idx) {
            if (pa_streq(name, name2)) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            pa_xfree(name);
            continue;
        }

        pa_dynarray_append(u->stream_names, name);
    }

    return 0;
}

static int parse_common(pa_config_parser_state *state) {
#define AUDIOGROUP_START "AudioGroup "
#define STREAM_START "Stream "
#define BIND_KEYWORD "bind:"
#define NONE_KEYWORD "none"

    char *section;
    struct userdata *u = (struct userdata *) state->userdata;
    int r;
    pa_binding_target_info *target_info;

    pa_assert(state);

    section = state->section;
    if (!section)
        goto error;

    if (strncmp(section, AUDIOGROUP_START, strlen(AUDIOGROUP_START)) == 0) {
        char *ag_name = section + strlen(AUDIOGROUP_START);
        struct audio_group *ag = (struct audio_group *) pa_hashmap_get(u->unused_audio_groups, ag_name);

        if (!ag) {
            /* first item for this audio group section, so create the struct */
            ag = audio_group_new(u, ag_name);
            pa_hashmap_put(u->unused_audio_groups, ag->id, ag);
        }

        if (strcmp(state->lvalue, "description") == 0)
            audio_group_set_description(ag, state->rvalue);

        else if (strcmp(state->lvalue, "volume-control") == 0) {
            if (pa_streq(state->rvalue, "create"))
                audio_group_set_volume_control_action(ag, CONTROL_ACTION_CREATE, NULL);

            else if (pa_streq(state->rvalue, NONE_KEYWORD))
                audio_group_set_volume_control_action(ag, CONTROL_ACTION_NONE, NULL);

            else if (pa_startswith(state->rvalue, BIND_KEYWORD)) {
                r = pa_binding_target_info_new_from_string(state->rvalue, "volume_control", &target_info);
                if (r < 0) {
                    pa_log("[%s:%u] Failed to parse binding target \"%s\".", state->filename, state->lineno, state->rvalue);
                    goto error;
                }

                audio_group_set_volume_control_action(ag, CONTROL_ACTION_BIND, target_info);
                pa_binding_target_info_free(target_info);

            } else {
                pa_log("[%s:%u] Failed to parse value \"%s\".", state->filename, state->lineno, state->rvalue);
                goto error;
            }
        }
        else if (strcmp(state->lvalue, "mute-control") == 0) {
            if (pa_streq(state->rvalue, "create"))
                audio_group_set_mute_control_action(ag, CONTROL_ACTION_CREATE, NULL);

            else if (pa_streq(state->rvalue, NONE_KEYWORD))
                audio_group_set_mute_control_action(ag, CONTROL_ACTION_NONE, NULL);

            else if (pa_startswith(state->rvalue, BIND_KEYWORD)) {
                r = pa_binding_target_info_new_from_string(state->rvalue, "mute_control", &target_info);
                if (r < 0) {
                    pa_log("[%s:%u] Failed to parse binding target \"%s\".", state->filename, state->lineno, state->rvalue);
                    goto error;
                }

                audio_group_set_mute_control_action(ag, CONTROL_ACTION_BIND, target_info);
                pa_binding_target_info_free(target_info);

            } else {
                pa_log("[%s:%u] Failed to parse value \"%s\".", state->filename, state->lineno, state->rvalue);
                goto error;
            }
        }
    }
    else if (strncmp(section, STREAM_START, strlen(STREAM_START)) == 0) {
        char *stream_name = section + strlen(STREAM_START);

        struct stream *stream = (struct stream *) pa_hashmap_get(u->unused_streams, stream_name);

        if (!stream) {
            /* first item for this stream section, so create the struct */
            stream = stream_new(u, stream_name);
            pa_hashmap_put(u->unused_streams, stream->id, stream);
        }

        if (pa_streq(state->lvalue, "audio-group-for-volume"))
            stream_set_audio_group_name_for_volume(stream, *state->rvalue ? state->rvalue : NULL);

        else if (pa_streq(state->lvalue, "audio-group-for-mute"))
            stream_set_audio_group_name_for_mute(stream, *state->rvalue ? state->rvalue : NULL);

        else if (strcmp(state->lvalue, "match") == 0) {
            if (!state->rvalue)
                goto error;

            stream->rule = parse_rule(state->rvalue);

            if (!stream->rule) {
                goto error;
            }
        }
    }

    return 0;

error:

    pa_log_error("failed parsing audio group definition file");
    return -1;

#undef NONE_KEYWORD
#undef AUDIO_GROUP_KEYWORD
#undef BIND_KEYWORD
#undef STREAM_START
#undef AUDIOGROUP_START
}

static void finalize_config(struct userdata *u) {
    const char *group_name;
    void *state;
    struct audio_group *group;
    const char *stream_name;
    unsigned idx;
    struct stream *stream;

    pa_assert(u);

    PA_HASHMAP_FOREACH(group_name, u->audio_group_names, state) {
        int r;

        group = pa_hashmap_remove(u->unused_audio_groups, group_name);
        if (!group)
            group = audio_group_new(u, group_name);

        r = audio_group_put(group);
        if (r < 0) {
            pa_log("Failed to create audio group %s.", group_name);
            audio_group_free(group);
            continue;
        }

        pa_assert_se(pa_hashmap_put(u->audio_groups, group->id, group) >= 0);
    }

    PA_HASHMAP_FOREACH(group, u->unused_audio_groups, state)
        pa_log_debug("Audio group %s is not used.", group->id);

    pa_hashmap_free(u->unused_audio_groups);
    u->unused_audio_groups = NULL;

    pa_hashmap_free(u->audio_group_names);
    u->audio_group_names = NULL;

    PA_DYNARRAY_FOREACH(stream_name, u->stream_names, idx) {
        stream = pa_hashmap_remove(u->unused_streams, stream_name);
        if (!stream) {
            pa_log("Reference to undefined stream %s, ignoring.", stream_name);
            continue;
        }

        stream_put(stream);
        pa_dynarray_append(u->streams, stream);
    }

    PA_HASHMAP_FOREACH(stream, u->unused_streams, state)
        pa_log_debug("Stream %s is not used.", stream->id);

    pa_hashmap_free(u->unused_streams);
    u->unused_streams = NULL;

    pa_dynarray_free(u->stream_names);
    u->stream_names = NULL;
}

static bool parse_configuration(struct userdata *u, const char *filename) {
    FILE *f;
    char *fn = NULL;

    pa_config_item table[] = {
        { "audio-groups", parse_audio_groups, NULL, "General" },
        { "streams", parse_streams, NULL, "General" },
        { NULL, parse_common, NULL, NULL },
        { NULL, NULL, NULL, NULL },
    };

    u->audio_group_names = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, pa_xfree);
    u->unused_audio_groups = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL,
                                                 (pa_free_cb_t) audio_group_free);
    u->stream_names = pa_dynarray_new(pa_xfree);
    u->unused_streams = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL,
                                            (pa_free_cb_t) stream_free);

    if (pa_is_path_absolute(filename))
        f = pa_open_config_file(filename, NULL, NULL, &fn);
    else {
        char *sys_conf_file;

        sys_conf_file = pa_sprintf_malloc(PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "%s", filename);
        f = pa_open_config_file(sys_conf_file, filename, NULL, &fn);
        pa_xfree(sys_conf_file);
    }

    if (f) {
        pa_config_parse(fn, f, table, NULL, u);
        pa_xfree(fn);
        fn = NULL;
        fclose(f);
        f = NULL;
    }

    finalize_config(u);

    return true;
}

void pa__done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    u = (struct userdata *) m->userdata;

    if (!u)
        return;

    if (u->new_stream_volume)
        pa_hook_slot_free(u->new_stream_volume);

    if (u->new_stream_mute)
        pa_hook_slot_free(u->new_stream_mute);

    if (u->streams)
        pa_dynarray_free(u->streams);

    if (u->audio_groups)
        pa_hashmap_free(u->audio_groups);

    if (u->api)
        pa_volume_api_unref(u->api);

    pa_xfree(u);
}

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    const char *filename;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto error;
    }

    u = m->userdata = pa_xnew0(struct userdata, 1);

    if (!u)
        goto error;

    u->api = pa_volume_api_get(m->core);

    if (!u->api)
        goto error;

    u->audio_groups = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL,
                                          (pa_free_cb_t) audio_group_free);
    u->streams = pa_dynarray_new((pa_free_cb_t) stream_free);

    filename = pa_modargs_get_value(ma, "filename", AUDIO_GROUP_CONFIG);

    if (!parse_configuration(u, filename))
        goto error;

    u->new_stream_volume = pa_hook_connect(&u->api->hooks[PA_VOLUME_API_HOOK_STREAM_SET_INITIAL_VOLUME_CONTROL], PA_HOOK_EARLY, (pa_hook_cb_t) set_volume_control_cb, u);
    u->new_stream_mute = pa_hook_connect(&u->api->hooks[PA_VOLUME_API_HOOK_STREAM_SET_INITIAL_MUTE_CONTROL], PA_HOOK_EARLY, (pa_hook_cb_t) set_mute_control_cb, u);

    if (!u->new_stream_volume || !u->new_stream_mute)
        goto error;

    pa_modargs_free(ma);

    return 0;

error:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}