/* this file is included from sam.c */

#include <termkey.h>
#include "vis-lua.h"

// FIXME: avoid this redirection?
typedef struct {
	CommandDef definition;
	VisCommandFunction *func;
	void *data;
} CmdUser;

b32 vis_cmd_register(Vis *vis, const char *name, const char *help, void *data, VisCommandFunction *func)
{
	b32 result = 0;
	if (name && (vis->usercmds || (vis->usercmds = map_new()))) {
		/* TODO(rnp): this will leak if the command is unregistered (probably not common)
		 * or overwritten(?) (maybe possible) */
		CmdUser *cmd         = push_struct(&vis->permanent, CmdUser);
		cmd->definition.name = push_s8_zero(&vis->permanent, c_str_to_s8(name));
	#if CONFIG_HELP
		if (help)
			cmd->definition.help = push_s8_zero(&vis->permanent, c_str_to_s8(help));
	#endif
		cmd->definition.flags = CMD_ARGV|CMD_FORCE|CMD_ONCE|CMD_ADDRESS_ALL;
		cmd->definition.fn    = command_user;
		cmd->func = func;
		cmd->data = data;
		if (map_put(vis->cmds, name, &cmd->definition)) {
			if (map_put(vis->usercmds, name, cmd)) {
				result = 1;
			} else {
				map_delete(vis->cmds, name);
			}
		}
	}
	return result;
}

b32 vis_cmd_unregister(Vis *vis, const char *name)
{
	b32 result = name == 0;
	if (!result) {
		CmdUser *cmd = map_get(vis->usercmds, name);
		result = cmd && map_delete(vis->cmds, name) && map_delete(vis->usercmds, name);
	}
	return result;
}

static void option_free(OptionDef *opt) {
	if (!opt)
		return;
	for (size_t i = 0; i < LENGTH(options); i++) {
		if (opt == &options[i])
			return;
	}

	for (const char **name = opt->names; *name; name++)
		free((char*)*name);
	free(VIS_HELP_USE((char*)opt->help));
	free(opt);
}

bool vis_option_register(Vis *vis, const char *names[], enum VisOption flags,
                         VisOptionFunction *func, void *context, const char *help) {

	if (!names || !names[0])
		return false;

	for (const char **name = names; *name; name++) {
		if (map_get(vis->options, *name))
			return false;
	}
	OptionDef *opt = calloc(1, sizeof *opt);
	if (!opt)
		return false;
	for (size_t i = 0; i < LENGTH(opt->names)-1 && names[i]; i++) {
		if (!(opt->names[i] = strdup(names[i])))
			goto err;
	}
	opt->flags = flags;
	opt->func = func;
	opt->context = context;
#if CONFIG_HELP
	if (help && !(opt->help = strdup(help)))
		goto err;
#endif
	for (const char **name = names; *name; name++)
		map_put(vis->options, *name, opt);
	return true;
err:
	option_free(opt);
	return false;
}

bool vis_option_unregister(Vis *vis, const char *name) {
	OptionDef *opt = map_get(vis->options, name);
	if (!opt)
		return false;
	for (const char **alias = opt->names; *alias; alias++) {
		if (!map_delete(vis->options, *alias))
			return false;
	}
	option_free(opt);
	return true;
}

static SAM_CMD_FN(command_user)
{
	/* TODO(rnp): cleanup: map_get needs length and doesn't need NUL */
	char    *name = (char *)push_s8_zero(&vis->sam.arena, command->definition->name).data;
	CmdUser *user = map_get(vis->usercmds, name);
	char **argv   = sam_tokens_to_argv(&vis->sam.arena, sts);
	return user && user->func(vis, win, user->data, command->force, (const char **)argv,
	                          selection, range);
}

void vis_shell_set(Vis *vis, const char *new_shell) {
	char *shell =  strdup(new_shell);
	if (!shell) {
		vis_info_show(vis, "Failed to change shell");
	} else {
		free(vis->shell);
		vis->shell = shell;
	}
}

/* parse human-readable boolean value in s. If successful, store the result in
 * outval and return true. Else return false and leave outval alone. */
static b32
parse_bool(s8 s, b32 *outval)
{
	static s8 true_strs[]  = {s8("1"), s8("true"),  s8("yes"), s8("on")};
	static s8 false_strs[] = {s8("0"), s8("false"), s8("no"),  s8("off")};

	b32 result = 0;
	for (i32 i = 0; !result && i < ARRAY_COUNT(true_strs); i++) {
		if (s8_case_ignore_equal(true_strs[i], s)) {
			*outval = 1;
			result  = 1;
		}
	}
	for (i32 i = 0; !result && i < ARRAY_COUNT(false_strs); i++) {
		if (s8_case_ignore_equal(false_strs[i], s)) {
			*outval = 0;
			result  = 1;
		}
	}
	return result;
}

