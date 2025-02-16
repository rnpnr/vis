/*
 * Heavily inspired (and partially based upon) the X11 version of
 * Rob Pike's sam text editor originally written for Plan 9.
 *
 *  Copyright © 2016-2020 Marc André Tanner <mat at brain-dump.org>
 *  Copyright © 1998 by Lucent Technologies
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose without fee is hereby granted, provided that this entire notice
 * is included in all copies of any software which is or includes a copy
 * or modification of this software and in all copies of the supporting
 * documentation for such software.
 *
 * THIS SOFTWARE IS BEING PROVIDED "AS IS", WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTY. IN PARTICULAR, NEITHER THE AUTHORS NOR LUCENT TECHNOLOGIES MAKE ANY
 * REPRESENTATION OR WARRANTY OF ANY KIND CONCERNING THE MERCHANTABILITY
 * OF THIS SOFTWARE OR ITS FITNESS FOR ANY PARTICULAR PURPOSE.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "sam.h"
#include "vis-core.h"
#include "buffer.h"
#include "text.h"
#include "text-motions.h"
#include "text-objects.h"
#include "text-regex.h"

/* TODO(rnp): replace with a bit-table (we only need 1 u64) */
#define IS_SAM_COMMAND(cp) \
((cp) == 'a' || (cp) == 'i' || (cp) == 'c' || (cp) == 'x' || (cp) == 'y' || (cp) == 'v' || \
 (cp) == 'g' || (cp) == 's' || (cp) == 'X' || (cp) == 'Y' || (cp) == 'e' || (cp) == 'r' || \
 (cp) == 'w' || (cp) == 'q')

/* TODO(rnp): there are probably more (bit-table as well) */
#define IS_SAM_DELIMETER(cp) \
((cp) == '/' || (cp) == '!' || (cp) == ';' || (cp) == ':' || (cp) == '%' || (cp) == '#' || \
 (cp) == '?' || (cp) == ',' || (cp) == '.' || (cp) == '+' || (cp) == '-' || (cp) == '=' || \
 (cp) == '\'')

#define IS_SAM_ADDRESS_DELIMETER(cp) ((cp) == ';' || (cp) == ',' || (cp) == '+' || (cp) == '-')

#define SAM_TOKEN_TYPES \
	X(ST_INVALID)     \
	X(ST_DELIMETER)   \
	X(ST_GROUP_END)   \
	X(ST_GROUP_START) \
	X(ST_NUMBER)      \
	X(ST_STRING)

#define X(name) name,
typedef enum { SAM_TOKEN_TYPES } SamTokenType;
#undef X

#define X(name) s8(#name),
static s8 sam_token_types[] = { SAM_TOKEN_TYPES };
#undef X

typedef struct {
	u8  *start;
	u32  length;
	SamTokenType type;
} SamToken;

typedef struct {
	SamToken *tokens;
	Arena    *backing;
	s8        raw; /* raw string for error reporting */
	i32       count;
	i32       read_index;
} SamTokenStream;

#define MAX_ARGV 8

struct Change {
	enum ChangeType {
		TRANSCRIPT_INSERT = 1 << 0,
		TRANSCRIPT_DELETE = 1 << 1,
		TRANSCRIPT_CHANGE = TRANSCRIPT_INSERT|TRANSCRIPT_DELETE,
	} type;
	Win *win;          /* window in which changed file is being displayed */
	Selection *sel;    /* selection associated with this change, might be NULL */
	Filerange range;   /* inserts are denoted by zero sized range (same start/end) */
	const char *data;  /* will be free(3)-ed after transcript has been processed */
	size_t len;        /* size in bytes of the chunk pointed to by data */
	Change *next;      /* modification position increase monotonically */
	int count;         /* how often should data be inserted? */
};

/* TODO(rnp): remove AT_REGEX, it belongs to Command */
typedef enum {
	AT_INVALID,
	AT_BYTE,
	AT_CHARACTER,
	AT_LINE,
	AT_MARK,
	AT_REGEX_BACKWARD,
	AT_REGEX_FORWARD,
} AddressSideType;

typedef struct {
	union {
		size_t        number;
		u32           character;
		enum VisMark  mark;
		Regex        *regex; /* NULL for default regex (TODO(rnp): which is???) */
	} u;
	AddressSideType type;
} AddressSide;

typedef struct {
	AddressSide left, right;
	u32 delimeter;
} Address;

typedef struct {
	int start, end; /* interval [n,m] */
	bool mod;       /* % every n-th match, implies n == m */
} Count;

typedef struct Command Command;

#define SAM_CMD_FN(name) b32 name(Vis *vis, Win *win, Command *cmd, SamTokenStream *sts, \
                                  Selection *sel, Filerange *range)
typedef SAM_CMD_FN(sam_cmd_fn);

typedef struct {
	s8 name;        /* command name */
	s8 help;        /* short, one-line help text */
	sam_cmd_fn *fn; /* command implementation */
	enum {
		CMD_NONE          = 0,       /* standalone command without any arguments */
		CMD_CMD           = 1 << 0,  /* does the command take a sub/target command? */
		CMD_REGEX         = 1 << 1,  /* regex after command? */
		CMD_REGEX_DEFAULT = 1 << 2,  /* is the regex optional i.e. can we use a default? */
		CMD_COUNT         = 1 << 3,  /* does the command support a count as in s2/../? */
		CMD_TEXT          = 1 << 4,  /* does the command need a text to insert? */
		CMD_ADDRESS_NONE  = 1 << 5,  /* is it an error to specify an address for the command? */
		CMD_ADDRESS_POS   = 1 << 6,  /* no address implies an empty range at current cursor position */
		CMD_ADDRESS_LINE  = 1 << 7,  /* if no address is given, use the current line */
		CMD_ADDRESS_AFTER = 1 << 8,  /* if no address is given, begin at the start of the next line */
		CMD_ADDRESS_ALL   = 1 << 9,  /* if no address is given, apply to whole file (independent of #cursors) */
		CMD_ADDRESS_ALL_1CURSOR = 1 << 10, /* if no address is given and only 1 cursor exists, apply to whole file */
		CMD_SHELL         = 1 << 11, /* command needs a shell command as argument */
		CMD_FORCE         = 1 << 12, /* can the command be forced with ! */
		CMD_ARGV          = 1 << 13, /* whether shell like argument splitting is desired */
		CMD_ONCE          = 1 << 14, /* command should only be executed once, not for every selection */
		CMD_LOOP          = 1 << 15, /* a looping construct like `x`, `y` */
		CMD_DESTRUCTIVE   = 1 << 16, /* command potentially destroys window */
	} flags;
} CommandDef;

struct Command {
	Address address;          /* range of text for command */
	Regex *regex;             /* regex to match, used by x, y, g, v, X, Y */
	CommandDef *definition;   /* which command is this? */
	Count count;              /* command count, defaults to [0,+inf] */
	int iteration;            /* current command loop iteration */
	b32 parsed;               /* for commands taking an arg, indicates if it is already parsed */
	s8  *args;
	char flags;               /* command specific flags */
	Command *cmd;             /* target of x, y, g, v, X, Y, { */
	Command *next;            /* next command in {} group */
	Command *prev;
};

/* sam commands */
static SAM_CMD_FN(cmd_insert);
static SAM_CMD_FN(cmd_append);
static SAM_CMD_FN(cmd_change);
static SAM_CMD_FN(cmd_delete);
static SAM_CMD_FN(cmd_guard);
static SAM_CMD_FN(cmd_extract);
static SAM_CMD_FN(cmd_select);
static SAM_CMD_FN(cmd_print);
static SAM_CMD_FN(cmd_files);
static SAM_CMD_FN(cmd_pipein);
static SAM_CMD_FN(cmd_pipeout);
static SAM_CMD_FN(cmd_filter);
static SAM_CMD_FN(cmd_launch);
static SAM_CMD_FN(cmd_substitute);
static SAM_CMD_FN(cmd_write);
static SAM_CMD_FN(cmd_read);
static SAM_CMD_FN(cmd_edit);
static SAM_CMD_FN(cmd_quit);
static SAM_CMD_FN(cmd_cd);
/* vi(m) commands */
static SAM_CMD_FN(cmd_set);
static SAM_CMD_FN(cmd_open);
static SAM_CMD_FN(cmd_qall);
static SAM_CMD_FN(cmd_split);
static SAM_CMD_FN(cmd_vsplit);
static SAM_CMD_FN(cmd_new);
static SAM_CMD_FN(cmd_vnew);
static SAM_CMD_FN(cmd_wq);
static SAM_CMD_FN(cmd_earlier_later);
static SAM_CMD_FN(cmd_help);
static SAM_CMD_FN(cmd_map);
static SAM_CMD_FN(cmd_unmap);
static SAM_CMD_FN(cmd_langmap);
static SAM_CMD_FN(cmd_user);

/* TODO(rnp): cleanup */
static SAM_CMD_FN(sam_execute);

