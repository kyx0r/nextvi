/* neatvi configuration file */
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
	{"html", "\\.html$"},				/* html */
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
	{"/", {9}, NULL}, /* <-- required, do not remove */

	{"c", {9}, NULL}, /* <-- optional, used by hww if set */
	{"c", {14}, "^.+\\\\$", 1},
	{"c", {4 | SYN_IT}, "(/\\*[^&&*/]*)|([^\"&&/*]*\\*/)", 0, 2},
	{"c", {10}, "\\<(signed|unsigned|char|short|int|long|float|double|void|\
enum|union|typedef|static|extern|register|struct|f32|u32|s32|u8|\
u64|s64|f64|s8|u16|s16|b32|int32_t|uint32_t|bool|const|size_t|\
int16_t|uint16_t|uint64_t|int64_t|uint8_t|int8_t|inline)\\>"},
	{"c", {12 | SYN_BD}, "\\<(true|false|asm|__asm|__asm__|memset|memcpy|malloc|\
free|realloc|NULL|stdin|stdout|errno)\\>"},
	{"c", {11}, "\\<(return|for|while|if|else|do|sizeof|goto|switch|case|\
default|break|continue)\\>"},
	{"c", {4 | SYN_IT}, "//.*$"},
	{"c", {6, 6, 5}, "#[ \t]*([a-zA-Z0-9_]+([ \t]*<.*>)?)"},
	{"c", {0, SYN_BD}, "([a-zA-Z0-9_]+)\\("},
	{"c", {5}, "\"([^\"]|\\\\\")*\""},
	{"c", {5}, "'([^\\]|\\\\.)'"},
	{"c", {9}, "[-+]?\\<(0[xX][0-9a-fA-FUL]+|[0-9.]{1,}[0-9eEfFuULl]+|[0-9]+)\\>"},

	{"roff", {9}, NULL},
	{"roff", {4, 0, 5 | SYN_BD, 4 | SYN_BD, 5 | SYN_BD, 4 | SYN_BD},
		"^[.'][ \t]*((SH.*)|(de) (.*)|([^ \t\\]{2,}))?.*$", 1},
	{"roff", {2 | SYN_IT}, "\\\\\".*$"},
	{"roff", {3}, "\\\\{1,2}[*$fgkmns]([^[(]|\\(..|\\[[^]]*\\])"},
	{"roff", {3}, "\\\\([^[(*$fgkmns]|\\(..|\\[[^]]*\\])"},
	{"roff", {3}, "\\$[^$]+\\$"},

	{"tex", {9}, NULL},
	{"tex", {4 | SYN_BD, 0, 3, 0, 5},
		"\\\\[^[{ \t]+(\\[([^]]+)\\])?(\\{([^}]*)\\})?"},
	{"tex", {3}, "\\$[^$]+\\$"},
	{"tex", {2 | SYN_IT}, "%.*$"},

	/* mail */
	{"msg", {9}, NULL},
	{"msg", {6 | SYN_BD}, "^From .*20..$"},
	{"msg", {6 | SYN_BD, 4 | SYN_BD}, "^Subject: (.*)$"},
	{"msg", {6 | SYN_BD, 2 | SYN_BD}, "^From: (.*)$"},
	{"msg", {6 | SYN_BD, 5 | SYN_BD}, "^To: (.*)$"},
	{"msg", {6 | SYN_BD, 5 | SYN_BD}, "^Cc: (.*)$"},
	{"msg", {6 | SYN_BD}, "^[-A-Za-z]+: .+$"},
	{"msg", {2 | SYN_IT}, "^> .*$"},

	/* makefile */
	{"mk", {9}, NULL},
	{"mk", {0, 3}, "([A-Za-z0-9_]*)[ \t]*="},
	{"mk", {3}, "\\$\\([a-zA-Z0-9_]+\\)"},
	{"mk", {2 | SYN_IT}, "#.*$"},
	{"mk", {0, SYN_BD}, "([A-Za-z_%.]+):"},

	/* shell script */
	{"sh", {9}, NULL},
	{"sh", {5 | SYN_BD}, "\\<(break|case|continue|do|done|elif|else|esac|fi|for|if|in|then|until|while)\\>"},
	{"sh", {2 | SYN_IT}, "#.*$"},
	{"sh", {4}, "\"([^\"\\]|\\\\.)*\""},
	{"sh", {4}, "'[^']*'"},
	{"sh", {4}, "`([^`\\]|\\\\.)*`"},
	{"sh", {1}, "\\$(\\{[^}]+\\}|[a-zA-Z_0-9]+)"},
	{"sh", {0, SYN_BD}, "^([a-zA-Z_0-9]*\\(\\)).*\\{"},

	/* python */
	{"py", {9}, NULL},
	{"py", {2}, "#.*$"},
	{"py", {5}, "\\<(and|break|class|continue|def|del|elif|else|except|finally|for|from|global)\\>"},
	{"py", {5}, "\\<(if|import|in|is|lambda|not|or|pass|print|raise|return|try|while)\\>"},
	{"py", {0, SYN_BD}, "([a-zA-Z0-9_]+)\\("},
	{"py", {4}, "[\"']([^\"']|\\\\\")*[\"']"},

	/* neatmail */
	{"nm", {9}, NULL},
	{"nm", {0 | SYN_BGMK(15), 6 | SYN_BD, 12 | SYN_BD, 5, 8 | SYN_BD},
		"^([ROU])([0-9]+)\t([^\t]*)\t([^\t]*)"},
	{"nm", {0 | SYN_BD | SYN_BGMK(6)}, "^[N].*$"},
	{"nm", {0 | SYN_BD | SYN_BGMK(13)}, "^[A-Z][HT].*$"},
	{"nm", {0 | SYN_BD | SYN_BGMK(11)}, "^[A-Z][MI].*$"},
	{"nm", {7 | SYN_BGMK(15)}, "^[A-Z][LJ].*$"},
	{"nm", {0 | SYN_BD | SYN_BGMK(7)}, "^[F].*$"},
	{"nm", {7 | SYN_IT}, "^\t.*$"},
	{"nm", {SYN_BD}, "^:.*$"},

	/* javascript */
	{"js", {9}, NULL},
	{"js", {10 | SYN_IT}, "(/\\*[^&&*/]*)|([^`'\"&&/*]*\\*/)", 0, 2},
	{"js", {12}, "\\<(abstract|arguments|await|boolean|\
break|byte|case|catch|char|class|const|continue|debugger|default|delete|do|\
double|else|enum|eval|export|extends|false|final|finally|float|for|function|\
goto|if|implements|import|in|instanceof|int|interface|let|long|native|new|\
null|package|private|protected|public|return|short|static|super|switch|synchronized|\
this|throw|throws|transient|true|try|typeof|var|void|volatile|while|with|yield)\\>"},
	{"js", {6 | SYN_BD}, "\\<(Array|Date|hasOwnProperty|Infinity|isFinite|isNaN|\
isPrototypeOf|length|Math|NaN|name|Number|Object|prototype|\
String|toString|undefined|valueOf)\\>"},
	{"js", {9}, "[-+]?\\<(0[xX][0-9a-fA-F]+|[0-9]+)\\>"},
	{"js", {10 | SYN_IT}, "//.*$"},
	{"js", {5}, "[\"'`]([^\"'`]|\\\\[\"'`])*[\"'`]"},

	/* html */
	{"html", {9}, NULL},
	{"html", {5 | SYN_IT}, "(/\\*[^&&*/]*)|([^\"&&/*]*\\*/)", 0, 2},
	{"html", {5 | SYN_IT}, "(<!--[^&&------>]*)|([^&&<!------]*-->)", 0, 2},
	{"html", {2}, "\\<(accept|accesskey|action|align|allow|alt|async|\
auto(capitalize|complete|focus|play)|background|\
bgcolor|border|buffered|challenge|charset|checked|cite|\
class|code(base)|color|cols|colspan|content(\
editable)|contextmenu|controls|coords|crossorigin|\
csp|data|datetime|decoding|def(ault|er)|dir|dirname|\
disabled|download|draggable|dropzone|enctype|enterkeyhint|\
equiv|for|form|action|novalidate|headers|height|\
hidden|high|href|lang|http|icon|id|importance|\
inputmode|integrity|intrinsicsize|ismap|itemprop|keytype|\
kind|label|lang|language|list|loading|loop|low|manifest|\
max|maxlength|media|method|min|minlength|multiple|muted|\
name|novalidate|open|optimum|pattern|ping|placeholder|\
poster|preload|property|radiogroup|readonly|referrerpolicy|\
rel|required|reversed|rows|rowspan|sandbox|scope|scoped|\
selected|shape|size|sizes|slot|span|spellcheck|src|srcdoc|\
srclang|srcset|start|step|style|summary|tabindex|target|\
title|translate|type|usemap|value|width|wrap)\\>"},
	{"html", {6}, "\\<(html|base|head|link|meta|body|address|article|\
aside|footer|header|hgroup|main|nav|section|blockquote|dd|\
div|dl|dt|figcaption|figure|hr|li|main|ol|p|pre|ul|a|abbr|\
b|bdi|bdo|br|dfn|em|i|kbd|mark|q|rb|rp|rt|rtc|\
ruby|s|samp|small|strong|sub|sup|time|tt|u|var|wbr|area|\
audio|img|map|track|video|applet|embed|iframe|noembed|object|\
param|picture|source|canvas|noscript|script|del|ins|caption|\
col|colgroup|table|tbody|td|tfoot|th|thead|tr|button|datalist|\
fieldset|input|legend|meter|optgroup|option|output|\
progress|select|textarea|details|dialog|menu|menuitem|\
element|shadow|template|acronym|applet|basefont|\
bgsound|big|blink|center|command|element|font|\
frame|frameset|image|isindex|keygen|listing|marquee|menuitem|\
multicol|nextid|nobr|noembed|noframes|plaintext|shadow|spacer|\
strike|tt|xmp|doctype|h1|h2|h3|h4|h5|h6)\\>"},
	{"html", {12}, "\"([^\"]|\\\\\")*\""},
	{"html", {9}, "#\\<[A-Fa-f0-9]+\\>"},
	{"html", {9}, "[-+]?\\<(0[xX][0-9a-fA-F]+|[0-9]+(px)?)\\>"},
	{"html", {3, 13}, "<(/)?[^>]+>", 1},
	{"html", {8}, "[^\t -,.-@[-^{-~]*:"},
	{"html", {SYN_BD}, "#[ \t]*[a-zA-Z0-9_]+"},
	{"html", {5}, "&[a-zA-Z0-9_]+"},

	/* diff */
	{"diff", {9}, NULL},
	{"diff", {1}, "^-.*$"},
	{"diff", {2}, "^\\+.*$"},
	{"diff", {6}, "^@.*$"},
	{"diff", {SYN_BD}, "^diff .*$"},

	/* status bar */
	{"---", {8 | SYN_BD, 4, 1}, "^(\".*\").*(\\[[wr]\\]).*$"},
	{"---", {8 | SYN_BD, 4, 9, 4, 11}, "^(\".*\").*( [0-9]{1,3}%) +(L[0-9]+) +(C[0-9]+).*$"},
	{"---", {8 | SYN_BD}, "^.*$"},
};
int hlslen = LEN(hls);

/* how to hightlight current line (hll option) */
#define SYN_LINE		14 | SYN_BD

/* how to hightlight text in the reverse direction */
#define SYN_REVDIR		7

/* right-to-left characters (used only in dctxs[] and dmarks[]) */
#define CR2L		"ءآأؤإئابةتثجحخدذرزسشصضطظعغـفقكلمنهوىييپچژکگی‌‍؛،»«؟ًٌٍَُِّْ"
/* neutral characters (used only in dctxs[] and dmarks[]) */
#define CNEUT		"-!\"#$%&'()*+,./:;<=>?@^_`{|}~ "

struct dircontext dctxs[] = {
	{-1, "^[" CR2L "]"},
	{+1, "^[a-zA-Z_0-9]"},
};
int dctxlen = LEN(dctxs);

struct dirmark dmarks[] = {
	{+0, +1, 1, "\\\\\\*\\[([^]]+)\\]"},
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