static SAM_CMD_FN(command_set)
{
	b32 toggle = sam_token_check_pop_force_flag(sts);
	s8  name   = sam_token_to_s8(sam_token_pop(sts));
	toggle     = toggle || sam_token_check_pop_force_flag(sts);

	char *name_zero = (char *)push_s8_zero(&vis->sam.arena, name).data;

	OptionDef *opt = map_closest(vis->options, name_zero);
	if (!opt) {
		vis_info_show(vis, "Unknown option: `%s'", name_zero);
		return false;
	}

	if (opt->flags & VIS_OPTION_DEPRECATED && strcmp(opt->context, name_zero) == 0)
		vis_info_show(vis, "%s is deprecated and will be removed in the next release", name_zero);

	if (!win && (opt->flags & VIS_OPTION_NEED_WINDOW)) {
		vis_info_show(vis, "Need active window for `:set %s'", name_zero);
		return false;
	}

	if (toggle) {
		if (!(opt->flags & VIS_OPTION_TYPE_BOOL)) {
			vis_info_show(vis, "Only boolean options can be toggled");
			return false;
		}
		if (sam_token_peek(sts).type != ST_INVALID) {
			vis_info_show(vis, "Can not specify option value when toggling");
			return false;
		}
	}

	Arg arg;
	if (opt->flags & VIS_OPTION_TYPE_STRING) {
		if (!(opt->flags & VIS_OPTION_VALUE_OPTIONAL) && sam_token_peek(sts).type != ST_STRING) {
			vis_info_show(vis, "Expecting string option value");
			return false;
		}
		/* TODO(rnp): can we just pass the s8 directly? */
		arg.s = (char *)push_s8_zero(&vis->sam.arena, sam_token_to_s8(sam_token_pop(sts))).data;
	} else if (opt->flags & VIS_OPTION_TYPE_BOOL) {
		if (sam_token_peek(sts).type == ST_INVALID) {
			arg.b = !toggle;
		} else {
			s8 arg2 = sam_token_to_s8(sam_token_pop(sts));
			if (!parse_bool(arg2, &arg.b)) {
				vis_info_show(vis, "Expecting boolean option value not: `%*s'",
				              (i32)arg2.len, (char *)arg2.data);
				return false;
			}
		}
	} else if (opt->flags & VIS_OPTION_TYPE_NUMBER) {
		SamToken number = sam_token_try_pop_number(sts);
		if (number.type == ST_INVALID) {
			vis_info_show(vis, "Expecting number");
			return false;
		}

		i64 lval = s8_to_i64(sam_token_to_s8(number));
		if (lval < 0) {
			vis_info_show(vis, "Expecting positive number");
			return false;
		}

		/* TODO(rnp): wtf ??? */
		if (lval > I32_MAX) {
			vis_info_show(vis, "Number overflow");
			return false;
		}

		arg.i = lval;
	} else {
		return false;
	}

	size_t opt_index = 0;
	for (; opt_index < LENGTH(options); opt_index++) {
		if (opt == &options[opt_index])
			break;
	}

	switch (opt_index) {
	case OPTION_SHELL:
		vis_shell_set(vis, arg.s);
		break;
	case OPTION_ESCDELAY:
	{
		termkey_set_waittime(vis->ui.termkey, arg.i);
		break;
	}
	case OPTION_EXPANDTAB:
		vis->win->expandtab = toggle ? !vis->win->expandtab : arg.b;
		break;
	case OPTION_AUTOINDENT:
		vis->autoindent = toggle ? !vis->autoindent : arg.b;
		break;
	case OPTION_TABWIDTH:
		view_tabwidth_set(&vis->win->view, arg.i);
		break;
	case OPTION_SHOW_SPACES:
	case OPTION_SHOW_TABS:
	case OPTION_SHOW_NEWLINES:
	case OPTION_SHOW_EOF:
	case OPTION_STATUSBAR:
	{
		const int values[] = {
			[OPTION_SHOW_SPACES] = UI_OPTION_SYMBOL_SPACE,
			[OPTION_SHOW_TABS] = UI_OPTION_SYMBOL_TAB|UI_OPTION_SYMBOL_TAB_FILL,
			[OPTION_SHOW_NEWLINES] = UI_OPTION_SYMBOL_EOL,
			[OPTION_SHOW_EOF] = UI_OPTION_SYMBOL_EOF,
			[OPTION_STATUSBAR] = UI_OPTION_STATUSBAR,
		};
		int flags = win->options;
		if (arg.b || (toggle && !(flags & values[opt_index])))
			flags |= values[opt_index];
		else
			flags &= ~values[opt_index];
		win_options_set(win, flags);
		break;
	}
	case OPTION_NUMBER: {
		enum UiOption opt = win->options;
		if (arg.b || (toggle && !(opt & UI_OPTION_LINE_NUMBERS_ABSOLUTE))) {
			opt &= ~UI_OPTION_LINE_NUMBERS_RELATIVE;
			opt |=  UI_OPTION_LINE_NUMBERS_ABSOLUTE;
		} else {
			opt &= ~UI_OPTION_LINE_NUMBERS_ABSOLUTE;
		}
		win_options_set(win, opt);
		break;
	}
	case OPTION_NUMBER_RELATIVE: {
		enum UiOption opt = win->options;
		if (arg.b || (toggle && !(opt & UI_OPTION_LINE_NUMBERS_RELATIVE))) {
			opt &= ~UI_OPTION_LINE_NUMBERS_ABSOLUTE;
			opt |=  UI_OPTION_LINE_NUMBERS_RELATIVE;
		} else {
			opt &= ~UI_OPTION_LINE_NUMBERS_RELATIVE;
		}
		win_options_set(win, opt);
		break;
	}
	case OPTION_CURSOR_LINE: {
		enum UiOption opt = win->options;
		if (arg.b || (toggle && !(opt & UI_OPTION_CURSOR_LINE)))
			opt |= UI_OPTION_CURSOR_LINE;
		else
			opt &= ~UI_OPTION_CURSOR_LINE;
		win_options_set(win, opt);
		break;
	}
	case OPTION_COLOR_COLUMN:
		if (arg.i >= 0)
			win->view.colorcolumn = arg.i;
		break;
	case OPTION_SAVE_METHOD:
		if (strcmp("auto", arg.s) == 0) {
			win->file->save_method = TEXT_SAVE_AUTO;
		} else if (strcmp("atomic", arg.s) == 0) {
			win->file->save_method = TEXT_SAVE_ATOMIC;
		} else if (strcmp("inplace", arg.s) == 0) {
			win->file->save_method = TEXT_SAVE_INPLACE;
		} else {
			vis_info_show(vis, "Invalid save method `%s', expected "
			              "'auto', 'atomic' or 'inplace'", arg.s);
			return false;
		}
		break;
	case OPTION_LOAD_METHOD:
		if (strcmp("auto", arg.s) == 0) {
			vis->load_method = TEXT_LOAD_AUTO;
		} else if (strcmp("read", arg.s) == 0) {
			vis->load_method = TEXT_LOAD_READ;
		} else if (strcmp("mmap", arg.s) == 0) {
			vis->load_method = TEXT_LOAD_MMAP;
		} else {
			vis_info_show(vis, "Invalid load method `%s', expected "
			              "'auto', 'read' or 'mmap'", arg.s);
			return false;
		}
		break;
	case OPTION_CHANGE_256COLORS:
		vis->change_colors = toggle ? !vis->change_colors : arg.b;
		break;
	case OPTION_LAYOUT: {
		enum UiLayout layout;
		if (strcmp("h", arg.s) == 0) {
			layout = UI_LAYOUT_HORIZONTAL;
		} else if (strcmp("v", arg.s) == 0) {
			layout = UI_LAYOUT_VERTICAL;
		} else {
			vis_info_show(vis, "Invalid layout `%s', expected 'h' or 'v'", arg.s);
			return false;
		}
		ui_arrange(&vis->ui, layout);
		break;
	}
	case OPTION_IGNORECASE:
		vis->ignorecase = toggle ? !vis->ignorecase : arg.b;
		break;
	case OPTION_BREAKAT:
		if (!view_breakat_set(&win->view, arg.s)) {
			vis_info_show(vis, "Failed to set breakat");
			return false;
		}
		break;
	case OPTION_WRAP_COLUMN:
		if (arg.i >= 0)
			win->view.wrapcolumn = arg.i;
		break;
	default:
		if (!opt->func)
			return false;
		return opt->func(vis, win, opt->context, toggle, opt->flags, name_zero, &arg);
	}

	return true;
}

