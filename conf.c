/* nextvi configuration file */
#include "kmap.h"

/* access mode of new files */
#define MKFILE_MODE		0600

struct filetype fts[] = {
	{"c", "\\.(c|h|cpp|hpp|cc|cs)$"},		/* C */
	{"roff", "\\.(ms|tr|roff|tmac|txt|[1-9])$"},	/* troff */
	{"tex", "\\.tex$"},				/* tex */
	{"msg", "letter$|mbox$|mail$"},			/* email */
	{"mk", "Makefile$|makefile$|\\.mk$"},		/* makefile */
	{"sh", "\\.sh$"},				/* shell script */
	{"py", "\\.py$"},				/* python */
	{"nm", "\\.nm$"},				/* neatmail */
	{"js", "\\.js$"},				/* javascript */
	{"html", "\\.(html?|css)$"},			/* html,css */
	{"diff", "\\.(patch|diff)$"}			/* diff */
};
int ftslen = LEN(fts);

/*
colors 0-15
0 = black | inverse
1 = red3
2 = green3
3 = yellow3
4 = blue2
5 = magenta3
6 = cyan3
7 = gray90
bright colors
8 = gray50
9 = red
10 = green
11 = yellow
12 = blue
13 = magenta
14 = cyan
15 = white
*/

struct highlight hls[] = {
	{"/", NULL, {9}}, /* <-- required, do not remove */

	{"c", "^.+\\\\$", {14}, {1}},
	{"c", NULL, {9}}, /* <-- optional, used by hww if set */
	{"c", "(/\\*[^&&*/]*)|([^\"&&/*]*\\*/)", {4 | SYN_IT}, {0}, 2},
	{"c", "\\<(signed|unsigned|char|short|int|long|float|double|void|\
enum|union|typedef|static|extern|register|struct|f32|u32|s32|u8|\
u64|s64|f64|s8|u16|s16|b32|int32_t|uint32_t|bool|const|size_t|\
int16_t|uint16_t|uint64_t|int64_t|uint8_t|int8_t|inline|restrict)\\>", {10}},
	{"c", "\\<(true|false|asm|__asm|__asm__|memset|memcpy|malloc|\
free|realloc|NULL|stdin|stdout|errno)\\>", {12 | SYN_BD}},
	{"c", "\\<(return|for|while|if|else|do|sizeof|goto|switch|case|\
default|break|continue)\\>", {11}},
	{"c", "//.*$", {4 | SYN_IT}},
	{"c", "#[ \t]*([a-zA-Z0-9_]+([ \t]*<.*>)?)", {6, 6, 5}},
	{"c", "([a-zA-Z0-9_]+)\\(", {0, SYN_BD}},
	{"c", "\"([^\"]|\\\\\")*\"", {5}},
	{"c", "'([^\\\\]|\\\\.)'", {5}},
	{"c", "[-+]?\\<(0[xX][0-9a-fA-FUL]+|[0-9.]{1,}[0-9eEfFuULl]+|[0-9]+)\\>", {9}},

	{"roff", NULL, {9}},
	{"roff", "^[.'][ \t]*((SH.*)|(de) (.*)|([^ \t\\\\]{2,}))?.*$",
		{4, 0, 5 | SYN_BD, 4 | SYN_BD, 5 | SYN_BD, 4 | SYN_BD}, {1}},
	{"roff", "\\\\\".*$", {2 | SYN_IT}},
	{"roff", "\\\\{1,2}[*$fgkmns]([^[\\(]|\\(..|\\[[^]]*\\])", {3}},
	{"roff", "\\\\([^[(*$fgkmns]|\\(..|\\[[^\\]]*\\])", {3}},
	{"roff", "\\$[^$]+\\$", {3}},

	{"tex", NULL, {9}},
	{"tex", "\\\\[^[{ \t]+(\\[([^\\]]+)\\])?(\\{([^}]*)\\})?",
		{4 | SYN_BD, 0, 3, 0, 5}},
	{"tex", "\\$[^$]+\\$", {3}},
	{"tex", "%.*$", {2 | SYN_IT}},

