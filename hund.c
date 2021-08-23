/*
 *  Copyright (C) 2017-2018 by Michał Czarnecki <czarnecky@va.pl>
 *
 *  This file is part of the Hund.
 *
 *  The Hund is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  The Hund is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <locale.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include "vi.h"
#ifndef LOGIN_NAME_MAX
#define LOGIN_NAME_MAX _SC_LOGIN_NAME_MAX
#endif

#define PATH_BUF_SIZE (PATH_MAX)
#define PATH_MAX_LEN (PATH_MAX-1)
#define NAME_BUF_SIZE (NAME_MAX+1)
#define NAME_MAX_LEN (NAME_MAX)
#define LOGIN_BUF_SIZE (LOGIN_NAME_MAX+1)
#define LOGIN_MAX_LEN (LOGIN_NAME_MAX)

#ifndef MIN
#define MIN(A,B) (((A) < (B)) ? (A) : (B))
#endif

#define S_ISTOOSPECIAL(M) (((M & S_IFMT) == S_IFBLK) || ((M & S_IFMT) == S_IFCHR) || ((M & S_IFMT) == S_IFIFO) || ((M & S_IFMT) == S_IFSOCK))

#define DOTDOT(N) (!strncmp((N), ".", 2) || !strncmp((N), "..", 3))
#define EXECUTABLE(M) ((M & 0111) && S_ISREG(M))
#define PATH_IS_RELATIVE(P) ((P)[0] != '/' && (P)[0] != '~')

#define MKDIR_DEFAULT_PERM (S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)

#define UPPERCASE(C) ('A' <= (C) && (C) <= 'Z')
#define LOWERCASE(C) ('a' <= (C) && (C) <= 'z')

typedef unsigned int fnum_t;// Number of Files

/* From LSB to MSB, by bit index */

/*
static const char* const mode_bit_meaning[] = {
	"execute/search by others",
	"write by others",
	"read by others",
	"execute/search by group",
	"write by group",
	"read by group",
	"execute/search by user",
	"write by user",
	"read by user",
	"sticky bit",
	"set group ID on execution",
	"set user ID on execution"
};
*/

static const char* const perm2rwx[] = {
	[00] = "---",
	[01] = "--x",
	[02] = "-w-",
	[03] = "-wx",
	[04] = "r--",
	[05] = "r-x",
	[06] = "rw-",
	[07] = "rwx",
};

struct file {
	struct stat s;
	int selected;
	unsigned char nl;
	char name[];
};

char* xstrlcpy(char*, const char*, size_t);
char* get_home(void);
int relative_chmod(const char* const, const mode_t, const mode_t);
int same_fs(const char* const, const char* const);
void file_list_clean(struct file*** const, fnum_t* const);
int scan_dir(const char* const, struct file*** const,
		fnum_t* const, fnum_t* const);
int link_copy_recalculate(const char* const,
		const char* const, const char* const);
int link_copy_raw(const char* const, const char* const);

#define SIZE_BUF_SIZE (5+1)
/*
 * Possible formats:
 * XU
 * XXU
 * XXXU
 * X.XU
 * X.XXU
 */
void pretty_size(off_t, char* buf);
int pushd(char* const, size_t* const, const char* const, size_t);
void popd(char* const, size_t* const);
int cd(char* const, size_t* const, const char* const, size_t);
int prettify_path_i(const char* const);
int current_dir_i(const char* const);
size_t imb(const char*, const char*);
int contains(const char* const, const char* const);

struct string {
	unsigned char len;
	char str[];
};

struct string_list {
	struct string** arr;
	fnum_t len;
};

fnum_t list_push(struct string_list* const, const char* const, size_t);
void list_copy(struct string_list* const, const struct string_list* const);
void list_free(struct string_list* const);
int file_to_list(const int, struct string_list* const);
int list_to_file(const struct string_list* const, int);
fnum_t string_on_list(const struct string_list* const, const char* const, size_t);
fnum_t blank_lines(const struct string_list* const);
int duplicates_on_list(const struct string_list* const);

size_t utf8_g2nb(const char* const);
size_t utf8_cp2nb(const unsigned int);
size_t utf8_width(const char*);
size_t utf8_w2nb(const char* const, size_t);
size_t utf8_wtill(const char*, const char* const);
int utf8_validate(const char* const);
void utf8_insert(char*, const char* const, const size_t);
size_t utf8_remove(char* const, const size_t);
unsigned cut_unwanted(const char*, char*, const char, size_t);

static const char compare_values[] = "nsacmdpxiugUG";
#define FV_ORDER_SIZE (sizeof(compare_values)-1)
enum key {
	KEY_NAME = 'n',
	KEY_SIZE = 's',
	KEY_ATIME = 'a',
	KEY_CTIME = 'c',
	KEY_MTIME = 'm',
	KEY_ISDIR = 'd',
	KEY_PERM = 'p',
	KEY_ISEXE = 'x',
	KEY_INODE = 'i',
	KEY_UID = 'u',
	KEY_GID = 'g',
	KEY_USER = 'U',
	KEY_GROUP = 'G',
};

static const char default_order[FV_ORDER_SIZE] = {
	KEY_NAME,
	KEY_ISEXE,
	KEY_ISDIR
};

enum column {
	COL_NONE = 0,
	COL_INODE,

	COL_LONGSIZE,
	COL_SHORTSIZE,

	COL_LONGPERM,
	COL_SHORTPERM,

	COL_UID,
	COL_USER,
	COL_GID,
	COL_GROUP,

	COL_LONGATIME,
	COL_SHORTATIME,
	COL_LONGCTIME,
	COL_SHORTCTIME,
	COL_LONGMTIME,
	COL_SHORTMTIME,
};

struct panel {
	char wd[PATH_BUF_SIZE];
	size_t wdlen;
	struct file** file_list;
	fnum_t num_files;
	fnum_t num_hidden;
	fnum_t selection;
	fnum_t num_selected;
	int scending;// 1 = ascending, -1 = descending

	char order[FV_ORDER_SIZE];
	enum column column;
	int show_hidden;
};

int visible(const struct panel* const, const fnum_t);
struct file* hfr(const struct panel* const);
void first_entry(struct panel* const);
void last_entry(struct panel* const);
void jump_n_entries(struct panel* const, const int);
void delete_file_list(struct panel* const);
fnum_t file_on_list(const struct panel* const, const char* const);
void file_highlight(struct panel* const, const char* const);
int file_find(struct panel* const, const char* const,
		const fnum_t, const fnum_t);
struct file* panel_select_file(struct panel* const);
int panel_enter_selected_dir(struct panel* const);
int panel_up_dir(struct panel* const);
void panel_toggle_hidden(struct panel* const);
int panel_scan_dir(struct panel* const);
void panel_sort(struct panel* const);
char* panel_path_to_selected(struct panel* const);
void panel_sorting_changed(struct panel* const);
void panel_selected_to_list(struct panel* const, struct string_list* const);
void select_from_list(struct panel* const, const struct string_list* const);
void panel_unselect_all(struct panel* const);
/*
 * TODO find a better name
 */

struct assign {
	fnum_t from, to;
};

int rename_prepare(const struct panel* const, struct string_list* const,
		struct string_list* const, struct string_list* const,
		struct assign** const, fnum_t* const);
int conflicts_with_existing(struct panel* const,
		const struct string_list* const);
void remove_conflicting(struct panel* const, struct string_list* const);

#define ESC_TIMEOUT_MS 125
#define CTRL_KEY(K) ((K) & 0x1f)

enum input_type {
	I_NONE = 0,
	I_UTF8,// utf will contain zero-terminated bytes of the glyph

	I_CTRL,// utf[0] will contain character

	I_ARROW_UP,
	I_ARROW_DOWN,
	I_ARROW_RIGHT,
	I_ARROW_LEFT,
	I_HOME,
	I_END,
	I_PAGE_UP,
	I_PAGE_DOWN,
	I_INSERT,
	I_BACKSPACE,
	I_DELETE,
	I_ESCAPE,
};

struct input {
	enum input_type t : 8;
	char utf[5];
};

struct s2i {// Sequence to Input

	char* seq;
	enum input_type t : 8;
};
// Sequence -> Key Mapping

static const struct s2i SKM[] = {
	{ "\x1b[@", I_INSERT },
	{ "\x1b[A", I_ARROW_UP },
	{ "\x1b[B", I_ARROW_DOWN },
	{ "\x1b[C", I_ARROW_RIGHT },
	{ "\x1b[D", I_ARROW_LEFT },
	{ "\x1b[H", I_HOME },
	{ "\x1b[F", I_END },
	{ "\x1b[P", I_DELETE },
	{ "\x1b[V", I_PAGE_UP },
	{ "\x1b[U", I_PAGE_DOWN },
	{ "\x1b[Y", I_END },

	{ "\x1bOA", I_ARROW_UP },
	{ "\x1bOB", I_ARROW_DOWN },
	{ "\x1bOC", I_ARROW_RIGHT },
	{ "\x1bOD", I_ARROW_LEFT },
	{ "\x1bOH", I_HOME },
	{ "\x1bOF", I_END },

	{ "\x1b[1~", I_HOME },
	{ "\x1b[3~", I_DELETE },
	{ "\x1b[4~", I_END },
	{ "\x1b[5~", I_PAGE_UP },
	{ "\x1b[6~", I_PAGE_DOWN },
	{ "\x1b[7~", I_HOME },
	{ "\x1b[8~", I_END },
	{ "\x1b[4h", I_INSERT },
	{ "\x1b", I_ESCAPE },
	{ NULL, I_NONE },
};

int start_raw_mode(struct termios* const);
int stop_raw_mode(struct termios* const);
struct input get_input(int);

enum char_attr {
	ATTR_NORMAL = 0,
	ATTR_BOLD = 1,
	ATTR_FAINT = 2,
	ATTR_ITALIC = 3,
	ATTR_UNDERLINE = 4,
	ATTR_BLINK = 5,
	ATTR_INVERSE = 7,
	ATTR_INVISIBLE = 8,

	ATTR_NOT_BOLD_OR_FAINT = 22,
	ATTR_NOT_ITALIC = 23,
	ATTR_NOT_UNDERLINE = 24,
	ATTR_NOT_BLINK = 25,
	ATTR_NOT_INVERSE = 27,
	ATTR_NOT_INVISIBLE = 28,

	ATTR_BLACK = '0',
	ATTR_RED = '1',
	ATTR_GREEN = '2',
	ATTR_YELLOW = '3',
	ATTR_BLUE = '4',
	ATTR_MAGENTA = '5',
	ATTR_CYAN = '6',
	ATTR_WHITE = '7',
	ATTR_DEFAULT = '9',

	ATTR_FOREGROUND = 1<<8,
	ATTR_BACKGROUND = 1<<9,
	ATTR_COLOR_256 = 1<<10,
	ATTR_COLOR_TRUE = 1<<11,
};

int char_attr(char* const, const size_t, const int,
		const unsigned char* const);
int move_cursor(const unsigned int, const unsigned int);
int window_size(int* const, int* const);

#define APPEND_BUFFER_INC 64
struct append_buffer {
	char* buf;
	size_t top;
	size_t capacity;
};

size_t append(struct append_buffer* const, const char* const, const size_t);
size_t append_attr(struct append_buffer* const, const int,
		const unsigned char* const);
size_t fill(struct append_buffer* const, const char, const size_t);

#define MSG_BUFFER_SIZE 128
#define KEYNAME_BUF_SIZE 16
// TODO adjust KEYNAME_BUF_SIZE

enum mode {
	MODE_MANAGER = 0,
	MODE_CHMOD,
	MODE_WAIT,
	MODE_NUM
};

enum command {
	CMD_NONE = 0,

	CMD_QUIT,
	CMD_QQUIT,
	CMD_HELP,

	CMD_COPY,
	CMD_MOVE,
	CMD_REMOVE,
	CMD_CREATE_DIR,
	CMD_RENAME,

	CMD_LINK,

	CMD_UP_DIR,
	CMD_EDIT_FILE,
	CMD_ENTER_DIR,


	CMD_ENTRY_UP,
	CMD_ENTRY_DOWN,

	CMD_SCREEN_UP,
	CMD_SCREEN_DOWN,

	CMD_ENTRY_FIRST,
	CMD_ENTRY_LAST,

	CMD_COMMAND,
	CMD_CD,

	CMD_REFRESH,
	CMD_SWITCH_PANEL,
	CMD_DUP_PANEL,
	CMD_SWAP_PANELS,

	CMD_DIR_VOLUME,
	CMD_TOGGLE_HIDDEN,

	CMD_SORT_REVERSE,
	CMD_SORT_CHANGE,

	CMD_COL,

	CMD_SELECT_FILE,
	CMD_SELECT_ALL,
	CMD_SELECT_NONE,

	CMD_SELECTED_NEXT,
	CMD_SELECTED_PREV,

	CMD_MARK_NEW,
	CMD_MARK_JUMP,

	CMD_FIND,

	CMD_CHMOD,
	CMD_CHANGE,
	CMD_RETURN,
	CMD_CHOWN,
	CMD_CHGRP,

	CMD_A,
	CMD_U,
	CMD_G,
	CMD_O,
	CMD_PL,
	CMD_MI,

	CMD_TASK_QUIT,
	CMD_TASK_PAUSE,
	CMD_TASK_RESUME,

	CMD_NUM,
};

enum theme_element {
	THEME_OTHER = 0,
	THEME_PATHBAR,
	THEME_STATUSBAR,
	THEME_ERROR,
	THEME_INFO,
	THEME_HINT_KEY,
	THEME_HINT_DESC,
	THEME_ENTRY_BLK_UNS,// unselected

	THEME_ENTRY_BLK_SEL,// selected

	THEME_ENTRY_CHR_UNS,
	THEME_ENTRY_CHR_SEL,
	THEME_ENTRY_FIFO_UNS,
	THEME_ENTRY_FIFO_SEL,
	THEME_ENTRY_REG_UNS,
	THEME_ENTRY_REG_SEL,
	THEME_ENTRY_REG_EXE_UNS,
	THEME_ENTRY_REG_EXE_SEL,
	THEME_ENTRY_DIR_UNS,
	THEME_ENTRY_DIR_SEL,
	THEME_ENTRY_SOCK_UNS,
	THEME_ENTRY_SOCK_SEL,
	THEME_ENTRY_LNK_UNS,
	THEME_ENTRY_LNK_SEL,

	THEME_ELEM_NUM
};

struct theme_attrs {
	int fg, bg;
	unsigned char fg_color[3], bg_color[3];
};

#define S_IFMT_TZERO 12
// ^ Trailing ZEROes

static const char mode_type_symbols[] = {
	[S_IFIFO>>S_IFMT_TZERO] = 'p',
	[S_IFCHR>>S_IFMT_TZERO] = 'c',
	[S_IFDIR>>S_IFMT_TZERO] = 'd',
	[S_IFBLK>>S_IFMT_TZERO] = 'b',
	[S_IFREG>>S_IFMT_TZERO] = '-',
	[S_IFLNK>>S_IFMT_TZERO] = 'l',
	[S_IFSOCK>>S_IFMT_TZERO] = 's',
};

static const char file_symbols[] = {
	[THEME_ENTRY_BLK_UNS] = '+',
	[THEME_ENTRY_CHR_UNS] = '-',
	[THEME_ENTRY_FIFO_UNS] = '|',
	[THEME_ENTRY_REG_UNS] = ' ',
	[THEME_ENTRY_REG_EXE_UNS] = '*',
	[THEME_ENTRY_DIR_UNS] = '/',
	[THEME_ENTRY_SOCK_UNS] = '=',
	[THEME_ENTRY_LNK_UNS] = '~',
};

static const struct theme_attrs theme_scheme[THEME_ELEM_NUM] = {
	[THEME_OTHER] = { ATTR_NORMAL, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_PATHBAR] = { ATTR_BLACK, ATTR_WHITE, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_STATUSBAR] = { ATTR_BLACK, ATTR_WHITE, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ERROR] = { ATTR_NORMAL, ATTR_RED, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_INFO] = { ATTR_WHITE, ATTR_BLACK, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_BLK_UNS] = { ATTR_RED, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_BLK_SEL] = { ATTR_NORMAL, ATTR_RED, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_CHR_UNS] = { ATTR_YELLOW, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_CHR_SEL] = { ATTR_NORMAL, ATTR_YELLOW, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_FIFO_UNS] = { ATTR_GREEN, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_FIFO_SEL] = { ATTR_NORMAL, ATTR_GREEN, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_REG_UNS] = { ATTR_NORMAL, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_REG_SEL] = { ATTR_NORMAL, ATTR_RED, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_REG_EXE_UNS] = { ATTR_MAGENTA, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_REG_EXE_SEL] = { ATTR_NORMAL, ATTR_MAGENTA, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_DIR_UNS] = { ATTR_CYAN, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_DIR_SEL] = { ATTR_NORMAL, ATTR_CYAN, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_SOCK_UNS] = { ATTR_GREEN, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_SOCK_SEL] = { ATTR_NORMAL, ATTR_GREEN, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_LNK_UNS] = { ATTR_CYAN, ATTR_NORMAL, { 0, 0, 0 }, { 0, 0, 0 } },
	[THEME_ENTRY_LNK_SEL] = { ATTR_NORMAL, ATTR_CYAN, { 0, 0, 0 }, { 0, 0, 0 } },
};

#define INPUT_LIST_LENGTH 4

struct input2cmd {
	struct input i[INPUT_LIST_LENGTH];
	enum mode m : 8;
	enum command c : 8;
};

#define IS_CTRL(I,K) (((I).t == I_CTRL) && ((I).utf[0] == (K)))
#define KUTF8(K) { .t = I_UTF8, .utf = K }
#define KSPEC(K) { .t = (K) }
#define KCTRL(K) { .t = I_CTRL, .utf[0] = (K) }

static struct input2cmd default_mapping[] = {
/* MODE MANGER */
	{ { KUTF8("q"), KUTF8("q") }, MODE_MANAGER, CMD_QUIT },
	{ { KUTF8("Q") }, MODE_MANAGER, CMD_QQUIT },

	{ { KUTF8("g"), KUTF8("g") }, MODE_MANAGER, CMD_ENTRY_FIRST },
	{ { KSPEC(I_HOME) }, MODE_MANAGER, CMD_ENTRY_FIRST },

	{ { KUTF8("G") }, MODE_MANAGER, CMD_ENTRY_LAST },
	{ { KSPEC(I_END) }, MODE_MANAGER, CMD_ENTRY_LAST },

	{ { KCTRL('B') }, MODE_MANAGER, CMD_SCREEN_UP },
	{ { KCTRL('F') }, MODE_MANAGER, CMD_SCREEN_DOWN },

	{ { KUTF8("j") }, MODE_MANAGER, CMD_ENTRY_DOWN },
	{ { KCTRL('N') }, MODE_MANAGER, CMD_ENTRY_DOWN },
	{ { KSPEC(I_ARROW_DOWN) }, MODE_MANAGER, CMD_ENTRY_DOWN },

	{ { KUTF8("k") }, MODE_MANAGER, CMD_ENTRY_UP },
	{ { KCTRL('P') }, MODE_MANAGER, CMD_ENTRY_UP },
	{ { KSPEC(I_ARROW_UP) }, MODE_MANAGER, CMD_ENTRY_UP },

	{ { KUTF8("g"), KUTF8("c") }, MODE_MANAGER, CMD_COPY },
	{ { KUTF8("g"), KUTF8("r") }, MODE_MANAGER, CMD_REMOVE },
	{ { KUTF8("g"), KUTF8("n") }, MODE_MANAGER, CMD_RENAME },
	{ { KUTF8("g"), KUTF8("m") }, MODE_MANAGER, CMD_MOVE },
	{ { KUTF8("g"), KUTF8("l")}, MODE_MANAGER, CMD_LINK },

	{ { KCTRL('I') }, MODE_MANAGER, CMD_SWITCH_PANEL },
	{ { KUTF8(" ") }, MODE_MANAGER, CMD_SWITCH_PANEL },

	{ { KUTF8("z") }, MODE_MANAGER, CMD_DUP_PANEL },
	{ { KUTF8("Z") }, MODE_MANAGER, CMD_SWAP_PANELS },

	{ { KUTF8("r"), KUTF8("r") }, MODE_MANAGER, CMD_REFRESH },
	{ { KCTRL('L') }, MODE_MANAGER, CMD_REFRESH },

	{ { KUTF8("g"), KUTF8("d") }, MODE_MANAGER, CMD_CREATE_DIR },

	{ { KUTF8("u") }, MODE_MANAGER, CMD_UP_DIR },
	{ { KUTF8("h") }, MODE_MANAGER, CMD_UP_DIR },
	{ { KSPEC(I_BACKSPACE) }, MODE_MANAGER, CMD_UP_DIR },

	{ { KUTF8("i") }, MODE_MANAGER, CMD_EDIT_FILE },
	{ { KCTRL('M') }, MODE_MANAGER, CMD_ENTER_DIR },
	{ { KCTRL('J') }, MODE_MANAGER, CMD_ENTER_DIR },
	{ { KUTF8("l") }, MODE_MANAGER, CMD_ENTER_DIR },
/* <TODO> */
	{ { KUTF8("v") }, MODE_MANAGER, CMD_SELECT_FILE },
	{ { KUTF8("V") }, MODE_MANAGER, CMD_SELECT_ALL },
	{ { KUTF8("0") }, MODE_MANAGER, CMD_SELECT_NONE },