static s8
file_open_dialog(Vis *vis, s8 file_name_pattern)
{
	s8 result = {0};
	if (file_name_pattern.len) {
		Buffer bufcmd = {0}, bufout = {0}, buferr = {0};

		s8 vis_open = s8(VIS_OPEN " ");
		if (buffer_put(&bufcmd, vis_open.data, vis_open.len) &&
		    buffer_append(&bufcmd, file_name_pattern.data, file_name_pattern.len))
		{
			Filerange empty = {0};
			int status = vis_pipe(vis, vis->win->file, &empty,
			                      (const char*[]){ buffer_content0(&bufcmd), NULL },
			                      &bufout, read_into_buffer, &buferr, read_into_buffer,
			                      false);

			if (status == 0) {
				result = buffer_to_s8(&bufout);
				if (result.len > 1 && result.data[result.len - 1] == 0)
					result.len--;
				result = push_s8_zero(&vis->sam.arena, s8_trim_space(result));
			} else if (status != 1) {
				vis_info_show(vis, "Command failed %s", buffer_content0(&buferr));
			}

			buffer_release(&bufcmd);
			buffer_release(&bufout);
			buffer_release(&buferr);
		}
	}
	return result;
}

static b32
openfiles(Vis *vis, SamTokenStream *sts)
{
	b32 result = 1;
	while (result && sam_token_peek(sts).type != ST_INVALID) {
		s8 file_name_pattern = sam_token_to_s8(sam_tokens_join_until_space(sts));
		s8 interned_name     = file_open_dialog(vis, file_name_pattern);
		if (interned_name.len) {
			if (!vis_window_new(vis, (char *)interned_name.data)) {
				vis_info_show(vis, "Failed to open: %s", (char *)interned_name.data);
				result = 0;
			}
		} else {
			result = 0;
		}
	}
	return result;
}