	/* mail */
	{"msg", NULL, {9}},
	{"msg", "^From .*20..$", {6 | SYN_BD}},
	{"msg", "^Subject: (.*)$", {6 | SYN_BD, 4 | SYN_BD}},
	{"msg", "^From: (.*)$", {6 | SYN_BD, 2 | SYN_BD}},
	{"msg", "^To: (.*)$", {6 | SYN_BD, 5 | SYN_BD}},
	{"msg", "^Cc: (.*)$", {6 | SYN_BD, 5 | SYN_BD}},
	{"msg", "^[-A-Za-z]+: .+$", {6 | SYN_BD}},
	{"msg", "^> .*$", {2 | SYN_IT}},

	/* makefile */
	{"mk", NULL, {9}},
	{"mk", "([A-Za-z0-9_]*)[ \t]*=", {0, 3}},
	{"mk", "\\$\\([a-zA-Z0-9_]+\\)", {3}},
	{"mk", "#.*$", {2 | SYN_IT}},
	{"mk", "([A-Za-z_%.]+):", {0, SYN_BD}},

	/* shell script */
	{"sh", NULL, {9}},
	{"sh", "\\<(break|case|continue|do|done|elif|else|esac|fi|for|if|in|then|until|while)\\>",
		{5 | SYN_BD}},
	{"sh", "#.*$", {2 | SYN_IT}},
	{"sh", "\"([^\"\\\\]|\\\\.)*\"", {4}},
	{"sh", "'[^']*'", {4}},
	{"sh", "`([^`\\\\]|\\\\.)*`", {4}},
	{"sh", "\\$(\\{[^}]+\\}|[a-zA-Z_0-9]+)", {1}},
	{"sh", "^([a-zA-Z_0-9]*\\(\\)).*\\{", {0, SYN_BD}},

	/* python */
	{"py", NULL, {9}},
	{"py", "#.*$", {2}},
	{"py", "\\<(and|break|class|continue|def|del|elif|else|except|finally|for|from|global)\\>", {5}},
	{"py", "\\<(if|import|in|is|lambda|not|or|pass|print|raise|return|try|while)\\>", {5}},
	{"py", "([a-zA-Z0-9_]+)\\(", {0, SYN_BD}},
	{"py", "[\"']([^\"']|\\\\\")*[\"']", {4}},

	/* neatmail */
	{"nm", NULL, {9}},
	{"nm", "^([ROU])([0-9]+)\t([^\t]*)\t([^\t]*)",
		{0 | SYN_BGMK(15), 6 | SYN_BD, 12 | SYN_BD, 5, 8 | SYN_BD}},
	{"nm", "^[N].*$", {0 | SYN_BD | SYN_BGMK(6)}},
	{"nm", "^[A-Z][HT].*$", {0 | SYN_BD | SYN_BGMK(13)}},
	{"nm", "^[A-Z][MI].*$", {0 | SYN_BD | SYN_BGMK(11)}},
	{"nm", "^[A-Z][LJ].*$", {7 | SYN_BGMK(15)}},
	{"nm", "^[F].*$", {0 | SYN_BD | SYN_BGMK(7)}},
	{"nm", "^\t.*$", {7 | SYN_IT}},
	{"nm", "^:.*$", {SYN_BD}},