static const CommandDef command_definition_table[] = {
	{
		.name  = s8("a"),
		.help  = SAM_HELP("Append text after range"),
		.fn    = cmd_append,
		.flags = CMD_TEXT,
	}, {
		.name  = s8("c"),
		.help  = SAM_HELP("Change text in range"),
		.fn    = cmd_change,
		.flags = CMD_TEXT,
	}, {
		.name  = s8("d"),
		.help  = SAM_HELP("Delete text in range"),
		.fn    = cmd_delete,
	}, {
		.name  = s8("g"),
		.help  = SAM_HELP("If range contains regexp, run command"),
		.fn    = cmd_guard,
		.flags = CMD_COUNT|CMD_REGEX|CMD_CMD,
	}, {
		.name  = s8("i"),
		.help  = SAM_HELP("Insert text before range"),
		.fn    = cmd_insert,
		.flags = CMD_TEXT,
	}, {
		.name  = s8("p"),
		.help  = SAM_HELP("Create selection covering range"),
		.fn    = cmd_print,
	}, {
		.name  = s8("s"),
		.help  = SAM_HELP("Substitute: use x/pattern/ c/replacement/ instead"),
		.fn    = cmd_substitute,
		.flags = CMD_SHELL|CMD_ADDRESS_LINE,
	}, {
		.name  = s8("v"),
		.help  = SAM_HELP("If range does not contain regexp, run command"),
		.fn    = cmd_guard,
		.flags = CMD_COUNT|CMD_REGEX|CMD_CMD,
	}, {
		.name  = s8("x"),
		.help  = SAM_HELP("Set range and run command on each match"),
		.fn    = cmd_extract,
		.flags = CMD_CMD|CMD_REGEX|CMD_REGEX_DEFAULT|CMD_ADDRESS_ALL_1CURSOR|CMD_LOOP,
	}, {
		.name  = s8("y"),
		.help  = SAM_HELP("As `x` but select unmatched text"),
		.fn    = cmd_extract,
		.flags = CMD_CMD|CMD_REGEX|CMD_ADDRESS_ALL_1CURSOR|CMD_LOOP,
	}, {
		.name  = s8("X"),
		.help  = SAM_HELP("Run command on files whose name matches"),
		.fn    = cmd_files,
		.flags = CMD_CMD|CMD_REGEX|CMD_REGEX_DEFAULT|CMD_ADDRESS_NONE|CMD_ONCE,
	}, {
		.name  = s8("Y"),
		.help  = SAM_HELP("As `X` but select unmatched files"),
		.fn    = cmd_files,
		.flags = CMD_CMD|CMD_REGEX|CMD_ADDRESS_NONE|CMD_ONCE,
	}, {
		.name  = s8(">"),
		.help  = SAM_HELP("Send range to stdin of command"),
		.fn    = cmd_pipeout,
		.flags = CMD_SHELL|CMD_ADDRESS_LINE,
	}, {
		.name  = s8("<"),
		.help  = SAM_HELP("Replace range by stdout of command"),
		.fn    = cmd_pipein,
		.flags = CMD_SHELL|CMD_ADDRESS_POS,
	}, {
		.name  = s8("|"),
		.help  = SAM_HELP("Pipe range through command"),
		.fn    = cmd_filter,
		.flags = CMD_SHELL,
	}, {
		.name  = s8("!"),
		.help  = SAM_HELP("Run the command"),
		.fn    = cmd_launch,
		.flags = CMD_SHELL|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("w"),
		.help  = SAM_HELP("Write range to named file"),
		.fn    = cmd_write,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_ALL,
	}, {
		.name  = s8("r"),
		.help  = SAM_HELP("Replace range by contents of file"),
		.fn    = cmd_read,
		.flags = CMD_ARGV|CMD_ADDRESS_AFTER,
	}, {
		/* TODO(rnp): we don't need this in the map */
		.name  = s8("{"),
		.help  = SAM_HELP("Start of command group"),
	}, {
		/* TODO(rnp): we don't need this in the map */
		.name  = s8("}"),
		.help  = SAM_HELP("End of command group"),
	}, {
		.name  = s8("e"),
		.help  = SAM_HELP("Edit file"),
		.fn    = cmd_edit,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_NONE|CMD_DESTRUCTIVE,
	}, {
		.name  = s8("q"),
		.help  = SAM_HELP("Quit the current window"),
		.fn    = cmd_quit,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_NONE|CMD_DESTRUCTIVE,
	}, {
		.name  = s8("cd"),
		.help  = SAM_HELP("Change directory"),
		.fn    = cmd_cd,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	},
	/* vi(m) related commands */
	{
		.name  = s8("help"),
		.help  = SAM_HELP("Show this help"),
		.fn    = cmd_help,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("map"),
		.help  = SAM_HELP("Map key binding `:map <mode> <lhs> <rhs>`"),
		.fn    = cmd_map,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("map-window"),
		.help  = SAM_HELP("As `map` but window local"),
		.fn    = cmd_map,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("unmap"),
		.help  = SAM_HELP("Unmap key binding `:unmap <mode> <lhs>`"),
		.fn    = cmd_unmap,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("unmap-window"),
		.help  = SAM_HELP("As `unmap` but window local"),
		.fn    = cmd_unmap,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("langmap"),
		.help  = SAM_HELP("Map keyboard layout `:langmap <locale-keys> <latin-keys>`"),
		.fn    = cmd_langmap,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("new"),
		.help  = SAM_HELP("Create new window"),
		.fn    = cmd_new,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("open"),
		.help  = SAM_HELP("Open file"),
		.fn    = cmd_open,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("qall"),
		.help  = SAM_HELP("Exit vis"),
		.fn    = cmd_qall,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_NONE|CMD_DESTRUCTIVE,
	}, {
		.name  = s8("set"),
		.help  = SAM_HELP("Set option"),
		.fn    = cmd_set,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("split"),
		.help  = SAM_HELP("Horizontally split window"),
		.fn    = cmd_split,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("vnew"),
		.help  = SAM_HELP("As `:new` but split vertically"),
		.fn    = cmd_vnew,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("vsplit"),
		.help  = SAM_HELP("Vertically split window"),
		.fn    = cmd_vsplit,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("wq"),
		.help  = SAM_HELP("Write file and quit"),
		.fn    = cmd_wq,
		.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_ALL|CMD_DESTRUCTIVE,
	}, {
		.name  = s8("earlier"),
		.help  = SAM_HELP("Go to older text state"),
		.fn    = cmd_earlier_later,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	}, {
		.name  = s8("later"),
		.help  = SAM_HELP("Go to newer text state"),
		.fn    = cmd_earlier_later,
		.flags = CMD_ARGV|CMD_ONCE|CMD_ADDRESS_NONE,
	},
	/* TODO(rnp): this is stupid, we know the length at compile time */
	{ .name = s8(""), .help = SAM_HELP("") },
};

/* :set command options */
typedef struct {
	const char *names[3];            /* name and optional alias */
	enum VisOption flags;            /* option type, etc. */
	VIS_HELP_DECL(const char *help;) /* short, one line help text */
	VisOptionFunction *func;         /* option handler, NULL for builtins */
	void *context;                   /* context passed to option handler function */
} OptionDef;

enum {
	OPTION_SHELL,
	OPTION_ESCDELAY,
	OPTION_AUTOINDENT,
	OPTION_EXPANDTAB,
	OPTION_TABWIDTH,
	OPTION_SHOW_SPACES,
	OPTION_SHOW_TABS,
	OPTION_SHOW_NEWLINES,
	OPTION_SHOW_EOF,
	OPTION_STATUSBAR,
	OPTION_NUMBER,
	OPTION_NUMBER_RELATIVE,
	OPTION_CURSOR_LINE,
	OPTION_COLOR_COLUMN,
	OPTION_SAVE_METHOD,
	OPTION_LOAD_METHOD,
	OPTION_CHANGE_256COLORS,
	OPTION_LAYOUT,
	OPTION_IGNORECASE,
	OPTION_BREAKAT,
	OPTION_WRAP_COLUMN,
};