static SAM_CMD_FN(command_open)
{
	b32 result;
	if (sam_token_peek(sts).type != ST_INVALID) {
		result = openfiles(vis, sts);
	} else {
		result = vis_window_new(vis, 0);
	}
	return result;
}

static void info_unsaved_changes(Vis *vis) {
	vis_info_show(vis, "No write since last change (add ! to override)");
}

static SAM_CMD_FN(command_edit)
{
	ASSERT(win);

	Win *oldwin = win;
	if (!command->force && !vis_window_closable(oldwin)) {
		info_unsaved_changes(vis);
		return false;
	}

	if (sam_token_peek(sts).type == ST_INVALID) {
		if (oldwin->file->refcount > 1) {
			/* TODO(rnp): this makes no sense, just reload all views */
			vis_info_show(vis, "Can not reload file with multiple views");
			return false;
		}
		return vis_window_reload(oldwin);
	}

	s8 file_name_pattern = sam_token_to_s8(sam_tokens_join_until_space(sts));
	if (sam_token_peek(sts).type != ST_INVALID) {
		vis_info_show(vis, "Only 1 filename allowed");
		return false;
	}

	b32 result = 0;
	s8 interned_name = file_open_dialog(vis, file_name_pattern);
	if (interned_name.len) {
		result = vis_window_new(vis, (char *)interned_name.data);
		if (!result)
			vis_info_show(vis, "Could not open: %s", (char *)interned_name.data);
	}

	result = result && vis->win != oldwin;
	if (result) {
		Win *newwin = vis->win;
		vis_window_swap(oldwin, newwin);
		vis_window_close(oldwin);
		vis_window_focus(newwin);
	}

	return result;
}

static SAM_CMD_FN(command_read)
{
	b32 result = 0;
#if 0

	/* TODO(rnp): cleanup */
	i32 count = sts->count - sts->read_index;
	ASSERT(count > 0);
	const char **argv = alloc(&vis->sam.arena, const char *, count + 3 + 1);
	argv[0] = (char *)cmd->definition->name.data;
	argv[1] = "cat";
	argv[2] = "--";

	for (i32 i = 3; i < count + 3; i++) {
		s8 file = sam_token_to_s8(sam_token_pop(sts));
		argv[i] = (char *)push_s8_zero(&vis->sam.arena, file).data;
	}

	const size_t first_file = 3;
	const char *args[MAX_ARGV] = { argv[0], "cat", "--" };
	const char **name = argv[1] ? &argv[1] : (const char*[]){ ".", NULL };
	for (size_t i = first_file; *name && i < LENGTH(args)-1; name++, i++) {
		const char *file = file_open_dialog(vis, *name);
		if (!file || !(args[i] = strdup(file)))
			goto err;
	}
	args[LENGTH(args)-1] = NULL;
	ret = command_pipein(vis, win, cmd, args, sel, range);
err:
	for (size_t i = first_file; i < LENGTH(args); i++)
		free((char*)args[i]);
	#endif

	return result;
}

static bool has_windows(Vis *vis) {
	for (Win *win = vis->windows; win; win = win->next) {
		if (!win->file->internal)
			return true;
	}
	return false;
}