	/* javascript */
	{"js", NULL, {9}},
	{"js", "(/\\*[^&&*/]*)|([^`'\"&&/*]*\\*/)", {10 | SYN_IT}, {0}, 2},
	{"js", "\\<(abstract|arguments|await|boolean|\
break|byte|case|catch|char|class|const|continue|debugger|default|delete|do|\
double|else|enum|eval|export|extends|false|final|finally|float|for|function|\
goto|if|implements|import|in|instanceof|int|interface|let|long|native|new|\
null|package|private|protected|public|return|short|static|super|switch|synchronized|\
this|throw|throws|transient|true|try|typeof|var|void|volatile|while|with|yield)\\>", {12}},
	{"js", "\\<(Array|Date|hasOwnProperty|Infinity|isFinite|isNaN|\
isPrototypeOf|length|Math|NaN|name|Number|Object|prototype|\
String|toString|undefined|valueOf)\\>", {6 | SYN_BD}},
	{"js", "[-+]?\\<(0[xX][0-9a-fA-F]+|[0-9]+)\\>", {9}},
	{"js", "//.*$", {10 | SYN_IT}},
	{"js", "[\"'`]([^\"'`]|\\\\[\"'`])*[\"'`]", {5}},

	/* html */
	{"html", "(\\{)[^}]*|(^[^{]*)?(\\})", {8, 5, 8, 5}, {1, 1, -1, -1}, 3},
	{"html", NULL, {9}},
	{"html", "(/\\*[^&&*/]*)|([^\"&&/*]*\\*/)", {5 | SYN_IT}, {0}, 2},
	{"html", "(<!--[^&&------>]*)|([^&&<!------]*-->)", {5 | SYN_IT}, {0}, 2},
	{"html", "([^\t -,.-/:-@[-^{-~]+:).+;", {0, 3}, {1, 1}},
	{"html", "\\<(accept|accesskey|align|allow|alt|async|\
auto(capitalize|complete|focus|play)|background|\
bgcolor|border|buffered|challenge|charset|checked|cite|\
class|code(base)|color|cols|colspan|content(\
editable)|contextmenu|controls|coords|crossorigin|\
csp|data|datetime|decoding|def(ault|er)|dir|dirname|\
disabled|download|draggable|dropzone|enctype|enterkeyhint|\
equiv|for|form|action|headers|height|hidden|high|href|http|\
icon|id|importance|inputmode|integrity|intrinsicsize|ismap|\
itemprop|keytype|kind|label|lang|language|list|loading|loop|\
max|maxlength|media|method|min|minlength|multiple|muted|\
name|novalidate|open|optimum|pattern|ping|placeholder|\
poster|preload|property|radiogroup|readonly|referrerpolicy|\
rel|required|reversed|rows|rowspan|sandbox|scope|scoped|\
selected|shape|size|sizes|slot|span|spellcheck|src|srcdoc|\
srclang|srcset|start|step|style|summary|tabindex|target|\
title|translate|type|usemap|value|width|wrap|low|manifest)\\>", {2}},
	{"html", "\\<(html|base|head|link|meta|body|address|article|\
aside|footer|header|hgroup|nav|section|blockquote|dd|\
div|dl|dt|figcaption|figure|hr|li|main|ol|p|pre|ul|a|abbr|\
b|bdi|bdo|br|dfn|em|i|kbd|mark|q|rb|rp|rt|rtc|\
ruby|s|samp|small|strong|sub|sup|time|tt|u|var|wbr|area|\
audio|img|map|track|video|embed|iframe|object|\
param|picture|source|canvas|noscript|script|del|ins|caption|\
col|colgroup|table|tbody|td|tfoot|th|thead|tr|button|datalist|\
fieldset|input|legend|meter|optgroup|option|output|\
progress|select|textarea|details|dialog|menu|\
shadow|template|acronym|applet|basefont|\
bgsound|big|blink|center|command|element|font|\
frame|frameset|image|isindex|keygen|listing|marquee|menuitem|\
multicol|nextid|nobr|noembed|noframes|plaintext|spacer|\
strike|tt|xmp|doctype|h1|h2|h3|h4|h5|h6)\\>", {6}},
	{"html", "\"([^\"]|\\\\\")*\"", {12}},
	{"html", "#\\<[A-Fa-f0-9]+\\>", {9}},
	{"html", "[-+]?\\<(0[xX][0-9a-fA-F]+|[0-9]+(px)?)\\>", {9}},
	{"html", "<(/)?[^>]+>", {3, 13}, {1}},
	{"html", "#[ \t]*[a-zA-Z0-9_]+", {SYN_BD}},
	{"html", "&[a-zA-Z0-9_]+", {5}},

