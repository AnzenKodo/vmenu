/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <pwd.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define TEXTW(X)              (drw_fontset_getwidth(drw, (X)) + lrpad)

/* enums */
enum { SchemeNorm, SchemeSel, SchemeOut, SchemeNormHighlight, SchemeSelHighlight, SchemeOutHighlight, SchemeBorder, SchemeLast }; /* color schemes */

struct item {
	char *text;
	struct item *left, *right;
	int out;
};

static char text[BUFSIZ] = "";
static char *embed;
static int bh, mw, mh;
static int inputw = 0, promptw;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;

static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;

static Drw *drw;
static Clr *scheme[SchemeLast];

static int instant = 0;                     /* -n  option; if 1, select single entry automatically */
static int centered = 0;                    /* -c  option; if 1, vmenu appears in center of screen */
static int topbar = 1;                      /* -b  option; if 0, vmenu appears at bottom     */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"monospace:size=10"
};
static const char *prompt      = NULL;      /* -p  option; prompt to the left of input field */
static const char *colors[SchemeLast][2] = {
	/*     fg         bg       */
	[SchemeNorm] = { "#bbbbbb", "#222222" },
	[SchemeSel] = { "#eeeeee", "#005577" },
	[SchemeOut] = { "#000000", "#00ffff" },
	[SchemeSelHighlight] = { "#ffc978", "#005577" },
	[SchemeNormHighlight] = { "#ffc978", "#222222" },
	[SchemeOutHighlight] = { "#ffc978", "#00ffff" },
	[SchemeBorder] = { "#cccccc", NULL },
};
/* -l and -g options; controls number of lines and columns in grid if > 0 */
static unsigned int lines      = 0;
static unsigned int columns    = 1;
/* Size of the window border */
static unsigned int border_width = 0;
/* -h option; minimum height of a menu line */
static unsigned int lineheight = 0;
static unsigned int min_lineheight = 8;
static int custom_width = 0;

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char *worddelimiters = " ";

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
	return MIN(w, n);
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * columns * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : (int)textw_clamp(next->text, n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : (int)textw_clamp(prev->left->text, n)) > n)
			break;
}

static int
max_textw(void)
{
	int len = 0;
	for (struct item *item = items; item && item->text; item++)
		len = MAX((int)TEXTW(item->text), len);
	return len;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKeyboard(dpy, CurrentTime);
	for (i = 0; i < SchemeLast; i++)
		drw_scm_free(drw, scheme[i], 2);
	for (i = 0; items && items[i].text; ++i)
		free(items[i].text);
	free(items);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
	FcFini();
}

static char *
cistrstr(const char *h, const char *n)
{
	size_t i;

	if (!n[0])
		return (char *)h;

	for (; *h; ++h) {
		for (i = 0; n[i] && tolower((unsigned char)n[i]) ==
		            tolower((unsigned char)h[i]); ++i)
			;
		if (n[i] == '\0')
			return (char *)h;
	}
	return NULL;
}