static const OptionDef options[] = {
	[OPTION_SHELL] = {
		{ "shell" },
		VIS_OPTION_TYPE_STRING,
		VIS_HELP("Shell to use for external commands (default: $SHELL, /etc/passwd, /bin/sh)")
	},
	[OPTION_ESCDELAY] = {
		{ "escdelay" },
		VIS_OPTION_TYPE_NUMBER,
		VIS_HELP("Milliseconds to wait to distinguish <Escape> from terminal escape sequences")
	},
	[OPTION_AUTOINDENT] = {
		{ "autoindent", "ai" },
		VIS_OPTION_TYPE_BOOL,
		VIS_HELP("Copy leading white space from previous line")
	},
	[OPTION_EXPANDTAB] = {
		{ "expandtab", "et" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Replace entered <Tab> with `tabwidth` spaces")
	},
	[OPTION_TABWIDTH] = {
		{ "tabwidth", "tw" },
		VIS_OPTION_TYPE_NUMBER|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Number of spaces to display (and insert if `expandtab` is enabled) for a tab")
	},
	[OPTION_SHOW_SPACES] = {
		{ "showspaces" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display replacement symbol instead of a space")
	},
	[OPTION_SHOW_TABS] = {
		{ "showtabs" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display replacement symbol for tabs")
	},
	[OPTION_SHOW_NEWLINES] = {
		{ "shownewlines" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display replacement symbol for newlines")
	},
	[OPTION_SHOW_EOF] = {
		{ "showeof" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display replacement symbol for lines after the end of the file")
	},
	[OPTION_STATUSBAR] = {
		{ "statusbar", "sb" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display status bar")
	},
	[OPTION_NUMBER] = {
		{ "numbers", "nu" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display absolute line numbers")
	},
	[OPTION_NUMBER_RELATIVE] = {
		{ "relativenumbers", "rnu" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Display relative line numbers")
	},
	[OPTION_CURSOR_LINE] = {
		{ "cursorline", "cul" },
		VIS_OPTION_TYPE_BOOL|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Highlight current cursor line")
	},
	[OPTION_COLOR_COLUMN] = {
		{ "colorcolumn", "cc" },
		VIS_OPTION_TYPE_NUMBER|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Highlight a fixed column")
	},
	[OPTION_SAVE_METHOD] = {
		{ "savemethod" },
		VIS_OPTION_TYPE_STRING|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Save method to use for current file 'auto', 'atomic' or 'inplace'")
	},
	[OPTION_LOAD_METHOD] = {
		{ "loadmethod" },
		VIS_OPTION_TYPE_STRING,
		VIS_HELP("How to load existing files 'auto', 'read' or 'mmap'")
	},
	[OPTION_CHANGE_256COLORS] = {
		{ "change256colors" },
		VIS_OPTION_TYPE_BOOL,
		VIS_HELP("Change 256 color palette to support 24bit colors")
	},
	[OPTION_LAYOUT] = {
		{ "layout" },
		VIS_OPTION_TYPE_STRING,
		VIS_HELP("Vertical or horizontal window layout")
	},
	[OPTION_IGNORECASE] = {
		{ "ignorecase", "ic" },
		VIS_OPTION_TYPE_BOOL,
		VIS_HELP("Ignore case when searching")
	},
	[OPTION_BREAKAT] = {
		{ "breakat", "brk" },
		VIS_OPTION_TYPE_STRING|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Characters which might cause a word wrap")
	},
	[OPTION_WRAP_COLUMN] = {
		{ "wrapcolumn", "wc" },
		VIS_OPTION_TYPE_NUMBER|VIS_OPTION_NEED_WINDOW,
		VIS_HELP("Wrap lines at minimum of window width and wrapcolumn")
	},
};

static s8
consume(s8 raw, u32 count)
{
	ASSERT(raw.len >= count);
	raw.data += count;
	raw.len  -= count;
	return raw;
}

static u32
peek(s8 raw)
{
	u32 result = (u32)-1;
	if (raw.len > 0)
		result = raw.data[0];
	return result;
}

static s8
consume_digits(s8 raw)
{
	ix count;
	for (count = 0; count < raw.len && ISDIGIT(raw.data[count]); count++)
		;
	return consume(raw, count);
}

static s8
sam_token_to_s8(SamToken token)
{
	s8 result = {.len = token.length, .data = token.start};
	return result;
}

static void
sam_token_print(Buffer *buffer, SamToken t)
{
	s8 type = s8("\n  type: ");
	buffer_append(buffer, t.start, t.length);
	buffer_append(buffer, type.data, type.len);
	buffer_append(buffer, sam_token_types[t.type].data, sam_token_types[t.type].len);
	buffer_append(buffer, "\n", 1);
}

static void __attribute__((format(printf, 4, 5)))
sam_error_at(Buffer *buffer, SamTokenStream *s, SamToken token, const char *fmt, ...)
{
	i32 padding = token.start - s->raw.data;
	if (padding < 0) padding = s->raw.len;
	s8  header  = s8("---Sam Error---\n");
	buffer_append(buffer, header.data, header.len);
	buffer_append(buffer, s->raw.data, s->raw.len);
	buffer_appendf(buffer, "\n%*s^\n%*s", padding, "", padding, "");
	va_list ap;
	va_start(ap, fmt);
	buffer_vappendf(buffer, fmt, ap);
	va_end(ap);
	/* TODO(rnp): fix buffer_appendf */
	buffer->len--;
	buffer_append(buffer, "\n", 1);
}

static SamToken
sam_token_peek(SamTokenStream *s)
{
	SamToken result = {.type = ST_INVALID};
	if (s->read_index < s->count)
		result = s->tokens[s->read_index];
	return result;
}

static SamToken
sam_token_pop(SamTokenStream *s)
{
	SamToken result = {.type = ST_INVALID};
	if (s->read_index < s->count)
		result = s->tokens[s->read_index++];
	return result;
}

static void
sam_token_push(SamTokenStream *s, SamToken tok)
{
	if (tok.length > 0) {
		*(push_struct(s->backing, SamToken)) = tok;
		s->count++;
	}
}

static SamToken *
sam_token_at(SamTokenStream *s, u32 type, s8 raw)
{
	SamToken *result = 0;
	if (raw.len) {
		result         = push_struct(s->backing, SamToken);
		result->start  = raw.data;
		result->type   = type;
		result->length = 1;
		s->count++;
	}
	return result;
}

static SamToken
sam_tokens_join_until_space(SamTokenStream *s)
{
	SamToken result = {.start = sam_token_peek(s).start};
	while (sam_token_peek(s).type != ST_INVALID) {
		if (result.start + result.length != sam_token_peek(s).start)
			break;
		SamToken token  = sam_token_pop(s);
		result.length  += token.length;
	}
	if (result.length)
		result.type = ST_STRING;
	return result;
}

static char **
sam_tokens_to_argv(Arena *arena, SamTokenStream *s)
{
	/* NOTE(rnp): allocates some extra space but this is simple */
	char **result = alloc(arena, char *, MAX(1, s->count - s->read_index + 1));
	i32    count  = 0;

	while (sam_token_peek(s).type != ST_INVALID) {
		s8 arg = sam_token_to_s8(sam_tokens_join_until_space(s));
		result[count++] = (char *)push_s8_zero(arena, arg).data;
	}

	return result;
}

static b32
sam_token_check_pop_force_flag(SamTokenStream *s)
{
	b32 result = sam_token_peek(s).type == ST_DELIMETER && *sam_token_peek(s).start == '!';
	if (result)
		sam_token_pop(s);
	return result;
}

static b32
valid_command_name(s8 s)
{
	b32 result = 1;
	for (ix i = 0; result && i < s.len; i++) {
		u32 cp = s.data[i];
		result &= !ISSPACE(cp) && !ISDIGIT(cp) && (!ISPUNCT(cp) || cp == '_');
		if (!result && cp == '-')
			result = i + 1 < s.len;
	}
	return result;
}

static void
sam_lex(SamTokenStream *s, s8 raw)
{
	SamToken accum = {.start = raw.data, .type = ST_STRING};
	while (raw.len > 0) {
		u32 cp = peek(raw);
		if (ISSPACE(cp)) {
			sam_token_push(s, accum);

			/* NOTE: whitespace can appear anywhere and is insignificant */
			raw = consume(raw, 1);

			accum = (SamToken){.start = raw.data, .type = ST_STRING};
		} else if (ISDIGIT(cp)) {
			sam_token_push(s, accum);

			SamToken *token = sam_token_at(s, ST_NUMBER, raw);
			raw = consume_digits(raw);
			token->length = raw.data - token->start;

			accum = (SamToken){.start = raw.data, .type = ST_STRING};
		} else if (cp == '{') {
			sam_token_push(s, accum);

			sam_token_at(s, ST_GROUP_START, raw);
			raw = consume(raw, 1);
				raw = consume(raw, 1);

			accum = (SamToken){.start = raw.data, .type = ST_STRING};
		} else if (cp == '}') {
			sam_token_push(s, accum);

			sam_token_at(s, ST_GROUP_END, raw);
			raw = consume(raw, 1);
				raw = consume(raw, 1);

			accum = (SamToken){.start = raw.data, .type = ST_STRING};
		} else if ((cp == '>' || cp == '<' || cp == '|') && accum.length == 0) {
			/* NOTE(rnp): special case for pipe commands at start of line */
			accum.length++;
			raw = consume(raw, 1);
			sam_token_push(s, accum);
			accum = (SamToken){.start = raw.data, .type = ST_STRING};
		} else if (IS_SAM_DELIMETER(cp)) {
			sam_token_push(s, accum);

			sam_token_at(s, ST_DELIMETER, raw);
			raw = consume(raw, 1);

			accum = (SamToken){.start = raw.data, .type = ST_STRING};
		} else {
			accum.length++;
			raw = consume(raw, 1);
		}
	}
	sam_token_push(s, accum);
}

bool sam_init(Vis *vis) {
	vis->sam.arena        = arena_new();
	vis->sam.token_stream = arena_new();

	if (!(vis->cmds = map_new()))
		return false;
	bool ret = true;
	for (const CommandDef *cmd = command_definition_table; cmd->name.len; cmd++)
		ret &= map_put(vis->cmds, (char *)cmd->name.data, cmd);

	if (!(vis->options = map_new()))
		return false;
	for (int i = 0; i < LENGTH(options); i++) {
		for (const char *const *name = options[i].names; *name; name++)
			ret &= map_put(vis->options, *name, &options[i]);
	}

	return ret;
}

const char *sam_error(enum SamError err) {
	static const char *error_msg[] = {
		[SAM_ERR_OK]              = "Success",
		[SAM_ERR_MEMORY]          = "Out of memory",
		[SAM_ERR_ADDRESS]         = "Bad address",
		[SAM_ERR_NO_ADDRESS]      = "Command takes no address",
		[SAM_ERR_UNMATCHED_BRACE] = "Unmatched `}'",
		[SAM_ERR_REGEX]           = "Bad regular expression",
		[SAM_ERR_TEXT]            = "Bad text",
		[SAM_ERR_SHELL]           = "Shell command expected",
		[SAM_ERR_COMMAND]         = "Unknown command",
		[SAM_ERR_EXECUTE]         = "Error executing command",
		[SAM_ERR_NEWLINE]         = "Newline expected",
		[SAM_ERR_MARK]            = "Invalid mark",
		[SAM_ERR_CONFLICT]        = "Conflicting changes",
		[SAM_ERR_WRITE_CONFLICT]  = "Can not write while changing",
		[SAM_ERR_LOOP_INVALID_CMD]  = "Destructive command in looping construct",
		[SAM_ERR_GROUP_INVALID_CMD] = "Destructive command in group",
		[SAM_ERR_COUNT]           = "Invalid count",
	};

	size_t idx = err;
	return idx < LENGTH(error_msg) ? error_msg[idx] : NULL;
}

static void change_free(Change *c) {
	if (c) {
		free((char*)c->data);
		free(c);
	}
}

static Change *change_new(Transcript *t, enum ChangeType type, Filerange *range, Win *win, Selection *sel) {
	Change *result = 0;
	if (text_range_valid(range)) {
		Change **prev, *next;
		if (t->latest && t->latest->range.end <= range->start) {
			prev = &t->latest->next;
			next = t->latest->next;
		} else {
			prev = &t->changes;
			next = t->changes;
		}
		while (next && next->range.end <= range->start) {
			prev = &next->next;
			next = next->next;
		}

		if (next && next->range.start < range->end) {
			t->error = SAM_ERR_CONFLICT;
		} else {
			result = calloc(1, sizeof(*result));
			if (result) {
				result->type  = type;
				result->range = *range;
				result->sel   = sel;
				result->win   = win;
				result->next  = next;
				*prev = result;
				t->latest = result;
			}
		}
	}
	return result;
}

static bool sam_transcript_error(Transcript *t, enum SamError error) {
	if (t->changes)
		t->error = error;
	return t->error;
}

static void sam_transcript_free(Transcript *t) {
	for (Change *c = t->changes, *next; c; c = next) {
		next = c->next;
		change_free(c);
	}
}

static bool sam_insert(Win *win, Selection *sel, size_t pos, const char *data, size_t len, int count) {
	Filerange range = text_range_new(pos, pos);
	Change *c = change_new(&win->file->transcript, TRANSCRIPT_INSERT, &range, win, sel);
	if (c) {
		c->data = data;
		c->len = len;
		c->count = count;
	}
	return c;
}

static bool sam_delete(Win *win, Selection *sel, Filerange *range) {
	return change_new(&win->file->transcript, TRANSCRIPT_DELETE, range, win, sel);
}

static bool sam_change(Win *win, Selection *sel, Filerange *range, const char *data, size_t len, int count) {
	Change *c = change_new(&win->file->transcript, TRANSCRIPT_CHANGE, range, win, sel);
	if (c) {
		c->data = data;
		c->len = len;
		c->count = count;
	}
	return c;
}

static void
address_free(Address a)
{
	if (a.left.type == AT_REGEX_BACKWARD || a.left.type == AT_REGEX_FORWARD) {
		text_regex_free(a.left.u.regex);
		a.left.u.regex = 0;
	}
	if (a.right.type == AT_REGEX_BACKWARD || a.right.type == AT_REGEX_FORWARD) {
		text_regex_free(a.right.u.regex);
		a.right.u.regex = 0;
	}
}

static void skip_spaces(const char **s) {
	while (**s == ' ' || **s == '\t')
		(*s)++;
}

static char *parse_until(const char **s, const char *until, const char *escchars, int type){
	Buffer buf = {0};
	size_t len = strlen(until);
	bool escaped = false;

	for (; **s && (!memchr(until, **s, len) || escaped); (*s)++) {
		if (type != CMD_SHELL && !escaped && **s == '\\') {
			escaped = true;
			continue;
		}

		char c = **s;

		if (escaped) {
			escaped = false;
			if (c == '\n')
				continue;
			if (c == 'n') {
				c = '\n';
			} else if (c == 't') {
				c = '\t';
			} else if (type != CMD_REGEX && type != CMD_TEXT && c == '\\') {
				// ignore one of the back slashes
			} else {
				bool delim = memchr(until, c, len);
				bool esc = escchars && memchr(escchars, c, strlen(escchars));
				if (!delim && !esc)
					buffer_append(&buf, "\\", 1);
			}
		}

		if (!buffer_append(&buf, &c, 1)) {
			buffer_release(&buf);
			return NULL;
		}
	}

	buffer_terminate(&buf);

	return buf.data;
}

static char *parse_delimited(const char **s, int type) {
	char delim[2] = { **s, '\0' };
	if (!delim[0] || isspace((unsigned char)delim[0]))
		return NULL;
	(*s)++;
	char *chunk = parse_until(s, delim, NULL, type);
	if (**s == delim[0])
		(*s)++;
	return chunk;
}

static int parse_number(const char **s) {
	char *end = NULL;
	int number = strtoull(*s, &end, 10);
	if (end == *s)
		return 0;
	*s = end;
	return number;
}

static char *parse_text(const char **s, Count *count) {
	skip_spaces(s);
	const char *before = *s;
	count->start = parse_number(s);
	if (*s == before)
		count->start = 1;
	if (**s != '\n') {
		before = *s;
		char *text = parse_delimited(s, CMD_TEXT);
		return (!text && *s != before) ? strdup("") : text;
	}

	Buffer buf = {0};
	const char *start = *s + 1;
	bool dot = false;

	for ((*s)++; **s && (!dot || **s != '\n'); (*s)++)
		dot = (**s == '.');

	if (!dot || !buffer_put(&buf, start, *s - start - 1) ||
	    !buffer_append(&buf, "\0", 1)) {
		buffer_release(&buf);
		return NULL;
	}

	return buf.data;
}

static void parse_argv(const char **s, const char *argv[], size_t maxarg) {
	for (size_t i = 0; i < maxarg; i++) {
		skip_spaces(s);
		if (**s == '"' || **s == '\'')
			argv[i] = parse_delimited(s, CMD_ARGV);
		else
			argv[i] = parse_until(s, " \t\n", "\'\"", CMD_ARGV);
	}
}

static Regex *parse_regex(Vis *vis, const char **s) {
	const char *before = *s;
	char *pattern = parse_delimited(s, CMD_REGEX);
	if (!pattern && *s == before)
		return NULL;
	Regex *regex = vis_regex(vis, pattern);
	free(pattern);
	return regex;
}

static AddressSide
parse_address_side(Vis *vis, SamTokenStream *sts, SamToken token)
{
	ASSERT(token.type == ST_NUMBER || token.type == ST_DELIMETER);

	AddressSide result = {0};

	if (token.type == ST_NUMBER) {
		result.type     = AT_LINE;
		result.u.number = s8_to_i64(sam_token_to_s8(token));
	} else {
		switch (*token.start) {
		case '#': /* character #n */
			if (sam_token_peek(sts).type == ST_NUMBER) {
				SamToken value  = sam_token_pop(sts);
				result.type     = AT_BYTE;
				result.u.number = s8_to_i64(sam_token_to_s8(value));
			} else {
				/* TODO(rnp): print error "expected number at position ..." */
			}
			break;
		case '\'': {
			if (sam_token_peek(sts).type == ST_STRING) {
				/* TODO(rnp): hack; one possible solution is to turn the
				 * token stream into a single linked list (with an end pointer)
				 * and split the token. Will review later if necessary */
				char mark = sts->tokens[sts->read_index].start[0];
				sts->tokens[sts->read_index].start++;
				sts->tokens[sts->read_index].length--;

				result.type   = AT_MARK;
				result.u.mark = vis_mark_from(vis, mark);
				if (result.u.mark == VIS_MARK_INVALID) {
					/* TODO(rnp): print error "invalid mark at position ..." */
				}
			} else {
				/* TODO(rnp): print error "expected mark at position ..." */
			}
		} break;
		case '/': /* regexp forwards */
		case '?': /* regexp backwards */
			if (sam_token_peek(sts).type == ST_STRING) {
				/* TODO(rnp): this basically just unescapes the string */
				//char *pattern = parse_delimited(s, CMD_REGEX);
				SamToken value = sam_token_pop(sts);
				s8 string      = push_s8_zero(&vis->sam.arena, sam_token_to_s8(value));
				result.type    = *token.start == '/'? AT_REGEX_FORWARD
				                                    : AT_REGEX_BACKWARD;
				result.u.regex = vis_regex(vis, (char *)string.data);
			}
			if (!result.u.regex) {
				/* TODO(rnp): print error "expected regex at position ..." */
			}
			break;
		case '$':
		case '.':
		case '%':
			result.type        = AT_CHARACTER;
			result.u.character = *token.start;
			break;
		}
	}

	return result;
}

static Address
parse_address(Vis *vis, SamTokenStream *sts)
{
	Address result = {0};

	SamTokenType test_type = sam_token_peek(sts).type;
	/* NOTE(rnp): left hand side */
	if (test_type == ST_NUMBER || test_type == ST_DELIMETER)
		result.left = parse_address_side(vis, sts, sam_token_pop(sts));

	/* NOTE(rnp): delimeter */
	SamToken test = sam_token_peek(sts);
	if (test.type == ST_DELIMETER && IS_SAM_ADDRESS_DELIMETER(*test.start)) {
		result.delimeter = *test.start;
		sam_token_pop(sts);
		/* NOTE(rnp): right hand side */
		test_type = sam_token_peek(sts).type;
		if (test_type == ST_NUMBER || test_type == ST_DELIMETER)
			result.right = parse_address_side(vis, sts, sam_token_pop(sts));
	}

	return result;
}

static void command_free(Command *cmd) {
	if (!cmd)
		return;

	for (Command *c = cmd->cmd, *next; c; c = next) {
		next = c->next;
		command_free(c);
	}

	address_free(cmd->address);
	text_regex_free(cmd->regex);
	free(cmd);
}

static CommandDef *
command_lookup(Vis *vis, s8 name)
{
	CommandDef *result = map_closest(vis->cmds, (char *)push_s8_zero(&vis->sam.arena, name).data);
	return result;
}

static Command *
command_parse(Vis *vis, SamTokenStream *sts, enum SamError *err)
{
	return 0;
	#if 0
	Command *cmd = calloc(1, sizeof(*cmd));
	if (!cmd)
		return NULL;
	cmd->address = parse_address(vis, sts, err);

	CommandDef *cmddef = 0;
	switch (sam_token_peek(sts).type) {
	case ST_INVALID:
		cmd->cmddef = cmddef = command_lookup(vis, s8("p"));
		break;
	case ST_STRING: {
		s8 string = sam_token_to_s8(sam_token_pop(sts));
		if (valid_command_name(string))
			cmd->cmddef = cmddef = command_lookup(vis, string);
		if (!cmddef) {
			/* TODO(rnp): invalid command name at ... */
		}
	} break;
	case ST_GROUP_START: {
		sam_token_pop(sts);
		cmd->cmddef = cmddef = command_lookup(vis, s8("{"));
		Command *prev = NULL, *next;
		int level = vis->sam.nesting_level++;
		do {
			/* TODO(rnp): no need for recursion */
			/* TODO(rnp): should we do lazy evaluation? */
			next = command_parse(vis, sts, err);
			if (*err)
				goto fail;
			if (prev)
				prev->next = next;
			else
				cmd->cmd = next;
		} while ((prev = next));

		if (level != vis->sam.nesting_level) {
			*err = SAM_ERR_UNMATCHED_BRACE;
			goto fail;
		}
	} break;
	case ST_GROUP_END:
		/* TODO(rnp): this was here before but isn't it wrong? */
		if (vis->sam.nesting_level-- == 0)
			*err = SAM_ERR_UNMATCHED_BRACE;
		goto fail;
	default:
		/* TODO(rnp): unexpected token ... at ... */
		goto fail;
	}

	/* TODO(rnp): we can do better */
	if (!cmddef) {
		*err = SAM_ERR_COMMAND;
		goto fail;
	}

	/* TODO(rnp): at this point we should just go into the command proper.
	 * it will have a better idea of what it needs where. we just need to
	 * adjust the cmd nesting a little bit; we can also run the validate
	 * functionality here with the nesting since we already know the commands */

	#if 0
	if (cmddef->flags & CMD_ADDRESS_NONE && cmd->address.left.type != AT_INVALID) {
		*err = SAM_ERR_NO_ADDRESS;
		goto fail;
	}

	if (cmddef->flags & CMD_FORCE && sam_token_peek(sts).type == ST_DELIMETER) {
		if (*sam_token_peek(sts).start == '!') {
			cmd->flags = '!';
			sam_token_pop(sts);
		}
	}

	if (cmddef->flags & CMD_COUNT) {
		SamTokenType test_type = sam_token_peek(sts).type;
		if (test_type == ST_DELIMETER && *sam_token_peek(sts).start == '%') {
			sam_token_pop(sts);
			cmd->count.mod = 1;
			test_type = sam_token_peek(sts).type;
		}

		if (cmd->count.mod) {
			if (test_type == ST_NUMBER) {
				i64 n = s8_to_i64(sam_token_to_s8(sam_token_pop(sts)));
				cmd->count.start = n;
				cmd->count.end   = n;
			} else {
				/* TODO(rnp): (append_count_error) expected count at ... */
				*err = SAM_ERR_COUNT;
				goto fail;
			}
		} else {
			if (test_type != ST_NUMBER) {
				*err = SAM_ERR_COUNT;
				goto fail;
			}
			cmd->count.start = s8_to_i64(sam_token_to_s8(sam_token_pop(sts)));
			test_type        = sam_token_peek(sts).type;

			if (test_type == ST_DELIMETER && *sam_token_peek(sts).start != ',') {
				cmd->count.end = cmd->count.start ? cmd->count.start : INT_MAX;
			} else {
				if (test_type != ST_NUMBER) {
					*err = SAM_ERR_COUNT;
					goto fail;
				}
				cmd->count.end = s8_to_i64(sam_token_to_s8(sam_token_pop(sts)));
			}
			if (!cmd->count.end) cmd->count.end = INT_MAX;
		}
		*err = 0;
	}

	if (cmddef->flags & CMD_REGEX) {
		if (!(cmddef->flags & CMD_REGEX_DEFAULT)) {
			if (sam_token_peek(sts).type == ST_STRING) {
				/* TODO(rnp): this basically just unescapes the string */
				//char *pattern = parse_delimited(s, CMD_REGEX);
				SamToken token = sam_token_pop(sts);
				s8 string      = sam_token_to_s8(token);
				char *tmp      = strndup((char *)string.data, string.len);
				if (tmp) {
					cmd->regex = vis_regex(vis, tmp);
					free(tmp);
				}
			}

			if (!cmd->regex && !(cmddef->flags & CMD_COUNT)) {
				/* TODO(rnp): print error "count or regex at position ..." */
				*err = SAM_ERR_REGEX;
				goto fail;
			}
		}
	}

	if (cmddef->flags & CMD_SHELL) {
		if (sam_token_peek(sts).type == ST_STRING) {
			SamToken start, end;
			start = end = sam_token_pop(sts);
			while (sam_token_peek(sts).type == ST_STRING)
				end = sam_token_pop(sts);
			/* TODO(rnp): cleanup */
			cmd->argv[1] = strndup((char *)start.start, (end.start + end.length) - start.start);
			if (cmd->argv[1])
				register_put0(vis, &vis->registers[VIS_REG_SHELL], cmd->argv[1]);
		} else {
			const char *last_cmd = register_get(vis, &vis->registers[VIS_REG_SHELL], 0);
			if (last_cmd) {
				/* TODO(rnp): cleanup */
				cmd->argv[1] = strdup(last_cmd);
			}
		}
		if (!cmd->argv[1]) {
			/* TODO(rnp): print error "expected shell command at position ..." */
			*err = SAM_ERR_SHELL;
			goto fail;
		}
	}

	if (cmddef->flags & CMD_TEXT && !(cmd->argv[1] = parse_text(s, &cmd->count))) {
		*err = SAM_ERR_TEXT;
		goto fail;
	}

	if (cmddef->flags & CMD_ARGV) {
		parse_argv(s, &cmd->argv[1], MAX_ARGV-2);
		cmd->argv[MAX_ARGV-1] = NULL;
	}

	if (cmddef->flags & CMD_CMD) {
		skip_spaces(s);
		if (cmddef->defcmd && (**s == '\n' || **s == '}' || **s == '\0')) {
			if (**s == '\n')
				(*s)++;
			if (!(cmd->cmd = calloc(1, sizeof(Command))))
				goto fail;
			/* TODO(rnp): this doesn't go here */
			//cmd->cmd->cmddef = command_lookup(vis, cmddef->defcmd);
		} else {
			if (!(cmd->cmd = command_parse(vis, s, err)))
				goto fail;
			if (strcmp(cmd->argv[0], "X") == 0 || strcmp(cmd->argv[0], "Y") == 0) {
				Command *sel = calloc(1, sizeof(*sel));
				if (!sel)
					goto fail;
				sel->cmd = cmd->cmd;
				sel->cmddef = &cmddef_select;
				cmd->cmd = sel;
			}
		}
	}
#endif

	return cmd;
fail:
	command_free(cmd);
	return NULL;
#endif
}

static Filerange
evaluate_address_side(AddressSide as, File *file, Selection *sel, Filerange range)
{
	Filerange result = text_range_empty();

	switch (as.type) {
	case AT_INVALID: ASSERT(0); break;
	case AT_BYTE: {
		result = (Filerange){.start = as.u.number, .end = as.u.number};
	} break;
	case AT_CHARACTER: {
		switch (as.u.character) {
		case '$': {
			size_t size = text_size(file->text);
			result = (Filerange){.start = size, .end = size};
		} break;
		case '.': result = range; break;
		case '%': result = text_range_new(0, text_size(file->text)); break;
		}
	} break;
	case AT_LINE: {
		result = (Filerange){0};
		if (as.u.number) {
			size_t line = text_pos_by_lineno(file->text, as.u.number);
			result = text_range_new(line, text_line_next(file->text, line));
		}
	} break;
	case AT_MARK: {
		size_t pos   = EPOS;
		Array *marks = &file->marks[as.u.mark];
		SelectionRegion *sr = array_get(marks, sel ? view_selections_number(sel) : 0);
		if (sr) pos = text_mark_get(file->text, sr->cursor);
		result = (Filerange){.start = pos, .end = pos};
	} break;
	case AT_REGEX_BACKWARD: {
		result = text_object_search_backward(file->text, range.start, as.u.regex);
	} break;
	case AT_REGEX_FORWARD: {
		result = text_object_search_forward(file->text, range.end, as.u.regex);
	} break;
	}

	return result;
}

static Filerange
evaluate_address(Address addr, File *file, Selection *sel, Filerange range)
{
	Filerange result = text_range_empty();

	switch (addr.delimeter) {
	case '+':
	case '-': {
		/* TODO(rnp): I'm not sure this is does what sam (as Pike described) does.
		 * Re-evaluate after making sure each side evaluates to a single position
		 * (this is supposed to happen but the original vis code did something else) */

		Filerange right = {0};
		if (addr.right.type != AT_INVALID)
			right = evaluate_address_side(addr.right, file, sel, range);

		size_t line;
		if (addr.delimeter == '+') {
			size_t offset = right.end != EPOS? right.end : 1;
			size_t start  = range.start, end = range.end;
			char c;
			if (start < end && text_byte_get(file->text, end-1, &c) && c == '\n')
				end--;
			line = text_lineno_by_pos(file->text, end);
			line = text_pos_by_lineno(file->text, line + offset);
		} else {
			size_t offset = right.start != EPOS? right.start : 1;
			line = text_lineno_by_pos(file->text, range.start);
			line = offset < line ? text_pos_by_lineno(file->text, line - offset) : 0;
		}

		result = text_range_new(line, text_line_next(file->text, line));
	} break;
	case ',':
	case ';': {
		Filerange left = {0}, right;
		if (addr.left.type != AT_INVALID)
			left = evaluate_address_side(addr.left, file, sel, range);

		if (addr.delimeter == ';')
			range = left;

		if (addr.right.type != AT_INVALID) {
			right = evaluate_address_side(addr.right, file, sel, range);
		} else {
			size_t size = text_size(file->text);
			right = (Filerange){.start = size, .end = size};
		}
		/* TODO: enforce strict ordering? */
		result = text_range_union(&left, &right);
	} break;
	}

	return result;
}

static bool count_evaluate(Command *cmd) {
	Count *count = &cmd->count;
	if (count->mod)
		return count->start ? cmd->iteration % count->start == 0 : true;
	return count->start <= cmd->iteration && cmd->iteration <= count->end;
}

static bool count_negative(Command *cmd) {
	if (cmd->count.start < 0 || cmd->count.end < 0)
		return true;
#if 0
	for (Command *c = cmd->cmd; c; c = c->next) {
		if (c->cmddef->fn != cmd_extract && c->cmddef->fn != cmd_select) {
			if (count_negative(c))
				return true;
		}
	}
#endif
	return false;
}

static void count_init(Command *cmd, int max) {
	Count *count = &cmd->count;
	cmd->iteration = 0;
	if (count->start < 0)
		count->start += max;
	if (count->end < 0)
		count->end += max;
	#if 0
	for (Command *c = cmd->cmd; c; c = c->next) {
		if (c->cmddef->fn != cmd_extract && c->cmddef->fn != cmd_select)
			count_init(c, max);
	}
	#endif
}

static Filerange
get_range_for_command(Command *c, Text *txt, size_t current_position, b32 multiple_cursors)
{
	Filerange result;
	if (c->address.left.type != AT_INVALID) {
		/* convert a single line range to a goto line motion */
		#if 0
		if (!multiple_cursors && cmd->cmddef->fn == cmd_print) {
			Address *addr = cmd->address;
			switch (addr->type) {
			case '+':
			case '-':
				addr = addr->right;
				/* fall through */
			case 'l':
				if (addr && addr->type == 'l' && !addr->right)
					addr->type = 'g';
				break;
			}
		}
		#endif
		result = text_range_new(current_position, current_position);
	} else if (c->definition->flags & CMD_ADDRESS_POS) {
		result = text_range_new(current_position, current_position);
	} else if (c->definition->flags & CMD_ADDRESS_LINE) {
		result = text_object_line(txt, current_position);
	} else if (c->definition->flags & CMD_ADDRESS_AFTER) {
		size_t next_line = text_line_next(txt, current_position);
		result = text_range_new(next_line, next_line);
	} else if (c->definition->flags & CMD_ADDRESS_ALL) {
		result = text_range_new(0, text_size(txt));
	} else if (!multiple_cursors && (c->definition->flags & CMD_ADDRESS_ALL_1CURSOR)) {
		result = text_range_new(0, text_size(txt));
	} else {
		result = text_range_new(current_position, text_char_next(txt, current_position));
	}
	return result;
}

static CommandDef *
lookup_command_definition(Vis *vis, SamToken command)
{
	ASSERT(command.type == ST_STRING);
	s8 name = sam_token_to_s8(command);
	CommandDef *result = map_closest(vis->cmds, (char *)push_s8_zero(&vis->sam.arena, name).data);
	return result;
}

static b32
validate_token_stream(SamExecutionState *sam, SamTokenStream *sts)
{
	b32 result  = sts->count > 0;
	i32 nesting = 0;
	for (i32 i = 0; result && i < sts->count; i++) {
		switch (sts->tokens[i].type) {
		case ST_INVALID:     result = 0; break;
		case ST_GROUP_START: nesting++;  break;
		case ST_GROUP_END:   nesting--;  break;
		default: break;
		}
	}
	result = nesting == 0;
	if (nesting != 0) {
		/* TODO(rnp): print unmatched grouping */
	}
	return result;
}

static SAM_CMD_FN(cmd_select)
{
	if (!win) {
		Filerange r = text_range_empty();
		return sam_execute(vis, 0, cmd->cmd, sts, 0, &r);
	}
	bool ret = true;
	View *view = &win->view;
	Text *txt = win->file->text;
	bool multiple_cursors = view->selection_count > 1;
	Selection *primary = view_selections_primary_get(view);

	if (vis->mode->visual)
		count_init(cmd->cmd, view->selection_count + 1);

	for (Selection *s = view_selections(view), *next; s && ret; s = next) {
		Filerange r;
		next = view_selections_next(s);
		if (vis->mode->visual) {
			r = view_selections_get(s);
		} else {
			r = get_range_for_command(cmd->cmd, txt, view_cursors_pos(s), multiple_cursors);
		}
		if (!text_range_valid(&r))
			r = (Filerange){0};
		ret = sam_execute(vis, win, cmd->cmd, sts, s, &r);
		if (cmd->cmd->definition->flags & CMD_ONCE)
			break;
	}

	if (vis->win && &vis->win->view == view && primary != view_selections_primary_get(view))
		view_selections_primary_set(view_selections(view));
	return ret;
}


static SAM_CMD_FN(sam_execute)
{
	bool ret = true;
	if (cmd->address.left.type != AT_INVALID && win)
		*range = evaluate_address(cmd->address, win->file, sel, *range);

	cmd->iteration++;
	switch (cmd->definition->name.data[0]) {
	case '{':
		for (Command *c = cmd->cmd; c && ret; c = c->next)
			ret &= sam_execute(vis, win, c, sts, 0, range);
		view_selections_dispose_force(sel);
		break;
	default:
		ret = cmd->definition->fn(vis, win, cmd, sts, sel, range);
		break;
	}
	return ret;
}

static void
execute_token_stream(Vis *vis, SamTokenStream *sts)
{
	ASSERT(sts->count);

	b32 first_command       = 1;
	b32 did_looping_command = 0;
	i32 nesting_level       = 0;

	Command *command = push_struct(&vis->sam.arena, Command);
	command->address = parse_address(vis, sts);

	while (sts->count != sts->read_index && !vis->sam.should_exit) {
		SamToken token = sam_token_pop(sts);
		switch (token.type) {
		case ST_GROUP_START: {
			Command *new = push_struct(&vis->sam.arena, Command);
			new->prev    = command;
			command = command->next = new;
		} break;
		case ST_GROUP_END: {
			nesting_level--;
			command = command->prev;
			ASSERT(command);
		} break;
		case ST_STRING: {
			command->definition = lookup_command_definition(vis, token);
			if (command->definition) {
				if (did_looping_command && command->definition->flags & CMD_DESTRUCTIVE) {
					/* TODO(rnp): print error: cannot execute destructive command
					 * after a looping command */
					vis->sam.should_exit = 1;
				} else {
					#if 0
					if (first_command) {
						first_command = 0;
					} else {
						//ready_for_command =
					}
					#endif
					Filerange range = text_range_empty();
					command->definition->fn(vis, vis->win, command, sts, 0, &range);
					/* TODO(rnp): do equivalent of cmd_select including execute */
				}
				did_looping_command |= (command->definition->flags & CMD_LOOP) != 0;
			} else {
				sam_error_at(&vis->sam.log, sts, token, "invalid command");
				vis->sam.should_exit = 1;
			}
		} break;
		default: {
			/* TODO(rnp): error: unexpected token at ... */
			vis->sam.should_exit = 1;
		} break;
		}
	}

	if (sts->count != sts->read_index) {
		sam_error_at(&vis->sam.log, sts, sam_token_peek(sts), "extra tokens at end of command");
		for (i32 i = sts->read_index; i < sts->count; i++) {
			buffer_appendf(&vis->sam.log, "token[%d]: ", i);
			/* TODO(rnp): fix buffer_appendf */
			vis->sam.log.len--;
			sam_token_print(&vis->sam.log, sts->tokens[i]);
		}
	}

	ASSERT(nesting_level == 0);
}

enum SamError sam_cmd(Vis *vis, s8 command_line) {
	ASSERT(command_line.len > 0);
	enum SamError err = SAM_ERR_OK;

	vis->sam.arena.start = vis->sam.arena.base;
	vis->sam.should_exit = 0;

	vis->sam.token_stream.start  = vis->sam.token_stream.base;
	SamTokenStream *token_stream = push_struct(&vis->sam.token_stream, SamTokenStream);
	token_stream->backing        = &vis->sam.token_stream;

	ix padding = -(uintptr_t)token_stream->backing->start & (__alignof__(SamToken) - 1);
	token_stream->tokens = (SamToken *)(token_stream->backing->start + padding);
	token_stream->raw    = command_line;

	sam_lex(token_stream, command_line);

	#if 0
	{
		buffer_appendf(&vis->sam.log, "sam_cmd(\":%.*s\"):\n", (i32)command_line.len, command_line.data);
		for (u32 i = 0; i < token_stream->count; i++) {
			buffer_appendf(&vis->sam.log, "token[%d]: ", i);
			/* TODO(rnp): fix buffer_appendf */
			vis->sam.log.len--;
			sam_token_print(&vis->sam.log, token_stream->tokens[i]);
		}
		buffer_append(&vis->sam.log, "\n", 1);
	}
	#endif

	if (validate_token_stream(&vis->sam, token_stream)) {
		for (File *file = vis->files; file; file = file->next) {
			if (file->internal)
				continue;
			file->transcript = (Transcript){0};
		}

		b32    visual      = vis->mode->visual;
		size_t primary_pos = vis->win ? view_cursor_get(&vis->win->view) : EPOS;

		execute_token_stream(vis, token_stream);

		for (File *file = vis->files; file; file = file->next) {
			if (file->internal)
				continue;
			Transcript *t = &file->transcript;
			if (t->error != SAM_ERR_OK) {
				err = t->error;
				sam_transcript_free(t);
				continue;
			}
			vis_file_snapshot(vis, file);
			ptrdiff_t delta = 0;
			for (Change *c = t->changes; c; c = c->next) {
				c->range.start += delta;
				c->range.end += delta;
				if (c->type & TRANSCRIPT_DELETE) {
					text_delete_range(file->text, &c->range);
					delta -= text_range_size(&c->range);
					if (c->sel && c->type == TRANSCRIPT_DELETE) {
						if (visual)
							view_selections_dispose_force(c->sel);
						else
							view_cursors_to(c->sel, c->range.start);
					}
				}
				if (c->type & TRANSCRIPT_INSERT) {
					for (int i = 0; i < c->count; i++) {
						text_insert(file->text, c->range.start, c->data, c->len);
						delta += c->len;
					}
					Filerange r = text_range_new(c->range.start,
					                             c->range.start + c->len * c->count);
					if (c->sel) {
						if (visual) {
							view_selections_set(c->sel, &r);
							c->sel->anchored = true;
						} else {
							if (memchr(c->data, '\n', c->len))
								view_cursors_to(c->sel, r.start);
							else
								view_cursors_to(c->sel, r.end);
						}
					} else if (visual) {
						Selection *sel = view_selections_new(&c->win->view, r.start);
						if (sel) {
							view_selections_set(sel, &r);
							sel->anchored = true;
						}
					}
				}
			}
			sam_transcript_free(&file->transcript);
			vis_file_snapshot(vis, file);
		}

		for (Win *win = vis->windows; win; win = win->next)
			view_selections_normalize(&win->view);

		if (vis->win) {
			if (primary_pos != EPOS && view_selection_disposed(&vis->win->view))
				view_cursors_to(vis->win->view.selection, primary_pos);
			view_selections_primary_set(view_selections(&vis->win->view));
			vis_jumplist_save(vis);
			bool completed = true;
			for (Selection *s = view_selections(&vis->win->view); s; s = view_selections_next(s)) {
				if (s->anchored) {
					completed = false;
					break;
				}
			}
			vis_mode_switch(vis, completed ? VIS_MODE_NORMAL : VIS_MODE_VISUAL);
		}
	}

	return err;
}

/* process text input, substitute register content for backreferences etc. */
Buffer text(Vis *vis, const char *text) {
	Buffer buf = {0};
	for (size_t len = strcspn(text, "\\&"); *text; len = strcspn(++text, "\\&")) {
		buffer_append(&buf, text, len);
		text += len;
		enum VisRegister regid = VIS_REG_INVALID;
		switch (text[0]) {
		case '&':
			regid = VIS_REG_AMPERSAND;
			break;
		case '\\':
			if ('1' <= text[1] && text[1] <= '9') {
				regid = VIS_REG_1 + text[1] - '1';
				text++;
			} else if (text[1] == '\\' || text[1] == '&') {
				text++;
			}
			break;
		case '\0':
			goto out;
		}

		const char *data;
		size_t reglen = 0;
		if (regid != VIS_REG_INVALID) {
			data = register_get(vis, &vis->registers[regid], &reglen);
		} else {
			data = text;
			reglen = 1;
		}
		buffer_append(&buf, data, reglen);
	}
out:
	return buf;
}

static SAM_CMD_FN(cmd_insert)
{
	b32 result = win != 0;
	if (result) {
		if (sam_token_peek(sts).type == ST_STRING) {
			s8 string = sam_token_to_s8(sam_token_pop(sts));
			/* TODO(rnp): malloc/strdup cleanup */
			char *tmp = strndup((char *)string.data, string.len);
			if (tmp) {
				Buffer buf = text(vis, tmp);
				result     = sam_insert(win, sel, range->start, buf.data, buf.len,
				                        cmd->count.start);
				if (!result)
					free(buf.data);
				free(tmp);
			}
		} else {
			/* TODO(rnp): expected string at ... */
		}
	}
	return result;
}

static SAM_CMD_FN(cmd_append)
{
	b32 result = win != 0;
	if (result) {
		if (sam_token_peek(sts).type == ST_STRING) {
			s8 string = sam_token_to_s8(sam_token_pop(sts));
			/* TODO(rnp): malloc/strdup cleanup */
			char *tmp = strndup((char *)string.data, string.len);
			if (tmp) {
				Buffer buf = text(vis, tmp);
				result     = sam_insert(win, sel, range->end, buf.data, buf.len,
				                        cmd->count.start);
				if (!result)
					free(buf.data);
				free(tmp);
			}
		} else {
			/* TODO(rnp): expected string at ... */
		}
	}
	return result;
}

static SAM_CMD_FN(cmd_change)
{
	b32 result = win != 0;
	if (result) {
		if (sam_token_peek(sts).type == ST_STRING) {
			s8 string = sam_token_to_s8(sam_token_pop(sts));
			/* TODO(rnp): malloc/strdup cleanup */
			char *tmp = strndup((char *)string.data, string.len);
			if (tmp) {
				Buffer buf = text(vis, tmp);
				result     = sam_change(win, sel, range, buf.data, buf.len,
				                        cmd->count.start);
				if (!result)
					free(buf.data);
				free(tmp);
			}
		} else {
			/* TODO(rnp): expected string at ... */
		}
	}
	return result;
}

static SAM_CMD_FN(cmd_delete)
{
	return win && sam_delete(win, sel, range);
}

static SAM_CMD_FN(cmd_guard)
{
	if (!win)
		return false;
	bool match = false;
	RegexMatch captures[1];
	size_t len = text_range_size(range);
	if (!cmd->regex)
		match = true;
	else if (!text_search_range_forward(win->file->text, range->start, len, cmd->regex, 1, captures, 0))
		match = captures[0].start < range->end;
	if ((count_evaluate(cmd) && match) ^ (cmd->definition->name.data[0] == 'v'))
		return sam_execute(vis, win, cmd->cmd, sts, sel, range);
	view_selections_dispose_force(sel);
	return true;
}

static i32
extract(Vis *vis, Win *win, Command *cmd, SamTokenStream *sts, Selection *sel,
        Filerange *range, bool simulate)
{
	bool ret = true;
	int count = 0;
	Text *txt = win->file->text;

	/* TODO(rnp): cleanup */
	b32 is_x = cmd->definition->name.data[0] == 'x';

	if (cmd->regex) {
		size_t start = range->start, end = range->end;
		size_t last_start = is_x ? EPOS : start;
		size_t nsub = 1 + text_regex_nsub(cmd->regex);
		if (nsub > MAX_REGEX_SUB)
			nsub = MAX_REGEX_SUB;
		RegexMatch match[MAX_REGEX_SUB];
		while (start <= end) {
			char c;
			int flags = start > range->start &&
			            text_byte_get(txt, start - 1, &c) && c != '\n' ?
			            REG_NOTBOL : 0;
			bool found = !text_search_range_forward(txt, start, end - start,
			                                        cmd->regex, nsub, match,
			                                        flags);
			Filerange r = text_range_empty();
			if (found) {
				if (is_x) r = text_range_new(match[0].start, match[0].end);
				else      r = text_range_new(last_start, match[0].start);
				if (match[0].start == match[0].end) {
					if (last_start == match[0].start) {
						start++;
						continue;
					}
					/* in Plan 9's regexp library ^ matches the beginning
					 * of a line, however in POSIX with REG_NEWLINE ^
					 * matches the zero-length string immediately after a
					 * newline. Try filtering out the last such match at EOF.
					 */
					if (end == match[0].start && start > range->start &&
					    text_byte_get(txt, end-1, &c) && c == '\n')
						break;
					start = match[0].end + 1;
				} else {
					start = match[0].end;
				}
			} else {
				if (!is_x)
					r = text_range_new(start, end);
				start = end + 1;
			}

			if (text_range_valid(&r)) {
				if (found) {
					for (size_t i = 0; i < nsub; i++) {
						Register *reg = &vis->registers[VIS_REG_AMPERSAND+i];
						register_put_range(vis, reg, txt, &match[i]);
					}
					last_start = match[0].end;
				} else {
					last_start = start;
				}
				if (simulate)
					count++;
				else
					ret &= sam_execute(vis, win, cmd->cmd, sts, 0, &r);
			}
		}
	} else {
		size_t start = range->start, end = range->end;
		while (start < end) {
			size_t next = text_line_next(txt, start);
			if (next > end)
				next = end;
			Filerange r = text_range_new(start, next);
			if (start == next || !text_range_valid(&r))
				break;
			if (simulate)
				count++;
			else
				ret = sam_execute(vis, win, cmd->cmd, sts, 0, &r);
			start = next;
		}
	}

	if (!simulate)
		view_selections_dispose_force(sel);
	return simulate ? count : ret;
}

static SAM_CMD_FN(cmd_extract)
{
	b32 result = win && text_range_valid(range);
	if (result) {
		i32 matches = 0;
		if (count_negative(cmd->cmd))
			matches = extract(vis, win, cmd, sts, sel, range, true);
		count_init(cmd->cmd, matches+1);
		result = extract(vis, win, cmd, sts, sel, range, false);
	}
	return result;
}

static SAM_CMD_FN(cmd_print)
{
	if (!win || !text_range_valid(range))
		return false;
	if (!sel)
		sel = view_selections_new_force(&win->view, range->start);
	if (!sel)
		return false;
	if (range->start != range->end) {
		view_selections_set(sel, range);
		sel->anchored = true;
	} else {
		view_cursors_to(sel, range->start);
		view_selection_clear(sel);
	}
	return true;
}

static SAM_CMD_FN(cmd_files)
{
	bool ret = true;
	for (Win *wn, *w = vis->windows; w; w = wn) {
		/* w can get freed by sam_execute() so store w->next early */
		wn = w->next;
		if (w->file->internal)
			continue;
		bool match = !cmd->regex ||
		             (w->file->name && text_regex_match(cmd->regex, w->file->name, 0) == 0);
		if (match ^ (cmd->definition->name.data[0] == 'Y')) {
			Filerange def = (Filerange){0};
			ret = sam_execute(vis, w, cmd->cmd, sts, 0, &def);
		}
	}
	return ret;
}

static SAM_CMD_FN(cmd_substitute)
{
	vis_info_show(vis, "Use :x/pattern/ c/replacement/ instead");
	return false;
}

/* cmd_write stores win->file's contents end emits pre/post events.
 * If the range r covers the whole file, it is updated to account for
 * potential file's text mutation by a FILE_SAVE_PRE callback.
 */
static SAM_CMD_FN(cmd_write)
{
	if (!win)
		return false;

	File *file = win->file;
	if (sam_transcript_error(&file->transcript, SAM_ERR_WRITE_CONFLICT))
		return false;

	Text *text = file->text;
	Filerange range_all = text_range_new(0, text_size(text));
	bool write_entire_file = text_range_equal(range, &range_all);

	b32 filename_is_arg_1 = 0;
	const char *filename  = 0;
	if (sam_token_peek(sts).type == ST_STRING) {
		s8 string = sam_token_to_s8(sam_token_pop(sts));
		filename  = (char *)push_s8_zero(&vis->sam.arena, string).data;
		filename_is_arg_1 = 1;
	}
	if (!filename)
		filename = file->name;
	if (!filename) {
		if (file->fd == -1) {
			vis_info_show(vis, "Filename expected");
			return false;
		}
		#if 0
		/* TODO(rnp) */
		if (!strchr(argv[0], 'q')) {
			vis_info_show(vis, "No filename given, use 'wq' to write to stdout");
			return false;
		}
		#endif

		if (!vis_event_emit(vis, VIS_EVENT_FILE_SAVE_PRE, file, (char*)NULL) && cmd->flags != '!') {
			vis_info_show(vis, "Rejected write to stdout by pre-save hook");
			return false;
		}
		/* a pre-save hook may have changed the text; need to re-take the range */
		if (write_entire_file)
			*range = text_range_new(0, text_size(text));

		bool visual = vis->mode->visual;

		for (Selection *s = view_selections(&win->view); s; s = view_selections_next(s)) {
			Filerange new_range = visual ? view_selections_get(s) : *range;
			ssize_t written = text_write_range(text, &new_range, file->fd);
			if (written == -1 || (size_t)written != text_range_size(&new_range)) {
				vis_info_show(vis, "Can not write to stdout");
				return false;
			}
			if (!visual)
				break;
		}

		/* make sure the file is marked as saved i.e. not modified */
		text_save(text, NULL);
		vis_event_emit(vis, VIS_EVENT_FILE_SAVE_POST, file, (char*)NULL);
		return true;
	}

	if (!filename_is_arg_1 && cmd->flags != '!') {
		if (vis->mode->visual) {
			vis_info_show(vis, "WARNING: file will be reduced to active selection");
			return false;
		}
		if (!write_entire_file) {
			vis_info_show(vis, "WARNING: file will be reduced to provided range");
			return false;
		}
	}

	for (const char **name = (const char*[]){filename, 0}; *name; name++) {
		char *path = absolute_path(*name);
		if (!path)
			return false;

		struct stat meta;
		bool existing_file = !stat(path, &meta);
		bool same_file = existing_file && file->name &&
		                 file->stat.st_dev == meta.st_dev && file->stat.st_ino == meta.st_ino;

		if (cmd->flags != '!') {
			if (same_file && file->stat.st_mtime && file->stat.st_mtime < meta.st_mtime) {
				vis_info_show(vis, "WARNING: file has been changed since reading it");
				goto err;
			}
			if (existing_file && !same_file) {
				vis_info_show(vis, "WARNING: file exists");
				goto err;
			}
		}

		if (!vis_event_emit(vis, VIS_EVENT_FILE_SAVE_PRE, file, path) && cmd->flags != '!') {
			vis_info_show(vis, "Rejected write to `%s' by pre-save hook", path);
			goto err;
		}
		/* a pre-save hook may have changed the text; need to re-take the range */
		if (write_entire_file)
			*range = text_range_new(0, text_size(text));

		TextSave *ctx = text_save_begin(text, AT_FDCWD, path, file->save_method);
		if (!ctx) {
			const char *msg = errno ? strerror(errno) : "try changing `:set savemethod`";
			vis_info_show(vis, "Can't write `%s': %s", path, msg);
			goto err;
		}

		bool failure = false;
		bool visual = vis->mode->visual;

		for (Selection *s = view_selections(&win->view); s; s = view_selections_next(s)) {
			Filerange new_range = visual ? view_selections_get(s) : *range;
			ssize_t written = text_save_write_range(ctx, &new_range);
			failure = (written == -1 || (size_t)written != text_range_size(&new_range));
			if (failure) {
				text_save_cancel(ctx);
				break;
			}

			if (!visual)
				break;
		}

		if (failure || !text_save_commit(ctx)) {
			vis_info_show(vis, "Can't write `%s': %s", path, strerror(errno));
			goto err;
		}

		if (!file->name) {
			file_name_set(file, path);
			same_file = true;
		}
		if (same_file || (!existing_file && strcmp(file->name, path) == 0))
			file->stat = text_stat(text);
		vis_event_emit(vis, VIS_EVENT_FILE_SAVE_POST, file, path);
		free(path);
		continue;

	err:
		free(path);
		return false;
	}
	return true;
}

static SAM_CMD_FN(cmd_filter)
{
	b32 result = 0;
	if (win) {
		if (sam_token_peek(sts).type != ST_INVALID) {
			Buffer bufout = {0}, buferr = {0};

			char **argv = sam_tokens_to_argv(&vis->sam.arena, sts);
			i32 status  = vis_pipe(vis, win->file, range, (const char **)argv, &bufout,
			                       read_into_buffer, &buferr, read_into_buffer, 0);

			if (vis->interrupted) {
				vis_info_show(vis, "Command cancelled");
			} else if (status == 0) {
				char *data  = bufout.data;
				bufout.data = 0;
				if (!sam_change(win, sel, range, data, bufout.len, 1))
					free(data);
			} else {
				vis_info_show(vis, "Command failed: %s", buffer_content0(&buferr));
			}

			buffer_release(&bufout);
			buffer_release(&buferr);

			result = !vis->interrupted && status == 0;
		} else {
			sam_error_at(&vis->sam.log, sts, sam_token_peek(sts),
			             "missing shell command");
		}
	}
	return result;
}

static SAM_CMD_FN(cmd_launch)
{
	Filerange invalid = text_range_new(sel ? view_cursors_pos(sel) : range->start, EPOS);
	return cmd_filter(vis, win, cmd, sts, sel, &invalid);
}

static SAM_CMD_FN(cmd_pipein)
{
	Filerange filter_range = (Filerange){.start = range->end, .end = range->end};
	b32 result = win && cmd_filter(vis, win, cmd, sts, sel, &filter_range);
	if (result)
		result = sam_delete(win, 0, range);
	return result;
}

static SAM_CMD_FN(cmd_pipeout)
{
	b32 result = 0;
	if (win) {
		if (sam_token_peek(sts).type != ST_INVALID) {
			Buffer buferr = {0};

			char **argv = sam_tokens_to_argv(&vis->sam.arena, sts);
			i32 status  = vis_pipe(vis, win->file, range, (const char **)argv, 0, 0,
			                       &buferr, read_into_buffer, 0);

			if (vis->interrupted)
				vis_info_show(vis, "Command cancelled");
			else if (status != 0)
				vis_info_show(vis, "Command failed: %s", buffer_content0(&buferr));

			buffer_release(&buferr);

			result = !vis->interrupted && status == 0;
		} else {
			sam_error_at(&vis->sam.log, sts, sam_token_peek(sts),
			             "missing shell command");
		}
	}
	return result;
}

static SAM_CMD_FN(cmd_cd)
{
	b32 result = 0;
	if (sam_token_peek(sts).type != ST_INVALID) {
		s8 directory = sam_token_to_s8(sam_tokens_join_until_space(sts));
		result       = chdir((char *)push_s8_zero(&vis->sam.arena, directory).data) == 0;
	} else {
		char *directory = getenv("HOME");
		result = directory && chdir(directory) == 0;
	}
	return result;
}

#include "vis-cmds.c"
