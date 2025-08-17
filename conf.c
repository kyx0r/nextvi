/* nextvi configuration file */
#include "kmap.h"

/* access mode of new files */
int conf_mode = 0600;
#define FTGEN(ft) static char ft_##ft[] = #ft;
#define FT(ft) ft_##ft
FTGEN(c) FTGEN(roff) FTGEN(tex) FTGEN(msg)
FTGEN(mk) FTGEN(sh) FTGEN(py) FTGEN(js) 
FTGEN(html) FTGEN(diff)

struct filetype fts[] = {
	{FT(c), "\\.(c|h|cpp|hpp|cc|cs)$"},			/* C */
	{FT(roff), "\\.(ms|tr|roff|tmac|txt|[1-9])$"},		/* troff */
	{FT(tex), "\\.tex$"},					/* tex */
	{FT(msg), "letter$|mbox$|mail$"},			/* email */
	{FT(mk), "[Mm]akefile$|\\.mk$"},			/* makefile */
	{FT(sh), "\\.(ba|z)?sh$|(ba|z|k)shrc$|profile$"},	/* shell script */
	{FT(py), "\\.py$"},					/* python */
	{FT(js), "\\.js$"},					/* javascript */
	{FT(html), "\\.(html?|css)$"},				/* html,css */
	{FT(diff), "\\.(patch|diff)$"}				/* diff */
};
int ftslen = LEN(fts);

#define IN	0	/* inverse | black */
#define RE	1	/* red */
#define GR	2	/* green */
#define YE	3	/* yellow */
#define BL	4	/* blue */
#define MA	5	/* magenta */
#define CY	6	/* cyan */
#define AY	7	/* gray */
#define AY1	8	/* bright gray */
#define RE1	9	/* bright red */
#define GR1	10	/* bright green */
#define YE1	11	/* bright yellow */
#define BL1	12	/* bright blue */
#define MA1	13	/* bright magenta */
#define CY1	14	/* bright cyan */
#define WH1	15	/* bright white */

#define A(...) (int[]){__VA_ARGS__}

struct highlight hls[] = {
	/* lbuf lines are *always "\n\0" terminated, for $ to work one needs to account for '\n' too */
	/* "/" is default hl, must have at least 1 entry for fallback */
	{"/", NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},  /* <-- optional, used by hll if set */
	{"/", NULL, A(RE1 | SYN_BGMK(GR1)), 0, 3}, /* <-- optional, used by hlp if set */
	{"/", NULL, A(RE1), 0, 1}, /* <-- optional, used by hlw if set */

	{FT(c), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(c), "^.+\\\\\n$", A(CY1 | SYN_SO)},
	{FT(c), "(/\\*(?:(?!^\\*/).)*)|((?:(?!^/\\*).)*\\*/(?>\".*\\*/.*\"))",
		A(BL | SYN_IT, BL, BL), 2},
	{FT(c), NULL, A(RE1 | SYN_BGMK(BL1)), 0, 3},
	{FT(c), NULL, A(RE1), 0, 1},
	{FT(c), "\\<(?:signed|unsigned|char|short|u?int(?:64_t|32_t|16_t|8_t)?|\
long|f(?:loat|64|32)|double|void|enum|union|typedef|static|extern|register|struct|\
s(?:64|32|16|8)|u(?:64|32|16|8)|b32|bool|const|size_t|inline|restrict|\
(true|false|_?_?asm_?_?|mem(?:set|cpy|cmp)|malloc|free|realloc|NULL|std(?:in|\
out|err)|errno)|(return|for|while|if|else|do|sizeof|goto|switch|case|\
default|break|continue))\\>", A(GR1, BL1 | SYN_BD, YE1)},
	{FT(c), "(\\?).+?(:)", A(IN | SYN_SO, YE, YE | SYN_SP)},
	{FT(c), "//.*", A(BL | SYN_IT)},
	{FT(c), "#[ \t]*([a-zA-Z0-9_]+([ \t]*<.*>)?)", A(CY, CY, MA)},
	{FT(c), "([a-zA-Z0-9_]+)\\(", A(IN, SYN_BD)},
	{FT(c), "\"(?:[^\"\\\\]|\\\\.)*\"|\"", A(MA)},
	{FT(c), "'(?:[^\\\\]|\\\\.|\\\\x[0-9a-fA-F]{1,2}|\\\\[0-9]+?)'", A(MA)},
	{FT(c), "[-+.]?\\<(?:0[xX][0-9a-fA-FUL]+|[0-9]+\\.?[0-9eEfFuULl]+|[0-9]+)\\>", A(RE1)},

