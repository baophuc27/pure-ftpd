#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simpleconf.h"

#define MAX_ARG_LENGTH 65536

typedef enum State_ {
    STATE_UNDEFINED,
    STATE_PROPNAME,
    STATE_AFTERPROPNAME,
    STATE_AFTERPROPNAME2,
    STATE_AFTERPROPNAMESEP,
    STATE_RCHAR,
    STATE_MATCH_ALPHA,
    STATE_MATCH_ALNUM,
    STATE_MATCH_DIGITS,
    STATE_MATCH_XDIGITS,
    STATE_MATCH_NOSPACE,
    STATE_MATCH_ANY,
    STATE_MATCH_SPACES,
    STATE_MATCH_BOOLEAN,

    STATE_TEMPLATE_UNDEFINED,
    STATE_TEMPLATE_RCHAR,
    STATE_TEMPLATE_SUBST_ESC
} State;

typedef struct Match_ {
    const char *str;
    size_t      str_len;
} Match;

typedef enum EntryResult_ {
    ENTRYRESULT_UNDEFINED,
    ENTRYRESULT_OK,
    ENTRYRESULT_IGNORE,
    ENTRYRESULT_PROPNOTFOUND,
    ENTRYRESULT_MISMATCH,
    ENTRYRESULT_SYNTAX,
    ENTRYRESULT_INVALID_ENTRY,
    ENTRYRESULT_INTERNAL,
    ENTRYRESULT_E2BIG
} EntryResult;

static const char *
skip_spaces(const char *str)
{
    while (*str != 0 && isspace((unsigned char)*str)) {
        str++;
    }
    return str;
}

static int
prefix_match(const char **str, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    size_t str_len    = strlen(*str);
    size_t i;
    int    x = 0;

    if (str_len < prefix_len) {
        return 0;
    }
    for (i = 0; i < prefix_len; i++) {
        x |= tolower((unsigned char)(*str)[i]) ^
             tolower((unsigned char)prefix[i]);
    }
    if (x == 0) {
        *str += prefix_len;
        return 1;
    }
    return 0;
}

static int
add_to_matches(Match *const matches, size_t *matches_len,
               const char *start, const char *end)
{
    size_t len;

    start = skip_spaces(start);
    len   = (size_t)(end - start);
    while (len > 0 && isspace((unsigned char)start[len - 1])) {
        len--;
    }
    if (len == 0) {
        return -1;
    }
    matches[*matches_len].str     = start;
    matches[*matches_len].str_len = len;
    (*matches_len)++;

    return 0;
}

static EntryResult
err_mismatch(const char **err_p, const char *line_pnt, const char *line)
{
    *err_p = *line_pnt != 0 ? line_pnt : line;

    return ENTRYRESULT_MISMATCH;
}

static EntryResult
err_syntax(const char **err_p, const char *line_pnt, const char *line)
{
    *err_p = *line_pnt != 0 ? line_pnt : line;

    return ENTRYRESULT_SYNTAX;
}