	{ { KUTF8(".") }, MODE_MANAGER, CMD_SELECTED_NEXT },
	{ { KUTF8(",") }, MODE_MANAGER, CMD_SELECTED_PREV },
/* </TODO> */
	{ { KUTF8("m") }, MODE_MANAGER, CMD_MARK_NEW },
	{ { KUTF8("'") }, MODE_MANAGER, CMD_MARK_JUMP },

	{ { KUTF8("/") }, MODE_MANAGER, CMD_FIND },
	{ { KCTRL('V') }, MODE_MANAGER, CMD_DIR_VOLUME },

	{ { KUTF8("x") }, MODE_MANAGER, CMD_TOGGLE_HIDDEN },

	{ { KUTF8("?") }, MODE_MANAGER, CMD_HELP },

	{ { KUTF8(":") }, MODE_MANAGER, CMD_COMMAND },
	{ { KUTF8("c"), KUTF8("d") }, MODE_MANAGER, CMD_CD },

	{ { KUTF8("c"), KUTF8("c") }, MODE_MANAGER, CMD_CHMOD },

	{ { KUTF8("s"), KUTF8("r") }, MODE_MANAGER, CMD_SORT_REVERSE },
	{ { KUTF8("s"), KUTF8("c") }, MODE_MANAGER, CMD_SORT_CHANGE },

	{ { KUTF8("t") }, MODE_MANAGER, CMD_COL, },
/* MODE CHMOD */
	{ { KUTF8("q"), KUTF8("q") }, MODE_CHMOD, CMD_RETURN },
	{ { KUTF8("c"), KUTF8("c") }, MODE_CHMOD, CMD_CHANGE },

	{ { KUTF8("c"), KUTF8("o") }, MODE_CHMOD, CMD_CHOWN },
	{ { KUTF8("c"), KUTF8("g") }, MODE_CHMOD, CMD_CHGRP },

	{ { KUTF8("a") }, MODE_CHMOD, CMD_A, },
	{ { KUTF8("u") }, MODE_CHMOD, CMD_U, },
	{ { KUTF8("g") }, MODE_CHMOD, CMD_G, },
	{ { KUTF8("o") }, MODE_CHMOD, CMD_O, },
	{ { KUTF8("+") }, MODE_CHMOD, CMD_PL, },
	{ { KUTF8("-") }, MODE_CHMOD, CMD_MI, },
/* MODE WAIT */
	{ { KUTF8("q"), KUTF8("q") }, MODE_WAIT, CMD_TASK_QUIT },
	{ { KUTF8("p"), KUTF8("p") }, MODE_WAIT, CMD_TASK_PAUSE },
	{ { KUTF8("r"), KUTF8("r") }, MODE_WAIT, CMD_TASK_RESUME },

};

static const size_t default_mapping_length =
	(sizeof(default_mapping)/sizeof(default_mapping[0]));

static const char* const cmd_help[] = {
	[CMD_QUIT] = "Quit hund",
	[CMD_QQUIT] = "Quit hund but retain state",
	[CMD_HELP] = "Display help",

	[CMD_COPY] = "Copy selected file to the other directory",
	[CMD_MOVE] = "Move selected file to the other directory",
	[CMD_REMOVE] = "Remove selected file",
	[CMD_CREATE_DIR] = "Create new directories",
	[CMD_RENAME] = "Rename selected files",

	[CMD_LINK] = "Create symlinks to selected files",

	[CMD_UP_DIR] = "Go up in directory tree",
	[CMD_EDIT_FILE] = "Edit highlighted file",
	[CMD_ENTER_DIR] = "Enter highlighted directory or open file",

	[CMD_ENTRY_UP] = "Go to previous entry",
	[CMD_ENTRY_DOWN] = "Go to next entry",

	[CMD_SCREEN_UP] = "Scroll 1 screen up",
	[CMD_SCREEN_DOWN] = "Scroll 1 screen down",

	[CMD_ENTRY_FIRST] = "Go to the top file in directory",
	[CMD_ENTRY_LAST] = "Go to the bottom file in directory",

	[CMD_COMMAND] = "Open command line",

	[CMD_CD] = "Jump to some directory",
	[CMD_REFRESH] = "Rescan directories and redraw UI",
	[CMD_SWITCH_PANEL] = "Switch active panel",
	[CMD_DUP_PANEL] = "Open current directory in the other panel",
	[CMD_SWAP_PANELS] = "Swap panels",

	[CMD_DIR_VOLUME] = "Calcualte volume of selected directory",
	[CMD_TOGGLE_HIDDEN] = "Toggle between hiding/showing hidden files",

	[CMD_SORT_REVERSE] = "Switch between ascending/descending sorting",
	[CMD_SORT_CHANGE] = "Change sorting",

	[CMD_COL] = "Change column",

	[CMD_SELECT_FILE] = "Select/unselect file",
	[CMD_SELECT_ALL] = "Select all visible files",
	[CMD_SELECT_NONE] = "Unselect all files",

	[CMD_SELECTED_NEXT] = "Jump to the next selected file",
	[CMD_SELECTED_PREV] = "Jump to the previous selected file",

	[CMD_MARK_NEW] = "Set mark at highlighted file",
	[CMD_MARK_JUMP] = "Jump to a mark",
	[CMD_FIND] = "Search for files in current directory (note: ^P and ^N moves up and down ^V can select)",

	[CMD_CHMOD] = "Change permissions of selected files",
	[CMD_CHANGE] = "Apply changes and return",
	[CMD_RETURN] = "Abort changes and return",
	[CMD_CHOWN] = "Change owner of file",
	[CMD_CHGRP] = "Change group of file",

	[CMD_A] = "Modify permissions for all",
	[CMD_U] = "Modify permissions for owner",
	[CMD_G] = "Modify permissions for group",
	[CMD_O] = "Modify permissions for others",
	[CMD_PL] = "Add permissions for all",
	[CMD_MI] = "Remove permissions for all",

	[CMD_TASK_QUIT] = "Abort task",
	[CMD_TASK_PAUSE] = "Pause task",
	[CMD_TASK_RESUME] = "Resume task",

	[CMD_NUM] = NULL,
};

static const char* const mode_strings[] = {
	[MODE_CHMOD] = "CHMOD",
	[MODE_MANAGER] = "FILE VIEW",
	[MODE_WAIT] = "WAIT",
};

static const char* const more_help[] = {
	"COMMAND LINE",
	"Press `:` to enter command line",
	"q\tExit hund",
	"h/help\tOpen help",
	"lm\tList marks",
	"noh/nos\tClear selection",
	"+x\tQuick chmod +x",
	"sh\tOpen shell",
	"sh ...\tExecute command in shell",
	"",
	"SORTING",
	"+\tascending",
	"-\tdescending",
	"n\tsort by name",
	"s\tsort by size",
	"a\tsort by atime",
	"c\tsort by ctime",
	"m\tsort by mtime",
	"d\tdirectories first*",
	"p\tsort by permission",
	"x\texecutables first*",
	"i\tsort by inode number",
	"u\tsort by uid",
	"U\tsort by user name",
	"g\tsort by gid",
	"G\tsort by group name",
	"*\treversing order of sorting makes them last",
	"Most important sorting key is at the end",
	"EXAMPLES",
	"+d\tdirectories are first",
	"-d\tdirectories are last",
	"+xd\tdirectories are first",
	"+dx\texecutables are first",
	"+nx\texecutables are first, then within each block",
	"   \t(executables and others) files are sorted by name",
	"",
	"COLUMN",
	"Only one column at a time can be visible",
	"t\thide column",
	"s/S\tshort/long size",
	"a/A\tshort/long atime",
	"c/C\tshort/long ctime",
	"m/M\tshort/long mtime",
	"p/P\tshort/long permission",
	"i\tinode number",
	"u\tuid",
	"U\tuser name",
	"g\tgid",
	"G\tgroup name",
	"",
	"MARKS",
	"m<letter>\tSet mark",
	"'<letter>\tGoto mark",
	"[a-z]\tSave path to highlighted file.",
	"[A-Z]\tSave only the working directory.",
	"     \tJump to the working directory and highlight first entry.",
	"",
	"CHMOD",
	"",// TODO

	NULL,
};

enum msg_type {
	MSG_NONE = 0,
	MSG_INFO = 1<<0,
//MSG_WARNING,
	MSG_ERROR = 1<<1,
};

static const char* const timefmt = "%Y-%m-%d %H:%M:%S";
#define TIME_SIZE (4+1+2+1+2+1+2+1+2+1+2+1)

enum {
	BUF_PATHBAR = 0,
	BUF_PANELS,
	BUF_STATUSBAR,
	BUF_BOTTOMBAR,
	BUF_NUM
};

enum dirty_flag {
	DIRTY_PATHBAR = 1<<BUF_PATHBAR,
	DIRTY_PANELS = 1<<BUF_PANELS,
	DIRTY_STATUSBAR = 1<<BUF_STATUSBAR,
	DIRTY_BOTTOMBAR = 1<<BUF_BOTTOMBAR,
	DIRTY_ALL = DIRTY_PATHBAR|DIRTY_PANELS|DIRTY_STATUSBAR|DIRTY_BOTTOMBAR,
};

struct ui {
	int scrh, scrw;// Last window dimensions
	int pw[2];// Panel Width
	int ph;// Panel Height
	int pxoff[2];// Panel X OFFset
	int run;
	enum mode m;
	enum msg_type mt;
	char msg[MSG_BUFFER_SIZE];
	char prch[16];// TODO adjust size
	char* prompt;
	int prompt_cursor_pos;
	int timeout;// microseconds
	struct append_buffer B[BUF_NUM];
	enum dirty_flag dirty;
	struct termios T;
	struct panel* fvs[2];
	struct panel* pv;
	struct panel* sv;
	struct input2cmd* kmap;
	size_t kml;// Key Mapping Length
	struct input K[INPUT_LIST_LENGTH];
	char* path;// path of chmodded file
// [0] = old value
// [1] = new/edited value
	mode_t perm[2];// permissions of chmodded file
	mode_t plus, minus;
	uid_t o[2];
	gid_t g[2];
	char perms[10];
	char time[TIME_SIZE];
	char user[LOGIN_BUF_SIZE];
	char group[LOGIN_BUF_SIZE];
};
struct ui* global_i;

typedef void (*draw_t)(struct ui* const, struct append_buffer* const);
void ui_pathbar(struct ui* const, struct append_buffer* const);
void ui_panels(struct ui* const, struct append_buffer* const);
void ui_statusbar(struct ui* const, struct append_buffer* const);
void ui_bottombar(struct ui* const, struct append_buffer* const);

static const draw_t do_draw[] = {
	[BUF_PATHBAR] = ui_pathbar,
	[BUF_PANELS] = ui_panels,
	[BUF_STATUSBAR] = ui_statusbar,
	[BUF_BOTTOMBAR] = ui_bottombar,
};

void ui_init(struct ui* const, struct panel* const,
		struct panel* const);
void ui_end(struct ui* const);
int help_to_fd(struct ui* const, const int);
void ui_draw(struct ui* const);
void ui_update_geometry(struct ui* const);
int chmod_open(struct ui* const, char* const);
void chmod_close(struct ui* const);

struct select_option {
	struct input i;
	char* h;// Hint

};

int ui_ask(struct ui* const, const char* const q,
		const struct select_option*, const size_t);
enum command get_cmd(struct ui* const);
int fill_textbox(struct ui* const, char* const,
		char** const, const size_t, struct input* const);
int prompt(struct ui* const, char* const, char*, const size_t);
void failed(struct ui* const, const char* const, const char* const);
int ui_rescan(struct ui* const, struct panel* const,
		struct panel* const);

enum spawn_flags {
	SF_SLIENT = 1<<0,
};

int spawn(char* const[], const enum spawn_flags);
size_t append_theme(struct append_buffer* const, const enum theme_element);

#if defined(__OpenBSD__)
#define HAS_FALLOCATE 0
#else
#define HAS_FALLOCATE 1
#endif
typedef unsigned long long xtime_ms_t;

xtime_ms_t xtime(void);

enum task_type {
	TASK_NONE = 0,
	TASK_REMOVE = 1<<0,
	TASK_COPY = 1<<1,
	TASK_MOVE = 1<<2,
	TASK_CHMOD = 1<<3,
};
/*
 * If Link Transparency is 1 in tree_walk,
 * tree_walk_step will output AT_FILE or AT_DIR,
 * If LT is 0, then AT_LINK is outputted
 */

enum tree_walk_state {
	AT_NOWHERE = 0,
	AT_EXIT = 1<<0,// finished reading tree
	AT_FILE = 1<<1,
	AT_LINK = 1<<2,
	AT_DIR = 1<<3,// on dir (will enter this dir)
	AT_DIR_END = 1<<4,// finished reading dir (will go up)
	AT_SPECIAL = 1<<5,// anything other than link, dir or regular file
};

struct dirtree {
	struct dirtree* up;// ..
	DIR* cd;// Current Directory
};
/*
 * It's basically an iterative directory tree walker
 * Reacting to AT_* steps is done in a simple loop and a switch statement.
 *
 * Iterative; non-recursive
 */

struct tree_walk {
	enum tree_walk_state tws;
	int tl;// Transparent Links
	struct dirtree* dt;
	struct stat cs;// Current Stat
	char* path;
	size_t pathlen;
};

enum task_flags {
	TF_RAW_LINKS = 1<<0,// Copy links raw instead of recalculating

	TF_OVERWRITE_CONFLICTS = 1<<1,
	TF_OVERWRITE_ONCE = 1<<2,
	TF_ASK_CONFLICTS = 1<<3,
	TF_SKIP_CONFLICTS = 1<<4,
	TF_DEREF_LINKS = 1<<5,// If copying/moving links, copy what they point to

	TF_SKIP_LINKS = 1<<6,
	TF_RECURSIVE_CHMOD = 1<<7,
	TF_RECALCULATE_LINKS = 1<<8,
	TF_ANY_LINK_METHOD = (TF_RAW_LINKS | TF_DEREF_LINKS
		| TF_SKIP_LINKS | TF_RECALCULATE_LINKS),
};

enum task_state {
	TS_CLEAN = 0,
	TS_ESTIMATE = 1<<0,// after task_new; runs task_estimate
	TS_CONFIRM = 1<<1,// after task_estimate is finished; task configuration
	TS_RUNNING = 1<<2,// task runs
	TS_PAUSED = 1<<3,
	TS_FAILED = 1<<4,// if something went wrong. on some errors task can retry
	TS_FINISHED = 1<<5// task succesfully finished; cleans up, returns to TS_CLEAN
};

struct task {
	enum task_type t;
	enum task_state ts;
	enum task_flags tf;
//    vvv basically pointers to panel->wd; to not free
	char* src;// Source directory path
	char* dst;// Destination directory path
	struct string_list sources;// Files to be copied
	struct string_list renamed;// Same size as sources,
	fnum_t current_source;
// NULL == no conflict, use origial name
// NONNULL = conflict, contains pointer to name replacement
	int err;// Last errno
	struct tree_walk tw;
	int in, out;
	fnum_t conflicts, symlinks, specials;
	ssize_t size_total, size_done;
	fnum_t files_total, files_done;
	fnum_t dirs_total, dirs_done;
	mode_t chp, chm;
	uid_t cho;
	gid_t chg;
};

void task_new(struct task* const, const enum task_type,
		const enum task_flags,
		char* const, char* const,
		const struct string_list* const,
		const struct string_list* const);
void task_clean(struct task* const);
int task_build_path(const struct task* const, char*);
typedef void (*task_action)(struct task* const, int* const);
void task_action_chmod(struct task* const, int* const);
void task_action_estimate(struct task* const, int* const);
void task_action_copyremove(struct task* const, int* const);
void task_do(struct task* const, task_action, const enum task_state);
int tree_walk_start(struct tree_walk* const, const char* const,
		const char* const, const size_t);
void tree_walk_end(struct tree_walk* const);
int tree_walk_step(struct tree_walk* const);
/*
 * GENERAL TODO
 * - Dir scanning via task?
 * - Creating links: offer relative or absolute link path
 * - Keybindings must make more sense
 * - cache login/group names or entire /etc/passwd
 * - Use piped less more
 * - Display input buffer
 * - Jump to file pointed by symlink (and return)
 * - Change symlink target
 * - simplify empty dir handling - maybe show . or .. ?
 */
static char* ed[] = {"$VISUAL", "$EDITOR", "vi", NULL};
static char* sh[] = {"$SHELL", "sh", NULL};

struct mark_path {
	size_t len;
	char data[];
};

struct marks {
	struct mark_path* AZ['Z'-'A'+1];
	struct mark_path* az['z'-'a'+1];
};

void marks_free(struct marks* const);
struct mark_path** marks_get(struct marks* const, const char);
struct mark_path* marks_set(struct marks* const, const char,
		const char* const, size_t, const char* const, size_t);
void marks_input(struct ui* const, struct marks* const);
void marks_jump(struct ui* const, struct marks* const);

void marks_free(struct marks* const M) {
	struct mark_path** mp;
	for (char c = ' '; c < 0x7f; ++c) {

		mp = marks_get(M, c);
		if (!mp || !*mp) continue;
		free(*mp);
		*mp = NULL;
	}
	free(M);
}

struct mark_path** marks_get(struct marks* const M, const char C) {
	if (UPPERCASE(C)) {
		return &M->AZ[C-'A'];
	}
	else if (LOWERCASE(C)) {
		return &M->az[C-'a'];
	}
	return NULL;
}

struct mark_path* marks_set(struct marks* const M, const char C,
		const char* const wd, size_t wdlen,
		const char* const n, size_t nlen) {
	struct mark_path** mp = marks_get(M, C);
	if (!mp) return NULL;
	*mp = realloc(*mp, sizeof(struct mark_path)+wdlen+1+nlen+1);
	memcpy((*mp)->data, wd, wdlen+1);
	memcpy((*mp)->data+wdlen+1, n, nlen+1);
	(*mp)->data[wdlen] = '/';
	(*mp)->len = wdlen+1+nlen;
	return *mp;
}

void marks_input(struct ui* const i, struct marks* const m) {
	const struct input in = get_input(-1);
	if (in.t != I_UTF8) return;
	const struct file* f = hfr(i->pv);
	if (!f) return;
	marks_set(m, in.utf[0], i->pv->wd,
		strnlen(i->pv->wd, PATH_MAX_LEN), f->name, f->nl);
}

void marks_jump(struct ui* const i, struct marks* const m) {
	const struct input in = get_input(-1);
	if (in.t != I_UTF8) return;
	const char C = in.utf[0];
	struct mark_path** mp = marks_get(m, C);
	if (!mp || !*mp) {
		failed(i, "jump to mark", "Unknown mark");
		return;
	}
	if (access((*mp)->data, F_OK)) {
		int e = errno;
		failed(i, "jump to mark", strerror(e));
		return;
	}
	if (UPPERCASE(C)) {
		memcpy(i->pv->wd, (*mp)->data, (*mp)->len+1);
		i->pv->wdlen = (*mp)->len;
		if (ui_rescan(i, i->pv, NULL)) {
			first_entry(i->pv);
		}
	} else {
		char* const file = (*mp)->data+current_dir_i((*mp)->data);
		const size_t flen = strlen(file);
		const size_t wdlen = (*mp)->len-flen-1;
		memcpy(i->pv->wd, (*mp)->data, wdlen);
		i->pv->wd[wdlen] = '\0';
		i->pv->wdlen = wdlen;
		if (ui_rescan(i, i->pv, NULL)) {
			file_highlight(i->pv, file);
		}
	}
}

#define vi_swap() \
hist_switch(); \
stop_raw_mode(&i->T); \
global_i->run = 0; \
vi(); \
global_i->run = 1; \
start_raw_mode(&i->T); \
hist_switch(); \
hist_done(); \
hist_set(0); \
xquit = 0; \

static inline void list_marks(struct ui* const i, struct marks* const m) {
	struct mark_path** mp;
	hist_set(1);
	hist_open();
	for (int i = ' '; i < 0x7f; ++i) {
		if (!(mp = marks_get(m, i)) || !*mp) continue;
		char h[(*mp)->len+3];
		h[0] = i;
		h[1] = ' ';
		memcpy(&h[2], (*mp)->data, (*mp)->len+1);
		hist_write(h);
	}
	vi_swap()
}

static int open_file_with(char* const p, char* const f) {
	if (!p) return 0;
	char* const arg[] = { p, f, NULL };
	return spawn(arg, 0);
}