	{FT(roff), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(roff), NULL, A(RE1), 0, 1},
	{FT(roff), "^[.'][ \t]*(([sS][hH].*)|(de) (.*)|([^ \t\\\\]{2,}))?.*",
		A(BL | SYN_SO, IN, MA | SYN_BD, BL | SYN_BD, MA | SYN_BD, BL | SYN_BD)},
	{FT(roff), "\\\\\".*", A(GR | SYN_IT)},
	{FT(roff), "\\\\{1,2}[*$fgkmns]([^[\\(]|\\(..|\\[[^\\]]*\\])", A(YE)},
	{FT(roff), "\\\\(?:[^[\\(*$fgkmns]|\\(..|\\[[^\\]]*\\])", A(YE)},
	{FT(roff), "\\$[^$]+\\$", A(YE)},

	{FT(tex), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(tex), NULL, A(RE1), 0, 1},
	{FT(tex), "\\\\[^[{ \t]+(\\[([^\\]]+)\\])?(\\{([^}]*)\\})?",
		A(BL | SYN_BD, IN, YE, IN, MA)},
	{FT(tex), "\\$[^$]+\\$", A(YE)},
	{FT(tex), "%.*", A(GR | SYN_IT)},

	{FT(msg), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(msg), NULL, A(RE1), 0, 1},
	{FT(msg), "^From .*20..\n$", A(CY | SYN_BD)},
	{FT(msg), "^Subject: (.*)", A(CY | SYN_BD, BL | SYN_BD)},
	{FT(msg), "^From: (.*)", A(CY | SYN_BD, GR | SYN_BD)},
	{FT(msg), "^To: (.*)", A(CY | SYN_BD, MA | SYN_BD)},
	{FT(msg), "^Cc: (.*)", A(CY | SYN_BD, MA | SYN_BD)},
	{FT(msg), "^[-A-Za-z]+: .+", A(CY | SYN_BD)},
	{FT(msg), "^> .*", A(GR | SYN_IT)},

	{FT(mk), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(mk), NULL, A(RE1), 0, 1},
	{FT(mk), "([A-Za-z0-9_]*)[ \t]*:?=", A(IN, YE)},
	{FT(mk), "\\$[\\({][a-zA-Z0-9_]+[\\)}]|\\$\\$", A(YE)},
	{FT(mk), "#.*", A(GR | SYN_IT)},
	{FT(mk), "([A-Za-z_%.\\-]+):", A(IN, SYN_BD)},

	{FT(sh), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(sh), NULL, A(RE1), 0, 1},
	{FT(sh), "\\<(?:break|case|continue|do|done|elif|else|esac|fi|for|if|in|then|until|while)\\>",
		A(MA | SYN_BD)},
	{FT(sh), "[ \t](#.*)|^(#.*)", A(IN, GR | SYN_IT, GR | SYN_IT)},
	{FT(sh), "\"(?:[^\"\\\\]|\\\\.)*\"", A(BL)},
	{FT(sh), "`(?:[^`\\\\]|\\\\.)*`", A(BL)},
	{FT(sh), "'[^']*'", A(BL)},
	{FT(sh), "\\$(?:\\{[^}]+\\}|[a-zA-Z_0-9]+|[!#$?*@-])", A(RE)},
	{FT(sh), "^([a-zA-Z_0-9]* *\\(\\)) *\\{", A(IN, SYN_BD)},
	{FT(sh), "^\\. .*", A(SYN_BD)},

	{FT(py), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(py), NULL, A(RE1), 0, 1},
	{FT(py), "#.*", A(GR)},
	{FT(py), "\\<(?:and|break|class|continue|def|del|elif|else|except|finally|\
for|from|global|if|import|in|is|lambda|not|or|pass|print|raise|return|try|while)\\>", A(MA)},
	{FT(py), "([a-zA-Z0-9_]+)\\(", A(IN, SYN_BD)},
	{FT(py), "\"{3}.*?\"{3}", A(CY)},
	{FT(py), "((?:(?:(?!^\"\"\").)*\"{3}\n$)|(?:\"{3}(?:(?!^\"\"\").)*)|\"{3})",
		A(CY, CY, CY), -1},
	{FT(py), "[\"](?:\\\\\"|[^\"])*?[\"]", A(BL)},
	{FT(py), "['](?:\\\\'|[^'])*?[']", A(BL)},

