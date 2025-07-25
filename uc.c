unsigned char utf8_length[256] = {
	/*	0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
	/* 0 */ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 1 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 2 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 3 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 4 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 5 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 6 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 7 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 8 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* 9 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* A */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* B */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	/* C */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* D */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* E */ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/* F */ 4, 4, 4, 4, 4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1
};

/* the number of utf-8 characters in a fat nulled s */
int uc_slen(char *s)
{
	int n;
	for (n = 0; uc_len(s); n++)
		s += uc_len(s);
	return n;
}

/* find the beginning of the character at s[i] */
char *uc_beg(char *beg, char *s)
{
	if (utf8_length[0xc0] == 1)
		return s;
	for (; s > beg && ((unsigned char)*s & 0xc0) == 0x80; s--);
	return s;
}

char *uc_chr(char *s, int off)
{
	int i = 0;
	if (!s)
		return "";
	while (uc_len(s)) {
		if (i++ == off)
			return s;
		s += uc_len(s);
	}
	return s;
}

/* the number of characters between s and s + off */
int uc_off(char *s, int off)
{
	char *e = s + off;
	int i;
	for (i = 0; s < e && uc_len(s); i++)
		s += uc_len(s);
	return i;
}

char *uc_subl(char *s, int beg, int end, int *rlen)
{
	char *sbeg = uc_chr(s, beg);
	char *send = uc_chr(sbeg, end - beg);
	int len = sbeg < send ? send - sbeg : 0;
	char *r = emalloc(len + 4);
	memcpy(r, sbeg, len);
	memset(r+len, '\0', 4);
	*rlen = len;
	return r;
}

char *uc_dup(const char *s)
{
	char *r = emalloc(strlen(s) + 1);
	return r ? strcpy(r, s) : NULL;
}

int uc_isspace(char *s)
{
	int c = s ? (unsigned char) *s : 0;
	return c < 0x7f && isspace(c);
}

int uc_isprint(char *s)
{
	int c = s ? (unsigned char) *s : 0;
	return c > 0x7f || isprint(c);
}

int uc_isalpha(char *s)
{
	int c = s ? (unsigned char) *s : 0;
	return c > 0x7f || isalpha(c);
}

int uc_isdigit(char *s)
{
	int c = s ? (unsigned char) *s : 0;
	return c < 0x7f && isdigit(c);
}

int uc_kind(char *c)
{
	if (uc_isspace(c))
		return 0;
	if (uc_isalpha(c) || uc_isdigit(c) || c[0] == '_')
		return 1;
	return 2;
}

#define UC_R2L(ch)	(((ch) & 0xff00) == 0x0600 || \
			((ch) & 0xfffc) == 0x200c || \
			((ch) & 0xff00) == 0xfb00 || \
			((ch) & 0xff00) == 0xfc00 || \
			((ch) & 0xff00) == 0xfe00)

/* sorted list of characters that can be shaped */
static struct achar {
	unsigned int c;		/* utf-8 code */
	unsigned int s;		/* single form */
	unsigned int i;		/* initial form */
	unsigned int m;		/* medial form */
	unsigned int f;		/* final form */
} achars[] = {
	{0x0621, 0xfe80},				/* hamza */
	{0x0622, 0xfe81, 0, 0, 0xfe82},			/* alef madda */
	{0x0623, 0xfe83, 0, 0, 0xfe84},			/* alef hamza above */
	{0x0624, 0xfe85, 0, 0, 0xfe86},			/* waw hamza */
	{0x0625, 0xfe87, 0, 0, 0xfe88},			/* alef hamza below */
	{0x0626, 0xfe89, 0xfe8b, 0xfe8c, 0xfe8a},	/* yeh hamza */
	{0x0627, 0xfe8d, 0, 0, 0xfe8e},			/* alef */
	{0x0628, 0xfe8f, 0xfe91, 0xfe92, 0xfe90},	/* beh */
	{0x0629, 0xfe93, 0, 0, 0xfe94},			/* teh marbuta */
	{0x062a, 0xfe95, 0xfe97, 0xfe98, 0xfe96},	/* teh */
	{0x062b, 0xfe99, 0xfe9b, 0xfe9c, 0xfe9a},	/* theh */
	{0x062c, 0xfe9d, 0xfe9f, 0xfea0, 0xfe9e},	/* jeem */
	{0x062d, 0xfea1, 0xfea3, 0xfea4, 0xfea2},	/* hah */
	{0x062e, 0xfea5, 0xfea7, 0xfea8, 0xfea6},	/* khah */
	{0x062f, 0xfea9, 0, 0, 0xfeaa},			/* dal */
	{0x0630, 0xfeab, 0, 0, 0xfeac},			/* thal */
	{0x0631, 0xfead, 0, 0, 0xfeae},			/* reh */
	{0x0632, 0xfeaf, 0, 0, 0xfeb0},			/* zain */
	{0x0633, 0xfeb1, 0xfeb3, 0xfeb4, 0xfeb2},	/* seen */
	{0x0634, 0xfeb5, 0xfeb7, 0xfeb8, 0xfeb6},	/* sheen */
	{0x0635, 0xfeb9, 0xfebb, 0xfebc, 0xfeba},	/* sad */
	{0x0636, 0xfebd, 0xfebf, 0xfec0, 0xfebe},	/* dad */
	{0x0637, 0xfec1, 0xfec3, 0xfec4, 0xfec2},	/* tah */
	{0x0638, 0xfec5, 0xfec7, 0xfec8, 0xfec6},	/* zah */
	{0x0639, 0xfec9, 0xfecb, 0xfecc, 0xfeca},	/* ain */
	{0x063a, 0xfecd, 0xfecf, 0xfed0, 0xfece},	/* ghain */
	{0x0640, 0x640, 0x640, 0x640},			/* tatweel */
	{0x0641, 0xfed1, 0xfed3, 0xfed4, 0xfed2},	/* feh */
	{0x0642, 0xfed5, 0xfed7, 0xfed8, 0xfed6},	/* qaf */
	{0x0643, 0xfed9, 0xfedb, 0xfedc, 0xfeda},	/* kaf */
	{0x0644, 0xfedd, 0xfedf, 0xfee0, 0xfede},	/* lam */
	{0x0645, 0xfee1, 0xfee3, 0xfee4, 0xfee2},	/* meem */
	{0x0646, 0xfee5, 0xfee7, 0xfee8, 0xfee6},	/* noon */
	{0x0647, 0xfee9, 0xfeeb, 0xfeec, 0xfeea},	/* heh */
	{0x0648, 0xfeed, 0, 0, 0xfeee},			/* waw */
	{0x0649, 0xfeef, 0, 0, 0xfef0},			/* alef maksura */
	{0x064a, 0xfef1, 0xfef3, 0xfef4, 0xfef2},	/* yeh */
	{0x067e, 0xfb56, 0xfb58, 0xfb59, 0xfb57},	/* peh */
	{0x0686, 0xfb7a, 0xfb7c, 0xfb7d, 0xfb7b},	/* tcheh */
	{0x0698, 0xfb8a, 0, 0, 0xfb8b},			/* jeh */
	{0x06a9, 0xfb8e, 0xfb90, 0xfb91, 0xfb8f},	/* fkaf */
	{0x06af, 0xfb92, 0xfb94, 0xfb95, 0xfb93},	/* gaf */
	{0x06cc, 0xfbfc, 0xfbfe, 0xfbff, 0xfbfd},	/* fyeh */
	{0x200c},					/* ZWNJ */
	{0x200d, 0, 0x200d, 0x200d},			/* ZWJ */
};