static void _keyname(const struct input* const in, char* const buf) {
// TODO
// TODO strncpy
	static const char* const N[] = {
		[I_ARROW_UP] = "up",
		[I_ARROW_DOWN] = "down",
		[I_ARROW_RIGHT] = "right",
		[I_ARROW_LEFT] = "left",
		[I_HOME] = "home",
		[I_END] = "end",
		[I_PAGE_UP] = "pgup",
		[I_PAGE_DOWN] = "pgdn",
		[I_INSERT] = "ins",
		[I_BACKSPACE] = "bsp",
		[I_DELETE] = "del",
		[I_ESCAPE] = "esc"
	};
	switch (in->t) {
	case I_NONE:
		strcpy(buf, "??");
		break;
	case I_UTF8:
		if (in->utf[0] == ' ') {
			strcpy(buf, "spc");
		}
		else {
			strcpy(buf, in->utf);
		}
		break;
	case I_CTRL:
		switch (in->utf[0]) {
		case 'I':
			strcpy(buf, "tab");
			break;
		case 'M':
			strcpy(buf, "enter");
			break;
		default:
			buf[0] = '^';
			strcpy(buf+1, in->utf);
			break;
		}
		break;
	default:
		strcpy(buf, N[in->t]);
		break;
	}
}

inline static void _find_all_keyseqs4cmd(const struct ui* const i,
		const enum command c, const enum mode m,
		const struct input2cmd* ic[], size_t* const ki) {
	*ki = 0;
	for (size_t k = 0; k < i->kml; ++k) {
		if (i->kmap[k].c != c || i->kmap[k].m != m) continue;
		ic[*ki] = &i->kmap[k];
		*ki += 1;
	}
}

static void open_help(struct ui* const i) {
	int l;
	hist_set(1);
	hist_open();
	for (size_t m = 0; m < MODE_NUM; ++m) {
		hist_write((char*)mode_strings[m]);
		const struct input2cmd* k[4];
		size_t ki = 0;
		for (size_t c = CMD_NONE+1; c < CMD_NUM; ++c) {
			_find_all_keyseqs4cmd(i, c, m, k, &ki);
			if (!ki) continue;// ^^^ may output empty array
			size_t maxsequences = 4;
			char key[KEYNAME_BUF_SIZE];
			char out[4096] = {0};
			for (size_t s = 0; s < ki; ++s) {
				unsigned j = 0;
				while (k[s]->i[j].t != I_NONE) {
					_keyname(&k[s]->i[j], key);
					strcat(out, key);
					j += 1;
				}
				maxsequences -= 1;
				strcat(out, "\t");
			}
			for (unsigned s = 0; s < maxsequences; ++s)
				strcat(out, "\t");
			strcat(out, cmd_help[c]);
			hist_write(out);
		}
	}
	l = 0;
	while (more_help[l]) {
		hist_write((char*)more_help[l]);
		l += 1;
	}
	hist_pos(0, 0, 0);
	vi_swap()
}

static int edit_list(struct string_list* const in,
		struct string_list* const out) {
	int err = 0, tmpfd;
	char tmpn[] = "/tmp/hund.XXXXXXXX";
	if ((tmpfd = mkstemp(tmpn)) == -1) return errno;
	if ((err = list_to_file(in, tmpfd))
	|| (err = open_file_with(xgetenv(ed), tmpn))
	|| (err = file_to_list(tmpfd, out))) {
// Failed; One operation failed, the rest was skipped.

	}
	if (close(tmpfd) && !err) err = errno;
	if (unlink(tmpn) && !err) err = errno;
	return err;
}

static void open_selected_with(struct ui* const i, char* const w) {
	char* path;
	int err;
	if ((path = panel_path_to_selected(i->pv))) {
		if ((err = open_file_with(w, path))) {
			failed(i, "open", strerror(err));
		}
		free(path);
	}
}

static void cmd_find(struct ui* const i) {
	if (!i->pv->num_files) return;
	char t[NAME_BUF_SIZE];
	char* t_top = t;
	memset(t, 0, sizeof(t));
	memcpy(i->prch, "/", 2);
	i->prompt = t;
	i->prompt_cursor_pos = 1;
	int r;
	const fnum_t S = i->pv->selection;
	const fnum_t N = i->pv->num_files;
	struct input o;
	fnum_t s = 0;// Start
	fnum_t e = N-1;// End

	for (;;) {
		i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
		ui_draw(i);
		r = fill_textbox(i, t, &t_top, NAME_MAX_LEN, &o);
		if (!r) {
			break;
		}
		else if (r == -1) {
			i->pv->selection = S;
			break;
		}
		else if (r == 2 || t_top != t) {
			if (IS_CTRL(o, 'V')) {
				panel_select_file(i->pv);
				continue;
			}
			else if (IS_CTRL(o, 'N') && i->pv->selection < N-1) {
				s = i->pv->selection+1;
				e = N-1;
			}
			else if (IS_CTRL(o, 'P') && i->pv->selection > 0) {
				s = i->pv->selection-1;
				e = 0;
			}
		}
		file_find(i->pv, t, s, e);
	}
	i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
	i->prompt = NULL;
}

/*
 * Returns:
 * 1 - success and there are files to work with (skipping may empty list)
 * 0 - failure or aborted
 */
inline static int _solve_name_conflicts_if_any(struct ui* const i,
		struct string_list* const s, struct string_list* const r) {
	static const char* const question = "Conflicting names.";
	static const struct select_option o[] = {
		{ KUTF8("r"), "rename" },
		{ KUTF8("m"), "merge" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("a"), "abort" }
	};
	int err;
	int was_conflict = 0;
	list_copy(r, s);
	while (conflicts_with_existing(i->sv, r)) {
		was_conflict = 1;
		switch (ui_ask(i, question, o, 4)) {
		case 0:// rename

			if ((err = edit_list(r, r))) {
				failed(i, "editor", strerror(err));
				return 0;
			}
			break;
		case 1:// merge
// merge policy is chosen after estimating
// (if there are any conflicts in the tree)
			list_free(r);
			return 1;
		case 2:// skip
			remove_conflicting(i->sv, s);
			list_copy(r, s);
			return s->len != 0;
		case 3:// abort
		default:
			return 0;
		}
	}
	if (!was_conflict) {
		list_free(r);
	}
	return 1;
}

static void prepare_task(struct ui* const i, struct task* const t,
		const enum task_type tt) {
	static const struct select_option o[] = {
		{ KUTF8("y"), "yes" },
		{ KUTF8("n"), "no" },
	};
	struct string_list S = { NULL, 0 };// Selected
	struct string_list R = { NULL, 0 };// Renamed
	panel_selected_to_list(i->pv, &S);
	if (!S.len) return;
	if (tt & (TASK_MOVE | TASK_COPY)) {
		if (!_solve_name_conflicts_if_any(i, &S, &R)) {
			list_free(&S);
			list_free(&R);
			return;
		}
	}
	enum task_flags tf = 0;
	if (tt == TASK_CHMOD && S_ISDIR(i->perm[0])
	&& !ui_ask(i, "Apply recursively?", o, 2)) {
		tf |= TF_RECURSIVE_CHMOD;
	}
	task_new(t, tt, tf, i->pv->wd, i->sv->wd, &S, &R);
	if (tt == TASK_CHMOD) {
		t->chp = i->plus;
		t->chm = i->minus;
		t->cho = ((i->o[0] == i->o[1]) ? (uid_t)-1 : i->o[1]);
		t->chg = ((i->g[0] == i->g[1]) ? (gid_t)-1 : i->g[1]);
		chmod_close(i);
	}
}

/*
 * op = Old Path
 * np = New Path
 * on = Old Name
 * nn = New Name
 */
static int _rename(char* const op, size_t* const opl,
		char* const np, size_t* const npl,
		const struct string* const on,
		const struct string* const nn) {
	int err = 0;
	if ((err = pushd(op, opl, on->str, on->len))) {
		return err;
	}
	if ((err = pushd(np, npl, nn->str, nn->len))) {
		popd(np, npl);
		return err;
	}
	if (rename(op, np)) {
		err = errno;
	}
	popd(np, npl);
	popd(op, opl);
	return err;
}

static int rename_trivial(const char* const wd, const size_t wdl,
		struct string_list* const S, struct string_list* const R) {
	int err = 0;
	char* op = malloc(wdl+1+NAME_BUF_SIZE);
	size_t opl = wdl;
	if (!op) {
		return ENOMEM;
	}
	char* np = malloc(wdl+1+NAME_BUF_SIZE);
	size_t npl = wdl;
	if (!np) {
		free(op);
		return ENOMEM;
	}
	memcpy(op, wd, wdl+1);
	memcpy(np, wd, wdl+1);
	for (fnum_t f = 0; f < S->len; ++f) {
		if (!S->arr[f]) {
			continue;
		}
		if ((err = _rename(op, &opl, np, &npl, S->arr[f], R->arr[f]))) {
			break;
		}
	}
	free(op);
	free(np);
	return err;
}

static int rename_interdependent(const char* const wd, const size_t wdl,
		struct string_list* const N,
		struct assign* const A, const fnum_t Al) {
	int err = 0;

	char* op = malloc(wdl+1+NAME_BUF_SIZE);
	size_t opl = wdl;
	memcpy(op, wd, wdl+1);

	char* np = malloc(wdl+1+NAME_BUF_SIZE);
	size_t npl = wdl;
	memcpy(np, wd, wdl+1);

	static const char* const tmpn = ".hund.rename.tmpdir.";
	size_t tmpnl = strlen(tmpn);
	struct string* tn = malloc(sizeof(struct string)+tmpnl+8+1);
	tn->len = tmpnl+8;
	snprintf(tn->str, tmpnl+8+1, "%s%08x", tmpn, getpid());
	fnum_t tc;// Temponary file Content

	for (;;) {
		fnum_t i = 0;
		while (i < Al && A[i].to == (fnum_t)-1) {
			i += 1;
		}
		if (i == Al) break;

		tc = A[i].to;
		const fnum_t tv = A[i].to;
		if ((err = _rename(op, &opl, np, &npl, N->arr[A[i].from], tn))) {
			break;
		}
		fnum_t from = A[i].from;
		do {
			fnum_t j = (fnum_t)-1;
			for (fnum_t f = 0; f < Al; ++f) {
				if (A[f].to == from) {
					j = f;
					break;
				}
			}
			if (j == (fnum_t)-1) break;
			if ((err = _rename(op, &opl, np, &npl,
				N->arr[A[j].from], N->arr[A[j].to]))) {
				free(op);
				free(np);
				free(tn);
				return err;
			}
			from = A[j].from;
			A[j].from = A[j].to = (fnum_t)-1;
		} while (from != tv);
		if ((err = _rename(op, &opl, np, &npl, tn, N->arr[tc]))) {
			break;
		}
		A[i].from = A[i].to = (fnum_t)-1;
	}
	free(op);
	free(np);
	free(tn);
	return err;
}

static void cmd_rename(struct ui* const i) {
	int err;
	struct string_list S = { NULL, 0 };// Selected files
	struct string_list R = { NULL, 0 };// Renamed files
	struct string_list N = { NULL, 0 };
	struct assign* a = NULL;
	fnum_t al = 0;
	static const struct select_option o[] = {
		{ KUTF8("y"), "yes" },
		{ KUTF8("n"), "no" },
		{ KUTF8("a"), "abort" }
	};
	int ok = 1;
	panel_selected_to_list(i->pv, &S);
	do {
		if ((err = edit_list(&S, &R))) {
			failed(i, "edit", strerror(err));
			list_free(&S);
			list_free(&R);
			return;
		}
		const char* msg = "There are conflicts. Retry?";
		if (blank_lines(&R)) {
			msg = "File contains blank lines. Retry?";
			ok = 0;
		}
		else if (S.len > R.len) {
			msg = "File does not contain enough lines. Retry?";
			ok = 0;

		}
		else if (S.len < R.len) {
			msg = "File contains too much lines. Retry?";
			ok = 0;
		}
		if ((!ok || !(ok = rename_prepare(i->pv, &S, &R, &N, &a, &al)))
		&& ui_ask(i, msg, o, 2) == 1) {
			list_free(&S);
			list_free(&R);
			return;
		}
	} while (!ok);
	if ((err = rename_trivial(i->pv->wd, i->pv->wdlen, &S, &R))
	|| (err = rename_interdependent(i->pv->wd, i->pv->wdlen, &N, a, al))) {
		failed(i, "rename", strerror(err));
	}
	list_free(&S);
	free(a);
	if (ui_rescan(i, i->pv, NULL)) {
		select_from_list(i->pv, &R);
		select_from_list(i->pv, &N);
	}
	list_free(&N);
	list_free(&R);
}

inline static void cmd_cd(struct ui* const i) {
// TODO
// TODO buffers
	int err = 0;
	struct stat s;
	char* path = malloc(PATH_BUF_SIZE);
	size_t pathlen = i->pv->wdlen;
	memcpy(path, i->pv->wd, pathlen+1);
	char* cdp = calloc(PATH_BUF_SIZE, sizeof(char));
	if (prompt(i, cdp, cdp, PATH_MAX_LEN)
	|| (err = cd(path, &pathlen, cdp, strnlen(cdp, PATH_MAX_LEN)))
	|| (access(path, F_OK) ? (err = errno) : 0)
	|| (stat(path, &s) ? (err = errno) : 0)
	|| (err = ENOTDIR, !S_ISDIR(s.st_mode))) {
		if (err) failed(i, "cd", strerror(err));
	}
	else {
		chdir(path);
		xstrlcpy(i->pv->wd, path, PATH_BUF_SIZE);
		i->pv->wdlen = pathlen;
		ui_rescan(i, i->pv, NULL);
	}
	free(path);
	free(cdp);
}

inline static void cmd_mkdir(struct ui* const i) {
	int err;
	struct string_list F = { NULL, 0 };
	const size_t wdl = strnlen(i->pv->wd, PATH_MAX_LEN);
	char* const P = malloc(wdl+1+NAME_BUF_SIZE);
	memcpy(P, i->pv->wd, wdl+1);
	size_t Pl = wdl;
	if ((err = edit_list(&F, &F))) {
		failed(i, "mkdir", strerror(err));
	}
	for (fnum_t f = 0; f < F.len; ++f) {
		if (!F.arr[f]) continue;
		if (memchr(F.arr[f]->str, '/', F.arr[f]->len+1)) {
			failed(i, "mkdir", "name contains '/'");
		}
		else if ((err = pushd(P, &Pl, F.arr[f]->str, F.arr[f]->len))
		|| (mkdir(P, MKDIR_DEFAULT_PERM) ? (err = errno) : 0)) {
			failed(i, "mkdir", strerror(err));
		}
		popd(P, &Pl);
	}
	free(P);
	list_free(&F);
	ui_rescan(i, i->pv, NULL);
}

inline static void cmd_change_sorting(struct ui* const i) {
// TODO cursor
	char old[FV_ORDER_SIZE];
	memcpy(old, i->pv->order, FV_ORDER_SIZE);
	char* buf = i->pv->order;
	char* top = i->pv->order + strlen(buf);
	int r;
	i->mt = MSG_INFO;
	xstrlcpy(i->msg, "-- SORT --", MSG_BUFFER_SIZE);
	i->dirty |= DIRTY_BOTTOMBAR | DIRTY_STATUSBAR;
	ui_draw(i);
	for (;;) {
		r = fill_textbox(i, buf, &top, FV_ORDER_SIZE, NULL);
		if (!r) break;
		else if (r == -1) {
			memcpy(i->pv->order, old, FV_ORDER_SIZE);
			return;
		}
		i->mt = MSG_INFO;
		i->dirty |= DIRTY_BOTTOMBAR | DIRTY_STATUSBAR;
		ui_draw(i);
	}
	for (size_t j = 0; j < FV_ORDER_SIZE; ++j) {
		int v = 0;
		for (size_t k = 0; k < strlen(compare_values); ++k) {
			v = v || (compare_values[k] == i->pv->order[j]);
		}
		if (!v) {
			i->pv->order[j] = 0;
		}
	}
	for (size_t j = 0; j < FV_ORDER_SIZE; ++j) {
		if (i->pv->order[0]) continue;
		memmove(&i->pv->order[0], &i->pv->order[1], FV_ORDER_SIZE-1);
		i->pv->order[FV_ORDER_SIZE-1] = 0;
	}
	panel_sorting_changed(i->pv);
}

static void cmd_mklnk(struct ui* const i) {
// TODO conflicts
//char tmpn[] = "/tmp/hund.XXXXXXXX";
//int tmpfd = mkstemp(tmpn);
	int err;
	struct string_list sf = { NULL, 0 };
	panel_selected_to_list(i->pv, &sf);
	size_t target_l = strnlen(i->pv->wd, PATH_MAX_LEN);
	size_t slpath_l = strnlen(i->sv->wd, PATH_MAX_LEN);
	char* target = malloc(target_l+1+NAME_BUF_SIZE);
	char* slpath = malloc(slpath_l+1+NAME_BUF_SIZE);
	memcpy(target, i->pv->wd, target_l+1);
	memcpy(slpath, i->sv->wd, slpath_l+1);
	for (fnum_t f = 0; f < sf.len; ++f) {
		pushd(target, &target_l, sf.arr[f]->str, sf.arr[f]->len);
		pushd(slpath, &slpath_l, sf.arr[f]->str, sf.arr[f]->len);
		if (symlink(target, slpath)) {
			err = errno;
			failed(i, "mklnk", strerror(err));
		}
		popd(target, &target_l);
		popd(slpath, &slpath_l);
	}
	free(target);
	free(slpath);
//close(tmpfd);
//unlink(tmpn);
	ui_rescan(i, i->sv, NULL);
	select_from_list(i->sv, &sf);
	list_free(&sf);
}

static void cmd_quick_chmod_plus_x(struct ui* const i) {
	char* path = NULL;
	struct stat s;
	if (!(path = panel_path_to_selected(i->pv))) return;
	if (stat(path, &s) || !S_ISREG(s.st_mode)) return;
	if (chmod(path, s.st_mode | (S_IXUSR | S_IXGRP | S_IXOTH))) {
		int e = errno;
		failed(i, "chmod", strerror(e));
	}
	ui_rescan(i, i->pv, NULL);
}

static void interpreter(struct ui* const i, struct task* const t,
		struct marks* const m, char* const line, size_t linesize) {
/* TODO document it */
	static char* anykey = "; read -n1 -r -p \"Press any key to continue...\" key\n"
	"if [ \"$key\" != '' ]; then echo; fi";
	(void)(t);
	const size_t line_len = strlen(line);
	if (!line[0] || line[0] == '\n' || line[0] == '#') {
		return;
	}
	if (!strcmp(line, "q")) {
		i->run = 0;
	}
	else if (!strcmp(line, "h") || !strcmp(line, "help")) {
		open_help(i);
	}
	else if (!strcmp(line, "+x")) {
		i->dirty |= DIRTY_STATUSBAR;// TODO
		cmd_quick_chmod_plus_x(i);
	}
	else if (!strcmp(line, "lm")) {
		list_marks(i, m);
	}
	else if (!strcmp(line, "sh")) {
		if (chdir(i->pv->wd)) {
			failed(i, "chdir", strerror(errno));
			return;
		}
		char* const arg[] = { xgetenv(sh), "-i", NULL };
		spawn(arg, 0);
	}
	else if (!memcmp(line, "sh ", 3)) {
		if (chdir(i->pv->wd)) {
			failed(i, "chdir", strerror(errno));
			return;
		}
		xstrlcpy(line+line_len, anykey, linesize-line_len);
		char* const arg[] = { xgetenv(sh), "-i", "-c", line+3, NULL };
		spawn(arg, 0);
	}
	else if (!memcmp(line, "o ", 2)) {
		open_selected_with(i, line+2);
	}
	else if (!memcmp(line, "mark ", 5)) {// TODO
		if (line[6] != ' ') {
			failed(i, "mark", "");// TODO
			return;
		}
		char* path = line+7;
		const size_t f = current_dir_i(path);
		*(path+f-1) = 0;
		const size_t wdl = strlen(path);
		const size_t fl = strlen(path+f);
		if (!marks_set(m, line[5], path, wdl, path+f, fl)) {
			failed(i, "mark", "");// TODO
		}
	}
	else if (!strcmp(line, "noh") || !strcmp(line, "nos")) {
		i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR;
		panel_unselect_all(i->pv);
	}
	else
		failed(i, "interpreter", "Unrecognized command");// TODO
}

static void _perm(struct ui* const i, const int unset, const int mask) {
	mode_t* m[2] = { &i->plus, &i->minus, };
	mode_t* tmp;
	int minus;
	struct input in = get_input(i->timeout);
	if (in.t != I_UTF8) return;
#define REL(M) do { *m[0] |= (mask & (M)); *m[1] &= ~(mask & (M)); } while (0);
#define SET(M) do { *m[0] = (~mask & *m[0]) | (mask & (M)); *m[1] = (~mask & *m[1]) | (mask & ~(M)); } while (0);
	minus = in.utf[0] == '-';
	if (in.utf[0] == '+' || in.utf[0] == '-') {
		in = get_input(i->timeout);
		if (in.t != I_UTF8) return;
		if (unset || minus) {
			tmp = m[0];
			m[0] = m[1];
			m[1] = tmp;
		}
		switch (in.utf[0]) {
		case '0': REL(00000); break;
		case 'x':
		case '1': REL(00111); break;
		case 'w':
		case '2': REL(00222); break;
		case '3': REL(00333); break;
		case 'r':
		case '4': REL(00444); break;
		case '5': REL(00555); break;
		case '6': REL(00666); break;
		case '7': REL(00777); break;
		case 't': REL(07000); break;
		}
	}
	else {
		if (unset) {
			tmp = m[0];
			m[0] = m[1];
			m[1] = tmp;
		}
		switch (in.utf[0]) {
		case '0': SET(00000); break;
		case '1': SET(00111); break;
		case '2': SET(00222); break;
		case '3': SET(00333); break;
		case '4': SET(00444); break;
		case '5': SET(00555); break;
		case '6': SET(00666); break;
		case '7': SET(00777); break;
		case 'r': REL(00444); break;
		case 'w': REL(00222); break;
		case 'x': REL(00111); break;
		case 't': REL(07000); break;
		}
	}
#undef REL
#undef SET
}