	{FT(js), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(js), NULL, A(RE1), 0, 1},
	{FT(js), "(/\\*(?:(?!^\\*/).)*)|((?:(?!^/\\*).)*\\*/(?![\"'`]))",
		A(GR1 | SYN_IT, GR1, GR1), 2},
	{FT(js), "\\<(?:abstract|arguments|await|boolean|\
break|byte|case|catch|char|class|const|continue|debugger|default|delete|do|\
double|else|enum|eval|export|extends|false|final|finally|float|for|function|\
goto|if|implements|import|in|instanceof|int|interface|let|long|native|new|\
null|package|private|protected|public|return|short|static|super|switch|synchronized|\
this|throw|throws|transient|true|try|typeof|var|void|volatile|while|with|yield|\
(Array|Date|hasOwnProperty|Infinity|isFinite|isNaN|isPrototypeOf|length|Math|NaN|\
name|Number|Object|prototype|String|toString|undefined|valueOf))\\>", A(BL1, CY | SYN_BD)},
	{FT(js), "[-+]?\\<(?:0[xX][0-9a-fA-F]+|[0-9]+)\\>", A(RE1)},
	{FT(js), "//.*", A(GR1 | SYN_IT)},
	{FT(js), "'(?:[^'\\\\]|\\\\.)*'", A(MA)},
	{FT(js), "\"(?:[^\"\\\\]|\\\\.)*\"", A(MA)},
	{FT(js), "`(?:[^`\\\\]|\\\\.)*`", A(MA)},

	{FT(html), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(html), "(\\{)[^}]*|(^[^{]*)?(\\})",
		A(AY1 | SYN_SO, MA | SYN_SO, AY1 | SYN_SP, MA | SYN_SP), 3},
	{FT(html), NULL, A(RE1), 0, 1},
	{FT(html), "(/\\*(?:(?!^\\*/).)*)|((?:(?!^/\\*).)*\\*/)",
		A(MA | SYN_IT, MA, MA), 2},
	{FT(html), "(<!--(?:(?!^-->).)*)|((?:(?!^<!--).)*-->)",
		A(MA | SYN_IT, MA, MA), 2},
	{FT(html), "([^\t -,.-/:-@[-^{-~]+:).+;", A(SYN_SO, YE | SYN_SO)},
	{FT(html), "\\<(?:accept|accesskey|align|allow|alt|async|\
auto(?:capitalize|complete|focus|play)|background|\
bgcolor|border|buffered|challenge|charset|checked|cite|\
class|code(?:base)|color|cols|colspan|content(?:editable)|\
contextmenu|controls|coords|crossorigin|import|url|\
csp|data|datetime|decoding|def(?:ault|er)|dir|dirname|\
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
title|translate|type|usemap|value|width|wrap|low|manifest|\
(html|base|head|link|meta|body|address|article|\
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
strike|tt|xmp|doctype|h1|h2|h3|h4|h5|h6|\
(fixed;|absolute;|relative;)))\\>", A(GR, CY, MA1)},
	{FT(html), "\"(?:[^\"\\\\]|\\\\.)*\"", A(BL1)},
	{FT(html), "'(?:[^'\\\\]|\\\\.)*'", A(MA)},
	{FT(html), "#\\<[A-Fa-f0-9]+\\>", A(RE1)},
	{FT(html), "[-+]?\\<(?:0[xX][0-9a-fA-F]+|[0-9]+(?:px|vw|vh|%|s)?)\\>", A(RE1)},
	{FT(html), "<(/)?[^>]+>", A(YE | SYN_SO, MA1)},
	/* do not use this regex on DFA engines, causes catastrophic backtracking */
	{FT(html), "([#.][ \t]*[a-zA-Z0-9_\\-]+\
(?:(?:[, \t]*[#.][a-zA-Z0-9_\\-]+)?)+)(?:.?""?){1,20}\\{", A(SYN_SO, SYN_BD)},
	{FT(html), "&[a-zA-Z0-9_]+;", A(MA)},

	{FT(diff), NULL, A(CY1 | SYN_BD | SYN_SO), 0, 2},
	{FT(diff), "^-.*", A(RE)},
	{FT(diff), "^\\+.*", A(GR)},
	{FT(diff), "^@.*", A(CY)},
	{FT(diff), "^diff .*", A(SYN_BD)},

	/* file manager */
	{"/fm", "^(?:(\\.\\.?(/))|(([^/]+/)+|/.*/|/))?.*\n$",
		A(AY1 | SYN_SP, BL | SYN_SP, CY | SYN_SP, CY)},
	{"/fm", "(/+)\\.\\.(/+)(?>^\\./)", A(BL | SYN_SP, CY, CY | SYN_SO)},
	{"/fm", "[^/]*\\.sh\n$", A(GR)},
	{"/fm", "[^/]*(?:\\.c|\\.h|\\.cpp|\\.cc)\n$", A(MA)},
	{"/fm", "/.*/", A(CY)},

	/* numbers highlight for ^v */
	{"/#", "[0lewEW]", A(CY1 | SYN_BD)},
	{"/#", "1([ \t]*[1-9][ \t]*)9", A(RE1, MA1 | SYN_BD)},
	{"/#", "9[ \t]*([1-9][ \t]*)1", A(RE1, MA1 | SYN_BD)},
	{"/#", "[1-9]", A(RE1)},