static SAM_CMD_FN(command_quit)
{
	b32 result = 0;
	if (command->force || vis_window_closable(win)) {
		vis_window_close(win);
		if (!has_windows(vis)) {
			i32 exit_code   = EXIT_SUCCESS;
			SamToken number = sam_token_try_pop_number(sts);
			if (number.type != ST_INVALID)
				exit_code = s8_to_i64(sam_token_to_s8(number));
			vis_exit(vis, exit_code);
		}
		result = 1;
	} else {
		info_unsaved_changes(vis);
	}
	return result;
}

static SAM_CMD_FN(command_qall)
{
	b32 result = 0;
	for (Win *next, *win = vis->windows; win; win = next) {
		next = win->next;
		if (!win->file->internal && (!text_modified(win->file->text) || command->force))
			vis_window_close(win);
	}
	if (!has_windows(vis)) {
		i32 exit_code   = EXIT_SUCCESS;
		SamToken number = sam_token_try_pop_number(sts);
		if (number.type != ST_INVALID)
			exit_code = s8_to_i64(sam_token_to_s8(number));
		vis_exit(vis, exit_code);
		result = 1;
	} else {
		info_unsaved_changes(vis);
	}
	return result;
}

static SAM_CMD_FN(command_split)
{
	ASSERT(win);
	b32 result = 0;
	enum UiOption options = win->options;
	ui_arrange(&vis->ui, UI_LAYOUT_HORIZONTAL);
	if (sam_token_peek(sts).type != ST_INVALID) {
		result = openfiles(vis, sts);
		if (result)
			win_options_set(vis->win, options);
	} else {
		result = vis_window_split(win);
	}
	return result;
}

static SAM_CMD_FN(command_vsplit)
{
	ASSERT(win);
	b32 result = 0;
	enum UiOption options = win->options;
	ui_arrange(&vis->ui, UI_LAYOUT_VERTICAL);
	if (sam_token_peek(sts).type != ST_INVALID) {
		result = openfiles(vis, sts);
		if (result)
			win_options_set(vis->win, options);
	} else {
		result = vis_window_split(win);
	}
	return result;
}

static SAM_CMD_FN(command_new)
{
	ui_arrange(&vis->ui, UI_LAYOUT_HORIZONTAL);
	return vis_window_new(vis, NULL);
}

static SAM_CMD_FN(command_vnew)
{
	ui_arrange(&vis->ui, UI_LAYOUT_VERTICAL);
	return vis_window_new(vis, NULL);
}

static SAM_CMD_FN(command_wq)
{
	ASSERT(win);
	b32 result = 0;
	File *file = win->file;
	b32 unmodified = file->fd == -1 && !file->name && !text_modified(file->text);
	if (unmodified || command_write(vis, win, command, sts, selection, range)) {
		SamTokenStream stream = {0};
		result = command_quit(vis, win, command, &stream, selection, range);
	}
	return result;
}

static SAM_CMD_FN(command_earlier_later)
{
	ASSERT(win);

	b32 result = 0;
	#if 0
	Text *txt = win->file->text;
	char *unit = "";
	long count = 1;
	size_t pos = EPOS;
	if (argv[1]) {
		errno = 0;
		count = strtol(argv[1], &unit, 10);
		if (errno || unit == argv[1] || count < 0) {
			vis_info_show(vis, "Invalid number");
			return false;
		}

		if (*unit) {
			while (*unit && isspace((unsigned char)*unit))
				unit++;
			switch (*unit) {
			case 'd': count *= 24; /* fall through */
			case 'h': count *= 60; /* fall through */
			case 'm': count *= 60; /* fall through */
			case 's': break;
			default:
				vis_info_show(vis, "Unknown time specifier (use: s,m,h or d)");
				return false;
			}

			if (argv[0][0] == 'e')
				count = -count; /* earlier, move back in time */

			pos = text_restore(txt, text_state(txt) + count);
		}
	}

	if (!*unit) {
		VisCountIterator it = vis_count_iterator_init(vis, count);
		while (vis_count_iterator_next(&it)) {
			if (argv[0][0] == 'e')
				pos = text_earlier(txt);
			else
				pos = text_later(txt);
		}
	}

	struct tm tm;
	time_t state = text_state(txt);
	char buf[32];
	strftime(buf, sizeof buf, "State from %H:%M", localtime_r(&state, &tm));
	vis_info_show(vis, "%s", buf);

	return pos != EPOS;
	#endif
	return result;
}

static int space_replace(char *dest, const char *src, size_t dlen) {
	int invisiblebytes = 0;
	size_t i, size = LENGTH("␣") - 1;
	for (i = 0; *src && i < dlen; src++) {
		if (*src == ' ' && i < dlen - size - 1) {
			memcpy(&dest[i], "␣", size);
			i += size;
			invisiblebytes += size - 1;
		} else {
			dest[i] = *src;
			i++;
		}
	}
	dest[i] = '\0';
	return invisiblebytes;
}

