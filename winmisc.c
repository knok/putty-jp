/*
 * winmisc.c: miscellaneous Windows-specific things.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "putty.h"
#include "winstuff.h"

void platform_get_x11_auth(char *display, int *proto,
                           unsigned char *data, int *datalen)
{
    /* We don't support this at all under Windows. */
}

Filename filename_from_str(const char *str)
{
    Filename ret;
    strncpy(ret.path, str, sizeof(ret.path));
    ret.path[sizeof(ret.path)-1] = '\0';
    return ret;
}

const char *filename_to_str(const Filename *fn)
{
    return fn->path;
}

int filename_equal(Filename f1, Filename f2)
{
    return !strcmp(f1.path, f2.path);
}

int filename_is_null(Filename fn)
{
    return !*fn.path;
}

int SaneDialogBox(HINSTANCE hinst,
		  LPCTSTR tmpl,
		  HWND hwndparent,
		  DLGPROC lpDialogFunc)
{
    HWND boxhwnd;
    MSG msg;
    
    boxhwnd = CreateDialog(hinst, tmpl, hwndparent, lpDialogFunc);
    while (GetMessage(&msg, NULL, 0, 0)) {
	if (!(boxinfo.flags & DF_END) && !IsDialogMessage(boxhwnd, &msg))
	    DispatchMessage(&msg);
	if (boxinfo.flags & DF_END) break;
    }
    boxinfo.flags=0;
    return boxinfo.result;
}

void SaneEndDialog(HWND hwnd, int ret)
{
    boxinfo.result = ret;
    boxinfo.flags |= DF_END;
    DestroyWindow(hwnd);
}

#ifdef DEBUG
static FILE *debug_fp = NULL;
static HANDLE debug_hdl = INVALID_HANDLE_VALUE;
static int debug_got_console = 0;

void dputs(char *buf)
{
    DWORD dw;

    if (!debug_got_console) {
	if (AllocConsole()) {
	    debug_got_console = 1;
	    debug_hdl = GetStdHandle(STD_OUTPUT_HANDLE);
	}
    }
    if (!debug_fp) {
	debug_fp = fopen("debug.log", "w");
    }

    if (debug_hdl != INVALID_HANDLE_VALUE) {
	WriteFile(debug_hdl, buf, strlen(buf), &dw, NULL);
    }
    fputs(buf, debug_fp);
    fflush(debug_fp);
}
#endif

#ifdef MINEFIELD
/*
 * Minefield - a Windows equivalent for Electric Fence
 */

#define PAGESIZE 4096

/*
 * Design:
 * 
 * We start by reserving as much virtual address space as Windows
 * will sensibly (or not sensibly) let us have. We flag it all as
 * invalid memory.
 * 
 * Any allocation attempt is satisfied by committing one or more
 * pages, with an uncommitted page on either side. The returned
 * memory region is jammed up against the _end_ of the pages.
 * 
 * Freeing anything causes instantaneous decommitment of the pages
 * involved, so stale pointers are caught as soon as possible.
 */

static int minefield_initialised = 0;
static void *minefield_region = NULL;
static long minefield_size = 0;
static long minefield_npages = 0;
static long minefield_curpos = 0;
static unsigned short *minefield_admin = NULL;
static void *minefield_pages = NULL;

static void minefield_admin_hide(int hide)
{
    int access = hide ? PAGE_NOACCESS : PAGE_READWRITE;
    VirtualProtect(minefield_admin, minefield_npages * 2, access, NULL);
}

static void minefield_init(void)
{
    int size;
    int admin_size;
    int i;

    for (size = 0x40000000; size > 0; size = ((size >> 3) * 7) & ~0xFFF) {
	minefield_region = VirtualAlloc(NULL, size,
					MEM_RESERVE, PAGE_NOACCESS);
	if (minefield_region)
	    break;
    }
    minefield_size = size;

    /*
     * Firstly, allocate a section of that to be the admin block.
     * We'll need a two-byte field for each page.
     */
    minefield_admin = minefield_region;
    minefield_npages = minefield_size / PAGESIZE;
    admin_size = (minefield_npages * 2 + PAGESIZE - 1) & ~(PAGESIZE - 1);
    minefield_npages = (minefield_size - admin_size) / PAGESIZE;
    minefield_pages = (char *) minefield_region + admin_size;

    /*
     * Commit the admin region.
     */
    VirtualAlloc(minefield_admin, minefield_npages * 2,
		 MEM_COMMIT, PAGE_READWRITE);

    /*
     * Mark all pages as unused (0xFFFF).
     */
    for (i = 0; i < minefield_npages; i++)
	minefield_admin[i] = 0xFFFF;

    /*
     * Hide the admin region.
     */
    minefield_admin_hide(1);

    minefield_initialised = 1;
}