static void chg_column(struct ui* const i) {
	xstrlcpy(i->msg, "-- COLUMN --", MSG_BUFFER_SIZE);
	i->mt = MSG_INFO;
	i->dirty |= DIRTY_BOTTOMBAR;
	ui_draw(i);
	struct input in = get_input(i->timeout);
	if (in.t != I_UTF8) return;
	switch (in.utf[0]) {
	case 't': i->pv->column = COL_NONE; break;
	case 'i': i->pv->column = COL_INODE; break;
	case 'S': i->pv->column = COL_LONGSIZE; break;
	case 's': i->pv->column = COL_SHORTSIZE; break;
	case 'P': i->pv->column = COL_LONGPERM; break;
	case 'p': i->pv->column = COL_SHORTPERM; break;
	case 'u': i->pv->column = COL_UID; break;
	case 'U': i->pv->column = COL_USER; break;
	case 'g': i->pv->column = COL_GID; break;
	case 'G': i->pv->column = COL_GROUP; break;
	case 'A': i->pv->column = COL_LONGATIME; break;
	case 'a': i->pv->column = COL_SHORTATIME; break;
	case 'C': i->pv->column = COL_LONGCTIME; break;
	case 'c': i->pv->column = COL_SHORTCTIME; break;
	case 'M': i->pv->column = COL_LONGMTIME; break;
	case 'm': i->pv->column = COL_SHORTMTIME; break;
	default: break;
	}
	i->dirty |= DIRTY_PANELS | DIRTY_BOTTOMBAR;
}

static void cmd_command(struct ui* const i, struct task* const t,
		struct marks* const m) {
	(void)(t);
	char cmd[1024];
	memset(cmd, 0, sizeof(cmd));
	char* t_top = cmd;
	i->prompt = cmd;

	memcpy(i->prch, ":", 2);
	i->prompt_cursor_pos = 1;
	int r = 1;
	struct input o;
	for (;;) {
		i->dirty |= DIRTY_BOTTOMBAR;
		ui_draw(i);
		r = fill_textbox(i, cmd, &t_top, sizeof(cmd)-1, &o);
		if (!r) {
			break;
		}
		if (r == -1) {
			i->prompt = NULL;
			return;
		}
	}
	i->dirty |= DIRTY_BOTTOMBAR;
	i->prompt = NULL;
	interpreter(i, t, m, cmd, sizeof(cmd));
}

static void process_input(struct ui* const i, struct task* const t,
		struct marks* const m) {
	char *s = NULL;
	char *path;
	struct panel* tmp = NULL;
	int err = 0;
	fnum_t f;
	i->dirty |= DIRTY_PANELS | DIRTY_STATUSBAR;
	if (i->m == MODE_CHMOD) {
		i->dirty |= DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
	}
	switch (get_cmd(i)) {
/* CHMOD */
	case CMD_RETURN:
		chmod_close(i);
		break;
	case CMD_CHANGE:
		i->dirty |= DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
		prepare_task(i, t, TASK_CHMOD);
		break;
	case CMD_CHOWN:
/* TODO in $VISUAL */
		s = calloc(LOGIN_BUF_SIZE, sizeof(char));
		if (!prompt(i, s, s, LOGIN_MAX_LEN)) {
			errno = 0;
			struct passwd* pwd = getpwnam(s);
			if (!pwd) {
				err = errno;
				failed(i, "chown", strerror(err));
			}
			else {
				i->o[1] = pwd->pw_uid;
				xstrlcpy(i->user, pwd->pw_name, LOGIN_BUF_SIZE);
			}
		}
		free(s);
		break;
	case CMD_CHGRP:
/* TODO in $VISUAL */
		s = calloc(LOGIN_BUF_SIZE, sizeof(char));
		if (!prompt(i, s, s, LOGIN_BUF_SIZE)) {
			errno = 0;
			struct group* grp = getgrnam(s);
			if (!grp) {
				err = errno;
				failed(i, "chgrp", strerror(err));
			}
			else {
				i->g[1] = grp->gr_gid;
				xstrlcpy(i->group, grp->gr_name, LOGIN_BUF_SIZE);
			}
		}
		free(s);
		break;
	case CMD_A: _perm(i, 0, 00777); break;
	case CMD_U: _perm(i, 0, S_ISUID | 0700); break;
	case CMD_G: _perm(i, 0, S_ISGID | 0070); break;
	case CMD_O: _perm(i, 0, S_ISVTX | 0007); break;
	case CMD_PL: _perm(i, 0, 0777); break;
	case CMD_MI: _perm(i, 1, 0777); break;
/* WAIT */
	case CMD_TASK_QUIT:
//t->done = 1; // TODO
		break;
	case CMD_TASK_PAUSE:
		t->ts = TS_PAUSED;
		break;
	case CMD_TASK_RESUME:
		t->ts = TS_RUNNING;
		break;
/* MANAGER */
	case CMD_QUIT:
		i->run = 0;
		break;
	case CMD_QQUIT:
		i->run = -1;
		break;
	case CMD_HELP:
		open_help(i);
		break;
	case CMD_SWITCH_PANEL:
		tmp = i->pv;
		i->pv = i->sv;
		i->sv = tmp;
		if (!visible(i->pv, i->pv->selection)) {
			first_entry(i->pv);
		}
		break;
	case CMD_DUP_PANEL:
		memcpy(i->sv->wd, i->pv->wd, PATH_BUF_SIZE);
		i->sv->wdlen = i->pv->wdlen;
		if (ui_rescan(i, i->sv, NULL)) {
			i->sv->selection = i->pv->selection;
			i->sv->show_hidden = i->pv->show_hidden;
		}
		break;
	case CMD_SWAP_PANELS:
		tmp = malloc(sizeof(struct panel));
		memcpy(tmp, i->pv, sizeof(struct panel));
		memcpy(i->pv, i->sv, sizeof(struct panel));
		memcpy(i->sv, tmp, sizeof(struct panel));
		free(tmp);
		i->dirty |= DIRTY_PATHBAR;
		break;
	case CMD_ENTRY_DOWN:
		jump_n_entries(i->pv, 1);
		break;
	case CMD_ENTRY_UP:
		jump_n_entries(i->pv, -1);
		break;
	case CMD_SCREEN_DOWN:
		jump_n_entries(i->pv, i->ph-1);
		break;
	case CMD_SCREEN_UP:
		jump_n_entries(i->pv, -(i->ph-1));
		break;
	case CMD_EDIT_FILE:
		err = panel_enter_selected_dir(i->pv);
		if (err == ENOTDIR) {
			if ((path = panel_path_to_selected(i->pv))) {
				ex_edit(path);
				i->run = -1;
				free(path);
			}
		}
		break;
	case CMD_ENTER_DIR:
		err = panel_enter_selected_dir(i->pv);
		if (err) {
			if (err == ENOTDIR) {
				open_selected_with(i, xgetenv(ed));
			}
			else  {
				failed(i, "enter dir", strerror(err));
			}
		}
		i->dirty |= DIRTY_PATHBAR;
		break;
	case CMD_UP_DIR:
		if ((err = panel_up_dir(i->pv))) {
			failed(i, "up dir", strerror(err));
		}
		i->dirty |= DIRTY_PATHBAR;
		break;
	case CMD_COPY:
		prepare_task(i, t, TASK_COPY);
		break;
	case CMD_MOVE:
		prepare_task(i, t, TASK_MOVE);
		break;
	case CMD_REMOVE:
		prepare_task(i, t, TASK_REMOVE);
		break;
	case CMD_COMMAND:
		cmd_command(i, t, m);
		break;
	case CMD_CD:
		cmd_cd(i);
		break;
	case CMD_CREATE_DIR:
		cmd_mkdir(i);
		break;
	case CMD_RENAME:
		cmd_rename(i);
		break;
	case CMD_LINK:
		cmd_mklnk(i);
		break;
	case CMD_DIR_VOLUME:
//estimate_volume_for_selected(i->pv);
		break;
	case CMD_SELECT_FILE:
		if (panel_select_file(i->pv)) {
			jump_n_entries(i->pv, 1);
		}
		break;
	case CMD_SELECT_ALL:
		i->pv->num_selected = 0;
		for (fnum_t f = 0; f < i->pv->num_files; ++f) {
			if (visible(i->pv, f)) {
				i->pv->file_list[f]->selected = 1;
				i->pv->num_selected += 1;
			}
		}
		break;
	case CMD_SELECT_NONE:
		panel_unselect_all(i->pv);
		break;
	case CMD_SELECTED_NEXT:
		if (!i->pv->num_selected
		|| i->pv->selection == i->pv->num_files-1) break;
		f = i->pv->selection+1;
		while (f < i->pv->num_files) {
			if (i->pv->file_list[f]->selected) {
				i->pv->selection = f;
				break;
			}
			f += 1;
		}
		break;
	case CMD_SELECTED_PREV:
		if (!i->pv->num_selected || !i->pv->selection) break;
		f = i->pv->selection;
		while (f) {
			f -= 1;
			if (i->pv->file_list[f]->selected) {
				i->pv->selection = f;
				break;
			}
		}
		break;
	case CMD_MARK_NEW:
		marks_input(i, m);
		break;
	case CMD_MARK_JUMP:
		marks_jump(i, m);
		break;
	case CMD_FIND:
		cmd_find(i);
		break;
	case CMD_ENTRY_FIRST:
		first_entry(i->pv);
		break;
	case CMD_ENTRY_LAST:
		last_entry(i->pv);
		break;
	case CMD_CHMOD:
		if ((s = panel_path_to_selected(i->pv))) {
			i->dirty = DIRTY_STATUSBAR | DIRTY_BOTTOMBAR;
			chmod_open(i, s);
		}
		else 
			failed(i, "chmod", strerror(ENAMETOOLONG));
		break;
	case CMD_TOGGLE_HIDDEN:
		panel_toggle_hidden(i->pv);
		break;
	case CMD_REFRESH:
		ui_rescan(i, i->pv, NULL);
		break;
	case CMD_SORT_REVERSE:
		i->pv->scending = -i->pv->scending;
		panel_sorting_changed(i->pv);
		break;
	case CMD_SORT_CHANGE:
		cmd_change_sorting(i);
		break;
	case CMD_COL:
		chg_column(i);
		break;
	default:
		i->dirty = 0;
		break;
	}
}

static void task_progress(struct ui* const i,
		struct task* const t,
		const char* const S) {
	i->mt = MSG_INFO;
	i->dirty |= DIRTY_BOTTOMBAR;
	int n = snprintf(i->msg, MSG_BUFFER_SIZE,
			"%s %d/%df, %d/%dd", S,
			t->files_done, t->files_total,
			t->dirs_done, t->dirs_total);
	if (t->t & (TASK_REMOVE | TASK_MOVE | TASK_COPY)) {
		char sdone[SIZE_BUF_SIZE];
		char stota[SIZE_BUF_SIZE];
		pretty_size(t->size_done, sdone);
		pretty_size(t->size_total, stota);
		n += snprintf(i->msg+n, MSG_BUFFER_SIZE-n,
			", %s/%s", sdone, stota);
	}
}

static void task_execute(struct ui* const i, struct task* const t) {
	task_action ta = NULL;
	char msg[512];// TODO

	char psize[SIZE_BUF_SIZE];
	static const struct select_option remove_o[] = {
		{ KUTF8("y"), "yes" },
		{ KUTF8("n"), "no" },
	};
// TODO skip all errors, skip same errno

	static const struct select_option error_o[] = {
		{ KUTF8("t"), "try again" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("a"), "abort" },
	};
	static const struct select_option manual_o[] = {
		{ KUTF8("o"), "overwrite" },
		{ KUTF8("O"), "overwrite all" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("S"), "skip all" },
	};
	static const struct select_option conflict_o[] = {
		{ KUTF8("i"), "ask" },
		{ KUTF8("o"), "overwrite all" },
		{ KUTF8("s"), "skip all" },
		{ KUTF8("a"), "abort" },
	};
	static const struct select_option symlink_o[] = {
		{ KUTF8("r"), "raw" },
		{ KUTF8("c"), "recalculate" },
		{ KUTF8("d"), "dereference" },
		{ KUTF8("s"), "skip" },
		{ KUTF8("a"), "abort" },
	};
	static const char* const symlink_q = "There are symlinks";
	switch (t->ts) {
	case TS_CLEAN:
		break;
	case TS_ESTIMATE:
		i->timeout = 500;
		i->m = MODE_WAIT;
		task_progress(i, t, "--");
		task_do(t, task_action_estimate, TS_CONFIRM);
		if (t->err) t->ts = TS_FAILED;
		if (t->tw.tws == AT_LINK && !(t->tf & (TF_ANY_LINK_METHOD))) {
			if (t->t & (TASK_COPY | TASK_MOVE)) {
				switch (ui_ask(i, symlink_q, symlink_o, 5)) {
				case 0: t->tf |= TF_RAW_LINKS; break;
				case 1: t->tf |= TF_RECALCULATE_LINKS; break;
				case 2: t->tf |= TF_DEREF_LINKS; break;
				case 3: t->tf |= TF_SKIP_LINKS; break;
				case 4: t->ts = TS_FINISHED; break;
				default: break;
				}
			}
			else if (t->t & TASK_REMOVE) {
				t->tf |= TF_RAW_LINKS;// TODO

			}
		}
		break;
	case TS_CONFIRM:
		t->ts = TS_RUNNING;
		if (t->t == TASK_REMOVE) {
			pretty_size(t->size_total, psize);
			snprintf(msg, sizeof(msg),
				"Remove %u files? (%s)",
				t->files_total, psize);
			if (ui_ask(i, msg, remove_o, 2)) {
				t->ts = TS_FINISHED;
			}
		}
		else if (t->conflicts && t->t & (TASK_COPY | TASK_MOVE)) {
			snprintf(msg, sizeof(msg),
				"There are %d conflicts", t->conflicts);
			switch (ui_ask(i, msg, conflict_o, 4)) {
			case 0: t->tf |= TF_ASK_CONFLICTS; break;
			case 1: t->tf |= TF_OVERWRITE_CONFLICTS; break;
			case 2: t->tf |= TF_SKIP_CONFLICTS; break;
			case 3: t->ts = TS_FINISHED; break;
			default: break;
			}
		}
		task_progress(i, t, "==");
		break;
	case TS_RUNNING:
		i->timeout = 500;
		if (t->t & (TASK_REMOVE | TASK_COPY | TASK_MOVE)) {
			ta = task_action_copyremove;
		}
		else if (t->t == TASK_CHMOD) {
			ta = task_action_chmod;
		}
		task_progress(i, t, ">>");
		task_do(t, ta, TS_FINISHED);
		break;
	case TS_PAUSED:
		i->timeout = -1;
		task_progress(i, t, "||");
		break;
	case TS_FAILED:
		snprintf(msg, sizeof(msg), "@ %s\r\n(%d) %s.",
			t->tw.path, t->err, strerror(t->err));
		if (t->err == EEXIST) {
			switch (ui_ask(i, msg, manual_o, 4)) {
			case 0:
				t->tf |= TF_OVERWRITE_ONCE;
				break;
			case 1:
				t->tf &= ~TF_ASK_CONFLICTS;
				t->tf |= TF_OVERWRITE_CONFLICTS;
				break;
			case 2:
				t->err = tree_walk_step(&t->tw);
				break;
			case 3:
				t->tf &= ~TF_ASK_CONFLICTS;
				t->tf |= TF_SKIP_CONFLICTS;
				break;
			}
			t->ts = TS_RUNNING;
		}
		else {
			switch (ui_ask(i, msg, error_o, 3)) {
			case 0:
				t->ts = TS_RUNNING;
				break;
			case 1:
				t->err = tree_walk_step(&t->tw);
				t->ts = TS_RUNNING;
				break;
			case 2:
				t->ts = TS_FINISHED;
				break;
			}
		}
		t->err = 0;
		break;
	case TS_FINISHED:
		i->timeout = -1;
		if (ui_rescan(i, i->pv, i->sv)) {
			if (t->t == TASK_MOVE) {
				jump_n_entries(i->pv, -1);
			}
			pretty_size(t->size_done, psize);
			i->mt = MSG_INFO;
			snprintf(i->msg, MSG_BUFFER_SIZE,
				"processed %u files, %u dirs; %s",
				t->files_done, t->dirs_done, psize);
		}
		task_clean(t);
		i->m = MODE_MANAGER;
		break;
	default:
		break;
	}
}

int hund(int argc, char **argv) {
	int err;
	char* init_wd[2] = {argc >= 1 ? argv[0] : NULL,
				argc >= 2 ? argv[1] : NULL};
	struct panel fvs[2];
	memset(fvs, 0, sizeof(fvs));
	fvs[0].scending = 1;
	memcpy(fvs[0].order, default_order, FV_ORDER_SIZE);
	fvs[1].scending = 1;
	memcpy(fvs[1].order, default_order, FV_ORDER_SIZE);

	struct ui i;
	ui_init(&i, &fvs[0], &fvs[1]);

	for (int v = 0; v < 2; ++v) {
		const char* const d = (init_wd[v] ? init_wd[v] : "");
		fvs[v].wdlen = 0;
		if (getcwd(fvs[v].wd, PATH_BUF_SIZE)) {
			fvs[v].wdlen = strnlen(fvs[v].wd, PATH_MAX_LEN);
			size_t dlen = strnlen(d, PATH_MAX_LEN);
			if ((err = cd(fvs[v].wd, &fvs[v].wdlen, d, dlen))
			|| (err = panel_scan_dir(&fvs[v]))) {
				fprintf(stderr, "failed to initalize:"
					" (%d) %s\n", err, strerror(err));
				ui_end(&i);
				return 1;
			}
		}
		else {
			memcpy(fvs[v].wd, "/", 2);
			fvs[v].wdlen = 1;
		}
		first_entry(&fvs[v]);
	}
	if (init_wd[0])
		if(chdir(init_wd[0]))
			return 1;
	struct task t;
	memset(&t, 0, sizeof(struct task));
	t.in = t.out = -1;
	static struct marks *m;
	if (!m)
		m = calloc(1, sizeof(struct marks));

	i.mt = MSG_INFO;
	xstrlcpy(i.msg, "Type ? for help.", MSG_BUFFER_SIZE);

	while (i.run > 0 || t.ts != TS_CLEAN) {
		ui_draw(&i);
		if (i.run > 0)
			process_input(&i, &t, m);
		task_execute(&i, &t);
	}
	for (int v = 0; v < 2; ++v) {
		delete_file_list(&fvs[v]);
	}
	err = i.run;
	term_done();
	term_init();
	if (!err) {
		marks_free(m);
		m = NULL;
	}
	task_clean(&t);
	ui_end(&i);
	return err;
}

/* Similar to strlcpy;
 * Copies string pointed by src to dest.
 * Assumes that dest is n bytes long,
 * so it can fit string of length n-1 + null terminator.
 * String in src cannot be longer than n-1.
 * Null terminator is always appended to dest.
 * All remaining space in dest upon copying src is null terminated.
 *
 * Written after compiler started to whine about strncpy()
 * (specified bound N equals destination size [-Wstringop-truncation])
 */
char* xstrlcpy(char* dest, const char* src, size_t n)
{
	size_t srcl = strnlen(src, n-1);
	size_t i;
	for (i = 0; i < srcl; ++i) {
		dest[i] = src[i];
	}
	for (; i < n; ++i) {
		dest[i] = 0;
	}
	return dest;
}

char* get_home(void) {
	char* home;
	if ((home = getenv("HOME"))) {
		return home;
	}
	errno = 0;
	struct passwd* pwd = getpwuid(geteuid());
	return (pwd ? pwd->pw_dir : NULL);
}

/*
 * Chmods given file using plus and minus masks
 * The following should be true: (plus & minus) == 0
 * (it's not checked)
 */
int relative_chmod(const char* const file,
		const mode_t plus, const mode_t minus) {
	struct stat s;
	if (stat(file, &s)) return errno;
	mode_t p = s.st_mode & 07777;
	p |= plus;
	p &= ~minus;
	return (chmod(file, p) ? errno : 0);
}

/*
 * Checks of two files are on the same filesystem.
 * If they are, moving files (and even whole directories)
 * can be done with just rename() function.
 */
int same_fs(const char* const a, const char* const b) {
	struct stat sa, sb;
	return !stat(a, &sa) && !stat(b, &sb) && (sa.st_dev == sb.st_dev);
}

/*
 * Cleans up list created by scan_dir()
 */
void file_list_clean(struct file*** const fl, fnum_t* const nf) {
	if (!*nf) return;
	for (fnum_t i = 0; i < *nf; ++i) {
		free((*fl)[i]);
	}
	free(*fl);
	*fl = NULL;
	*nf = 0;
}