	/* diff */
	{"diff", NULL, {9}},
	{"diff", "^-.*$", {1}},
	{"diff", "^\\+.*$", {2}},
	{"diff", "^@.*$", {6}},
	{"diff", "^diff .*$", {SYN_BD}},

	/* file manager */
	{"fm", "^\\.+(((/)\\.\\.+)+)?", {4, 4, 4, 6}},
	{"fm", "[^/]*\\.sh$", {2}},
	{"fm", "[^/]*(\\.c|\\.h|\\.cpp)$", {5, 5}},
	{"fm", "/.+/([^/]+$)?", {6, 8}, {1, 1}},
	{"fm", "(/).+[^/]+$", {8, 6}, {1, 1}},

	/* status bar */
	{"---", "^(\".*\").*(\\[[wr]\\]).*$", {8 | SYN_BD, 4, 1}},
	{"---", "^(\".*\").*( [0-9]{1,3}%) +(L[0-9]+) +(C[0-9]+).*$", {8 | SYN_BD, 4, 9, 4, 11}},
	{"---", "^.*$", {8 | SYN_BD}},
};
int hlslen = LEN(hls);

/* how to hightlight current line (hll option) */
#define SYN_LINE		14 | SYN_BD

/* how to hightlight text in the reverse direction */
#define SYN_REVDIR		7

/* right-to-left characters (used only in dctxs[] and dmarks[]) */
#define CR2L		"ءآأؤإئابةتثجحخدذرزسشصضطظعغـفقكلمنهوىييپچژکگی‌‍؛،»«؟ًٌٍَُِّْ"
/* neutral characters (used only in dctxs[] and dmarks[]) */
#define CNEUT		"-!\"#$%&'\\()*+,./:;<=>?@^_`{|}~ "

struct dircontext dctxs[] = {
	{-1, "^[" CR2L "]"},
	{+1, "^[a-zA-Z_0-9]"},
};
int dctxlen = LEN(dctxs);

struct dirmark dmarks[] = {
	{+0, +1, 1, "\\\\\\*\\[([^\\]]+)\\]"},
	{+1, -1, 0, "[" CR2L "][" CNEUT CR2L "]*[" CR2L "]"},
	{-1, +1, 0, "[a-zA-Z0-9_][^" CR2L "\\\\`$']*[a-zA-Z0-9_]"},
	{+0, +1, 0, "\\$([^$]+)\\$"},
	{+0, +1, 1, "\\\\[a-zA-Z0-9_]+\\{([^}]+)\\}"},
	{-1, +1, 0, "\\\\[^ \t" CR2L "]+"},
};
int dmarkslen = LEN(dmarks);

struct placeholder placeholders[] = {
	{0x200c, "-", 1}, // ‌
	{0x200d, "-", 1}, // ‍
};
int placeholderslen = LEN(placeholders);

int conf_hlrev(void)
{
	return SYN_REVDIR;
}

int conf_hlline(void)
{
	return SYN_LINE;
}

int conf_mode(void)
{
	return MKFILE_MODE;
}

char **conf_kmap(int id)
{
	return kmaps[id];
}

int conf_kmapfind(char *name)
{
	int i;
	for (i = 0; i < LEN(kmaps); i++)
		if (name && kmaps[i][0] && !strcmp(name, kmaps[i][0]))
			return i;
	return 0;
}

char *conf_digraph(int c1, int c2)
{
	int i;
	for (i = 0; i < LEN(digraphs); i++)
		if (digraphs[i][0][0] == c1 && digraphs[i][0][1] == c2)
			return digraphs[i][1];
	return NULL;
}