static bool print_keylayout(const char *key, void *value, void *data) {
	char buf[64];
	int invisiblebytes = space_replace(buf, key, sizeof(buf));
	return text_appendf(data, "  %-*s\t%s\n", 18+invisiblebytes, buf, (char*)value);
}

static bool print_keybinding(const char *key, void *value, void *data) {
	KeyBinding *binding = value;
	const char *desc = binding->alias;
	if (!desc && binding->action)
		desc = VIS_HELP_USE(binding->action->help);
	char buf[64];
	int invisiblebytes = space_replace(buf, key, sizeof(buf));
	return text_appendf(data, "  %-*s\t%s\n", 18+invisiblebytes, buf, desc ? desc : "");
}

static void print_mode(Mode *mode, Text *txt) {
	if (!map_empty(mode->bindings))
		text_appendf(txt, "\n %*s\n\n", (int)mode->name.len, mode->name.data);
	map_iterate(mode->bindings, print_keybinding, txt);
}

static bool print_action(const char *key, void *value, void *data) {
	const char *help = VIS_HELP_USE(((KeyAction*)value)->help);
	return text_appendf(data, "  %-30s\t%s\n", key, help ? help : "");
}

static bool print_cmd(const char *key, void *value, void *data) {
	CommandDef *cmd = value;
	char usage[256];
	b32 is_s = s8_equal(cmd->name, s8("s"));
	snprintf(usage, sizeof usage, "%s%s%s%s%s%s%s",
	         (char *)cmd->name.data,
	         (cmd->flags & CMD_FORCE) ? "[!]" : "",
	         (cmd->flags & CMD_TEXT) ? "/text/" : "",
	         (cmd->flags & CMD_REGEX) ? "/regexp/" : "",
	         (cmd->flags & CMD_CMD) ? " command" : "",
	         (cmd->flags & CMD_SHELL) ? (is_s ? "/regexp/text/" : " shell-command") : "",
	         (cmd->flags & CMD_ARGV) ? " [args...]" : "");
	return text_appendf(data, "  %-30s %.*s\n", usage, (i32)cmd->help.len, (char *)cmd->help.data);
}

static bool print_option(const char *key, void *value, void *txt) {
	char desc[256];
	const OptionDef *opt = value;
	const char *help = VIS_HELP_USE(opt->help);
	if (strcmp(key, opt->names[0]))
		return true;
	snprintf(desc, sizeof desc, "%s%s%s%s%s",
	         opt->names[0],
	         opt->names[1] ? "|" : "",
	         opt->names[1] ? opt->names[1] : "",
	         opt->flags & VIS_OPTION_TYPE_BOOL ? " on|off" : "",
	         opt->flags & VIS_OPTION_TYPE_NUMBER ? " nn" : "");
	return text_appendf(txt, "  %-30s %s\n", desc, help ? help : "");
}

static void print_symbolic_keys(Vis *vis, Text *txt) {
	static const int keys[] = {
		TERMKEY_SYM_BACKSPACE,
		TERMKEY_SYM_TAB,
		TERMKEY_SYM_ENTER,
		TERMKEY_SYM_ESCAPE,
		//TERMKEY_SYM_SPACE,
		TERMKEY_SYM_DEL,
		TERMKEY_SYM_UP,
		TERMKEY_SYM_DOWN,
		TERMKEY_SYM_LEFT,
		TERMKEY_SYM_RIGHT,
		TERMKEY_SYM_BEGIN,
		TERMKEY_SYM_FIND,
		TERMKEY_SYM_INSERT,
		TERMKEY_SYM_DELETE,
		TERMKEY_SYM_SELECT,
		TERMKEY_SYM_PAGEUP,
		TERMKEY_SYM_PAGEDOWN,
		TERMKEY_SYM_HOME,
		TERMKEY_SYM_END,
		TERMKEY_SYM_CANCEL,
		TERMKEY_SYM_CLEAR,
		TERMKEY_SYM_CLOSE,
		TERMKEY_SYM_COMMAND,
		TERMKEY_SYM_COPY,
		TERMKEY_SYM_EXIT,
		TERMKEY_SYM_HELP,
		TERMKEY_SYM_MARK,
		TERMKEY_SYM_MESSAGE,
		TERMKEY_SYM_MOVE,
		TERMKEY_SYM_OPEN,
		TERMKEY_SYM_OPTIONS,
		TERMKEY_SYM_PRINT,
		TERMKEY_SYM_REDO,
		TERMKEY_SYM_REFERENCE,
		TERMKEY_SYM_REFRESH,
		TERMKEY_SYM_REPLACE,
		TERMKEY_SYM_RESTART,
		TERMKEY_SYM_RESUME,
		TERMKEY_SYM_SAVE,
		TERMKEY_SYM_SUSPEND,
		TERMKEY_SYM_UNDO,
		TERMKEY_SYM_KP0,
		TERMKEY_SYM_KP1,
		TERMKEY_SYM_KP2,
		TERMKEY_SYM_KP3,
		TERMKEY_SYM_KP4,
		TERMKEY_SYM_KP5,
		TERMKEY_SYM_KP6,
		TERMKEY_SYM_KP7,
		TERMKEY_SYM_KP8,
		TERMKEY_SYM_KP9,
		TERMKEY_SYM_KPENTER,
		TERMKEY_SYM_KPPLUS,
		TERMKEY_SYM_KPMINUS,
		TERMKEY_SYM_KPMULT,
		TERMKEY_SYM_KPDIV,
		TERMKEY_SYM_KPCOMMA,
		TERMKEY_SYM_KPPERIOD,
		TERMKEY_SYM_KPEQUALS,
	};

	TermKey *termkey = vis->ui.termkey;
	text_appendf(txt, "  ␣ (a literal \" \" space symbol must be used to refer to <Space>)\n");
	for (size_t i = 0; i < LENGTH(keys); i++) {
		text_appendf(txt, "  <%s>\n", termkey_get_keyname(termkey, keys[i]));
	}
}