/*
 * Cleans up old data and scans working directory,
 * putting data into variables passed in arguments.
 *
 * On ENOMEM: cleans everything
 * TODO test
 *
 * wd = Working Directory
 * fl = File List
 * nf = Number of Files
 * nhf = Number of Hidden Files
 */
struct node {
	struct node* next;
	struct file* file;
};

int scan_dir(const char* const wd, struct file*** const fl,
		fnum_t* const nf, fnum_t* const nhf) {
	DIR* dir = opendir(wd);
	if (!dir) return errno;
	const size_t wdlen = strnlen(wd, PATH_MAX_LEN);

	char fpath[PATH_BUF_SIZE];
	memcpy(fpath, wd, wdlen+1);
	size_t fpathlen = wdlen;

	file_list_clean(fl, nf);
	*nhf = 0;

	int err = 0;
	struct node *H = NULL, *tmp;
	struct dirent* de;
	struct file* nfr;
	while ((de = readdir(dir)) != NULL) {
		if (DOTDOT(de->d_name)) continue;
		*nf += 1;
		const size_t nl = strnlen(de->d_name, NAME_MAX_LEN);
		if (!(tmp = calloc(1, sizeof(struct node)))
		|| !(nfr = malloc(sizeof(struct file)+nl+1))) {
			err = errno;
			if (tmp) free(tmp);
			while (H) {
				tmp = H;
				H = H->next;
				if (tmp->file) free(tmp->file);
				free(tmp);
			}
			*nf = *nhf = 0;
			break;
		}
		tmp->next = H;
		tmp->file = nfr;
		H = tmp;

		if (de->d_name[0] == '.') {
			*nhf += 1;
		}
		nfr->selected = 0;
		nfr->nl = (unsigned char)nl;
		memcpy(nfr->name, de->d_name, nl+1);

		if (!(err = pushd(fpath, &fpathlen, nfr->name, nl))) {
			if (lstat(fpath, &nfr->s)) {
				err = errno;
				memset(&nfr->s, 0, sizeof(struct stat));
			}
			popd(fpath, &fpathlen);
		}
		else {
			memset(&nfr->s, 0, sizeof(struct stat));
		}
	}
	*fl = malloc(*nf * sizeof(struct file*));
	fnum_t i = 0;
	while (H) {
		tmp = H;
		H = H->next;
		(*fl)[i] = tmp->file;
		free(tmp);
		i += 1;
	}
	closedir(dir);
	return err;
}

int link_copy_recalculate(const char* const wd,
		const char* const src, const char* const dst) {
	struct stat src_s;
	if (!wd || !src || !dst) return EINVAL;
	if (lstat(src, &src_s)) return errno;
	if (!S_ISLNK(src_s.st_mode)) return EINVAL;

	char lpath[PATH_BUF_SIZE];
	const ssize_t lpathlen = readlink(src, lpath, sizeof(lpath));
	if (lpathlen == -1) return errno;
	lpath[lpathlen] = 0;
	if (!PATH_IS_RELATIVE(lpath)) {
		return (symlink(lpath, dst) ? errno : 0);
	}
	const size_t wdlen = strnlen(wd, PATH_MAX_LEN);
	char tpath[PATH_BUF_SIZE];
	memcpy(tpath, wd, wdlen);
	memcpy(tpath+wdlen, "/", 1);
	memcpy(tpath+wdlen+1, lpath, lpathlen+1);
	return (symlink(tpath, dst) ? errno : 0);
}

/*
 * Copies link without recalculating path
 * Allows dangling pointers
 * TODO TEST
 */
int link_copy_raw(const char* const src, const char* const dst) {
	char lpath[PATH_BUF_SIZE];
	const ssize_t ll = readlink(src, lpath, sizeof(lpath));
	if (ll == -1) return errno;
	lpath[ll] = 0;
	if (symlink(lpath, dst)) {
		int e = errno;
		if (e != ENOENT) return e;
	}
	return 0;
}

void pretty_size(off_t s, char* const buf) {
	static const char* const units = "BKMGTPEZ";
	const char* unit = units;
	unsigned rest = 0;
	while (s >= 1024) {
		rest = s % 1024;
		s /= 1024;
		unit += 1;
	}
	if (s >= 1000) {
		unit += 1;
		rest = s;
		s = 0;
	}
	rest *= 1000;
	rest /= 1024;
	rest /= 10;
	const char d[3] = { s/100, (s/10)%10, s%10 };
	const char r[2] = { rest/10, rest%10 };
	memset(buf, 0, SIZE_BUF_SIZE);
	size_t top = 0;
	for (size_t i = 0; i < 3; ++i) {
		if (d[i] || top || (!top && i == 2)) {
			buf[top++] = '0'+d[i];
		}
	}
	if (!d[0] && !d[1] && (r[0] || r[1])) {
		buf[top++] = '.';
		buf[top++] = '0'+r[0];
		if (r[1]) buf[top++] = '0'+r[1];
	}
	buf[top] = *unit;
}

/*
 * Push Directory
 */
int pushd(char* const P, size_t* const Pl, const char* const D, size_t Dl) {
	if (memcmp(P, "/", 2)) {
		if (*Pl+1+Dl > PATH_MAX_LEN) return ENAMETOOLONG;
		P[*Pl] = '/';
		*Pl += 1;
	}
	else if (*Pl+Dl > PATH_MAX_LEN) return ENAMETOOLONG;
	memcpy(P+*Pl, D, Dl+1);
	*Pl += Dl;
	return 0;
}

/*
 * Pop directory
 */
void popd(char* const P, size_t* const Pl) {
	while (*Pl > 1 && P[*Pl-1] != '/') {
		P[*Pl-1] = 0;
		*Pl -= 1;
	}
	if (*Pl > 1) {
		P[*Pl-1] = 0;
		*Pl -= 1;
	}
}

/*
 * cd - Change Directory
 * Does not change current working directory,
 * but modifies path passed in arguments.
 *
 * Returns ENAMETOOLONG if PATH_MAX would be exceeded.
 * In such case 'current' is left unchanged.
 *
 * 'current' must contain an absolute path.
 *
 * 'dest' may be either absolute or relative path.
 * Besides directory names it can contain . or ..
 * which are appiled to current directory.
 * '~' at the begining is an alias for user's HOME directory.
 *
 * Path returned in 'current' will always be absolute
 * and it will never contain '/' at the end
 * (with exception of root directory).
 *
 * If both 'current' and 'dest' are empty strings
 * 'current' will be corrected to "/".
 *
 * If 'dest' is empty string and 'current' ends with '/',
 * that '/' will be cleared.
 *
 * If 'current' is empty string,
 * then it is assumed to be root directory.
 *
 * Multiple consecutive '/' will be shortened to only one '/'
 * e.g. cd("/", "a///b////c") -> "/a/b/c"
 */
int cd(char* const current, size_t* const cl,
		const char* const dest, size_t rem) {
	char B[PATH_BUF_SIZE];
	size_t Bl;
	const char* a = dest;
	if (dest[0] == '/') {
		memset(B, 0, sizeof(B));
		Bl = 0;
		a += 1;
		rem -= 1;
	}
	else {
		if (dest[0] == '/' && dest[1] == 0) {
			B[0] = 0;
			Bl = 0;
		}
		else {
			memcpy(B, current, *cl);
			memset(B+*cl, 0, PATH_BUF_SIZE-*cl);
			Bl = *cl;
			if (B[Bl-1] == '/') {
				B[--Bl] = 0;
			}
		}
	}
	const char* b;
	while (rem) {
		b = memchr(a, '/', rem);

		if (!b) b = a+rem;
		else rem -= 1;

		if (b-a == 1 && *a == '.');
		else if (b-a == 2 && a[0] == '.' && a[1] == '.') {
			while (Bl > 0 && B[Bl-1] != '/') {
				B[--Bl] = 0;
			}
			B[--Bl] = 0;
		}
		else if (b-a == 1 && a == dest && a[0] == '~') {
			const char* const home = get_home();
			const size_t hl = strnlen(home, PATH_MAX_LEN);
			memcpy(B, home, hl);
			memset(B+hl, 0, PATH_BUF_SIZE-hl);
			Bl = hl;
		}
		else if (b-a) {
			if (Bl + 1 + (b-a) >= PATH_MAX_LEN) {
				return ENAMETOOLONG;
			}
			B[Bl++] = '/';
			memcpy(B+Bl, a, b-a);
			Bl += b-a;
		}
		rem -= b-a;
		a = b+1;
	}
	if (B[0] == 0) {
		B[0] = '/';
		B[1] = 0;
		Bl = 1;
	}
	memcpy(current, B, Bl+1);
	*cl = Bl;
	return 0;
}

/*
 * Returns place in buffer, where path after ~ starts
 * So that pretty path is just
 * printf("~%s", path+prettify_path_i(path));
 * If cannot be prettified, returns 0
 */
int prettify_path_i(const char* const path) {
	const char* const home = get_home();
	const size_t hlen = strnlen(home, PATH_MAX_LEN);
	if (!memcmp(path, home, hlen)) {
		return hlen;
	}
	return 0;
}

/*
 * Instead of populating another buffer,
 * it just points to place in buffer,
 * where current dir's name starts
 */
int current_dir_i(const char* const path) {
	const int plen = strnlen(path, PATH_MAX_LEN);
	int i = plen-1;// i will point last slash in path
	while (path[i] != '/' && i >= 0)
		i -= 1;
	return i+1;// i will point last slash in path
}

/* Initial Matching Bytes */
size_t imb(const char* a, const char* b) {
	size_t m = 0;
	while (*a && *b && *a == *b) {
		a += 1;
		b += 1;
		m += 1;
	}
	return m;
}

/* Checks if STRing contains SUBString */
int contains(const char* const str, const char* const subs) {
	const size_t subs_len = strnlen(subs, PATH_MAX_LEN);
	const size_t str_len = strnlen(str, PATH_MAX_LEN);
	if (subs_len > str_len) return 0;
	if (subs_len == str_len) return !memcmp(str, subs, str_len);
	for (size_t j = 0; strnlen(str+j, PATH_MAX_LEN) >= subs_len; ++j) {
		if (subs_len == imb(str+j, subs)) return 1;
	}
	return 0;
}

fnum_t list_push(struct string_list* const L, const char* const s, size_t sl) {
	void* tmp = realloc(L->arr, (L->len+1) * sizeof(struct string*));
	if (!tmp) return (fnum_t)-1;
	L->arr = tmp;
	if (s) {
		if (sl == (size_t)-1) {
			sl = strnlen(s, NAME_MAX_LEN);
		}
		L->arr[L->len] = malloc(sizeof(struct string)+sl+1);
		L->arr[L->len]->len = sl;
		memcpy(L->arr[L->len]->str, s, sl+1);
	}
	else {
		L->arr[L->len] = NULL;
	}
	L->len += 1;
	return L->len - 1;
}

void list_copy(struct string_list* const D, const struct string_list* const S) {
	D->len = S->len;
	D->arr = malloc(D->len*sizeof(struct string*));
	for (fnum_t i = 0; i < D->len; ++i) {
		D->arr[i] = malloc(sizeof(struct string)+S->arr[i]->len+1);
		memcpy(D->arr[i]->str, S->arr[i]->str, S->arr[i]->len+1);
		D->arr[i]->len = S->arr[i]->len;
	}
}

void list_free(struct string_list* const list) {
	if (!list->arr) return;
	for (fnum_t i = 0; i < list->len; ++i) {
		if (list->arr[i]) free(list->arr[i]);
	}
	free(list->arr);
	list->arr = NULL;
	list->len = 0;
}

/*
 * Reads file from fd and forms a list of lines
 * Blank line encountered: string_list is not allocated. Pointer is set to NULL.
 *
 * TODO flexible buffer length
 * TODO more testing
 * TODO support for other line endings
 * TODO redo
 */
int file_to_list(const int fd, struct string_list* const list) {
	list_free(list);
	char name[NAME_BUF_SIZE];
	size_t nlen = 0, top = 0;
	ssize_t rd = 0;
	char* nl;
	if (lseek(fd, 0, SEEK_SET) == -1) return errno;
	memset(name, 0, sizeof(name));
	for (;;) {
		rd = read(fd, name+top, sizeof(name)-top);
		if (rd == -1) {
			int e = errno;
			list_free(list);
			return e;
		}
		if (!rd && !*name) break;
		;
		if (!(nl = memchr(name, '\n', sizeof(name)))
		&& !(nl = memchr(name, 0, sizeof(name)))) {
			list_free(list);
			return ENAMETOOLONG;
		}
		*nl = 0;
		nlen = nl-name;
		void* tmp_str = realloc(list->arr,
				(list->len+1)*sizeof(struct string*));
		if (!tmp_str) {
			int e = errno;
			list_free(list);
			return e;
		}
		list->arr = tmp_str;
		if (nlen) {
			list->arr[list->len] = malloc(sizeof(struct string)+nlen+1);
			list->arr[list->len]->len = nlen;
			memcpy(list->arr[list->len]->str, name, nlen+1);
		}
		else {
			list->arr[list->len] = NULL;
		}
		top = NAME_BUF_SIZE-(nlen+1);
		memmove(name, name+nlen+1, top);
		memset(name+top, 0, nlen+1);
		list->len += 1;
	}
	return 0;
}

int list_to_file(const struct string_list* const list, int fd) {
	if (lseek(fd, 0, SEEK_SET)) return errno;
	char name[NAME_BUF_SIZE];
	for (fnum_t i = 0; i < list->len; ++i) {
		memcpy(name, list->arr[i]->str, list->arr[i]->len);
		name[list->arr[i]->len] = '\n';
		if (write(fd, name, list->arr[i]->len+1) <= 0) return errno;
	}
	return 0;
}

fnum_t string_on_list(const struct string_list* const L,
		const char* const s, size_t sl) {
	for (fnum_t f = 0; f < L->len; ++f) {
		if (!memcmp(s, L->arr[f]->str, MIN(sl, L->arr[f]->len)+1)) {
			return f;
		}
	}
	return (fnum_t)-1;
}

fnum_t blank_lines(const struct string_list* const list) {
	fnum_t n = 0;
	for (fnum_t i = 0; i < list->len; ++i) {
		if (!list->arr[i]) n += 1;
	}
	return n;
}

/*
 * Tells if there are duplicates on list
 * TODO optimize
 */
int duplicates_on_list(const struct string_list* const list) {
	for (fnum_t f = 0; f < list->len; ++f) {
		for (fnum_t g = 0; g < list->len; ++g) {
			if (f == g) continue;
			const char* const a = list->arr[f]->str;
			const char* const b = list->arr[g]->str;
			size_t s = 1+MIN(list->arr[f]->len, list->arr[g]->len);
			if (a && b && !memcmp(a, b, s)) return 1;
		}
	}
	return 0;
}

/* This file contains UI-related functions
 * These functions are supposed to draw elements of UI.
 * They are supposed to read panel contents, but never modify it.
 */
static enum theme_element mode2theme(const mode_t m) {
	if (EXECUTABLE(m)) return THEME_ENTRY_REG_EXE_UNS;
	static const enum theme_element tm[] = {
		[S_IFBLK>>S_IFMT_TZERO] = THEME_ENTRY_BLK_UNS,
		[S_IFCHR>>S_IFMT_TZERO] = THEME_ENTRY_CHR_UNS,
		[S_IFIFO>>S_IFMT_TZERO] = THEME_ENTRY_FIFO_UNS,
		[S_IFREG>>S_IFMT_TZERO] = THEME_ENTRY_REG_UNS,
		[S_IFDIR>>S_IFMT_TZERO] = THEME_ENTRY_DIR_UNS,
		[S_IFSOCK>>S_IFMT_TZERO] = THEME_ENTRY_SOCK_UNS,
		[S_IFLNK>>S_IFMT_TZERO] = THEME_ENTRY_LNK_UNS,
	};
	return tm[(m & S_IFMT) >> S_IFMT_TZERO];
}

int sig_hund(int sig) {
	if (!global_i || global_i->run <= 0)
		return 0;
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		global_i->run = 0;
		break;
	case SIGTSTP:
		stop_raw_mode(&global_i->T);
		signal(SIGTSTP, SIG_DFL);
		kill(getpid(), SIGTSTP);
		break;
	case SIGCONT:
		start_raw_mode(&global_i->T);
		global_i->dirty |= DIRTY_ALL;
		ui_draw(global_i);
		setup_signals();
		break;
	case SIGWINCH:
		for (int b = 0; b < BUF_NUM; ++b) {
			free(global_i->B[b].buf);
			global_i->B[b].top = global_i->B[b].capacity = 0;
			global_i->B[b].buf = NULL;
		}
		global_i->dirty |= DIRTY_ALL;
		ui_draw(global_i);
		break;
	default:
		break;
	}
	return 1;
}

void ui_init(struct ui* const i, struct panel* const pv,
		struct panel* const sv) {
	setlocale(LC_ALL, "");
	i->scrh = i->scrw = i->pw[0] = i->pw[1] = 0;
	i->ph = i->pxoff[0] = i->pxoff[1] = 0;
	i->run = 1;

	i->m = MODE_MANAGER;
	i->mt = MSG_NONE;

	i->prompt = NULL;
	i->prompt_cursor_pos = i->timeout = -1;

	memset(i->B, 0, BUF_NUM*sizeof(struct append_buffer));
	i->dirty = DIRTY_ALL;

	i->fvs[0] = i->pv = pv;
	i->fvs[1] = i->sv = sv;

	i->kml = default_mapping_length;
	i->kmap = malloc(i->kml*sizeof(struct input2cmd));
	for (size_t k = 0; k < i->kml; ++k) {
		memcpy(&i->kmap[k], &default_mapping[k], sizeof(struct input2cmd));
	}
	memset(i->K, 0, sizeof(struct input)*INPUT_LIST_LENGTH);

	i->perm[0] = i->perm[1] = 0;
	i->o[0] = i->o[1] = 0;
	i->g[0] = i->g[1] = 0;

	global_i = i;
	int err;
	if ((err = start_raw_mode(&i->T))) {
		fprintf(stderr, "failed to initalize screen: (%d) %s\n",
				err, strerror(err));
		exit(EXIT_FAILURE);
	}
}

void ui_end(struct ui* const i) {
	for (int b = 0; b < BUF_NUM; ++b) {
		if (i->B[b].buf) free(i->B[b].buf);
	}
	int err;
	if ((err = stop_raw_mode(&i->T))) {
		fprintf(stderr, "failed to deinitalize screen: (%d) %s\n",
				err, strerror(err));
		exit(EXIT_FAILURE);
	}
	global_i = NULL;
	free(i->kmap);
	memset(i, 0, sizeof(struct ui));
}

void ui_pathbar(struct ui* const i, struct append_buffer* const ab) {
	append(ab, CSI_CLEAR_LINE);
	append_theme(ab, THEME_PATHBAR);
	for (size_t j = 0; j < 2; ++j) {
		const int p = prettify_path_i(i->fvs[j]->wd);
		const int t = p ? 1 : 0;
		const char* wd = i->fvs[j]->wd + p;
		const size_t wdw = utf8_width(wd) + t;
		const size_t rem = i->pw[j]-2;
		size_t padding, wdl;
		fill(ab, ' ', 1);
		if (wdw > rem) {
			padding = 0;
			wd += utf8_w2nb(wd, wdw - rem) - t;
			wdl = utf8_w2nb(wd, rem);
		}
		else {
			if (t) fill(ab, '~', 1);
			padding = rem - wdw;
			wdl = i->fvs[j]->wdlen - p + t;
		}
		append(ab, wd, wdl);
		fill(ab, ' ', 1+padding);
	}
	append_attr(ab, ATTR_NORMAL, NULL);
	append(ab, "\r\n", 2);
}

static size_t stringify_p(const mode_t m, char* const perms) {
	perms[0] = mode_type_symbols[(m & S_IFMT) >> S_IFMT_TZERO];
	memcpy(perms+1, perm2rwx[(m>>6) & 07], 3);
	memcpy(perms+1+3, perm2rwx[(m>>3) & 07], 3);
	memcpy(perms+1+3+3, perm2rwx[(m>>0) & 07], 3);
	if (m & S_ISUID) {
		perms[3] = 's';
		if (!(m & S_IXUSR)) perms[3] ^= 0x20;
	}
	if (m & S_ISGID) {
		perms[6] = 's';
		if (!(m & S_IXGRP)) perms[6] ^= 0x20;
	}
	if (m & S_ISVTX) {
		perms[9] = 't';
		if (!(m & S_IXOTH)) perms[9] ^= 0x20;
	}
	return 10;
}

static size_t stringify_u(const uid_t u, char* const user) {
	const struct passwd* const pwd = getpwuid(u);
	if (pwd) return snprintf(user, LOGIN_BUF_SIZE, "%s", pwd->pw_name);
	else return snprintf(user, LOGIN_BUF_SIZE, "%u", u);
}

static size_t stringify_g(const gid_t g, char* const group) {
	const struct group* const grp = getgrgid(g);
	if (grp) return snprintf(group, LOGIN_BUF_SIZE, "%s", grp->gr_name);
	else return snprintf(group, LOGIN_BUF_SIZE, "%u", g);
}