static EntryResult
try_entry(const SimpleConfEntry *const entry, const char *line,
          char **arg_p, const char **err_p)
{
    const char *in_pnt;
    const char *line_pnt;
    const char *match_start = NULL;
    const char *out_pnt;
    const char *prop_name      = NULL;
    const char *wildcard_start = NULL;
    char *      arg;
    Match       matches[10];
    size_t      arg_len;
    size_t      matches_len;
    size_t      wildcard_len;
    State       state = STATE_UNDEFINED;
    int         expect_char;
    int         is_boolean;
    int         is_enabled;
    int         c = 0;
    int         d = 0;

    *err_p = NULL;
    *arg_p = NULL;
    line   = skip_spaces(line);
    if (*line == 0 || *line == '#') {
        return ENTRYRESULT_IGNORE;
    }
    in_pnt      = entry->in;
    line_pnt    = line;
    prop_name   = in_pnt;
    matches_len = 0;
    expect_char = 0;
    is_enabled  = 0;
    is_boolean  = 0;
    state       = STATE_PROPNAME;
    while (*in_pnt != 0 || *line_pnt != 0) {
        c = *(const unsigned char *)line_pnt;
        d = *(const unsigned char *)in_pnt;
        switch (state) {
        case STATE_PROPNAME:
            if (isspace(d)) {
                in_pnt++;
                state = STATE_AFTERPROPNAME;
            } else if (d == '?') {
                is_boolean = 1;
                in_pnt++;
            } else if (c != 0 && d != 0 && tolower(c) == tolower(d)) {
                in_pnt++;
                line_pnt++;
            } else {
                return ENTRYRESULT_PROPNOTFOUND;
            }
            continue;
        case STATE_AFTERPROPNAME:
            if (c == '=' || c == ':') {
                state = STATE_AFTERPROPNAMESEP;
                line_pnt++;
            } else if (isspace(c)) {
                state = STATE_AFTERPROPNAME2;
                line_pnt++;
            } else {
                return err_syntax(err_p, line_pnt, line);
            }
            continue;
        case STATE_AFTERPROPNAME2:
            if (c == '=' || c == ':') {
                state = STATE_AFTERPROPNAMESEP;
                line_pnt++;
            } else if (isspace(c)) {
                line_pnt++;
            } else {
                state = STATE_AFTERPROPNAMESEP;
            }
            continue;
        case STATE_AFTERPROPNAMESEP:
            if (c == '=' || c == ':') {
                return err_syntax(err_p, line_pnt, line);
            } else if (isspace(c)) {
                line_pnt++;
            } else {
                if (*in_pnt == 0 || in_pnt == prop_name) {
                    return err_syntax(err_p, line_pnt, line);
                }
                wildcard_start = line_pnt;
                state = STATE_RCHAR;
            }
            continue;
        case STATE_RCHAR:
            if (d == 0) {
                return err_mismatch(err_p, line_pnt, line);
            } else if (d == '(') {
                match_start = line_pnt;
                in_pnt++;
            } else if (d == ')') {
                if (match_start == NULL ||
                    matches_len >= (sizeof matches) / (sizeof matches[0]) ||
                    add_to_matches(matches, &matches_len, match_start,
                                   line_pnt) != 0) {
                    return err_mismatch(err_p, line, line);
                }
                in_pnt++;
            } else if (prefix_match(&in_pnt, "<alpha>")) {
                expect_char = 1;
                state = STATE_MATCH_ALPHA;
            } else if (prefix_match(&in_pnt, "<alnum>")) {
                expect_char = 1;
                state = STATE_MATCH_ALNUM;
            } else if (prefix_match(&in_pnt, "<digits>")) {
                expect_char = 1;
                state = STATE_MATCH_DIGITS;
            } else if (prefix_match(&in_pnt, "<xdigits>")) {
                expect_char = 1;
                state = STATE_MATCH_XDIGITS;
            } else if (prefix_match(&in_pnt, "<nospace>")) {
                expect_char = 1;
                state = STATE_MATCH_NOSPACE;
            } else if (prefix_match(&in_pnt, "<any>")) {
                expect_char = 1;
                state = STATE_MATCH_ANY;
            } else if (prefix_match(&in_pnt, "<bool>")) {
                if (is_enabled) {
                    return ENTRYRESULT_INVALID_ENTRY;
                }
                state = STATE_MATCH_BOOLEAN;
            } else if (d == '<') {
                return ENTRYRESULT_INVALID_ENTRY;
            } else if (isspace(d)) {
                in_pnt++;
                expect_char = 1;
                state = STATE_MATCH_SPACES;
            } else if (isgraph(d)) {
                if (c == d) {
                    in_pnt++;
                    line_pnt++;
                } else {
                    return err_mismatch(err_p, line_pnt, line);
                }
            } else {
                return err_mismatch(err_p, line_pnt, line);
            }
            continue;
        case STATE_MATCH_ALPHA:
            if (isalpha(c)) {
                expect_char = 0;
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_ALNUM:
            if (isalnum(c)) {
                expect_char = 0;
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_DIGITS:
            if (isdigit(c)) {
                expect_char = 0;
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_XDIGITS:
            if (isxdigit(c)) {
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_NOSPACE:
            if (isgraph(c)) {
                expect_char = 0;
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_ANY:
            if (isprint(c)) {
                expect_char = 0;
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_SPACES:
            if (isspace(c)) {
                expect_char = 0;
                line_pnt++;
            } else {
                state = STATE_RCHAR;
            }
            continue;
        case STATE_MATCH_BOOLEAN: {
            if (prefix_match(&line_pnt, "yes")  || prefix_match(&line_pnt, "on") ||
                prefix_match(&line_pnt, "true") || prefix_match(&line_pnt, "1")) {
                is_enabled = 1;
                state      = STATE_RCHAR;
            } else if (prefix_match(&line_pnt, "no")    || prefix_match(&line_pnt, "off") ||
                       prefix_match(&line_pnt, "false") || prefix_match(&line_pnt, "0")) {
                is_enabled = 0;
                state      = STATE_RCHAR;
            } else {
                return err_syntax(err_p, line_pnt, line);
            }
            continue;
        }
        default:
            return ENTRYRESULT_INVALID_ENTRY;
        }
    }
    switch (state) {
    case STATE_RCHAR:
    case STATE_MATCH_ALPHA:
    case STATE_MATCH_ALNUM:
    case STATE_MATCH_DIGITS:
    case STATE_MATCH_XDIGITS:
    case STATE_MATCH_NOSPACE:
    case STATE_MATCH_ANY:
    case STATE_MATCH_SPACES:
    case STATE_MATCH_BOOLEAN:
        break;
    default:
        return err_mismatch(err_p, line_pnt, line);
    }
    if (expect_char != 0) {
        return err_mismatch(err_p, line_pnt, line);
    }
    if (d == ')') {
        if (match_start == NULL ||
            matches_len >= (sizeof matches) / (sizeof matches[0]) ||
            add_to_matches(matches, &matches_len, match_start, line_pnt) != 0) {
            return ENTRYRESULT_SYNTAX;
        }
    }
    if (is_boolean != 0 && is_enabled == 0) {
        return ENTRYRESULT_IGNORE;
    }
    if (wildcard_start == NULL) {
        wildcard_len = 0;
    } else {
        wildcard_len = (size_t) (line_pnt - wildcard_start);
    }
    out_pnt = entry->out;
    if ((arg = malloc(MAX_ARG_LENGTH + 1)) == NULL) {
        return ENTRYRESULT_INTERNAL;
    }
    arg_len = 0;
    state   = STATE_TEMPLATE_RCHAR;
    while (arg_len < MAX_ARG_LENGTH && *out_pnt != 0) {
        d = *(const unsigned char *)out_pnt;
        switch (state) {
        case STATE_TEMPLATE_RCHAR:
            if (d == '$') {
                out_pnt++;
                state = STATE_TEMPLATE_SUBST_ESC;
            } else {
                arg[arg_len] = (char)d;
                arg_len++;
                out_pnt++;
            }
            continue;
        case STATE_TEMPLATE_SUBST_ESC:
            if (d == '*') {
                size_t i = 0;

                while (arg_len < MAX_ARG_LENGTH && i < wildcard_len) {
                    arg[arg_len++] = wildcard_start[i++];
                }
                out_pnt++;
                state = STATE_TEMPLATE_RCHAR;
            } else if (d >= '0' && d <= '9') {
                size_t match_id = (size_t)(d - '0');
                size_t i        = 0;

                if (match_id >= matches_len) {
                    return ENTRYRESULT_INVALID_ENTRY;
                }
                while (
                    arg_len < MAX_ARG_LENGTH && i < matches[match_id].str_len) {
                    arg[arg_len++] = matches[match_id].str[i++];
                }
                out_pnt++;
                state = STATE_TEMPLATE_RCHAR;
            } else {
                return ENTRYRESULT_INVALID_ENTRY;
            }
            continue;
        default:
            abort();
        }
    }
    if (arg_len >= MAX_ARG_LENGTH) {
        free(arg);
        errno = E2BIG;
        return ENTRYRESULT_E2BIG;
    }
    arg[arg_len] = 0;
    *arg_p       = arg;

    return ENTRYRESULT_OK;
}

static char *
chomp(char *str)
{
    size_t i = strlen(str);
    int    c;

    if (i == 0) {
        return str;
    }
    do {
        i--;
        c = (unsigned char) str[i];
        if (isspace(c)) {
            str[i] = 0;
        } else {
            break;
        }
    } while (i != 0);

    return str;
}

void
sc_argv_free(int argc, char *argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static void
argv_fp_free(int argc, char *argv[], FILE *fp)
{
    int errno_save = errno;
    int i;

    if (fp != NULL) {
        (void) fclose(fp);
    }
    sc_argv_free(argc, argv);
    errno = errno_save;
}

int
sc_build_command_line_from_file(const char *file_name,
                                const SimpleConfEntry entries[],
                                size_t entries_count, char *app_name,
                                int *argc_p, char ***argv_p)
{
    FILE *       fp;
    char *       arg;
    char **      argv = NULL;
    char **      argv_tmp;
    const char * err = NULL;
    const char * err_tmp;
    char         line[MAX_ARG_LENGTH];
    unsigned int line_count = 0;
    int          argc       = 0;
    int          try_next   = 1;
    size_t       i;

    *argc_p = 0;
    *argv_p = NULL;
    if ((fp = fopen(file_name, "r")) == NULL) {
        fprintf(stderr, "Unable to open [%s]: %s\n", file_name, strerror(errno));
        return -1;
    }
    if ((argv = malloc(sizeof arg)) == NULL ||
        (app_name = strdup(app_name)) == NULL) {
        argv_fp_free(argc, argv, fp);
        return -1;
    }
    argv[argc++] = app_name;
    while (fgets(line, (int)(sizeof line), fp) != NULL) {
        chomp(line);
        line_count++;
        for (i = 0; i < entries_count; i++) {
            try_next = 1;
            switch (try_entry(&entries[i], line, &arg, &err_tmp)) {
            case ENTRYRESULT_IGNORE:
                try_next = 0;
                break;
            case ENTRYRESULT_PROPNOTFOUND:
                break;
            case ENTRYRESULT_E2BIG:
                argv_fp_free(argc, argv, fp);
                return -1;
            case ENTRYRESULT_INVALID_ENTRY:
                fprintf(stderr, "Bogus rule: [%s]\n", entries[i].in);
                abort();
            case ENTRYRESULT_INTERNAL:
                argv_fp_free(argc, argv, fp);
                return -1;
            case ENTRYRESULT_MISMATCH:
                err = err_tmp;
                continue;
            case ENTRYRESULT_SYNTAX: {
                err = err_tmp;
                if (err != NULL && *err != 0) {
                    fprintf(stderr, "%s:%u:%u: syntax error line %u: [%s].\n",
                        file_name, line_count, (unsigned int)(err - line + 1),
                        line_count, err);
                } else {
                    fprintf(stderr, "%s:%u:1: syntax error line %u: [%s].\n",
                        file_name, line_count, line_count, line);
                }
                argv_fp_free(argc, argv, fp);
                return -1;
            }
            case ENTRYRESULT_OK:
                try_next = 0;
                if (arg == NULL || *arg == 0) {
                    free(arg);
                    break;
                }
                if (argc >= INT_MAX / (int)(sizeof arg)) {
                    abort();
                }
                if ((argv_tmp = realloc(argv, (sizeof arg) *
                                        ((size_t) argc + 1))) == NULL) {
                    argv_fp_free(argc, argv, fp);
                    return -1;
                }
                argv         = argv_tmp;
                argv[argc++] = arg;
                break;
            default:
                abort();
            }
            if (try_next == 0) {
                break;
            }
        }
        if (try_next != 0 && i >= entries_count) {
            if (err != NULL && *err != 0) {
                fprintf(stderr, "%s:%u:%u: syntax error line %u: [%s].\n",
                    file_name, line_count, (unsigned int)(err - line + 1),
                    line_count, err);
            } else {
                fprintf(stderr, "%s:%u:1: property not found line %u: [%s].\n",
                    file_name, line_count, line_count, line);
            }
            argv_fp_free(argc, argv, fp);
            return -1;
        }
    }
    (void) fclose(fp);
    *argc_p = argc;
    *argv_p = argv;

    return 0;
}