static SAM_CMD_FN(command_help)
{
	if (!vis_window_new(vis, NULL))
		return false;

	Text *txt = vis->win->file->text;

	text_appendf(txt, "vis %s (PID: %ld)\n\n", VERSION, (long)getpid());

	text_append_s8(txt, s8(" Modes\n\n"));
	for (int i = 0; i < LENGTH(vis_modes); i++) {
		Mode *mode = &vis_modes[i];
		char *help = mode->help.len? (char *)mode->help.data : "";
		text_appendf(txt, "  %-18s\t%s\n", mode->name.data, help);
	}

	if (!map_empty(vis->keymap)) {
		text_appendf(txt, "\n Layout specific mappings (affects all modes except INSERT/REPLACE)\n\n");
		map_iterate(vis->keymap, print_keylayout, txt);
	}

	print_mode(&vis_modes[VIS_MODE_NORMAL], txt);
	print_mode(&vis_modes[VIS_MODE_OPERATOR_PENDING], txt);
	print_mode(&vis_modes[VIS_MODE_VISUAL], txt);
	print_mode(&vis_modes[VIS_MODE_INSERT], txt);

	text_append_s8(txt, s8("\n :-Commands\n\n"));
	for (u32 i = 0; i < ARRAY_COUNT(command_definitions_for_help); i++)
		print_cmd(0, (void *)(command_definitions_for_help + i), txt);
	map_iterate(vis->cmds, print_cmd, txt);

	text_append_s8(txt, s8("\n Marks\n\n"
	                       "  a-z General purpose marks\n"));
	for (size_t i = 0; i < LENGTH(vis_marks); i++) {
		const char *help = VIS_HELP_USE(vis_marks[i].help);
		text_appendf(txt, "  %c   %s\n", vis_marks[i].name, help ? help : "");
	}

	text_append_s8(txt, s8("\n Registers\n\n"
	                       "  a-z General purpose registers\n"
	                       "  A-Z Append to corresponding general purpose register\n"));
	for (size_t i = 0; i < LENGTH(vis_registers); i++) {
		const char *help = VIS_HELP_USE(vis_registers[i].help);
		text_appendf(txt, "  %c   %s\n", vis_registers[i].name, help ? help : "");
	}

	text_append_s8(txt, s8("\n :set command options\n\n"));
	map_iterate(vis->options, print_option, txt);

	text_append_s8(txt, s8("\n Key binding actions\n\n"));
	map_iterate(vis->actions, print_action, txt);

	text_append_s8(txt, s8("\n Symbolic keys usable for key bindings prefix with C-, "
	                       "S-, and M- for Ctrl, Shift and Alt respectively)\n\n"));
	print_symbolic_keys(vis, txt);

	char *paths[] = { NULL, NULL };
	char *paths_description[] = {
		"Lua paths used to load runtime files (? will be replaced by filename):",
		"Lua paths used to load C libraries (? will be replaced by filename):",
	};

	/* TODO(rnp): cleanup */
	if (vis_lua_paths_get(vis, &paths[0], &paths[1])) {
		for (size_t i = 0; i < LENGTH(paths); i++) {
			text_appendf(txt, "\n %s\n\n", paths_description[i]);
			for (char *elem = paths[i], *next; elem; elem = next) {
				if ((next = strstr(elem, ";")))
					*next++ = '\0';
				if (*elem)
					text_appendf(txt, "  %s\n", elem);
			}
			free(paths[i]);
		}
	}

	text_append_s8(txt, s8("\n Compile time configuration\n\n"));

	const struct {
		const char *name;
		bool enabled;
	} configs[] = {
		{ "Curses support: ", CONFIG_CURSES },
		{ "Lua support: ", CONFIG_LUA },
		{ "Lua LPeg statically built-in: ", CONFIG_LPEG },
		{ "TRE based regex support: ", CONFIG_TRE },
		{ "POSIX ACL support: ", CONFIG_ACL },
		{ "SELinux support: ", CONFIG_SELINUX },
	};

	for (size_t i = 0; i < LENGTH(configs); i++)
		text_appendf(txt, "  %-32s\t%s\n", configs[i].name, configs[i].enabled ? "yes" : "no");

	text_save(txt, NULL);
	view_cursors_to(vis->win->view.selection, 0);

	if (sam_token_peek(sts).type != ST_INVALID) {
		s8 search_term = push_s8_zero(&vis->sam.arena,
		                              sam_token_to_s8(sam_tokens_join_until_space(sts)));
		vis_motion(vis, VIS_MOVE_SEARCH_FORWARD, (char *)search_term.data);
	}

	return 1;
}