static void _column(const enum column C, const struct file* const cfr,
		char* const buf, const size_t bufsize, size_t* const buflen) {
// TODO adjust width of columns
// TODO inode, longsize, shortsize: length may be very different
	struct tm T;
	time_t t;
	const char* tfmt;
	time_t tspec;
	switch (C) {
		case COL_LONGATIME:
		case COL_SHORTATIME:
			tspec = cfr->s.st_atim.tv_sec;
			break;
		case COL_LONGCTIME:
		case COL_SHORTCTIME:
			tspec = cfr->s.st_ctim.tv_sec;
			break;
		case COL_LONGMTIME:
		case COL_SHORTMTIME:
			tspec = cfr->s.st_mtim.tv_sec;
			break;
		default:
			tspec = 0;
			break;
	}
	switch (C) {
	case COL_INODE:
		*buflen = snprintf(buf, bufsize, "%6lu", cfr->s.st_ino);// TODO fmt type

		break;
	case COL_LONGSIZE:
		*buflen = snprintf(buf, bufsize, "%10ld", cfr->s.st_size);// TODO fmt type

		break;
	case COL_SHORTSIZE:
		pretty_size(cfr->s.st_size, buf);
		*buflen = strlen(buf);
		memmove(buf+(SIZE_BUF_SIZE-1-*buflen), buf, *buflen);
		memset(buf, ' ', (SIZE_BUF_SIZE-1-*buflen));
		*buflen = SIZE_BUF_SIZE-1;
		break;
	case COL_LONGPERM:
		*buflen = stringify_p(cfr->s.st_mode, buf);
		buf[*buflen] = 0;
		break;
	case COL_SHORTPERM:
		*buflen = snprintf(buf, bufsize,
			"%04o", cfr->s.st_mode & 07777);
		break;
	case COL_UID:
		*buflen = snprintf(buf, bufsize, "%u", cfr->s.st_uid);
		break;
	case COL_USER:
		*buflen = stringify_u(cfr->s.st_uid, buf);
		break;
	case COL_GID:
		*buflen = snprintf(buf, bufsize, "%u", cfr->s.st_gid);
		break;
	case COL_GROUP:
		*buflen = stringify_g(cfr->s.st_gid, buf);
		break;
	case COL_LONGATIME:
	case COL_LONGCTIME:
	case COL_LONGMTIME:
		if (!localtime_r(&tspec, &T)) {
			memset(&T, 0, sizeof(struct tm));
		}
		strftime(buf, TIME_SIZE, timefmt, &T);
		*buflen = strlen(buf);
		break;
	case COL_SHORTATIME:
	case COL_SHORTCTIME:
	case COL_SHORTMTIME:
		t = time(NULL);
		if (t - tspec > 60*60*24*365) {
			tfmt = "%y'%b";
		}
		else if (t - tspec > 60*60*24) {
			tfmt = "%b %d";
		}
		else {
			tfmt = " %H:%M";
		}
		if (!localtime_r(&tspec, &T)) {
			memset(&T, 0, sizeof(struct tm));
		}
		strftime(buf, TIME_SIZE, tfmt, &T);
		*buflen = strlen(buf);
		break;
	default:
		buf[0] = 0;
		*buflen = 0;
		break;
	}
}

static void _entry(struct ui* const i, const struct panel* const fv,
		const size_t width, const fnum_t e) {
	struct append_buffer* const ab = &i->B[BUF_PANELS];
// TODO scroll filenames that are too long to fit in the panel width

	const struct file* const cfr = fv->file_list[e];
// File SYMbol
	enum theme_element fsym = mode2theme(cfr->s.st_mode);

	char name[NAME_BUF_SIZE];
	const unsigned u = cut_unwanted(cfr->name, name, '.', NAME_BUF_SIZE);

	char column[48];
	size_t cl;
	_column(fv->column, cfr, column, sizeof(column), &cl);

	if (1+(fv->column != COL_NONE)+cl+1 > width) return;
	const size_t name_allowed = width - (1+(fv->column != COL_NONE)+cl+1);
	const size_t name_width = utf8_width(name);
	const size_t name_draw = (name_width < name_allowed
			? name_width : name_allowed);

	char open = file_symbols[fsym];
	char close = ' ';

	if (e == fv->selection) {
		if (!fv->num_selected) {
			open = '[';
			close = ']';
		}
		if (fv == i->pv) {
			fsym += 1;
		}
		else {
			append_attr(ab, ATTR_UNDERLINE, NULL);
		}
	}
	if (cfr->selected) {
		open = '[';
		close = ']';
		append_attr(ab, ATTR_BOLD, NULL);
	}
	append_theme(ab, fsym);
	fill(ab, open, 1);
	append(ab, name, utf8_w2nb(name, name_draw));
	fill(ab, ' ', width - (1+name_draw+cl+1));
	append(ab, column, cl);
	if (u) {
		close = '*';
		append_attr(ab, ATTR_YELLOW|ATTR_BOLD|ATTR_FOREGROUND, NULL);
	}
	fill(ab, close, 1);
	append_attr(ab, ATTR_NORMAL, NULL);
}

/*
 * - Max Entries = how many entries I need to fill
 * all available space in file view
 * (selection excluded - it's always drawn)
 *
 * me = panel height - 1 for path bar, 1 for selection, 1 for info bar
 *
 * - Entries Over = how many entries are over selection
 * - Entries Under = how many entries are under selection
 *
 * - Begin Index = from which index should I start looking for
 * visible entries to catch all I need
 *
 * - Over Index = iterator; an index offset relative from selection,
 *   selection+oi = effective index
 * - Under Index = iterator; an index offset relative from selection,
 *   selection-ui = effective index
 */
/*
 * At which index should I start looking
 * for visible entries to catch all that can be displayed
 */
static fnum_t _start_search_index(const struct panel* const s,
		const fnum_t nhf, const fnum_t me) {
	fnum_t eo = 0;// Entries Over
	fnum_t oi = 1;// Over Index
	fnum_t bi = 0;// Begin Index
	fnum_t eu = 0;// Entries Under
	fnum_t ui = 1;// Under Index

/* How many entries are under selection? */
	while (s->num_files-nhf && s->selection+ui < s->num_files && eu < me/2) {
		if (visible(s, s->selection+ui)) eu += 1;
		ui += 1;
	}
/* How many entries are over selection?
	 * (If there are few entries under, then use up all remaining space)
	 */
	while (s->num_files-nhf && s->selection >= oi && eo + 1 + eu <= me) {
		if (visible(s, s->selection-oi)) eo += 1;
		bi = s->selection-oi;
		oi += 1;
	}
	return bi;
}

void ui_statusbar(struct ui* const i, struct append_buffer* const ab) {
// TODO now that there are columns...
	const struct file* const _hfr = hfr(i->pv);
	struct tm T;
	if (!_hfr || !localtime_r(&(_hfr->s.st_mtim.tv_sec), &T)) {
		memset(&T, 0, sizeof(struct tm));
	}
	strftime(i->time, TIME_SIZE, timefmt, &T);

	char S[10 +1+10 +2 +1+10+2 +1+FV_ORDER_SIZE +1];
	const fnum_t nhf = (i->pv->show_hidden ? 0 : i->pv->num_hidden);
	int sl = 0;
	sl += snprintf(S, sizeof(S), "%u", i->pv->num_files-nhf);
	if (!i->pv->show_hidden) {
		sl += snprintf(S+sl, sizeof(S)-sl, "+%u", i->pv->num_hidden);
	}
	sl += snprintf(S+sl, sizeof(S)-sl, "f ");
	if (i->pv->num_selected) {
		sl += snprintf(S+sl, sizeof(S)-sl,
				"[%u] ", i->pv->num_selected);
	}
	sl += snprintf(S+sl, sizeof(S)-sl, "%c%.*s",
		(i->pv->scending > 0 ? '+' : '-'),
		(int)FV_ORDER_SIZE, i->pv->order);

	const size_t cw = utf8_width(S);
	const size_t uw = utf8_width(i->user);
	const size_t gw = utf8_width(i->group);
	const size_t sw = uw+1+gw+1+10+1+TIME_SIZE+1;
	append_theme(ab, THEME_STATUSBAR);
	fill(ab, ' ', 1);
	if ((size_t)i->scrw < cw+sw) {
		fill(ab, ' ', i->scrw-1);
		append_attr(ab, ATTR_NORMAL, NULL);
		append(ab, "\r\n", 2);
		return;
	}
	append(ab, S, sl);
	fill(ab, ' ', i->scrw-cw-sw);// Padding

	append(ab, i->user, strnlen(i->user, LOGIN_MAX_LEN));
	fill(ab, ' ', 1);
	append(ab, i->group, strnlen(i->group, LOGIN_MAX_LEN));
	fill(ab, ' ', 1);
	append(ab, &i->perms[0], 1);
	for (size_t p = 1; p < 10; ++p) {
		const mode_t m[2] = {
			(i->perm[0] & 0777) & (0400 >> (p-1)),
			(i->perm[1] & 0777) & (0400 >> (p-1))
		};
		if (m[0] != m[1]) append_attr(ab, ATTR_UNDERLINE, NULL);
		append(ab, &i->perms[p], 1);
		if (m[0] != m[1]) append_attr(ab, ATTR_NOT_UNDERLINE, NULL);
	}
	fill(ab, ' ', 1);
	append(ab, i->time, strnlen(i->time, TIME_SIZE));
	fill(ab, ' ', 1);
	append_attr(ab, ATTR_NORMAL, NULL);
	append(ab, "\r\n", 2);
}

void ui_panels(struct ui* const i, struct append_buffer* const ab) {
	fnum_t e[2] = {
		_start_search_index(i->fvs[0], i->fvs[0]->num_hidden, i->ph-1),
		_start_search_index(i->fvs[1], i->fvs[1]->num_hidden, i->ph-1),
	};
	for (int L = 0; L < i->ph; ++L) {
		append(ab, CSI_CLEAR_LINE);
		for (size_t p = 0; p < 2; ++p) {
			const fnum_t nf = i->fvs[p]->num_files;
			while (e[p] < nf && !visible(i->fvs[p], e[p])) {
				e[p] += 1;
			}
			if (e[p] >= nf) {
				append_theme(ab, THEME_OTHER);
				fill(ab, ' ', i->pw[p]);
				append_attr(ab, ATTR_NORMAL, NULL);
			}
			else {
				_entry(i, i->fvs[p], i->pw[p], e[p]);
			}
			e[p] += 1;
		}
		append(ab, "\r\n", 2);
	}
}

void ui_bottombar(struct ui* const i, struct append_buffer* const ab) {
	append(ab, CSI_CLEAR_LINE);
	if (i->prompt) {
		const size_t aw = utf8_width(i->prch);
		const size_t pw = utf8_width(i->prompt);
		size_t padding;
		if ((size_t)i->scrw > pw+aw) {
			padding = i->scrw-(pw+aw);
		}
		else {
			padding = 0;
		}
		append(ab, i->prch, aw);
		append(ab, i->prompt, strlen(i->prompt));
		if (padding) fill(ab, ' ', padding);
	}
	else if (i->mt) {
		int cp = 0;
		switch (i->mt) {
		case MSG_INFO: cp = THEME_INFO; break;
		case MSG_ERROR: cp = THEME_ERROR; break;
		default: break;
		}
		append_theme(ab, cp);
		append(ab, i->msg, strlen(i->msg));
		append_attr(ab, ATTR_NORMAL, NULL);
		fill(ab, ' ', i->scrw-utf8_width(i->msg));
		i->mt = MSG_NONE;
	}
	else if (i->m == MODE_CHMOD) {
		append(ab, "-- CHMOD --", 11);
		char p[4+1+1+4+1+1+4+1+1+4+1];
		fill(ab, ' ', i->scrw-(sizeof(p)-1)-11);
		snprintf(p, sizeof(p), "%04o +%04o -%04o =%04o",
				i->perm[0] & 07777, i->plus,
				i->minus, i->perm[1] & 07777);
		append(ab, p, sizeof(p));
	}
	else {
// TODO input buffer
	}
}

void ui_draw(struct ui* const i) {
	ui_update_geometry(i);
	if (i->m == MODE_MANAGER || i->m == MODE_WAIT) {
		const struct file* const H = hfr(i->pv);
		if (H) {
			stringify_p(H->s.st_mode, i->perms);
			stringify_u(H->s.st_uid, i->user);
			stringify_g(H->s.st_gid, i->group);
		}
		else {
			memset(i->perms, '-', 10);
			i->user[0] = i->group[0] = 0;
		}
	}
	else if (i->m == MODE_CHMOD) {
		i->perm[1] = (i->perm[0] | i->plus) & ~i->minus;
		stringify_p(i->perm[1], i->perms);
		stringify_u(i->o[1], i->user);
		stringify_g(i->g[1], i->group);
	}
	write(STDOUT_FILENO, CSI_CURSOR_HIDE_TOP_LEFT);
	for (int b = 0; b < BUF_NUM; ++b) {
		if (i->dirty & (1 << b)) {
			i->B[b].top = 0;
			do_draw[b](i, &i->B[b]);
		}
		write(STDOUT_FILENO, i->B[b].buf, i->B[b].top);
	}
	i->dirty = 0;
	if (i->prompt && i->prompt_cursor_pos >= 0) {
		write(STDOUT_FILENO, CSI_CURSOR_SHOW);
		move_cursor(i->scrh, i->prompt_cursor_pos%i->scrw+1);
	}
}

void ui_update_geometry(struct ui* const i) {
	window_size(&i->scrh, &i->scrw);
	i->pw[0] = i->scrw/2;
	i->pw[1] = i->scrw - i->pw[0];
	i->ph = i->scrh - 3;
	i->pxoff[0] = 0;
	i->pxoff[1] = i->scrw/2;
}

int chmod_open(struct ui* const i, char* const path) {
	struct stat s;
	if (stat(path, &s)) return errno;
	errno = 0;
	struct passwd* pwd = getpwuid(s.st_uid);
	if (!pwd) return errno;
	errno = 0;
	struct group* grp = getgrgid(s.st_gid);
	if (!grp) return errno;

	i->o[0] = i->o[1] = s.st_uid;
	i->g[0] = i->g[1] = s.st_gid;
	i->plus = i->minus = 0;
	i->perm[0] = i->perm[1] = s.st_mode;
	i->path = path;
	i->m = MODE_CHMOD;
	xstrlcpy(i->user, pwd->pw_name, LOGIN_BUF_SIZE);
	xstrlcpy(i->group, grp->gr_name, LOGIN_BUF_SIZE);
	return 0;
}

void chmod_close(struct ui* const i) {
	i->m = MODE_MANAGER;
	free(i->path);
	i->path = NULL;
	i->perm[0] = i->perm[1] = 0;
}

int ui_ask(struct ui* const i, const char* const q,
		const struct select_option* o, const size_t oc) {
	const int oldtimeout = i->timeout;
	i->timeout = -1;
	int T = 0;
	char P[512];// TODO

	i->prch[0] = 0;
	i->prompt = P;
	i->prompt_cursor_pos = -1;
	T += snprintf(P+T, sizeof(P)-T, "%s ", q);
	for (size_t j = 0; j < oc; ++j) {
		if (j) T += snprintf(P+T, sizeof(P)-T, ", ");
		char b[KEYNAME_BUF_SIZE];
		_keyname(&o[j].i, b);
		T += snprintf(P+T, sizeof(P)-T, "%s=%s", b, o[j].h);
	}
	i->dirty |= DIRTY_BOTTOMBAR;
	ui_draw(i);
	struct input in;
	for (;;) {
		in = get_input(i->timeout);
		for (size_t j = 0; j < oc; ++j) {
			if (!memcmp(&in, &o[j].i, sizeof(struct input))) {
				i->dirty |= DIRTY_BOTTOMBAR;
				i->prompt = NULL;
				i->timeout = oldtimeout;
				return j;
			}
		}
	}
}

/*
 * Find matching mappings
 * If there are a few, do nothing, wait longer.
 * If there is only one, send it.
 */
enum command get_cmd(struct ui* const i) {
#define ISIZE (sizeof(struct input)*INPUT_LIST_LENGTH)
	int Kn = 0;
	while (Kn < INPUT_LIST_LENGTH && i->K[Kn].t != I_NONE) {
		Kn += 1;
	}
	if (Kn == INPUT_LIST_LENGTH) {
		memset(i->K, 0, ISIZE);
		Kn = 0;
	}
	i->K[Kn] = get_input(i->timeout);
	if (i->K[Kn].t == I_NONE
	|| i->K[Kn].t == I_ESCAPE
	|| IS_CTRL(i->K[Kn], '[')) {
		memset(i->K, 0, ISIZE);
		return CMD_NONE;
	}
	int pm = 0;// partial match
	for (size_t m = 0; m < i->kml; ++m) {
		if (i->kmap[m].m != i->m) continue;
		if (!memcmp(i->K, i->kmap[m].i, ISIZE)) {
			memset(i->K, 0, ISIZE);
			return i->kmap[m].c;
		}
		int M = 1;
		for (int t = 0; t < Kn+1; ++t) {
			M = M && !memcmp(&i->kmap[m].i[t],
				&i->K[t], sizeof(struct input));
		}
		pm += M;
	}
	if (!pm) memset(i->K, 0, ISIZE);
	return CMD_NONE;
#undef ISIZE
}

/*
 * Gets input to buffer
 * Responsible for cursor movement (buftop) and guarding buffer bounds (bsize)
 * If text is ready (enter pressed) returns 0,
 * If aborted, returns -1.
 * If keeps gathering, returns 1.
 * Unhandled inputs are put into 'o' (if not NULL) and 2 is returned
 */
int fill_textbox(struct ui* const I, char* const buf, char** const buftop,
		const size_t bsize, struct input* const o) {
	const struct input i = get_input(I->timeout);
	if (i.t == I_NONE) return 1;
	if (IS_CTRL(i, '[') || i.t == I_ESCAPE) return -1;
	else if ((IS_CTRL(i, 'J') || IS_CTRL(i, 'M'))
	|| (i.t == I_UTF8 && (i.utf[0] == '\n' || i.utf[0] == '\r'))) {
/* If empty, abort.
		 * Otherwise ready.
		 */
		return (*buftop == buf ? -1 : 0);
	}
	else if (i.t == I_UTF8 && strlen(buf)+utf8_g2nb(i.utf) <= bsize) {
		utf8_insert(buf, i.utf, utf8_wtill(buf, *buftop));
		*buftop += utf8_g2nb(i.utf);
	}
	else if (IS_CTRL(i, 'H') || i.t == I_BACKSPACE) {
		if (*buftop != buf) {
			const size_t wt = utf8_wtill(buf, *buftop);
			*buftop -= utf8_remove(buf, wt-1);
		}
		else if (!strnlen(buf, bsize)) {
			return -1;
		}
	}
	else if (IS_CTRL(i, 'D') || i.t == I_DELETE) {
		utf8_remove(buf, utf8_wtill(buf, *buftop));
	}
	else if (IS_CTRL(i, 'A') || i.t == I_HOME) {
		*buftop = buf;
	}
	else if (IS_CTRL(i, 'E') || i.t == I_END) {
		*buftop = buf+strnlen(buf, bsize);
	}
	else if (IS_CTRL(i, 'U')) {
		*buftop = buf;
		memset(buf, 0, bsize);
	}
	else if (IS_CTRL(i, 'K')) {
		const size_t clen = strnlen(buf, bsize);
		memset(*buftop, 0, strnlen(*buftop, bsize-clen));
	}
	else if (IS_CTRL(i, 'F') || i.t == I_ARROW_RIGHT) {
		if ((size_t)(*buftop - buf) < bsize) {
			*buftop += utf8_g2nb(*buftop);
		}
	}
	else if (IS_CTRL(i, 'B') || i.t == I_ARROW_LEFT) {
		if (*buftop - buf) {
			const size_t gt = utf8_wtill(buf, *buftop);
			*buftop = buf;
			for (size_t i = 0; i < gt-1; ++i) {
				*buftop += utf8_g2nb(*buftop);
			}
		}
	}
	I->prompt_cursor_pos = utf8_width(buf)-utf8_width(*buftop)+1;
	if (o) *o = i;
	return 2;
}

int prompt(struct ui* const i, char* const t,
		char* t_top, const size_t t_size) {
	strcpy(i->prch, ">");
	i->prompt = t;
	i->prompt_cursor_pos = 1;
	i->dirty |= DIRTY_BOTTOMBAR;
	ui_draw(i);
	int r;
	do {
		r = fill_textbox(i, t, &t_top, t_size, NULL);
		i->dirty |= DIRTY_BOTTOMBAR;
		ui_draw(i);
	} while (r && r != -1);
	i->dirty |= DIRTY_BOTTOMBAR;
	i->prompt = NULL;
	i->prompt_cursor_pos = -1;
	return r;
}

int ui_rescan(struct ui* const i, struct panel* const a,
		struct panel* const b) {
	int err;
	i->dirty = DIRTY_ALL;
	if (a && (err = panel_scan_dir(a))) {
		failed(i, "directory scan", strerror(err));
		return 0;
	}
	if (b && (err = panel_scan_dir(b))) {
		failed(i, "directory scan", strerror(err));
		return 0;
	}
	return 1;
}