static void minefield_bomb(void)
{
    div(1, *(int *) minefield_pages);
}

static void *minefield_alloc(int size)
{
    int npages;
    int pos, lim, region_end, region_start;
    int start;
    int i;

    npages = (size + PAGESIZE - 1) / PAGESIZE;

    minefield_admin_hide(0);

    /*
     * Search from current position until we find a contiguous
     * bunch of npages+2 unused pages.
     */
    pos = minefield_curpos;
    lim = minefield_npages;
    while (1) {
	/* Skip over used pages. */
	while (pos < lim && minefield_admin[pos] != 0xFFFF)
	    pos++;
	/* Count unused pages. */
	start = pos;
	while (pos < lim && pos - start < npages + 2 &&
	       minefield_admin[pos] == 0xFFFF)
	    pos++;
	if (pos - start == npages + 2)
	    break;
	/* If we've reached the limit, reset the limit or stop. */
	if (pos >= lim) {
	    if (lim == minefield_npages) {
		/* go round and start again at zero */
		lim = minefield_curpos;
		pos = 0;
	    } else {
		minefield_admin_hide(1);
		return NULL;
	    }
	}
    }

    minefield_curpos = pos - 1;

    /*
     * We have npages+2 unused pages starting at start. We leave
     * the first and last of these alone and use the rest.
     */
    region_end = (start + npages + 1) * PAGESIZE;
    region_start = region_end - size;
    /* FIXME: could align here if we wanted */

    /*
     * Update the admin region.
     */
    for (i = start + 2; i < start + npages + 1; i++)
	minefield_admin[i] = 0xFFFE;   /* used but no region starts here */
    minefield_admin[start + 1] = region_start % PAGESIZE;

    minefield_admin_hide(1);

    VirtualAlloc((char *) minefield_pages + region_start, size,
		 MEM_COMMIT, PAGE_READWRITE);
    return (char *) minefield_pages + region_start;
}

static void minefield_free(void *ptr)
{
    int region_start, i, j;

    minefield_admin_hide(0);

    region_start = (char *) ptr - (char *) minefield_pages;
    i = region_start / PAGESIZE;
    if (i < 0 || i >= minefield_npages ||
	minefield_admin[i] != region_start % PAGESIZE)
	minefield_bomb();
    for (j = i; j < minefield_npages && minefield_admin[j] != 0xFFFF; j++) {
	minefield_admin[j] = 0xFFFF;
    }

    VirtualFree(ptr, j * PAGESIZE - region_start, MEM_DECOMMIT);

    minefield_admin_hide(1);
}

static int minefield_get_size(void *ptr)
{
    int region_start, i, j;

    minefield_admin_hide(0);

    region_start = (char *) ptr - (char *) minefield_pages;
    i = region_start / PAGESIZE;
    if (i < 0 || i >= minefield_npages ||
	minefield_admin[i] != region_start % PAGESIZE)
	minefield_bomb();
    for (j = i; j < minefield_npages && minefield_admin[j] != 0xFFFF; j++);

    minefield_admin_hide(1);

    return j * PAGESIZE - region_start;
}

void *minefield_c_malloc(size_t size)
{
    if (!minefield_initialised)
	minefield_init();
    return minefield_alloc(size);
}

void minefield_c_free(void *p)
{
    if (!minefield_initialised)
	minefield_init();
    minefield_free(p);
}

/*
 * realloc _always_ moves the chunk, for rapid detection of code
 * that assumes it won't.
 */
void *minefield_c_realloc(void *p, size_t size)
{
    size_t oldsize;
    void *q;
    if (!minefield_initialised)
	minefield_init();
    q = minefield_alloc(size);
    oldsize = minefield_get_size(p);
    memcpy(q, p, (oldsize < size ? oldsize : size));
    minefield_free(p);
    return q;
}

#endif				/* MINEFIELD */