static SAM_CMD_FN(command_langmap)
{
	return 0;
	#if 0
	const char *nonlatin = argv[1];
	const char *latin = argv[2];
	bool mapped = true;

	if (!latin || !nonlatin) {
		vis_info_show(vis, "usage: langmap <non-latin keys> <latin keys>");
		return false;
	}

	while (*latin && *nonlatin) {
		size_t i = 0, j = 0;
		char latin_key[8], nonlatin_key[8];
		do {
			if (i < sizeof(latin_key)-1)
				latin_key[i++] = *latin;
			latin++;
		} while (!ISUTF8(*latin));
		do {
			if (j < sizeof(nonlatin_key)-1)
				nonlatin_key[j++] = *nonlatin;
			nonlatin++;
		} while (!ISUTF8(*nonlatin));
		latin_key[i] = '\0';
		nonlatin_key[j] = '\0';
		mapped &= vis_keymap_add(vis, nonlatin_key, strdup(latin_key));
	}

	return mapped;
	#endif
}

static SAM_CMD_FN(command_map)
{
	return 0;
	#if 0
	bool mapped = false;
	bool local = strstr(argv[0], "-") != NULL;
	enum VisMode mode = vis_mode_from(vis, c_str_to_s8(argv[1]));

	if (local && !win) {
		vis_info_show(vis, "Invalid window for :%s", argv[0]);
		return false;
	}

	if (mode == VIS_MODE_INVALID || !argv[2] || !argv[3]) {
		vis_info_show(vis, "usage: %s mode lhs rhs", argv[0]);
		return false;
	}

	const char *lhs = argv[2];
	KeyBinding *binding = vis_binding_new(vis);
	if (!binding || !(binding->alias = strdup(argv[3])))
		goto err;

	if (local)
		mapped = vis_window_mode_map(win, mode, cmd->flags == '!', lhs, binding);
	else
		mapped = vis_mode_map(vis, mode, cmd->flags == '!', lhs, binding);

err:
	if (!mapped) {
		vis_info_show(vis, "Failed to map `%s' in %s mode%s", lhs, argv[1],
		              cmd->flags != '!' ? ", mapping already exists, "
		              "override with `!'" : "");
		vis_binding_free(vis, binding);
	}
	return mapped;
	#endif
}

static SAM_CMD_FN(command_unmap)
{
	b32 result        = 0;
	s8 mode_s8        = sam_token_to_s8(sam_token_pop(sts));
	s8 lhs_s8         = sam_token_to_s8(sam_token_pop(sts));
	enum VisMode mode = vis_mode_from(vis, mode_s8);

	if (lhs_s8.len && mode != VIS_MODE_INVALID) {
		b32 window_local = command->definition->name.len > 5;
		char *lhs = (char *)push_s8_zero(&vis->sam.arena, lhs_s8).data;
		if (window_local) result = vis_window_mode_unmap(win, mode, lhs);
		else              result = vis_mode_unmap(vis, mode, lhs);

		if (!result)
			vis_info_show(vis, "failed to unmap `%.*s` in %.*s mode", (i32)lhs_s8.len,
			              (char *)lhs_s8.data, (i32)mode_s8.len, (char *)mode_s8.data);
	} else {
		vis_info_show(vis, "usage: %s mode lhs", (char *)command->definition->name.data);
	}

	return result;
}