static void
drawhighlights(struct item *item, int x, int y, int maxw)
{
	char restorechar, tokens[sizeof text], *highlight,  *token;
	int indentx, highlightlen;

	drw_setscheme(drw, scheme[item == sel ? SchemeSelHighlight : item->out ? SchemeOutHighlight : SchemeNormHighlight]);
	strcpy(tokens, text);
	for (token = strtok(tokens, " "); token; token = strtok(NULL, " ")) {
		highlight = fstrstr(item->text, token);
		while (highlight) {
			// Move item str end, calc width for highlight indent, & restore
			highlightlen = highlight - item->text;
			restorechar = *highlight;
			item->text[highlightlen] = '\0';
			indentx = TEXTW(item->text);
			item->text[highlightlen] = restorechar;

			// Move highlight str end, draw highlight, & restore
			restorechar = highlight[strlen(token)];
			highlight[strlen(token)] = '\0';
			if (indentx - (lrpad / 2) - 1 < maxw)
				drw_text(
					drw,
					x + indentx - (lrpad / 2) - 1,
					y,
					MIN(maxw - indentx, (int)TEXTW(highlight) - lrpad),
					bh, 0, highlight, 0
				);
			highlight[strlen(token)] = restorechar;

			if (strlen(highlight) - strlen(token) < strlen(token)) break;
			highlight = fstrstr(highlight + strlen(token), token);
		}
	}
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	int r = drw_text(drw, x, y, w, bh, lrpad / 2, item->text, 0);
	drawhighlights(item, x, y, w);
	return r;
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = border_width, y = border_width, fh = drw->fonts->h, w;

	if (border_width) {
		drw_setscheme(drw, scheme[SchemeBorder]);
		drw_rect(drw, 0, 0, mw, mh, 1, 0);
	}
	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, border_width, border_width, mw - 2 * border_width, mh - 2 * border_width, 1, 1);

	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemeSel]);
		x = drw_text(drw, x, border_width, promptw, bh, lrpad / 2, prompt, 0);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? (int)(mw - x - border_width) : inputw;
	drw_text(drw, x, border_width, w, bh, lrpad / 2, text, 0);

	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	if ((curpos += lrpad / 2 - 1) < (unsigned int)w) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x + curpos, border_width + 2 + (bh - fh) / 2, 2, fh - 4, 1, 0);
	}

	if (lines > 0) {
		/* draw grid */
		int i = 0;
		for (item = curr; item != next; item = item->right, i++)
			drawitem(
				item,
				x + ((i / lines) * ((mw - x - border_width) / columns)),
				y + (((i % lines) + 1) * bh),
				(mw - x - border_width) / columns
			);
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, border_width, w, bh, lrpad / 2, "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, border_width, textw_clamp(item->text, mw - x - TEXTW(">") - border_width));
		if (next) {
			w = TEXTW(">");
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w - border_width, border_width, w, bh, lrpad / 2, ">", 0);
		}
	}
	drw_map(drw, win, 0, 0, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	if (embed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %zu bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;

	if (instant && matches && matches == matchend && !lsubstr) {
		puts(matches->text);
		cleanup();
		exit(0);
	}

	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void
keypress(XKeyEvent *ev)
{
	char buf[64];
	int len;
	int i, offscreen = 0;
	struct item *tmpsel;
	KeySym ksym = NoSymbol;
	Status status;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars: /* composed string from input method */
		goto insert;
	case XLookupKeySym:
	case XLookupBoth: /* a KeySym and a string are returned: use keysym */
		break;
	}

	if (ev->state & ControlMask) {
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_y: /* paste selection */
		case XK_Y:
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Left:
		case XK_KP_Left:
			movewordedge(-1);
			goto draw;
		case XK_Right:
		case XK_KP_Right:
			movewordedge(+1);
			goto draw;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	} else if (ev->state & Mod1Mask) {
		switch(ksym) {
		case XK_b:
			movewordedge(-1);
			goto draw;
		case XK_f:
			movewordedge(+1);
			goto draw;
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	}

	switch(ksym) {
	default:
insert:
		if (!iscntrl((unsigned char)*buf))
			insert(buf, len);
		break;
	case XK_Delete:
	case XK_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if (cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
	case XK_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
	case XK_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
	case XK_KP_Left:
		if (columns > 1) {
			if (!sel)
				return;
			tmpsel = sel;
			for (i = 0; i < (int)lines; i++) {
				if (!tmpsel->left || tmpsel->left->right != tmpsel) {
					if (offscreen)
						break;
					return;
				}
				if (tmpsel == curr)
					offscreen = 1;
				tmpsel = tmpsel->left;
			}
			sel = tmpsel;
			if (offscreen) {
				curr = prev;
				calcoffsets();
			}
			break;
		}
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
	case XK_KP_Up:
		if (sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XK_Next:
	case XK_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
	case XK_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		puts((sel && !(ev->state & ShiftMask)) ? sel->text : text);
		if (!(ev->state & ControlMask)) {
			cleanup();
			exit(0);
		}
		if (sel)
			sel->out = 1;
		break;
	case XK_Right:
	case XK_KP_Right:
		if (columns > 1) {
			if (!sel)
				return;
			tmpsel = sel;
			for (i = 0; i < (int)lines; i++) {
				if (!tmpsel->right || tmpsel->right->left != tmpsel) {
					if (offscreen)
						break;
					return;
				}
				tmpsel = tmpsel->right;
				if (tmpsel == next)
					offscreen = 1;
			}
			sel = tmpsel;
			if (offscreen) {
				curr = next;
				calcoffsets();
			}
			break;
		}
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if (lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
	case XK_KP_Down:
		if (sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
		break;
	case XK_Tab:
		if (!sel)
			return;
		cursor = strnlen(sel->text, sizeof text - 1);
		memcpy(text, sel->text, cursor);
		text[cursor] = '\0';
		match();
		break;
	}

draw:
	drawmenu();
}

static void
buttonpress(XEvent *e)
{
	struct item *item;
	XButtonPressedEvent *ev = &e->xbutton;
	int x = border_width, y = border_width, h = bh, w;

	if (ev->window != win)
		return;

	/* right-click: exit */
	if (ev->button == Button3)
		exit(1);

	if (prompt && *prompt)
		x += promptw;

	/* input field */
	w = (lines > 0 || !matches) ? (int)(mw - x - border_width) : inputw;

	/* left-click on input: clear input,
	 * NOTE: if there is no left-arrow the space for < is reserved so
	 *       add that to the input width */
	if (ev->button == Button1 &&
	   ((lines <= 0 && ev->x >= (int)border_width && ev->x <= x + w +
	   ((!prev || !curr->left) ? (int)TEXTW("<") : 0)) ||
	   (lines > 0 && ev->y >= y && ev->y <= y + h))) {
		insert(NULL, -cursor);
		drawmenu();
		return;
	}
	/* middle-mouse click: paste selection */
	if (ev->button == Button2) {
		XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
		                  utf8, utf8, win, CurrentTime);
		drawmenu();
		return;
	}
	/* scroll up */
	if (ev->button == Button4 && prev) {
		sel = curr = prev;
		calcoffsets();
		drawmenu();
		return;
	}
	/* scroll down */
	if (ev->button == Button5 && next) {
		sel = curr = next;
		calcoffsets();
		drawmenu();
		return;
	}
	if (ev->button != Button1)
		return;
	if (ev->state & ~ControlMask)
		return;
	if (lines > 0) {
		/* vertical list: (ctrl)left-click on item */
		w = mw - x - border_width;
		for (item = curr; item != next; item = item->right) {
			y += h;
			if (ev->y >= y && ev->y <= (y + h)) {
				puts(item->text);
				if (!(ev->state & ControlMask))
					exit(0);
				sel = item;
				if (sel) {
					sel->out = 1;
					drawmenu();
				}
				return;
			}
		}
	} else if (matches) {
		/* left-click on left arrow */
		x += inputw;
		w = TEXTW("<");
		if (prev && curr->left) {
			if (ev->x >= x && ev->x <= x + w) {
				sel = curr = prev;
				calcoffsets();
				drawmenu();
				return;
			}
		}
		/* horizontal list: (ctrl)left-click on item */
		for (item = curr; item != next; item = item->right) {
			x += w;
			w = textw_clamp(item->text, mw - x - TEXTW(">") - border_width);
			if (ev->x >= x && ev->x <= x + w) {
				puts(item->text);
				if (!(ev->state & ControlMask))
					exit(0);
				sel = item;
				if (sel) {
					sel->out = 1;
					drawmenu();
				}
				return;
			}
		}
		/* left-click on right arrow */
		w = TEXTW(">");
		x = mw - w - border_width;
		if (next && ev->x >= x && ev->x <= x + w) {
			sel = curr = next;
			calcoffsets();
			drawmenu();
			return;
		}
	}
}

static void
paste(void)
{
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                   utf8, &da, &di, &dl, &dl, (unsigned char **)&p)
	    == Success && p) {
		insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
	drawmenu();
}

static void
readstdin(void)
{
	char *line = NULL;
	size_t i, itemsiz = 0, linesiz = 0;
	ssize_t len;

	/* read each line from stdin and add it to the item list */
	for (i = 0; (len = getline(&line, &linesiz, stdin)) != -1; i++) {
		if (i + 1 >= itemsiz) {
			itemsiz += 256;
			if (!(items = realloc(items, itemsiz * sizeof(*items))))
				die("cannot realloc %zu bytes:", itemsiz * sizeof(*items));
		}
		if (line[len - 1] == '\n')
			line[len - 1] = '\0';
		if (!(items[i].text = strdup(line)))
			die("strdup:");

		items[i].out = 0;
	}
	free(line);
	if (items)
		items[i].text = NULL;
	lines = MIN(lines, i);
}

static void
run(void)
{
	XEvent ev;

	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case DestroyNotify:
			if (ev.xdestroywindow.window != win)
				break;
			cleanup();
			exit(1);
		case ButtonPress:
			buttonpress(&ev);
			break;
		case Expose:
			if (ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (ev.xfocus.window != win)
				grabfocus();
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case SelectionNotify:
			if (ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"vmenu", "vmenu"};
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window pw;
	int a, di, n, area = 0;
#endif
	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, colors[j], 2);

	clip = XInternAtom(dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = drw->fonts->h + 2;
	bh = MAX(bh, (int)lineheight);
	lines = MAX(lines, 0u);
	mh = (lines + 1) * bh + 2 * border_width;
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;
#ifdef XINERAMA
	i = 0;
	if (parentwin == root && (info = XineramaQueryScreens(dpy, &n))) {
		XGetInputFocus(dpy, &w, &di);
		if (mon >= 0 && mon < n)
			i = mon;
		else if (w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while (w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if (XGetWindowAttributes(dpy, pw, &wa))
				for (j = 0; j < n; j++)
					if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if (mon < 0 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for (i = 0; i < n; i++)
				if (INTERSECT(x, y, 1, 1, info[i]) != 0)
					break;

		if (centered) {
			mw = (custom_width > 0) ? custom_width : MAX(max_textw() + promptw, 100);
			mw = MIN(mw, (int)info[i].width);
			x = info[i].x_org + ((info[i].width  - mw) / 2);
			y = info[i].y_org + ((info[i].height - mh) / 2);
		} else {
			x = info[i].x_org;
			y = info[i].y_org + (topbar ? 0 : info[i].height - mh);
			mw = (custom_width > 0) ? MIN(custom_width, (int)info[i].width) : info[i].width;
		}
		XFree(info);
	} else
#endif
	{
		if (!XGetWindowAttributes(dpy, parentwin, &wa))
			die("could not get embedding window attributes: 0x%lx",
			    parentwin);
		if (centered) {
			mw = (custom_width > 0) ? custom_width : MAX(max_textw() + promptw, 100);
			mw = MIN(mw, wa.width);
			x = (wa.width  - mw) / 2;
			y = (wa.height - mh) / 2;
		} else {
			x = 0;
			y = topbar ? 0 : wa.height - mh;
			mw = (custom_width > 0) ? MIN(custom_width, wa.width) : wa.width;
		}
	}
	inputw = mw / 3; /* input width: ~33% of monitor width */
	match();

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask | ButtonPressMask;
	win = XCreateWindow(dpy, root, x, y, mw, mh, 0,
	                    CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	XSetClassHint(dpy, win, &ch);

	/* input methods */
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dpy, win);
	if (embed) {
		XReparentWindow(dpy, win, parentwin, x, y);
		XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < (int)du && dws[i] != win; ++i)
				XSelectInput(dpy, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char default_config_content[] =
	"# vmenu configuration file (config.conf)\n"
	"# This file is read at runtime. Command line arguments override these settings.\n"
	"\n"
	"# Appearance\n"
	"instant = 0\n"
	"centered = 0\n"
	"topbar = 1\n"
	"font = \"monospace:size=10\"\n"
	"prompt = \"\"\n"
	"\n"
	"# Normal colors\n"
	"normal_background = \"#222222\"\n"
	"normal_foreground = \"#bbbbbb\"\n"
	"\n"
	"# Selected colors\n"
	"selected_background = \"#005577\"\n"
	"selected_foreground = \"#eeeeee\"\n"
	"\n"
	"# Outline colors\n"
	"outline_background = \"#00ffff\"\n"
	"outline_foreground = \"#000000\"\n"
	"\n"
	"# Highlight colors\n"
	"normal_highlight_background = \"#222222\"\n"
	"normal_highlight_foreground = \"#ffc978\"\n"
	"selected_highlight_background = \"#005577\"\n"
	"selected_highlight_foreground = \"#ffc978\"\n"
	"outline_highlight_background = \"#00ffff\"\n"
	"outline_highlight_foreground = \"#ffc978\"\n"
	"\n"
	"# Border settings\n"
	"border_background = \"\"\n"
	"border_foreground = \"#cccccc\"\n"
	"border_width = 0\n"
	"\n"
	"# Layout settings\n"
	"width = 0\n"
	"lines = 0\n"
	"columns = 1\n"
	"line_height = 0\n"
	"minimum_line_height = 8\n"
	"\n"
	"# Behavior\n"
	"worddelimiters = \" \"\n";

static const char *get_home_dir(void) {
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw) {
			home = pw->pw_dir;
		}
	}
	return home;
}

static char config_path_buf[PATH_MAX];

static const char *get_default_config_path(void) {
	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	const char *home = get_home_dir();

	if (xdg_config_home && xdg_config_home[0] != '\0') {
		snprintf(config_path_buf, sizeof(config_path_buf), "%s/vmenu/config.conf", xdg_config_home);
		struct stat st;
		if (stat(config_path_buf, &st) == 0) {
			return config_path_buf;
		}
	}

	if (home && home[0] != '\0') {
		static char fallback_buf[PATH_MAX];
		snprintf(fallback_buf, sizeof(fallback_buf), "%s/.config/vmenu/config.conf", home);
		struct stat st;
		if (stat(fallback_buf, &st) == 0) {
			return fallback_buf;
		}
	}

	if (xdg_config_home && xdg_config_home[0] != '\0') {
		snprintf(config_path_buf, sizeof(config_path_buf), "%s/vmenu/config.conf", xdg_config_home);
		return config_path_buf;
	}

	if (home && home[0] != '\0') {
		snprintf(config_path_buf, sizeof(config_path_buf), "%s/.config/vmenu/config.conf", home);
		return config_path_buf;
	}

	return NULL;
}

static int create_parent_dirs(const char *path) {
	char temp[PATH_MAX];
	char *p = NULL;
	size_t len;

	snprintf(temp, sizeof(temp), "%s", path);
	len = strlen(temp);
	if (len == 0)
		return -1;

	for (p = temp + len - 1; p > temp; p--) {
		if (*p == '/') {
			*p = '\0';
			break;
		}
	}

	for (p = temp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(temp, 0755) != 0 && errno != EEXIST) {
		return -1;
	}
	return 0;
}

static void write_default_config(const char *path) {
	if (create_parent_dirs(path) != 0) {
		fprintf(stderr, "warning: could not create parent directories for %s\n", path);
	}
	FILE *fp = fopen(path, "w");
	if (fp) {
		fputs(default_config_content, fp);
		fclose(fp);
	} else {
		fprintf(stderr, "warning: could not write default config to %s\n", path);
	}
}

static char *trim(char *str) {
	char *end;
	while (isspace((unsigned char)*str)) str++;
	if (*str == 0) return str;
	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end)) end--;
	end[1] = '\0';
	return str;
}

static char *parse_string(char *val) {
	char *end = val + strlen(val) - 1;
	while (end > val && isspace((unsigned char)*end)) {
		*end = '\0';
		end--;
	}
	if ((*val == '"' && *end == '"') || (*val == '\'' && *end == '\'')) {
		*end = '\0';
		return val + 1;
	}
	return val;
}

static void read_config(const char *path) {
	FILE *fp = fopen(path, "r");
	if (!fp)
		return;

	char line[1024];
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		if (*p == '#' || *p == '\0')
			continue;

		char *eq = strchr(p, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char *key = trim(p);
		char *val = eq + 1;

		while (isspace((unsigned char)*val))
			val++;

		char *val_end = val + strlen(val) - 1;
		while (val_end >= val && (*val_end == '\r' || *val_end == '\n')) {
			*val_end = '\0';
			val_end--;
		}

		if (strcmp(key, "instant") == 0) {
			instant = atoi(val);
		} else if (strcmp(key, "centered") == 0) {
			centered = atoi(val);
		} else if (strcmp(key, "topbar") == 0) {
			topbar = atoi(val);
		} else if (strcmp(key, "font") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				fonts[0] = strdup(parsed);
		} else if (strcmp(key, "prompt") == 0) {
			prompt = strdup(parse_string(val));
		} else if (strcmp(key, "normal_background") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeNorm][ColBg] = strdup(parsed);
		} else if (strcmp(key, "normal_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeNorm][ColFg] = strdup(parsed);
		} else if (strcmp(key, "selected_background") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeSel][ColBg] = strdup(parsed);
		} else if (strcmp(key, "selected_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeSel][ColFg] = strdup(parsed);
		} else if (strcmp(key, "outline_background") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeOut][ColBg] = strdup(parsed);
		} else if (strcmp(key, "outline_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeOut][ColFg] = strdup(parsed);
		} else if (strcmp(key, "normal_highlight_background") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeNormHighlight][ColBg] = strdup(parsed);
		} else if (strcmp(key, "normal_highlight_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeNormHighlight][ColFg] = strdup(parsed);
		} else if (strcmp(key, "selected_highlight_background") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeSelHighlight][ColBg] = strdup(parsed);
		} else if (strcmp(key, "selected_highlight_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeSelHighlight][ColFg] = strdup(parsed);
		} else if (strcmp(key, "outline_highlight_background") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeOutHighlight][ColBg] = strdup(parsed);
		} else if (strcmp(key, "outline_highlight_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeOutHighlight][ColFg] = strdup(parsed);
		} else if (strcmp(key, "border_background") == 0) {
			char *parsed = parse_string(val);
			colors[SchemeBorder][ColBg] = (parsed[0] != '\0') ? strdup(parsed) : NULL;
		} else if (strcmp(key, "border_foreground") == 0) {
			char *parsed = parse_string(val);
			if (parsed[0] != '\0')
				colors[SchemeBorder][ColFg] = strdup(parsed);
		} else if (strcmp(key, "lines") == 0) {
			lines = atoi(val);
			if (columns == 0) columns = 1;
		} else if (strcmp(key, "columns") == 0) {
			columns = atoi(val);
			if (columns == 0) columns = 1;
		} else if (strcmp(key, "width") == 0) {
			custom_width = atoi(val);
		} else if (strcmp(key, "border_width") == 0) {
			border_width = atoi(val);
		} else if (strcmp(key, "line_height") == 0) {
			lineheight = atoi(val);
		} else if (strcmp(key, "minimum_line_height") == 0) {
			min_lineheight = atoi(val);
		} else if (strcmp(key, "worddelimiters") == 0) {
			worddelimiters = strdup(parse_string(val));
		}
	}
	fclose(fp);
}

static void
usage(void)
{
	die("usage: vmenu [-b|--bottom] [-c|--centered] [-f|--fast] [-i|--case-insensitive]\n"
	    "             [-n|--instant] [-v|--version] [-pc|--print-config]\n"
	    "             [-g|--generate-config [path]] [-l|--lines lines]\n"
	    "             [-G|--columns columns] [-h|--height height] [-W|--width width]\n"
	    "             [-p|--prompt prompt] [-fn|--font font] [-fs|--font-size size]\n"
	    "             [-m|--monitor monitor]\n"
	    "             [-nb|--normal-background color] [-nf|--normal-foreground color]\n"
	    "             [-sb|--selected-background color] [-sf|--selected-foreground color]\n"
	    "             [-ob|--outline-background color] [-of|--outline-foreground color]\n"
	    "             [-bw|--border-width width] [-bc|--border-color color]\n"
	    "             [-w|--window-id windowid] [-cf|--config configfile]");
}

int
main(int argc, char *argv[])
{
	XWindowAttributes wa;
	int i, fast = 0;
	const char *fontsize = NULL;
	const char *config_file = NULL;
	int generate_config = 0;         /* 1 = write config, then exit */
	int print_config = 0;            /* 1 = dump default config to stdout, then exit */
	const char *generate_config_path = NULL; /* optional custom path for --generate-config */

	/* 1. Scan for config-related options first */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--generate-config")) {
			generate_config = 1;
			/* check if the next argument is a path (doesn't start with '-') */
			if (i + 1 < argc && argv[i + 1][0] != '-')
				generate_config_path = argv[++i];
		} else if (!strcmp(argv[i], "-pc") || !strcmp(argv[i], "--print-config")) {
			print_config = 1;
		} else if (!strcmp(argv[i], "-cf") || !strcmp(argv[i], "--config")) {
			if (i + 1 == argc)
				usage();
			config_file = argv[++i];
		}
	}

	/* 2. Handle special exit-early flags */

	/* --print-config: dump default config to stdout and exit */
	if (print_config) {
		fputs(default_config_content, stdout);
		exit(0);
	}

	/* --generate-config [path]: write default config to path or XDG default */
	if (generate_config) {
		const char *path = generate_config_path
			? generate_config_path
			: get_default_config_path();
		if (!path) {
			fprintf(stderr, "error: could not resolve config path\n");
			exit(1);
		}
		write_default_config(path);
		printf("Default config generated at: %s\n", path);
		exit(0);
	}

	/* 3. Load configuration file */
	if (config_file) {
		struct stat st;
		if (stat(config_file, &st) != 0) {
			fprintf(stderr, "error: specified config file does not exist: %s\n", config_file);
			exit(1);
		}
		read_config(config_file);
	} else {
		const char *path = get_default_config_path();
		if (path) {
			struct stat st;
			if (stat(path, &st) != 0) {
				/* config not found — auto-generate and notify */
				write_default_config(path);
				fprintf(stderr, "info: config not found, generated default at: %s\n", path);
			}
			read_config(path);
		}
	}

	/* 3. Parse command line options to override configuration settings */
	for (i = 1; i < argc; i++) {
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			puts("vmenu-"VERSION);
			exit(0);
		} else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--bottom")) {
			topbar = 0;
		} else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fast")) {
			fast = 1;
		} else if (!strcmp(argv[i], "-i") || !strcmp(argv[i], "--case-insensitive")) {
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		} else if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--centered")) {
			centered = 1;
		} else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--instant")) {
			instant = 1;
		} else if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--generate-config")) {
			/* already handled — also skip optional path argument if present */
			if (i + 1 < argc && argv[i + 1][0] != '-')
				i++;
			continue;
		} else if (!strcmp(argv[i], "-pc") || !strcmp(argv[i], "--print-config")) {
			/* already handled */
			continue;
		} else {
			/* these options take one argument */
			if (i + 1 == argc)
				usage();
			if (!strcmp(argv[i], "-cf") || !strcmp(argv[i], "--config")) {
				i++; /* already handled, just skip argument */
			} else if (!strcmp(argv[i], "-G") || !strcmp(argv[i], "--columns")) {
				columns = atoi(argv[++i]);
				if (columns == 0) columns = 1;
				if (lines == 0) lines = 1;
			} else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--lines")) {
				lines = atoi(argv[++i]);
				if (columns == 0) columns = 1;
			} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--height")) {
				lineheight = atoi(argv[++i]);
				lineheight = MAX(lineheight, min_lineheight);
			} else if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--monitor")) {
				mon = atoi(argv[++i]);
			} else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--prompt")) {
				prompt = argv[++i];
			} else if (!strcmp(argv[i], "-fn") || !strcmp(argv[i], "--font")) {
				fonts[0] = argv[++i];
			} else if (!strcmp(argv[i], "-fs") || !strcmp(argv[i], "--font-size")) {
				fontsize = argv[++i];
			} else if (!strcmp(argv[i], "-nb") || !strcmp(argv[i], "--normal-background")) {
				colors[SchemeNorm][ColBg] = argv[++i];
			} else if (!strcmp(argv[i], "-nf") || !strcmp(argv[i], "--normal-foreground")) {
				colors[SchemeNorm][ColFg] = argv[++i];
			} else if (!strcmp(argv[i], "-sb") || !strcmp(argv[i], "--selected-background")) {
				colors[SchemeSel][ColBg] = argv[++i];
			} else if (!strcmp(argv[i], "-sf") || !strcmp(argv[i], "--selected-foreground")) {
				colors[SchemeSel][ColFg] = argv[++i];
			} else if (!strcmp(argv[i], "-ob") || !strcmp(argv[i], "--outline-background")) {
				colors[SchemeOut][ColBg] = argv[++i];
			} else if (!strcmp(argv[i], "-of") || !strcmp(argv[i], "--outline-foreground")) {
				colors[SchemeOut][ColFg] = argv[++i];
			} else if (!strcmp(argv[i], "-bw") || !strcmp(argv[i], "--border-width")) {
				border_width = atoi(argv[++i]);
			} else if (!strcmp(argv[i], "-bc") || !strcmp(argv[i], "--border-color")) {
				colors[SchemeBorder][ColFg] = argv[++i];
			} else if (!strcmp(argv[i], "-W") || !strcmp(argv[i], "--width")) {
				custom_width = atoi(argv[++i]);
			} else if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--window-id")) {
				embed = argv[++i];
			} else {
				usage();
			}
		}
	}

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	if (!embed || !(parentwin = strtol(embed, NULL, 0)))
		parentwin = root;
	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx",
		    parentwin);
	if (fontsize) {
		static char fontbuf[256];
		const char *fn = fonts[0];
		char *sizeptr = strstr(fn, ":size=");
		if (sizeptr) {
			size_t len = sizeptr - fn;
			if (len >= sizeof(fontbuf))
				len = sizeof(fontbuf) - 1;
			strncpy(fontbuf, fn, len);
			fontbuf[len] = '\0';
			char *endptr = sizeptr + 6;
			while (*endptr && (isdigit((unsigned char)*endptr) || *endptr == '.')) {
				endptr++;
			}
			snprintf(fontbuf + len, sizeof(fontbuf) - len, ":size=%s%s", fontsize, endptr);
		} else {
			snprintf(fontbuf, sizeof(fontbuf), "%s:size=%s", fn, fontsize);
		}
		fonts[0] = fontbuf;
	}
	drw = drw_create(dpy, screen, root, wa.width, wa.height);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (fast && !isatty(0)) {
		grabkeyboard();
		readstdin();
	} else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}