static struct achar *find_achar(unsigned int c)
{
	int h, m, l;
	h = LEN(achars);
	l = 0;
	/* using binary search to find c */
	while (l < h) {
		m = (h + l) >> 1;
		if (achars[m].c == c)
			return &achars[m];
		if (c < achars[m].c)
			h = m;
		else
			l = m + 1;
	}
	return NULL;
}

static int can_join(int c1, int c2)
{
	struct achar *a1 = find_achar(c1);
	struct achar *a2 = find_achar(c2);
	return a1 && a2 && (a1->i || a1->m) && (a2->f || a2->m);
}

static int uc_cshape(int cur, int prev, int next)
{
	int c = cur;
	int join_prev, join_next;
	struct achar *ac = find_achar(c);
	if (!ac)		/* ignore non-Arabic characters */
		return c;
	join_prev = can_join(prev, c);
	join_next = can_join(c, next);
	if (join_prev && join_next)
		c = ac->m;
	if (join_prev && !join_next)
		c = ac->f;
	if (!join_prev && join_next)
		c = ac->i;
	if (!join_prev && !join_next)
		c = ac->c;	/* some fonts do not have a glyph for ac->s */
	return c ? c : cur;
}

/*
 * return nonzero for Arabic combining characters
 *
 * The standard Arabic diacritics:
 * + 0x064b: fathatan
 * + 0x064c: dammatan
 * + 0x064d: kasratan
 * + 0x064e: fatha
 * + 0x064f: damma
 * + 0x0650: kasra
 * + 0x0651: shadda
 * + 0x0652: sukun
 * + 0x0653: madda above
 * + 0x0654: hamza above
 * + 0x0655: hamza below
 * + 0x0670: superscript alef
 */
int uc_acomb(int c)
{
	return (c >= 0x064b && c <= 0x0655) ||		/* the standard diacritics */
		(c >= 0xfc5e && c <= 0xfc63) ||		/* shadda ligatures */
		c == 0x0670;				/* superscript alef */
}

static void uc_cput(char *d, int c)
{
	int l = 0;
	if (c > 0xffff) {
		*d++ = 0xf0 | (c >> 18);
		l = 3;
	} else if (c > 0x7ff) {
		*d++ = 0xe0 | (c >> 12);
		l = 2;
	} else if (c > 0x7f) {
		*d++ = 0xc0 | (c >> 6);
		l = 1;
	} else {
		*d++ = c;
	}
	while (l--)
		*d++ = 0x80 | ((c >> (l * 6)) & 0x3f);
	*d = '\0';
}

/* shape the given arabic character; returns a static buffer */
char *uc_shape(char *beg, char *s, int c)
{
	static char out[16];
	char *r;
	int tmp, l, prev = 0, next = 0;
	if (!c || !UC_R2L(c))
		return NULL;
	r = s;
	while (r > beg) {
		r = uc_beg(beg, r - 1);
		uc_code(tmp, r, l)
		if (!uc_acomb(tmp)) {
			uc_code(prev, r, l)
			break;
		}
	}
	r = s;
	while (uc_len(r)) {
		r += uc_len(r);
		uc_code(tmp, r, l)
		if (!uc_acomb(tmp)) {
			uc_code(next, r, l)
			break;
		}
	}
	uc_cput(out, uc_cshape(c, prev, next));
	return out;
}