void failed(struct ui* const i, const char* const what,
		const char* const why) {
	i->mt = MSG_ERROR;
	i->dirty |= DIRTY_BOTTOMBAR;
	snprintf(i->msg, MSG_BUFFER_SIZE, "%s failed: %s", what, why);
}

int spawn(char* const arg[], const enum spawn_flags f) {
	int ret = 0, status = 0, nullfd;
	pid_t pid;
	if (write(STDOUT_FILENO, CSI_CLEAR_ALL) == -1) return errno;
	if ((ret = stop_raw_mode(&global_i->T))) return ret;
	if (write(STDOUT_FILENO, "\r\n", 2) == -1) return errno;
	if ((pid = fork()) == -1) {
		ret = errno;
	}
	else if (pid == 0) {
		if (f & SF_SLIENT) {
			nullfd = open("/dev/null", O_WRONLY, 0100);
			if (dup2(nullfd, STDERR_FILENO) == -1) ret = errno;
			if (dup2(nullfd, STDOUT_FILENO) == -1) ret = errno;
			close(nullfd);
		}
		execvp(arg[0], arg);
		exit(EXIT_FAILURE);
	}
	else {
		global_i->dirty |= DIRTY_BOTTOMBAR;
		global_i->mt = MSG_INFO;
		xstrlcpy(global_i->msg,
			"external program is running...", MSG_BUFFER_SIZE);
		waitpid(pid, &status, 0);
	}
	global_i->msg[0] = 0;
	ret = start_raw_mode(&global_i->T);
	global_i->dirty = DIRTY_ALL;
	ui_draw(global_i);
	return ret;
}

size_t append_theme(struct append_buffer* const ab,
		const enum theme_element te) {
	size_t n = 0;
	n += append_attr(ab, theme_scheme[te].fg | ATTR_FOREGROUND,
			theme_scheme[te].fg_color);
	n += append_attr(ab, theme_scheme[te].bg | ATTR_BACKGROUND,
			theme_scheme[te].bg_color);
	return n;
}

/*
 * Checks if file is visible at the moment.
 * Invalid selections are assumed to be invisible.
 * Everything on empty list is invisible.
 * These assumptions make visible() more useful.
 *
 * Truth table - is file visible at the moment?
 *          file.ext | .hidden.ext
 * show_hidden = 1 |1|1|
 * show_hidden = 0 |1|0|
 */
int visible(const struct panel* const fv, const fnum_t i) {
	return fv->num_files && i < fv->num_files
		&& (fv->show_hidden || fv->file_list[i]->name[0] != '.');
}

/* Highlighted File Record */
struct file* hfr(const struct panel* const fv) {
	return (visible(fv, fv->selection)
		? fv->file_list[fv->selection] : NULL);
}

inline void first_entry(struct panel* const fv) {
	fv->selection = fv->num_files-1;
	jump_n_entries(fv, -fv->num_files);
}

inline void last_entry(struct panel* const fv) {
	fv->selection = 0;
	jump_n_entries(fv, fv->num_files);
}

void jump_n_entries(struct panel* const fv, const int n) {
	if (!fv->num_files) {
		fv->selection = 0;
		return;
	}
	fnum_t N = (n > 0 ? n : -n);
	const int d = (n > 0 ? 1 : -1);
	fnum_t i = fv->selection;
	do {
		if (n < 0 && !i) return;
		i += d;
		if (n > 0 && i >= fv->num_files) return;
		if (visible(fv, i)) {
			N -= 1;
			fv->selection = i;
		}
	} while (N);
}

inline void delete_file_list(struct panel* const fv) {
	file_list_clean(&fv->file_list, &fv->num_files);
	fv->selection = fv->num_hidden = 0;
}

/*
 * Returns index of given file on list or -1 if not present
 */
fnum_t file_on_list(const struct panel* const fv, const char* const name) {
	fnum_t i = 0;
	while (i < fv->num_files && strcmp(fv->file_list[i]->name, name)) {
		i += 1;
	}
	return (i == fv->num_files ? (fnum_t)-1 : i);
}

/* Finds and highlighs file with given name */
void file_highlight(struct panel* const fv, const char* const N) {
	fnum_t i = 0;
	while (i < fv->num_files && strcmp(fv->file_list[i]->name, N)) {
		i += 1;
	}
	if (i == fv->num_files) return;
	if (visible(fv, i)) {
		fv->selection = i;
	}
	else {
		first_entry(fv);
	}
}

int file_find(struct panel* const fv, const char* const name,
		const fnum_t start, const fnum_t end) {
	const int d = start <= end ? 1 : -1;
	for (fnum_t i = start; (d > 0 ? i <= end : i >= end); i += d) {
		if (visible(fv, i) && contains(fv->file_list[i]->name, name)) {
			fv->selection = i;
			return 1;
		}
		if (i == end) break;
	}
	return 0;
}

struct file* panel_select_file(struct panel* const fv) {
	struct file* fr;
	if ((fr = hfr(fv))) {
		if ((fr->selected = !fr->selected)) {
			fv->num_selected += 1;
		}
		else {
			fv->num_selected -= 1;
		}
	}
	return fr;
}

int panel_enter_selected_dir(struct panel* const fv) {
	const struct file* H;
	int err;
	if (!(H = hfr(fv))) return 0;
	if ((err = pushd(fv->wd, &fv->wdlen, H->name, H->nl))
	|| (err = panel_scan_dir(fv))) {
		panel_up_dir(fv);
		return err;
	}
	first_entry(fv);
	return 0;
}

int panel_up_dir(struct panel* const fv) {
	int err;
	char prevdir[NAME_BUF_SIZE];
	xstrlcpy(prevdir, fv->wd+current_dir_i(fv->wd), NAME_BUF_SIZE);
	popd(fv->wd, &fv->wdlen);
	if ((err = panel_scan_dir(fv))) {
		return err;
	}
	file_highlight(fv, prevdir);
	return 0;
}

void panel_toggle_hidden(struct panel* const fv) {
	fv->show_hidden = !fv->show_hidden;
	if (!visible(fv, fv->selection)) {
		first_entry(fv);
	}
	if (fv->show_hidden) return;
	for (fnum_t f = 0; f < fv->num_files; ++f) {
		if (fv->file_list[f]->selected && !visible(fv, f)) {
			fv->file_list[f]->selected = 0;
			fv->num_selected -= 1;
		}
	}
}

int panel_scan_dir(struct panel* const fv) {
	int err;
	fv->num_selected = 0;
	err = scan_dir(fv->wd, &fv->file_list, &fv->num_files, &fv->num_hidden);
	if (err) return err;
	panel_sort(fv);
	if (!fv->num_files) {
		fv->selection = 0;
	}
	if (fv->selection >= fv->num_files-1) {
		last_entry(fv);
	}
	if (!visible(fv, fv->selection)){
		jump_n_entries(fv, 1);
	}
	return 0;
}

/*
 * Return:
 * -1: a < b
 *  0: a == b
 *  1: a > b
 *  ...just like memcmp, strcmp
 */
static int frcmp(const enum key cmp,
		const struct file* const a,
		const struct file* const b) {
	const struct passwd *pa, *pb;
	const struct group *ga, *gb;
	switch (cmp) {
	case KEY_NAME:
		return strcmp(a->name, b->name);
	case KEY_SIZE:
		return (a->s.st_size < b->s.st_size ? -1 : 1);
	case KEY_ATIME:
		return (a->s.st_atim.tv_sec < b->s.st_atim.tv_sec ? -1 : 1);
	case KEY_CTIME:
		return (a->s.st_ctim.tv_sec < b->s.st_ctim.tv_sec ? -1 : 1);
	case KEY_MTIME:
		return (a->s.st_mtim.tv_sec < b->s.st_mtim.tv_sec ? -1 : 1);
	case KEY_ISDIR:
		if (S_ISDIR(a->s.st_mode) != S_ISDIR(b->s.st_mode)) {
			return (S_ISDIR(b->s.st_mode) ? 1 : -1);
		}
		break;
	case KEY_PERM:
		return ((a->s.st_mode & 07777) - (b->s.st_mode & 07777));
	case KEY_ISEXE:
		if (EXECUTABLE(a->s.st_mode) != EXECUTABLE(b->s.st_mode)) {
			return (EXECUTABLE(b->s.st_mode) ? 1 : -1);
		}
		break;
	case KEY_INODE:
		return (a->s.st_ino < b->s.st_ino ? -1 : 1);
	case KEY_UID:
		return (a->s.st_uid < b->s.st_uid ? -1 : 1);
	case KEY_GID:
		return (a->s.st_gid < b->s.st_gid ? -1 : 1);
	case KEY_USER:
		if ((pa = getpwuid(a->s.st_uid))
		&& (pb = getpwuid(b->s.st_uid))) {
			return strcmp(pa->pw_name, pb->pw_name);
		}
		break;
	case KEY_GROUP:
		if ((ga = getgrgid(a->s.st_gid))
		&& (gb = getgrgid(b->s.st_gid))) {
			return strcmp(ga->gr_name, gb->gr_name);
		}
		break;
	default:
		break;
	}
	return 0;
}

/*
 * D = destination
 * S = source
 */
inline static void merge(const enum key cmp, const int scending,
		struct file** D, struct file** S,
		const fnum_t beg, const fnum_t mid, const fnum_t end) {
	fnum_t sa = beg;
	fnum_t sb = mid;
	for (fnum_t d = beg; d < end; ++d) {
		if (sa < mid && sb < end) {
			if (0 >= scending * frcmp(cmp, S[sa], S[sb])) {
				D[d] = S[sa];
				sa += 1;
			}
			else {
				D[d] = S[sb];
				sb += 1;
			}
		}
		else if (sa == mid) {
			D[d] = S[sb];
			sb += 1;
		}
		else if (sb == end) {
			D[d] = S[sa];
			sa += 1;
		}
	}
}

void merge_sort(struct panel* const fv, const enum key cmp) {
// TODO inplace if possible
	struct file** tmp;
	struct file** A = fv->file_list;
	struct file** B = calloc(fv->num_files,
			sizeof(struct file*));
	for (fnum_t L = 1; L < fv->num_files; L *= 2) {
		for (fnum_t S = 0; S < fv->num_files; S += L+L) {
			const fnum_t mid = MIN(S+L, fv->num_files);
			const fnum_t end = MIN(S+L+L, fv->num_files);
			merge(cmp, fv->scending, B, A, S, mid, end);
		}
		tmp = A;
		A = B;
		B = tmp;
	}
	fv->file_list = A;
	free(B);
}

void panel_sort(struct panel* const fv) {
	for (size_t i = 0; i < FV_ORDER_SIZE; ++i) {
		if (fv->order[i]) merge_sort(fv, fv->order[i]);
	}
}

char* panel_path_to_selected(struct panel* const fv) {
	const struct file* H;
	if (!(H = hfr(fv))) return NULL;
	const size_t wdl = strnlen(fv->wd, PATH_MAX_LEN);
	char* p = malloc(wdl+1+NAME_BUF_SIZE);
	memcpy(p, fv->wd, wdl+1);
	size_t plen = wdl;
	if (pushd(p, &plen, H->name, H->nl)) {
		free(p);
		return NULL;
	}
	return p;
}

void panel_sorting_changed(struct panel* const fv) {
	char before[NAME_BUF_SIZE];
	memcpy(before, fv->file_list[fv->selection]->name,
		fv->file_list[fv->selection]->nl+1);
	panel_sort(fv);
	file_highlight(fv, before);
}

/*
 * If there is no selection, the highlighted file is selected
 */
void panel_selected_to_list(struct panel* const fv,
		struct string_list* const L) {
	fnum_t start = 0;
	fnum_t stop = fv->num_files-1;
	const struct file* fr;
	if (!fv->num_selected) {
		if (!(fr = panel_select_file(fv))) {
			memset(L, 0, sizeof(struct string_list));
			return;
		}
		start = fv->selection;
		stop = fv->selection;
	}
	L->len = 0;
	L->arr = calloc(fv->num_selected, sizeof(struct string*));
	fnum_t f = start, s = 0;
	for (; f <= stop && s < fv->num_selected; ++f) {
		if (!fv->file_list[f]->selected) continue;
		const size_t fnl = fv->file_list[f]->nl;
		L->arr[L->len] = malloc(sizeof(struct string)+fnl+1);
		L->arr[L->len]->len = fnl;
		memcpy(L->arr[L->len]->str, fv->file_list[f]->name, fnl+1);
		L->len += 1;
		s += 1;
	}
}

void select_from_list(struct panel* const fv,
		const struct string_list* const L) {
	for (fnum_t i = 0; i < L->len; ++i) {
		if (!L->arr[i]) continue;
		for (fnum_t s = 0; s < fv->num_files; ++s) {
			if (!strcmp(L->arr[i]->str, fv->file_list[s]->name)) {
				fv->file_list[s]->selected = 1;
				fv->num_selected += 1;
				break;
			}
		}
	}
}

void panel_unselect_all(struct panel* const fv) {
	fv->num_selected = 0;
	for (fnum_t f = 0; f < fv->num_files; ++f) {
		fv->file_list[f]->selected = 0;
	}
}

/*
 * Needed by rename operation.
 * Checks conflicts with existing files and allows complicated swaps.
 * Pointless renames ('A' -> 'A') are removed from S and R.
 * On unsolvable conflict 0 is retured and no data is modified.
 *
 * S - selected files
 * R - new names for selected files
 *
 * N - list of names
 * a - list of indexes to N
 * at - length of a
 *
 * TODO optimize; plenty of loops, copying, allocation
 * TODO move somewhere else?
 * TODO inline it (?); only needed once
 * TODO code repetition
 */
int rename_prepare(const struct panel* const fv,
		struct string_list* const S,
		struct string_list* const R,
		struct string_list* const N,
		struct assign** const a, fnum_t* const at) {
	*at = 0;
//          vvvvvv TODO calculate size
	*a = calloc(S->len, sizeof(struct assign));
	int* tofree = calloc(S->len, sizeof(int));
	for (fnum_t f = 0; f < R->len; ++f) {
		if (memchr(R->arr[f]->str, '/', R->arr[f]->len)) {
// TODO signal what is wrong
			free(*a);
			*a = NULL;
			*at = 0;
			free(tofree);
			return 0;
		}
		struct string* Rs = R->arr[f];
		struct string* Ss = S->arr[f];
		const fnum_t Ri = file_on_list(fv, Rs->str);
		if (Ri == (fnum_t)-1) continue;
		const fnum_t Si = string_on_list(S, Rs->str, Rs->len);
		if (Si != (fnum_t)-1) {
			const fnum_t NSi = string_on_list(N, Ss->str, Ss->len);
			if (NSi == (fnum_t)-1) {
				(*a)[*at].from = list_push(N, Ss->str, Ss->len);
			}
			else {
				(*a)[*at].from = NSi;
			}
			const fnum_t NRi = string_on_list(N, Rs->str, Rs->len);
			if (NRi == (fnum_t)-1) {
				(*a)[(*at)].to = list_push(N, Rs->str, Rs->len);
			}
			else {
				(*a)[(*at)].to = NRi;
			}
			*at += 1;
			tofree[f] = 1;
		}
		else {
			free(*a);
			*a = NULL;
			*at = 0;
			free(tofree);
			return 0;
		}
	}
	for (fnum_t f = 0; f < R->len; ++f) {
		if (!strcmp(S->arr[f]->str, R->arr[f]->str)) {
			tofree[f] = 1;
		}
	}
	for (fnum_t f = 0; f < *at; ++f) {
		for (fnum_t g = 0; g < *at; ++g) {
			if (f == g) continue;
			if ((*a)[f].from == (*a)[g].from
			|| (*a)[f].to == (*a)[g].to) {
				free(*a);
				*a = NULL;
				*at = 0;
				free(tofree);
				return 0;
			}
		}
	}
	for (fnum_t f = 0; f < *at; ++f) {
		if ((*a)[f].from == (*a)[f].to) {
			(*a)[f].from = (*a)[f].to = (fnum_t)-1;
		}
	}
	for (fnum_t f = 0; f < R->len; ++f) {
		if (!tofree[f]) continue;
		free(S->arr[f]);
		free(R->arr[f]);
		S->arr[f] = R->arr[f] = NULL;
	}
	free(tofree);
	return 1;
}

int conflicts_with_existing(struct panel* const fv,
		const struct string_list* const list) {
	for (fnum_t f = 0; f < list->len; ++f) {
		if (file_on_list(fv, list->arr[f]->str) != (fnum_t)-1) {
			return 1;
		}
	}
	return 0;
}

void remove_conflicting(struct panel* const fv,
		struct string_list* const list) {
	struct string_list repl = { NULL, 0 };
	for (fnum_t f = 0; f < list->len; ++f) {
		if (file_on_list(fv, list->arr[f]->str) == (fnum_t)-1) {
			list_push(&repl, list->arr[f]->str, list->arr[f]->len);
		}
	}
	list_free(list);
	*list = repl;
}

/* Glyph To Number of Bytes
 * how long the current glyph is (in bytes)
 * returns 0 if initial byte is invalid
 */
/* It's simple, really
 * I'm looking at top 5 bits (32 possibilities) of initial byte
 * 00001xxx -> 1 byte
 * 00010xxx -> 1 byte
 * 00011xxx -> 1 byte
 * 16 x 1 byte
 * 01111xxx -> 1 byte
 * 10000xxx -> invalid
 * 8 x invalid
 * 10111xxx -> invalid
 * 11000xxx -> 2 bytes
 * 4 x 2 bytes
 * 11011xxx -> 2 bytes
 * 11100xxx -> 3 bytes
 * 2 x 3 bytes
 * 11101xxx -> 3 bytes
 * 11110xxx -> 4 bytes
 * 1 x 4 bytes
 * 11111xxx -> invalid (see implementation)
 */
size_t utf8_g2nb(const char* const g) {
// Top 5 bits To Length
	static const char t2l[32] = {
		1, 1, 1, 1, 1, 1, 1, 1,//00000xxx - 00111xxx
		1, 1, 1, 1, 1, 1, 1, 1,//01000xxx - 01111xxx
		0, 0, 0, 0, 0, 0, 0, 0,//10000xxx - 10111xxx
		2, 2, 2, 2, 3, 3, 4, 0//11000xxx - 11111xxx

	};
/* This cast is very important */
	return t2l[(unsigned char)(*g) >> 3];
}

/* CodePoint To Number of Bytes */
size_t utf8_cp2nb(const unsigned int cp) {
	if (cp < (unsigned int) 0x80) return 1;
	if (cp < (unsigned int) 0x0800) return 2;
	if (cp < (unsigned int) 0x010000) return 3;
	if (cp < (unsigned int) 0x200000) return 4;
	return 0;
}

/* Apparent width */
size_t utf8_width(const char* b) {
	size_t g = 0;
	size_t s, c;
	while (*b && (s = utf8_g2nb(b)) != 0) {
		uc_code(c, b)
		g += uc_wid((char*)b, c);
		b += s;
	}
	return g;
}

/*
 * Width To Number of Bytes
 * Calculates how much bytes will fill given width
 */
size_t utf8_w2nb(const char* const b, size_t w) {
	size_t c, r = 0;
	while (*(b+r) && w > 0) {
		r += utf8_g2nb(b+r);
		uc_code(c, (b+r))
		w -= uc_wid((char*)b+r, c);
	}
	return r;
}

/*
 * Width till some address in that string
 */
size_t utf8_wtill(const char* a, const char* const b) {
	size_t c, w = 0;
	while (b - a > 0) {
		uc_code(c, a)
		w += uc_wid((char*)a, c);
		a += utf8_g2nb(a);
	}
	return w;
}

int utf8_validate(const char* const b) {
	const size_t bl = strlen(b);
	size_t i = 0;
	while (i < bl) {
		const size_t s = utf8_g2nb(b+i);
		if (!s) break;
		/* Now check if bytes following the
		 * inital byte are like 10xxxxxx
		 */
		if (s > 1) {
			const char* v = b+i+1;
			while (v < b+i+s) {
				if ((*v & 0xc0) != 0x80) return 0;
				v += 1;
			}
		}
		i += s;
	}
	return i == bl;
}

void utf8_insert(char* a, const char* const b, const size_t pos) {
	const size_t bl = strlen(b);
	for (size_t i = 0; i < pos; ++i) {
		a += utf8_g2nb(a);
	}
	memmove(a+bl, a, strlen(a));
	memcpy(a, b, bl);
}

/*
 * Remove glyph at index
 * returns length in bytes of removed glyph
 */
size_t utf8_remove(char* const a, const size_t j) {
	char* t = a;
	for (size_t i = 0; i < j; ++i) {
		t += utf8_g2nb(t);
	}
	const size_t rl = utf8_g2nb(t);// Removed glyph Length
	memmove(t, t+rl, strlen(t));
	return rl;
}

/*
 * Copies only valid utf8 characters and non-control ascii to buf
 */
unsigned cut_unwanted(const char* str, char* buf, const char c, size_t n) {
	unsigned u = 0;
	while (*str && n) {
		const size_t nb = utf8_g2nb(str);
		if (!nb || (nb == 1 && *str < ' ')) {
			*buf = c;
			buf += 1;
			str += 1;
			u += 1;
		}
		else {
			memcpy(buf, str, nb);
			buf += nb;
			str += nb;
		}
		n -= 1;
	}
	*buf = 0;// null-terminator
	return u;
}

