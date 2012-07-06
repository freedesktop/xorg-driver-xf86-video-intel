/*
 * Copyright © 1998 Keith Packard
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>

#include "fb.h"

static inline void add(RegionPtr region,
		       int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
	BoxPtr r;

	if (region->data->numRects == region->data->size)
		RegionRectAlloc(region, 1);

	r = RegionBoxptr(region) + region->data->numRects++;
	r->x1 = x1; r->y1 = y1;
	r->x2 = x2; r->y2 = y2;

	if (x1 < region->extents.x1)
		region->extents.x1 = x1;
	if (x2 > region->extents.x2)
		region->extents.x2 = x2;
}

/* Convert bitmap clip mask into clipping region.
 * First, goes through each line and makes boxes by noting the transitions
 * from 0 to 1 and 1 to 0.
 * Then it coalesces the current line with the previous if they have boxes
 * at the same X coordinates.
 */
RegionPtr
fbBitmapToRegion(PixmapPtr pixmap)
{
	const register FbBits mask0 = FB_ALLONES & ~FbScrRight(FB_ALLONES, 1);
	register RegionPtr region;
	const FbBits *bits, *line, *end;
	int width, y1, y2, base, x1;
	int stride, i;

	DBG(("%s bitmap=%dx%d\n", __FUNCTION__,
	     pixmap->drawable.width, pixmap->drawable.height));

	region = RegionCreate(NULL, 1);
	if (!region)
		return NullRegion;

	line = (FbBits *) pixmap->devPrivate.ptr;
	stride = pixmap->devKind >> (FB_SHIFT - 3);

	width = pixmap->drawable.width;
	region->extents.x1 = width;
	region->extents.x2 = 0;
	y2 = 0;
	while (y2 < pixmap->drawable.height) {
		y1 = y2++;
		bits = line;
		line += stride;
		while (y2 < pixmap->drawable.height &&
		       memcmp(bits, line, (width+7)>>3) == 0)
			line += stride, y2++;

		if (READ(bits) & mask0)
			x1 = 0;
		else
			x1 =-1;

		/* Process all words which are fully in the pixmap */
		end = bits + (width >> FB_SHIFT);
		for (base = 0; bits < end; base += FB_UNIT) {
			FbBits w = READ(bits++);
			if (x1 < 0) {
				if (!~w)
					continue;
			} else {
				if (!w)
					continue;
			}
			for (i = 0; i < FB_UNIT; i++) {
				if (w & mask0) {
					if (x1 < 0)
						x1 = base + i;
				} else {
					if (x1 >= 0) {
						add(region, x1, y1, base + i, y2);
						x1 = -1;
					}
				}
				w = FbScrLeft(w, 1);
			}
		}
		if (width & FB_MASK) {
			FbBits w = READ(bits++);
			for (i = 0; i < (width & FB_MASK); i++) {
				if (w & mask0) {
					if (x1 < 0)
						x1 = base + i;
				} else {
					if (x1 >= 0) {
						add(region, x1, y1, base + i, y2);
						x1 = -1;
					}
				}
				w = FbScrLeft(w, 1);
			}
		}
		if (x1 >= 0)
			add(region, x1, y1, width, y2);
	}

	if (region->data->numRects) {
		region->extents.y1 = RegionBoxptr(region)->y1;
		region->extents.y2 = RegionEnd(region)->y2;
		if (region->data->numRects == 1) {
			free(region->data);
			region->data = NULL;
		}
	} else
		region->extents.x1 = region->extents.x2 = 0;

	return region;
}