static int dwchars[][2] = {
	{0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x2329}, {0x232A, 0x232A},
	{0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
	{0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693},
	{0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
	{0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3},
	{0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705},
	{0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E},
	{0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0},
	{0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
	{0x2E80, 0x2E99}, {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5}, {0x2FF0, 0x2FFF},
	{0x3000, 0x3000}, {0x3001, 0x3003}, {0x3004, 0x3004}, {0x3005, 0x3005},
	{0x3006, 0x3006}, {0x3007, 0x3007}, {0x3008, 0x3008}, {0x3009, 0x3009},
	{0x300A, 0x300A}, {0x300B, 0x300B}, {0x300C, 0x300C}, {0x300D, 0x300D},
	{0x300E, 0x300E}, {0x300F, 0x300F}, {0x3010, 0x3010}, {0x3011, 0x3011},
	{0x3012, 0x3013}, {0x3014, 0x3014}, {0x3015, 0x3015}, {0x3016, 0x3016},
	{0x3017, 0x3017}, {0x3018, 0x3018}, {0x3019, 0x3019}, {0x301A, 0x301A},
	{0x301B, 0x301B}, {0x301C, 0x301C}, {0x301D, 0x301D}, {0x301E, 0x301F},
	{0x3020, 0x3020}, {0x3021, 0x3029}, {0x302A, 0x302D}, {0x302E, 0x302F},
	{0x3030, 0x3030}, {0x3031, 0x3035}, {0x3036, 0x3037}, {0x3038, 0x303A},
	{0x303B, 0x303B}, {0x303C, 0x303C}, {0x303D, 0x303D}, {0x303E, 0x303E},
	{0x3041, 0x3096}, {0x3099, 0x309A}, {0x309B, 0x309C}, {0x309D, 0x309E},
	{0x309F, 0x309F}, {0x30A0, 0x30A0}, {0x30A1, 0x30FA}, {0x30FB, 0x30FB},
	{0x30FC, 0x30FE}, {0x30FF, 0x30FF}, {0x3105, 0x312F}, {0x3131, 0x318E},
	{0x3190, 0x3191}, {0x3192, 0x3195}, {0x3196, 0x319F}, {0x31A0, 0x31BF},
	{0x31C0, 0x31E3}, {0x31EF, 0x31EF}, {0x31F0, 0x31FF}, {0x3200, 0x321E},
	{0x3220, 0x3229}, {0x322A, 0x3247}, {0x3250, 0x3250}, {0x3251, 0x325F},
	{0x3260, 0x327F}, {0x3280, 0x3289}, {0x328A, 0x32B0}, {0x32B1, 0x32BF},
	{0x32C0, 0x32FF}, {0x3300, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
	{0xA000, 0xA014}, {0xA015, 0xA015}, {0xA016, 0xA48C}, {0xA490, 0xA4C6},
	{0xA960, 0xA97C}, {0xAC00, 0xD7A3}, {0xF900, 0xFA6D}, {0xFA6E, 0xFA6F},
	{0xFA70, 0xFAD9}, {0xFADA, 0xFAFF}, {0xFE10, 0xFE16}, {0xFE17, 0xFE17},
	{0xFE18, 0xFE18}, {0xFE19, 0xFE19}, {0xFE30, 0xFE30}, {0xFE31, 0xFE32},
	{0xFE33, 0xFE34}, {0xFE35, 0xFE35}, {0xFE36, 0xFE36}, {0xFE37, 0xFE37},
	{0xFE38, 0xFE38}, {0xFE39, 0xFE39}, {0xFE3A, 0xFE3A}, {0xFE3B, 0xFE3B},
	{0xFE3C, 0xFE3C}, {0xFE3D, 0xFE3D}, {0xFE3E, 0xFE3E}, {0xFE3F, 0xFE3F},
	{0xFE40, 0xFE40}, {0xFE41, 0xFE41}, {0xFE42, 0xFE42}, {0xFE43, 0xFE43},
	{0xFE44, 0xFE44}, {0xFE45, 0xFE46}, {0xFE47, 0xFE47}, {0xFE48, 0xFE48},
	{0xFE49, 0xFE4C}, {0xFE4D, 0xFE4F}, {0xFE50, 0xFE52}, {0xFE54, 0xFE57},
	{0xFE58, 0xFE58}, {0xFE59, 0xFE59}, {0xFE5A, 0xFE5A}, {0xFE5B, 0xFE5B},
	{0xFE5C, 0xFE5C}, {0xFE5D, 0xFE5D}, {0xFE5E, 0xFE5E}, {0xFE5F, 0xFE61},
	{0xFE62, 0xFE62}, {0xFE63, 0xFE63}, {0xFE64, 0xFE66}, {0xFE68, 0xFE68},
	{0xFE69, 0xFE69}, {0xFE6A, 0xFE6B}, {0xFF01, 0xFF03}, {0xFF04, 0xFF04},
	{0xFF05, 0xFF07}, {0xFF08, 0xFF08}, {0xFF09, 0xFF09}, {0xFF0A, 0xFF0A},
	{0xFF0B, 0xFF0B}, {0xFF0C, 0xFF0C}, {0xFF0D, 0xFF0D}, {0xFF0E, 0xFF0F},
	{0xFF10, 0xFF19}, {0xFF1A, 0xFF1B}, {0xFF1C, 0xFF1E}, {0xFF1F, 0xFF20},
	{0xFF21, 0xFF3A}, {0xFF3B, 0xFF3B}, {0xFF3C, 0xFF3C}, {0xFF3D, 0xFF3D},
	{0xFF3E, 0xFF3E}, {0xFF3F, 0xFF3F}, {0xFF40, 0xFF40}, {0xFF41, 0xFF5A},
	{0xFF5B, 0xFF5B}, {0xFF5C, 0xFF5C}, {0xFF5D, 0xFF5D}, {0xFF5E, 0xFF5E},
	{0xFF5F, 0xFF5F}, {0xFF60, 0xFF60}, {0xFFE0, 0xFFE1}, {0xFFE2, 0xFFE2},
	{0xFFE3, 0xFFE3}, {0xFFE4, 0xFFE4}, {0xFFE5, 0xFFE6}, {0x16FE0, 0x16FE1},
	{0x16FE2, 0x16FE2}, {0x16FE3, 0x16FE3}, {0x16FE4, 0x16FE4}, {0x16FF0, 0x16FF1},
	{0x17000, 0x187F7}, {0x18800, 0x18AFF}, {0x18B00, 0x18CD5}, {0x18D00, 0x18D08},
	{0x1AFF0, 0x1AFF3}, {0x1AFF5, 0x1AFFB}, {0x1AFFD, 0x1AFFE}, {0x1B000, 0x1B0FF},
	{0x1B100, 0x1B122}, {0x1B132, 0x1B132}, {0x1B150, 0x1B152}, {0x1B155, 0x1B155},
	{0x1B164, 0x1B167}, {0x1B170, 0x1B2FB}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
	{0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B},
	{0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320},
	{0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
	{0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F3FA},
	{0x1F3FB, 0x1F3FF}, {0x1F400, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
	{0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A},
	{0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F5FF}, {0x1F600, 0x1F64F},
	{0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7},
	{0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
	{0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF},
	{0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA88}, {0x1FA90, 0x1FABD}, {0x1FABF, 0x1FAC5},
	{0x1FACE, 0x1FADB}, {0x1FAE0, 0x1FAE8}, {0x1FAF0, 0x1FAF8}, {0x20000, 0x2A6DF},
	{0x2A6E0, 0x2A6FF}, {0x2A700, 0x2B739}, {0x2B73A, 0x2B73F}, {0x2B740, 0x2B81D},
	{0x2B81E, 0x2B81F}, {0x2B820, 0x2CEA1}, {0x2CEA2, 0x2CEAF}, {0x2CEB0, 0x2EBE0},
	{0x2EBE1, 0x2EBEF}, {0x2EBF0, 0x2EE5D}, {0x2EE5E, 0x2F7FF}, {0x2F800, 0x2FA1D},
	{0x2FA1E, 0x2FA1F}, {0x2FA20, 0x2FFFD}, {0x30000, 0x3134A}, {0x3134B, 0x3134F},
	{0x31350, 0x323AF}, {0x323B0, 0x3FFFD},
};

static int zwchars[][2] = {
	{0xAD, 0xAD}, {0x300, 0x36F}, {0x483, 0x489}, {0x591, 0x5BD},
	{0x5BF, 0x5BF}, {0x5C1, 0x5C2}, {0x5C4, 0x5C5}, {0x5C7, 0x5C7},
	{0x600, 0x605}, {0x610, 0x61A}, {0x61C, 0x61C}, {0x64B, 0x65F},
	{0x670, 0x670}, {0x6D6, 0x6DD}, {0x6DF, 0x6E4}, {0x6E7, 0x6E8},
	{0x6EA, 0x6ED}, {0x70F, 0x70F}, {0x711, 0x711}, {0x730, 0x74A},
	{0x7A6, 0x7B0}, {0x7EB, 0x7F3}, {0x7FD, 0x7FD}, {0x816, 0x819},
	{0x81B, 0x823}, {0x825, 0x827}, {0x829, 0x82D}, {0x859, 0x85B},
	{0x890, 0x891}, {0x898, 0x89F}, {0x8CA, 0x902}, {0x93A, 0x93A},
	{0x93C, 0x93C}, {0x941, 0x948}, {0x94D, 0x94D}, {0x951, 0x957},
	{0x962, 0x963}, {0x981, 0x981}, {0x9BC, 0x9BC}, {0x9C1, 0x9C4},
	{0x9CD, 0x9CD}, {0x9E2, 0x9E3}, {0x9FE, 0x9FE}, {0xA01, 0xA02},
	{0xA3C, 0xA3C}, {0xA41, 0xA42}, {0xA47, 0xA48}, {0xA4B, 0xA4D},
	{0xA51, 0xA51}, {0xA70, 0xA71}, {0xA75, 0xA75}, {0xA81, 0xA82},
	{0xABC, 0xABC}, {0xAC1, 0xAC5}, {0xAC7, 0xAC8}, {0xACD, 0xACD},
	{0xAE2, 0xAE3}, {0xAFA, 0xAFF}, {0xB01, 0xB01}, {0xB3C, 0xB3C},
	{0xB3F, 0xB3F}, {0xB41, 0xB44}, {0xB4D, 0xB4D}, {0xB55, 0xB56},
	{0xB62, 0xB63}, {0xB82, 0xB82}, {0xBC0, 0xBC0}, {0xBCD, 0xBCD},
	{0xC00, 0xC00}, {0xC04, 0xC04}, {0xC3C, 0xC3C}, {0xC3E, 0xC40},
	{0xC46, 0xC48}, {0xC4A, 0xC4D}, {0xC55, 0xC56}, {0xC62, 0xC63},
	{0xC81, 0xC81}, {0xCBC, 0xCBC}, {0xCBF, 0xCBF}, {0xCC6, 0xCC6},
	{0xCCC, 0xCCD}, {0xCE2, 0xCE3}, {0xD00, 0xD01}, {0xD3B, 0xD3C},
	{0xD41, 0xD44}, {0xD4D, 0xD4D}, {0xD62, 0xD63}, {0xD81, 0xD81},
	{0xDCA, 0xDCA}, {0xDD2, 0xDD4}, {0xDD6, 0xDD6}, {0xE31, 0xE31},
	{0xE34, 0xE3A}, {0xE47, 0xE4E}, {0xEB1, 0xEB1}, {0xEB4, 0xEBC},
	{0xEC8, 0xECE}, {0xF18, 0xF19}, {0xF35, 0xF35}, {0xF37, 0xF37},
	{0xF39, 0xF39}, {0xF71, 0xF7E}, {0xF80, 0xF84}, {0xF86, 0xF87},
	{0xF8D, 0xF97}, {0xF99, 0xFBC}, {0xFC6, 0xFC6}, {0x102D, 0x1030},
	{0x1032, 0x1037}, {0x1039, 0x103A}, {0x103D, 0x103E}, {0x1058, 0x1059},
	{0x105E, 0x1060}, {0x1071, 0x1074}, {0x1082, 0x1082}, {0x1085, 0x1086},
	{0x108D, 0x108D}, {0x109D, 0x109D}, {0x1160, 0x11FF}, {0x135D, 0x135F},
	{0x1712, 0x1714}, {0x1732, 0x1733}, {0x1752, 0x1753}, {0x1772, 0x1773},
	{0x17B4, 0x17B5}, {0x17B7, 0x17BD}, {0x17C6, 0x17C6}, {0x17C9, 0x17D3},
	{0x17DD, 0x17DD}, {0x180B, 0x180F}, {0x1885, 0x1886}, {0x18A9, 0x18A9},
	{0x1920, 0x1922}, {0x1927, 0x1928}, {0x1932, 0x1932}, {0x1939, 0x193B},
	{0x1A17, 0x1A18}, {0x1A1B, 0x1A1B}, {0x1A56, 0x1A56}, {0x1A58, 0x1A5E},
	{0x1A60, 0x1A60}, {0x1A62, 0x1A62}, {0x1A65, 0x1A6C}, {0x1A73, 0x1A7C},
	{0x1A7F, 0x1A7F}, {0x1AB0, 0x1ACE}, {0x1B00, 0x1B03}, {0x1B34, 0x1B34},
	{0x1B36, 0x1B3A}, {0x1B3C, 0x1B3C}, {0x1B42, 0x1B42}, {0x1B6B, 0x1B73},
	{0x1B80, 0x1B81}, {0x1BA2, 0x1BA5}, {0x1BA8, 0x1BA9}, {0x1BAB, 0x1BAD},
	{0x1BE6, 0x1BE6}, {0x1BE8, 0x1BE9}, {0x1BED, 0x1BED}, {0x1BEF, 0x1BF1},
	{0x1C2C, 0x1C33}, {0x1C36, 0x1C37}, {0x1CD0, 0x1CD2}, {0x1CD4, 0x1CE0},
	{0x1CE2, 0x1CE8}, {0x1CED, 0x1CED}, {0x1CF4, 0x1CF4}, {0x1CF8, 0x1CF9},
	{0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
	{0x2066, 0x206F}, {0x20D0, 0x20F0}, {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F},
	{0x2DE0, 0x2DFF}, {0x302A, 0x302D}, {0x3099, 0x309A}, {0xA66F, 0xA672},
	{0xA674, 0xA67D}, {0xA69E, 0xA69F}, {0xA6F0, 0xA6F1}, {0xA802, 0xA802},
	{0xA806, 0xA806}, {0xA80B, 0xA80B}, {0xA825, 0xA826}, {0xA82C, 0xA82C},
	{0xA8C4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA8FF, 0xA8FF}, {0xA926, 0xA92D},
	{0xA947, 0xA951}, {0xA980, 0xA982}, {0xA9B3, 0xA9B3}, {0xA9B6, 0xA9B9},
	{0xA9BC, 0xA9BD}, {0xA9E5, 0xA9E5}, {0xAA29, 0xAA2E}, {0xAA31, 0xAA32},
	{0xAA35, 0xAA36}, {0xAA43, 0xAA43}, {0xAA4C, 0xAA4C}, {0xAA7C, 0xAA7C},
	{0xAAB0, 0xAAB0}, {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF},
	{0xAAC1, 0xAAC1}, {0xAAEC, 0xAAED}, {0xAAF6, 0xAAF6}, {0xABE5, 0xABE5},
	{0xABE8, 0xABE8}, {0xABED, 0xABED}, {0xD7B0, 0xD7C6}, {0xD7CB, 0xD7FB},
	{0xFB1E, 0xFB1E}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
	{0xFFF9, 0xFFFB}, {0x101FD, 0x101FD}, {0x102E0, 0x102E0}, {0x10376, 0x1037A},
	{0x10A01, 0x10A03}, {0x10A05, 0x10A06}, {0x10A0C, 0x10A0F}, {0x10A38, 0x10A3A},
	{0x10A3F, 0x10A3F}, {0x10AE5, 0x10AE6}, {0x10D24, 0x10D27}, {0x10EAB, 0x10EAC},
	{0x10EFD, 0x10EFF}, {0x10F46, 0x10F50}, {0x10F82, 0x10F85}, {0x11001, 0x11001},
	{0x11038, 0x11046}, {0x11070, 0x11070}, {0x11073, 0x11074}, {0x1107F, 0x11081},
	{0x110B3, 0x110B6}, {0x110B9, 0x110BA}, {0x110BD, 0x110BD}, {0x110C2, 0x110C2},
	{0x110CD, 0x110CD}, {0x11100, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11134},
	{0x11173, 0x11173}, {0x11180, 0x11181}, {0x111B6, 0x111BE}, {0x111C9, 0x111CC},
	{0x111CF, 0x111CF}, {0x1122F, 0x11231}, {0x11234, 0x11234}, {0x11236, 0x11237},
	{0x1123E, 0x1123E}, {0x11241, 0x11241}, {0x112DF, 0x112DF}, {0x112E3, 0x112EA},
	{0x11300, 0x11301}, {0x1133B, 0x1133C}, {0x11340, 0x11340}, {0x11366, 0x1136C},
	{0x11370, 0x11374}, {0x11438, 0x1143F}, {0x11442, 0x11444}, {0x11446, 0x11446},
	{0x1145E, 0x1145E}, {0x114B3, 0x114B8}, {0x114BA, 0x114BA}, {0x114BF, 0x114C0},
	{0x114C2, 0x114C3}, {0x115B2, 0x115B5}, {0x115BC, 0x115BD}, {0x115BF, 0x115C0},
	{0x115DC, 0x115DD}, {0x11633, 0x1163A}, {0x1163D, 0x1163D}, {0x1163F, 0x11640},
	{0x116AB, 0x116AB}, {0x116AD, 0x116AD}, {0x116B0, 0x116B5}, {0x116B7, 0x116B7},
	{0x1171D, 0x1171F}, {0x11722, 0x11725}, {0x11727, 0x1172B}, {0x1182F, 0x11837},
	{0x11839, 0x1183A}, {0x1193B, 0x1193C}, {0x1193E, 0x1193E}, {0x11943, 0x11943},
	{0x119D4, 0x119D7}, {0x119DA, 0x119DB}, {0x119E0, 0x119E0}, {0x11A01, 0x11A0A},
	{0x11A33, 0x11A38}, {0x11A3B, 0x11A3E}, {0x11A47, 0x11A47}, {0x11A51, 0x11A56},
	{0x11A59, 0x11A5B}, {0x11A8A, 0x11A96}, {0x11A98, 0x11A99}, {0x11C30, 0x11C36},
	{0x11C38, 0x11C3D}, {0x11C3F, 0x11C3F}, {0x11C92, 0x11CA7}, {0x11CAA, 0x11CB0},
	{0x11CB2, 0x11CB3}, {0x11CB5, 0x11CB6}, {0x11D31, 0x11D36}, {0x11D3A, 0x11D3A},
	{0x11D3C, 0x11D3D}, {0x11D3F, 0x11D45}, {0x11D47, 0x11D47}, {0x11D90, 0x11D91},
	{0x11D95, 0x11D95}, {0x11D97, 0x11D97}, {0x11EF3, 0x11EF4}, {0x11F00, 0x11F01},
	{0x11F36, 0x11F3A}, {0x11F40, 0x11F40}, {0x11F42, 0x11F42}, {0x13430, 0x13440},
	{0x13447, 0x13455}, {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36}, {0x16F4F, 0x16F4F},
	{0x16F8F, 0x16F92}, {0x16FE4, 0x16FE4}, {0x1BC9D, 0x1BC9E}, {0x1BCA0, 0x1BCA3},
	{0x1CF00, 0x1CF2D}, {0x1CF30, 0x1CF46}, {0x1D167, 0x1D169}, {0x1D173, 0x1D182},
	{0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244}, {0x1DA00, 0x1DA36},
	{0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84}, {0x1DA9B, 0x1DA9F},
	{0x1DAA1, 0x1DAAF}, {0x1E000, 0x1E006}, {0x1E008, 0x1E018}, {0x1E01B, 0x1E021},
	{0x1E023, 0x1E024}, {0x1E026, 0x1E02A}, {0x1E08F, 0x1E08F}, {0x1E130, 0x1E136},
	{0x1E2AE, 0x1E2AE}, {0x1E2EC, 0x1E2EF}, {0x1E4EC, 0x1E4EF}, {0x1E8D0, 0x1E8D6},
	{0x1E944, 0x1E94A}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F},
};
int zwlen = LEN(zwchars);
int def_zwlen = LEN(zwchars);

static int bchars[][2] = {
	{0x00000, 0x0001f}, {0x00080, 0x0009f}, {0x00300, 0x0036f},
	{0x00379, 0x00379}, {0x00380, 0x00383}, {0x0038d, 0x0038d},
	{0x00483, 0x00489}, {0x00527, 0x00530}, {0x00558, 0x00558},
	{0x00588, 0x00588}, {0x0058c, 0x005bd}, {0x005c1, 0x005c2},
	{0x005c5, 0x005c5}, {0x005c8, 0x005cf}, {0x005ec, 0x005ef},
	{0x005f6, 0x00605}, {0x00611, 0x0061a}, {0x0061d, 0x0061d},
	{0x0064b, 0x0065f}, {0x006d6, 0x006e4}, {0x006e8, 0x006e8},
	{0x006eb, 0x006ed}, {0x0070f, 0x0070f}, {0x00730, 0x0074c},
	{0x007a7, 0x007b0}, {0x007b3, 0x007bf}, {0x007ec, 0x007f3},
	{0x007fc, 0x007ff}, {0x00817, 0x00819}, {0x0081c, 0x00823},
	{0x00826, 0x00827}, {0x0082a, 0x0082f}, {0x00840, 0x00903},
	{0x0093b, 0x0093c}, {0x0093f, 0x0094f}, {0x00952, 0x00957},
	{0x00963, 0x00963}, {0x00974, 0x00978}, {0x00981, 0x00984},
	{0x0098e, 0x0098e}, {0x00992, 0x00992}, {0x009b1, 0x009b1},
	{0x009b4, 0x009b5}, {0x009bb, 0x009bc}, {0x009bf, 0x009cd},
	{0x009d0, 0x009db}, {0x009e2, 0x009e5}, {0x009fd, 0x00a04},
	{0x00a0c, 0x00a0e}, {0x00a12, 0x00a12}, {0x00a31, 0x00a31},
	{0x00a37, 0x00a37}, {0x00a3b, 0x00a58}, {0x00a5f, 0x00a65},
	{0x00a71, 0x00a71}, {0x00a76, 0x00a84}, {0x00a92, 0x00a92},
	{0x00ab1, 0x00ab1}, {0x00aba, 0x00abc}, {0x00abf, 0x00acf},
	{0x00ad2, 0x00adf}, {0x00ae3, 0x00ae5}, {0x00af2, 0x00b04},
	{0x00b0e, 0x00b0e}, {0x00b12, 0x00b12}, {0x00b31, 0x00b31},
	{0x00b3a, 0x00b3c}, {0x00b3f, 0x00b5b}, {0x00b62, 0x00b65},
	{0x00b73, 0x00b82}, {0x00b8b, 0x00b8d}, {0x00b96, 0x00b98},
	{0x00b9d, 0x00b9d}, {0x00ba1, 0x00ba2}, {0x00ba6, 0x00ba7},
	{0x00bac, 0x00bad}, {0x00bbb, 0x00bcf}, {0x00bd2, 0x00be5},
	{0x00bfc, 0x00c04}, {0x00c11, 0x00c11}, {0x00c34, 0x00c34},
	{0x00c3b, 0x00c3c}, {0x00c3f, 0x00c57}, {0x00c5b, 0x00c5f},
	{0x00c63, 0x00c65}, {0x00c71, 0x00c77}, {0x00c81, 0x00c84},
	{0x00c91, 0x00c91}, {0x00cb4, 0x00cb4}, {0x00cbb, 0x00cbc},
	{0x00cbf, 0x00cdd}, {0x00ce2, 0x00ce5}, {0x00cf3, 0x00d04},
	{0x00d11, 0x00d11}, {0x00d3a, 0x00d3c}, {0x00d3f, 0x00d5f},
	{0x00d63, 0x00d65}, {0x00d77, 0x00d78}, {0x00d81, 0x00d84},
	{0x00d98, 0x00d99}, {0x00dbc, 0x00dbc}, {0x00dbf, 0x00dbf},
	{0x00dc8, 0x00df3}, {0x00df6, 0x00e00}, {0x00e34, 0x00e3e},
	{0x00e48, 0x00e4e}, {0x00e5d, 0x00e80}, {0x00e85, 0x00e86},
	{0x00e8b, 0x00e8c}, {0x00e8f, 0x00e93}, {0x00ea0, 0x00ea0},
	{0x00ea6, 0x00ea6}, {0x00ea9, 0x00ea9}, {0x00eb1, 0x00eb1},
	{0x00eb5, 0x00ebc}, {0x00ebf, 0x00ebf}, {0x00ec7, 0x00ecf},
	{0x00edb, 0x00edb}, {0x00edf, 0x00eff}, {0x00f19, 0x00f19},
	{0x00f37, 0x00f37}, {0x00f3e, 0x00f3f}, {0x00f6d, 0x00f84},
	{0x00f87, 0x00f87}, {0x00f8d, 0x00fbd}, {0x00fcd, 0x00fcd},
	{0x00fda, 0x00fff}, {0x0102c, 0x0103e}, {0x01057, 0x01059},
	{0x0105f, 0x01060}, {0x01063, 0x01064}, {0x01068, 0x0106d},
	{0x01072, 0x01074}, {0x01083, 0x0108d}, {0x0109a, 0x0109d},
	{0x010c7, 0x010cf}, {0x010fe, 0x010ff}, {0x0124e, 0x0124f},
	{0x01259, 0x01259}, {0x0125f, 0x0125f}, {0x0128e, 0x0128f},
	{0x012b6, 0x012b7}, {0x012c1, 0x012c1}, {0x012c7, 0x012c7},
	{0x01311, 0x01311}, {0x01317, 0x01317}, {0x0135c, 0x0135f},
	{0x0137e, 0x0137f}, {0x0139b, 0x0139f}, {0x013f6, 0x013ff},
	{0x0169e, 0x0169f}, {0x016f2, 0x016ff}, {0x01712, 0x0171f},
	{0x01733, 0x01734}, {0x01738, 0x0173f}, {0x01753, 0x0175f},
	{0x01771, 0x0177f}, {0x017b5, 0x017d3}, {0x017de, 0x017df},
	{0x017eb, 0x017ef}, {0x017fb, 0x017ff}, {0x0180c, 0x0180d},
	{0x0181a, 0x0181f}, {0x01879, 0x0187f}, {0x018ab, 0x018af},
	{0x018f7, 0x018ff}, {0x0191e, 0x0193f}, {0x01942, 0x01943},
	{0x0196f, 0x0196f}, {0x01976, 0x0197f}, {0x019ad, 0x019c0},
	{0x019c9, 0x019cf}, {0x019dc, 0x019dd}, {0x01a18, 0x01a1d},
	{0x01a56, 0x01a7f}, {0x01a8b, 0x01a8f}, {0x01a9b, 0x01a9f},
	{0x01aaf, 0x01b04}, {0x01b35, 0x01b44}, {0x01b4d, 0x01b4f},
	{0x01b6c, 0x01b73}, {0x01b7e, 0x01b82}, {0x01ba2, 0x01bad},
	{0x01bbb, 0x01bff}, {0x01c25, 0x01c3a}, {0x01c4b, 0x01c4c},
	{0x01c81, 0x01cd2}, {0x01cd5, 0x01ce8}, {0x01cf2, 0x01cff},
	{0x01dc1, 0x01dff}, {0x01f17, 0x01f17}, {0x01f1f, 0x01f1f},
	{0x01f47, 0x01f47}, {0x01f4f, 0x01f4f}, {0x01f5a, 0x01f5a},
	{0x01f5e, 0x01f5e}, {0x01f7f, 0x01f7f}, {0x01fc5, 0x01fc5},
	{0x01fd5, 0x01fd5}, {0x01ff0, 0x01ff1}, {0x01fff, 0x01fff},
	{0x0200c, 0x0200f}, {0x02029, 0x0202e}, {0x02061, 0x0206f},
	{0x02073, 0x02073}, {0x02095, 0x0209f}, {0x020ba, 0x020ff},
	{0x0218b, 0x0218f}, {0x023ea, 0x023ff}, {0x02428, 0x0243f},
	{0x0244c, 0x0245f}, {0x026e2, 0x026e2}, {0x026e5, 0x026e7},
	{0x02705, 0x02705}, {0x0270b, 0x0270b}, {0x0274c, 0x0274c},
	{0x02753, 0x02755}, {0x02760, 0x02760}, {0x02796, 0x02797},
	{0x027bf, 0x027bf}, {0x027cd, 0x027cf}, {0x02b4e, 0x02b4f},
	{0x02b5b, 0x02bff}, {0x02c5f, 0x02c5f}, {0x02cf0, 0x02cf8},
	{0x02d27, 0x02d2f}, {0x02d67, 0x02d6e}, {0x02d71, 0x02d7f},
	{0x02d98, 0x02d9f}, {0x02daf, 0x02daf}, {0x02dbf, 0x02dbf},
	{0x02dcf, 0x02dcf}, {0x02ddf, 0x02dff}, {0x02e33, 0x02e7f},
	{0x02ef4, 0x02eff}, {0x02fd7, 0x02fef}, {0x02ffd, 0x02fff},
	{0x0302b, 0x0302f}, {0x03097, 0x0309a}, {0x03101, 0x03104},
	{0x0312f, 0x03130}, {0x031b8, 0x031bf}, {0x031e5, 0x031ef},
	{0x032ff, 0x032ff}, {0x04db7, 0x04dbf}, {0x09fcd, 0x09fff},
	{0x0a48e, 0x0a48f}, {0x0a4c8, 0x0a4cf}, {0x0a62d, 0x0a63f},
	{0x0a661, 0x0a661}, {0x0a670, 0x0a672}, {0x0a675, 0x0a67d},
	{0x0a699, 0x0a69f}, {0x0a6f1, 0x0a6f1}, {0x0a6f9, 0x0a6ff},
	{0x0a78e, 0x0a7fa}, {0x0a806, 0x0a806}, {0x0a823, 0x0a827},
	{0x0a82d, 0x0a82f}, {0x0a83b, 0x0a83f}, {0x0a879, 0x0a881},
	{0x0a8b5, 0x0a8cd}, {0x0a8db, 0x0a8f1}, {0x0a8fd, 0x0a8ff},
	{0x0a927, 0x0a92d}, {0x0a948, 0x0a95e}, {0x0a97e, 0x0a983},
	{0x0a9b4, 0x0a9c0}, {0x0a9da, 0x0a9dd}, {0x0a9e1, 0x0a9ff},
	{0x0aa2a, 0x0aa3f}, {0x0aa4c, 0x0aa4f}, {0x0aa5b, 0x0aa5b},
	{0x0aa7c, 0x0aa7f}, {0x0aab2, 0x0aab4}, {0x0aab8, 0x0aab8},
	{0x0aabf, 0x0aabf}, {0x0aac3, 0x0aada}, {0x0aae1, 0x0abbf},
	{0x0abe4, 0x0abea}, {0x0abed, 0x0abef}, {0x0abfb, 0x0abff},
	{0x0d7a5, 0x0d7af}, {0x0d7c8, 0x0d7ca}, {0x0d7fd, 0x0f8ff},
	{0x0fa2f, 0x0fa2f}, {0x0fa6f, 0x0fa6f}, {0x0fadb, 0x0faff},
	{0x0fb08, 0x0fb12}, {0x0fb19, 0x0fb1c}, {0x0fb37, 0x0fb37},
	{0x0fb3f, 0x0fb3f}, {0x0fb45, 0x0fb45}, {0x0fbb3, 0x0fbd2},
	{0x0fd41, 0x0fd4f}, {0x0fd91, 0x0fd91}, {0x0fdc9, 0x0fdef},
	{0x0fdff, 0x0fe0f}, {0x0fe1b, 0x0fe2f}, {0x0fe67, 0x0fe67},
	{0x0fe6d, 0x0fe6f}, {0x0fefd, 0x0ff00}, {0x0ffc0, 0x0ffc1},
	{0x0ffc9, 0x0ffc9}, {0x0ffd1, 0x0ffd1}, {0x0ffd9, 0x0ffd9},
	{0x0ffde, 0x0ffdf}, {0x0ffef, 0x0fffb}, {0x0ffff, 0x0ffff},
	{0x10027, 0x10027}, {0x1003e, 0x1003e}, {0x1004f, 0x1004f},
	{0x1005f, 0x1007f}, {0x100fc, 0x100ff}, {0x10104, 0x10106},
	{0x10135, 0x10136}, {0x1018c, 0x1018f}, {0x1019d, 0x101cf},
	{0x101fe, 0x1027f}, {0x1029e, 0x1029f}, {0x102d2, 0x102ff},
	{0x10324, 0x1032f}, {0x1034c, 0x1037f}, {0x103c4, 0x103c7},
	{0x103d7, 0x103ff}, {0x1049f, 0x1049f}, {0x104ab, 0x107ff},
	{0x10807, 0x10807}, {0x10836, 0x10836}, {0x1083a, 0x1083b},
	{0x1083e, 0x1083e}, {0x10860, 0x108ff}, {0x1091d, 0x1091e},
	{0x1093b, 0x1093e}, {0x10941, 0x109ff}, {0x10a02, 0x10a0f},
	{0x10a18, 0x10a18}, {0x10a35, 0x10a3f}, {0x10a49, 0x10a4f},
	{0x10a5a, 0x10a5f}, {0x10a81, 0x10aff}, {0x10b37, 0x10b38},
	{0x10b57, 0x10b57}, {0x10b74, 0x10b77}, {0x10b81, 0x10bff},
	{0x10c4a, 0x10e5f}, {0x10e80, 0x11082}, {0x110b1, 0x110ba},
	{0x110c2, 0x11fff}, {0x12370, 0x123ff}, {0x12464, 0x1246f},
	{0x12475, 0x12fff}, {0x13430, 0x1cfff}, {0x1d0f7, 0x1d0ff},
	{0x1d128, 0x1d128}, {0x1d166, 0x1d169}, {0x1d16e, 0x1d182},
	{0x1d186, 0x1d18b}, {0x1d1ab, 0x1d1ad}, {0x1d1df, 0x1d1ff},
	{0x1d243, 0x1d244}, {0x1d247, 0x1d2ff}, {0x1d358, 0x1d35f},
	{0x1d373, 0x1d3ff}, {0x1d49d, 0x1d49d}, {0x1d4a1, 0x1d4a1},
	{0x1d4a4, 0x1d4a4}, {0x1d4a8, 0x1d4a8}, {0x1d4ba, 0x1d4ba},
	{0x1d4c4, 0x1d4c4}, {0x1d50b, 0x1d50c}, {0x1d51d, 0x1d51d},
	{0x1d53f, 0x1d53f}, {0x1d547, 0x1d549}, {0x1d6a6, 0x1d6a7},
	{0x1d7cd, 0x1d7cd}, {0x1d801, 0x1efff}, {0x1f02d, 0x1f02f},
	{0x1f095, 0x1f0ff}, {0x1f10c, 0x1f10f}, {0x1f130, 0x1f130},
	{0x1f133, 0x1f13c}, {0x1f140, 0x1f141}, {0x1f144, 0x1f145},
	{0x1f148, 0x1f149}, {0x1f150, 0x1f156}, {0x1f159, 0x1f15e},
	{0x1f161, 0x1f178}, {0x1f17d, 0x1f17e}, {0x1f181, 0x1f189},
	{0x1f18f, 0x1f18f}, {0x1f192, 0x1f1ff}, {0x1f202, 0x1f20f},
	{0x1f233, 0x1f23f}, {0x1f24a, 0x1ffff}, {0x2a6d8, 0x2a6ff},
	{0x2b736, 0x2f7ff}, {0x2fa1f, 0x10ffff},
};
int bclen = LEN(bchars);
int def_bclen = LEN(bchars);

static int find(int c, int tab[][2], int n)
{
	if (c < tab[0][0] || !n)
		return 0;
	int m, l = 0;
	int h = n - 1;
	while (l <= h) {
		m = (h + l) / 2;
		if (c >= tab[m][0] && c <= tab[m][1])
			return 1;
		if (c < tab[m][0])
			h = m - 1;
		else
			l = m + 1;
	}
	return 0;
}

/* double-width characters */
static int uc_isdw(int c)
{
	return find(c, dwchars, LEN(dwchars));
}

/* (nonprintable) zero width & combining characters */
int uc_isbell(int c)
{
	return find(c, zwchars, zwlen) || find(c, bchars, bclen);
}

/* printing width */
int uc_wid(int c)
{
	if (uc_isdw(c))
		return 2;
	return zwlen || !find(c, zwchars, def_zwlen);
}