xtime_ms_t xtime(void) {
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void task_new(struct task* const t, const enum task_type tp,
		const enum task_flags tf,
		char* const src, char* const dst,
		const struct string_list* const sources,
		const struct string_list* const renamed) {
	t->t = tp;
	t->ts = TS_ESTIMATE;
	t->tf = tf;
	t->src = src;// TODO lengths

	t->dst = dst;
	t->sources = *sources;
	t->renamed = *renamed;
	t->in = t->out = -1;
	t->current_source = 0;
	t->err = 0;
	t->conflicts = t->symlinks = t->specials = 0;
	t->size_total = t->size_done = 0;
	t->files_total = t->files_done = t->dirs_total = t->dirs_done = 0;
	t->chp = t->chm = 0;
	t->cho = -1;
	t->chg = -1;
	memset(&t->tw, 0, sizeof(struct tree_walk));
}

static void _close_files(struct task* const t) {
	if (t->in != -1) close(t->in);
	if (t->out != -1) close(t->out);
	t->in = t->out = -1;
}

void task_clean(struct task* const t) {
	list_free(&t->sources);
	list_free(&t->renamed);
	t->src = t->dst = NULL;
	t->t = TASK_NONE;
	t->tf = 0;
	t->ts = TS_CLEAN;
	t->conflicts = t->specials = t->err = 0;
	_close_files(t);
}

int task_build_path(const struct task* const t, char* R) {
// TODO
// TODO be smarter about checking the length
	const char* S = NULL;
	const char* D = NULL;
	size_t D_len = 0;
	size_t old_len = strnlen(t->src, PATH_MAX_LEN);
	if (t->renamed.len && t->renamed.arr[t->current_source]) {
		S = t->sources.arr[t->current_source]->str;
		D = t->renamed.arr[t->current_source]->str;
		D_len += strnlen(D, NAME_MAX_LEN);
		old_len += 1+strnlen(S, NAME_MAX_LEN);
	}
	char* const _R = R;
	memset(_R, 0, PATH_BUF_SIZE);
	const size_t dst_len = strnlen(t->dst, PATH_MAX_LEN);
	memcpy(R, t->dst, dst_len);
	R += dst_len;
	if (*(R-1) != '/') {
		*R = '/';
		R += 1;
	}
	if (R - _R > PATH_MAX_LEN) {
		memset(_R, 0, PATH_BUF_SIZE);
		return ENAMETOOLONG;
	}
	memcpy(R, D, D_len);
	R += D_len;
	if (*(R-1) != '/') {
		*R = '/';
		R += 1;
	}
	if (R - _R > PATH_MAX_LEN) {
		memset(_R, 0, PATH_BUF_SIZE);
		return ENAMETOOLONG;
	}
	const char* P = t->tw.path;
	P += old_len;
	if (*P == '/') {
		P += 1;
		old_len -= 1;
	}
	const size_t ppart = t->tw.pathlen-old_len;
	if ((R - _R)+ppart > PATH_MAX_LEN) {
		memset(_R, 0, PATH_BUF_SIZE);
		return ENAMETOOLONG;
	}
	memcpy(R, P, ppart);
	if (*(R+ppart-1) == '/') {// TODO

		*(R+ppart-1) = 0;
	}
	return 0;
}

void task_action_chmod(struct task* const t, int* const c) {
	switch (t->tw.tws) {
	case AT_LINK:
	case AT_FILE:
		t->files_done += 1;
		break;
	case AT_DIR:
		t->dirs_done += 1;
		break;
	default:
		break;
	}
	if ((t->chp != 0 || t->chm != 0)
	&& (t->err = relative_chmod(t->tw.path, t->chp, t->chm))) {
		return;
	}
	if (chown(t->tw.path, t->cho, t->chg) ? (t->err = errno) : 0) return;
	if ((t->err = tree_walk_step(&t->tw))) return;
	if (!(t->tf & TF_RECURSIVE_CHMOD)) {// TODO find a better way to chmod once

		t->tw.tws = AT_EXIT;
	}
	*c -= 1;// TODO

}

void task_action_estimate(struct task* const t, int* const c) {
	switch (t->tw.tws) {
	case AT_LINK:
		if (!(t->tf & (TF_ANY_LINK_METHOD))) {
			*c = 0;// TODO

			return;
		}
		t->symlinks += 1;
		t->files_total += 1;
		break;
	case AT_FILE:
		t->files_total += 1;
		break;
	case AT_DIR:
		t->dirs_total += 1;
		break;
	default:
		t->specials += 1;
		break;
	}
	const int Q = t->tw.tws & (AT_DIR | AT_LINK | AT_FILE);
	if ((t->t == TASK_COPY || t->t == TASK_MOVE) && Q) {
		char new_path[PATH_BUF_SIZE];
		task_build_path(t, new_path);
		if (!access(new_path, F_OK)) {
			t->conflicts += 1;
		}
	}
	t->size_total += t->tw.cs.st_size;// TODO
	*c -= 1;
	if ((t->err = tree_walk_step(&t->tw))) {
		t->ts = TS_FAILED;
	}
}

void task_do(struct task* const t, task_action ta,
		const enum task_state onend) {
	int c;
	if (t->ts & TS_ESTIMATE) {
		c = 1024*2;
	}
	else {
		c = 1024 * 1024 * 16;
	}
	if (t->tw.tws == AT_NOWHERE) {
		t->err = tree_walk_start(&t->tw, t->src,
			t->sources.arr[t->current_source]->str,
			t->sources.arr[t->current_source]->len);
		if (t->err) {
			t->tw.tws = AT_NOWHERE;
			return;
		}
	}
	if (t->tf & TF_DEREF_LINKS) {
		t->tw.tl = 1;
	}
	while (t->tw.tws != AT_EXIT && !t->err && c > 0) {
		ta(t, &c);
	}
	if (t->err) {
		t->ts = TS_FAILED;
	}
	else if (t->tw.tws == AT_EXIT) {
		t->current_source += 1;
		t->tw.tws = AT_NOWHERE;
		if (t->current_source == t->sources.len) {
			t->ts = onend;
			tree_walk_end(&t->tw);
			t->current_source = 0;
		}
	}
}

static int _files_opened(const struct task* const t) {
	return t->out != -1 && t->in != -1;
}

static int _open_files(struct task* const t,
		const char* const dst, const char* const src) {
	t->out = open(src, O_RDONLY);
	if (t->out == -1) {
		return errno;// TODO TODO IMPORTANT

	}
	t->in = open(dst, O_WRONLY | O_CREAT, t->tw.cs.st_mode);
	if (t->in == -1) {
		close(t->out);
		t->out = -1;
		return errno;
	}
	struct stat outs;
	if (fstat(t->out, &outs)) return errno;
#if HAS_FALLOCATE

	if (outs.st_size > 0) {
		int e = posix_fallocate(t->in, 0, outs.st_size);
// TODO detect earlier if fallocate is supported

		if (e != EOPNOTSUPP && e != ENOSYS) return e;
	}
#endif
	return 0;
}

static int _copy(struct task* const t, const char* const src,
		const char* const dst, int* const c) {
	char buf[BUFSIZ];
// TODO if it fails at any point it should se\k back
// to enable retrying
	int e = 0;
	if (!_files_opened(t) && (e = _open_files(t, dst, src))) {
		return e;
	}
	ssize_t wb = -1, rb = -1;
	while (*c > 0 && _files_opened(t)) {
		rb = read(t->out, buf, sizeof(buf));
		if (!rb) {// done copying

			t->files_done += 1;
			_close_files(t);
			return e;
		}
		if (rb == -1) {
			e = errno;
			_close_files(t);
			return e;
		}
		wb = write(t->in, buf, rb);
		if (wb == -1) {
			e = errno;
			_close_files(t);
			return e;
		}
		t->size_done += wb;
		*c -= wb;
	}
	return 0;
}

/*
 * No effects on failure
 */
static int _stat_file(struct tree_walk* const tw) {
// lstat/stat can errno:
// ENOENT, EACCES, ELOOP, ENAMETOOLONG, ENOMEM, ENOTDIR, EOVERFLOW
	const struct stat old_cs = tw->cs;
	const enum tree_walk_state old_tws = tw->tws;
	if (lstat(tw->path, &tw->cs)) {
		tw->cs = old_cs;
		return errno;
	}
	tw->tws = AT_NOWHERE;
	do {
		switch (tw->cs.st_mode & S_IFMT) {
		case S_IFDIR:
			tw->tws = AT_DIR;
			break;
		case S_IFREG:
			tw->tws = AT_FILE;
			break;
		case S_IFLNK:
			if (!tw->tl) {
				tw->tws = AT_LINK;
			}
			else if (stat(tw->path, &tw->cs)) {
				int err = errno;
				if (err == ENOENT || err == ELOOP) {
					tw->tws = AT_LINK;
				}
				else {
					tw->cs = old_cs;
					tw->tws = old_tws;
					return err;
				}
			}
			break;
		default:
			tw->tws = AT_SPECIAL;
			break;
		}
	} while (tw->tws == AT_NOWHERE);
	return 0;
}

int tree_walk_start(struct tree_walk* const tw,
		const char* const path,
		const char* const file,
		const size_t file_len) {
	if (tw->path) free(tw->path);
	tw->pathlen = strnlen(path, PATH_MAX_LEN);
	tw->path = malloc(PATH_BUF_SIZE);
	memcpy(tw->path, path, tw->pathlen+1);
	pushd(tw->path, &tw->pathlen, file, file_len);
	tw->tl = 0;
	if (tw->dt) free(tw->dt);
	tw->dt = calloc(1, sizeof(struct dirtree));
	return _stat_file(tw);
}

void tree_walk_end(struct tree_walk* const tw) {
	struct dirtree* DT = tw->dt;
	while (DT) {
		struct dirtree* UP = DT->up;
		free(DT);
		DT = UP;
	}
	free(tw->path);
	memset(tw, 0, sizeof(struct tree_walk));
}

//TODO: void tree_walk_skip(struct tree_walk* const tw) {}
int tree_walk_step(struct tree_walk* const tw) {
	struct dirtree *new_dt, *up;
	switch (tw->tws)  {
	case AT_LINK:
	case AT_FILE:
		if (!tw->dt->cd) {
			tw->tws = AT_EXIT;
			return 0;
		}
		popd(tw->path, &tw->pathlen);
		break;
	case AT_DIR:
/* Go deeper */
		new_dt = calloc(1, sizeof(struct dirtree));
		new_dt->up = tw->dt;
		tw->dt = new_dt;
		errno = 0;
		if (!(new_dt->cd = opendir(tw->path))) {
			tw->dt = new_dt->up;
			free(new_dt);
			return errno;
		}
		break;
	case AT_DIR_END:
/* Go back */
		if (tw->dt->cd) closedir(tw->dt->cd);
		up = tw->dt->up;
		free(tw->dt);
		tw->dt = up;
		popd(tw->path, &tw->pathlen);
		break;
	default:
		break;
	}
	if (!tw->dt || !tw->dt->up) {// last dir
		free(tw->dt);
		tw->dt = NULL;
		tw->tws = AT_EXIT;
		return 0;
	}
	struct dirent* ce;
	do {
		errno = 0;
		ce = readdir(tw->dt->cd);
	} while (ce && DOTDOT(ce->d_name));
// TODO errno
	if (!ce) {
		tw->tws = AT_DIR_END;
		return errno;
	}
	const size_t nl = strnlen(ce->d_name, NAME_MAX_LEN);
	pushd(tw->path, &tw->pathlen, ce->d_name, nl);
	return _stat_file(tw);
}

inline static int _copyremove_step(struct task* const t, int* const c) {
// TODO absolute mess; simplify
// TODO skipped counter
	char np[PATH_BUF_SIZE];
	const int cp = t->t & (TASK_COPY | TASK_MOVE);
	const int rm = t->t & (TASK_MOVE | TASK_REMOVE);
	const int ov = t->tf & TF_OVERWRITE_CONFLICTS;
	int err = 0;
/* QUICK MOVE */
	if ((t->t & TASK_MOVE) && same_fs(t->src, t->dst)) {
		task_build_path(t, np);
		if (rename(t->tw.path, np)) {
			return errno;
		}
		t->tw.tws = AT_EXIT;
		t->size_done = t->size_total;
		t->files_done = t->files_total;
		t->dirs_done = t->dirs_total;
		return 0;
	}
/* SKIP LINKS FLAG */
	if ((t->tw.tws & AT_LINK) && (t->tf & TF_SKIP_LINKS)) {
		return 0;
	}
/* COPYING */
	if (cp) {
		task_build_path(t, np);
/* IF DESTINATION EXISTS */
		if (!access(np, F_OK)) {
			if (t->tf & TF_SKIP_CONFLICTS) return 0;
			if (ov || (t->tf & TF_OVERWRITE_ONCE)) {
				t->tf &= ~TF_OVERWRITE_ONCE;
				if ((t->tw.tws & (AT_FILE | AT_LINK))
					&& unlink(np)) return errno;
			}
			else if (t->tf & TF_ASK_CONFLICTS) {
				return EEXIST;
			}
		}

		switch (t->tw.tws) {
		case AT_FILE:
			if ((err = _copy(t, t->tw.path, np, c))) {
				return err;
			}
/* Opened = unfinished.
			 * Next call will push copying forward. */


			if (_files_opened(t)) {
				return 0;
			}
			break;
		case AT_LINK:
			if (t->tf & TF_RAW_LINKS) {
				err = link_copy_raw(t->tw.path, np);
			}
			else {
				err = link_copy_recalculate(t->src,
						t->tw.path, np);
			}
			if (err) return err;
			t->size_done += t->tw.cs.st_size;
			t->files_done += 1;
			break;
		case AT_DIR:
			if (mkdir(np, t->tw.cs.st_mode)) {
/* TODO
				 * One cannot remove non-empty directory
				 * to prevent EEXIST error on overwrite flag
				 */
				err = errno;
				if (ov && err == EEXIST) {
					err = 0;
				}
			}
			if (err) return err;
			t->size_done += t->tw.cs.st_size;
			t->dirs_done += 1;
			*c -= t->tw.cs.st_size;
			break;
		default:
			break;
		}
	}
	if (rm) {
		if (t->tw.tws & (AT_FILE | AT_LINK)) {
			if (unlink(t->tw.path)) {
				return errno;
			}
			if (!cp) {// TODO

				t->size_done += t->tw.cs.st_size;
				t->files_done += 1;
				*c -= t->tw.cs.st_size;
			}
		}
		else if (t->tw.tws & AT_DIR_END) {
			if (rmdir(t->tw.path)) {
				return errno;
			}
			t->size_done += t->tw.cs.st_size;
			t->dirs_done += 1;
			*c -= t->tw.cs.st_size;
		}
	}
	return 0;
}

void task_action_copyremove(struct task* const t, int* const c) {
	if ((t->err = _copyremove_step(t, c))
	|| (_files_opened(t))
	|| (t->err = tree_walk_step(&t->tw))) {
		return;
	}
}

ssize_t xread(int fd, void* buf, ssize_t count, int timeout_us) {
	struct timespec T = { 0, (suseconds_t)timeout_us*1000 };
	fd_set rfds;
	int retval;
	if (timeout_us > 0) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
	}
	ssize_t rd;
	do {
		if (timeout_us > 0) {
			retval = pselect(fd+1, &rfds, NULL, NULL, &T, NULL);
			if (retval == -1 || !retval) {
				FD_CLR(fd, &rfds);
				return 0;
			}
		}
		rd = read(fd, buf, count);
	} while (rd < 0 && errno == EINTR && errno == EAGAIN);
	FD_CLR(fd, &rfds);
	return rd;
}

static enum input_type which_key(char* const seq) {
	int i = 0;
	while (SKM[i].seq != NULL && SKM[i].t != I_NONE) {
		if (!strcmp(SKM[i].seq, seq)) return SKM[i].t;
		i += 1;
	}
	return I_NONE;
}

struct input get_input(int timeout_us) {
	const int fd = STDIN_FILENO;
	struct input i;
	memset(&i, 0, sizeof(struct input));
	int utflen;
	char seq[7];
	memset(seq, 0, sizeof(seq));
	if (xread(fd, seq, 1, timeout_us) == 1 && seq[0] == '\x1b') {
		if (xread(fd, seq+1, 1, ESC_TIMEOUT_MS*1000) == 1
		&& (seq[1] == '[' || seq[1] == 'O')) {
			if (xread(fd, seq+2, 1, 0) == 1
			&& '0' <= seq[2] && seq[2] <= '9') {
				xread(fd, seq+3, 1, 0);
			}
		}
		i.t = which_key(seq);
	} else if (seq[0] == 0x7f) {
#if defined(__linux__) || defined(__linux) || defined(linux)
		i.t = I_BACKSPACE;
#else
		i.t = I_DELETE;
#endif
	}
	else if (!(seq[0] & 0x60)) {
		i.t = I_CTRL;
		i.utf[0] = seq[0] | 0x40;
	}
	else if ((utflen = utf8_g2nb(seq))) {
		i.t = I_UTF8;
		i.utf[0] = seq[0];
		int b;
		for (b = 1; b < utflen; ++b) {
			if (xread(fd, i.utf+b, 1, 0) != 1) {
				i.t = I_NONE;
				memset(i.utf, 0, 5);
				return i;
			}
		}
		for (; b < 5; ++b) {
			i.utf[b] = 0;
		}
	}
	return i;
}

int start_raw_mode(struct termios* const before) {
	const int fd = STDIN_FILENO;
	if (write(STDOUT_FILENO, CSI_SCREEN_ALTERNATIVE) == -1
	|| tcgetattr(fd, before)) {
		return errno;
	}
	struct termios raw = *before;
	raw.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
			|INLCR|IGNCR|ICRNL|IXON);
	raw.c_oflag &= ~OPOST;
	raw.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	raw.c_cflag &= ~(CSIZE|PARENB);
	raw.c_cflag |= CS8;
	raw.c_iflag &= ~(BRKINT);
	raw.c_lflag |= ISIG;
	write(STDOUT_FILENO, CSI_CURSOR_HIDE);
	return (tcsetattr(fd, TCSAFLUSH, &raw) ? errno : 0);
}

int stop_raw_mode(struct termios* const before) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, before)) {
		return errno;
	}
	write(STDOUT_FILENO, CSI_SCREEN_NORMAL);
	write(STDOUT_FILENO, CSI_CURSOR_SHOW);
	write(STDOUT_FILENO, "\r\n", 2);
	return 0;
}

int char_attr(char* const B, const size_t S,
		const int F, const unsigned char* const v) {
	if (F < 32) {
		return snprintf(B, S, "\x1b[%dm", F);
	}
	if (!(F & (ATTR_FOREGROUND | ATTR_BACKGROUND))) return 0;
	const char fb = ((F & ATTR_FOREGROUND) ? '3' : '4');
	const char C = F & 0x0000007f;
	if (C) {
		return snprintf(B, S, "\x1b[%c%cm", fb, C);
	}
	if (F & ATTR_COLOR_256) {
		return snprintf(B, S, "\x1b[%c8;5;%hhum", fb, v[0]);
	}
	if (F & ATTR_COLOR_TRUE) {
		return snprintf(B, S, "\x1b[%c8;2;%hhu;%hhu;%hhum",
				fb, v[0], v[1], v[2]);
	}
	return 0;
}

int move_cursor(const unsigned int R, const unsigned int C) {
	char buf[1+1+4+1+4+1+1];
	size_t n = snprintf(buf, sizeof(buf), "\x1b[%u;%uH", R, C);
	return (write(STDOUT_FILENO, buf, n) == -1 ? errno : 0);
}

int window_size(int* const R, int* const C) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
		*R = *C = 0;
		return errno;
	}
	*R = ws.ws_row;
	*C = ws.ws_col;
	return 0;
}

size_t append(struct append_buffer* const ab,
		const char* const b, const size_t s) {
	if (ab->capacity - ab->top < s) {
		do {
			ab->capacity += APPEND_BUFFER_INC;
		} while (ab->capacity - ab->top < s);
		void* tmp = realloc(ab->buf, ab->capacity);
		if (!tmp) return 0;
		ab->buf = tmp;
	}
	memcpy(ab->buf+ab->top, b, s);
	ab->top += s;
	return s;
}

size_t append_attr(struct append_buffer* const ab,
		const int F, const unsigned char* const v) {
	char attr[1+1+1+1+1+1+1+3+1+3+1+3+1+1];
	int n = char_attr(attr, sizeof(attr), F, v);
	return append(ab, attr, n);
}

size_t fill(struct append_buffer* const ab, const char C, const size_t s) {
	if (ab->capacity - ab->top < s) {
		do {
			ab->capacity += APPEND_BUFFER_INC;
		} while (ab->capacity - ab->top < s);
		void* tmp = realloc(ab->buf, ab->capacity);
		if (!tmp) return 0;
		ab->buf = tmp;
	}
	memset(ab->buf+ab->top, C, s);
	ab->top += s;
	return s;
}