	/* numbers highlight for # */
	{"/##", "[0-9]+", A(RE1 | SYN_BD)},

	/* autocomplete dropdown */
	{"/ac", "[^ \t-/:-@[-^{-~]+(?:(\n$)|\n)|\n|([^\n]+(\n))",
		A(IN, SYN_BGMK(RE1), SYN_BGMK(AY1), SYN_BGMK(AY))},
	{"/ac", "[^ \t-/:-@[-^{-~]+$|(.+$)", A(IN, SYN_BGMK(AY1))},
	
	/* ex mode (is never '\n' terminated) */
	{"/ex", "^[^:].*$", A(AY1 | SYN_BD)},
	{"/ex", "^.*$", A(AY1 | SYN_BD | SYN_SO)},
	{"/ex", ":(?!^#)[ \t]*((((?:[/?][^/?]*[/?]?)?[.%$]?(?:'[a-z])?([0-9]*)?)\
(?:([+-])[0-9]+)?)[ \t]*(?:([,;])((?:[/?][^/?]*[/?]?)?[.%$]?(?:'[a-z])?([0-9]*)?)\
(?:([+-])([0-9]+))?)*)((pac|pr|ai|ish|ic|grp|shape|seq|sep|tbs|td|order|hl[lwpr]?|\
left|lim|led|vis|mpt)|[@&!=dk]|b[psx]?|p[uh]?|ac?|e[a!]?!?|f(?:[tdp]|[ \t]?([?/]))?|inc|i|\
(?:g!?|s)[ \t]?(.)?|q!?|reg|rd?|w[!q]?!?|u[czb]?|x!?|ya!?|cm!?|cd?)?",
		A(BL1 | SYN_BD, RE, RE, RE, RE, WH1, MA1, RE, RE, WH1, RE, GR1, CY1, MA1, MA1)},
	{"/ex", "\\\\(.)|(:)#", A(AY1 | SYN_BD, YE, BL1)},
	{"/ex", "!(?:[^!\\\\]|\\\\.)*!?|[%#][0-9]*", A(WH1 | SYN_BD)},

	/* status bar (is never '\n' terminated) */
	{"/-", "^(\".*\").*(\\[[wrf]\\]).*$", A(AY1 | SYN_BD, BL, RE)},
	{"/-", "^<(.+)> [^ ]+ ([0-9]+L) ([0-9]+W) (S[0-9]+) (O[0-9]+) (C[0-9]+)$",
		A(AY1 | SYN_BD, RE1, BL, YE, MA, CY1, YE1)},
	{"/-", "^(\".*\").* ([0-9]{1,3}%) (L[0-9]+) (C[0-9]+) (B-?[0-9]+)?.*$",
		A(AY1 | SYN_BD, BL, RE1, BL, YE1, GR)},
	{"/-", "^.*$", A(AY1 | SYN_BD)},
};
int hlslen = LEN(hls);

/* how to highlight text in the reverse direction */
int conf_hlrev = SYN_BGMK(8);

/* right-to-left characters (used only in dctxs[] and dmarks[]) */
#define CR2L		"ءآأؤإئابةتثجحخدذرزسشصضطظعغـفقكلمنهوىييپچژکگی‌‍؛،»«؟ًٌٍَُِّْٔ"
/* neutral characters (used only in dctxs[] and dmarks[]) */
#define CNEUT		"-!\"#$%&'\\()*+,./:;<=>?@\\^_`{|}~ "

struct dircontext dctxs[] = {
	{"^[" CR2L "]", -1},
	{"^[a-zA-Z_0-9]", +1},
};
int dctxlen = LEN(dctxs);

struct dirmark dmarks[] = {
	{"[" CR2L "][" CNEUT CR2L "]*[" CR2L "]", +1, {-1}},
	{"(^[ \t]*)([^" CR2L "]*)([" CR2L "]*)([^" CR2L "]*)", -1, {0, 1, -1, 1, -1}},
};
int dmarkslen = LEN(dmarks);

struct placeholder _ph[2] = {
	{{0x0,0x1f}, "^", 1, 1},
	{{0x200c,0x200d}, "-", 1, 3},
};
struct placeholder *ph = _ph;
int phlen = LEN(_ph);

char **conf_kmap(int id)
{
	return kmaps[id];
}

int conf_kmapfind(char *name)
{
	for (int i = 0; i < LEN(kmaps); i++)
		if (name && kmaps[i][0] && !strcmp(name, kmaps[i][0]))
			return i;
	return 0;
}

char *conf_digraph(int c1, int c2)
{
	for (int i = 0; i < LEN(digraphs); i++)
		if (digraphs[i][0][0] == c1 && digraphs[i][0][1] == c2)
			return digraphs[i][1];
	return NULL;
}
