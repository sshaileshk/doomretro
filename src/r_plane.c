/*
========================================================================

                               DOOM RETRO
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
  Copyright (C) 2013-2015 Brad Harding.

  DOOM RETRO is a fork of CHOCOLATE DOOM by Simon Howard.
  For a complete list of credits, see the accompanying AUTHORS file.

  This file is part of DOOM RETRO.

  DOOM RETRO is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM RETRO is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM RETRO. If not, see <http://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM RETRO is in no way affiliated with nor endorsed by
  id Software LLC.

========================================================================
*/

#include "c_console.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_timer.h"
#include "p_local.h"
#include "r_local.h"
#include "r_sky.h"
#include "w_wad.h"
#include "z_zone.h"

#define MAXVISPLANES    128                             // must be a power of 2

static visplane_t       *visplanes[MAXVISPLANES];       // killough
static visplane_t       *freetail;                      // killough
static visplane_t       **freehead = &freetail;         // killough
visplane_t              *floorplane;
visplane_t              *ceilingplane;

// killough -- hash function for visplanes
// Empirically verified to be fairly uniform:
#define visplane_hash(picnum, lightlevel, height) \
    (((unsigned int)(picnum) * 3 + (unsigned int)(lightlevel) + \
    (unsigned int)(height) * 7) & (MAXVISPLANES - 1))

size_t                 maxopenings;
int                    *openings;                       // dropoff overflow
int                    *lastopening;                    // dropoff overflow

// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
int                     floorclip[SCREENWIDTH];         // dropoff overflow
int                     ceilingclip[SCREENWIDTH];       // dropoff overflow

// spanstart holds the start of a plane span
// initialized to 0 at start
static int              spanstart[SCREENHEIGHT];

// texture mapping
static lighttable_t     **planezlight;
static fixed_t          planeheight;

static fixed_t          xoffs, yoffs;                   // killough 2/28/98: flat offsets

fixed_t                 yslope[SCREENHEIGHT];
fixed_t                 distscale[SCREENWIDTH];

boolean                 swirlingliquid = SWIRLINGLIQUID_DEFAULT;

//
// R_MapPlane
//
// Uses global vars:
//  planeheight
//  ds_source
//  viewx
//  viewy
//
// BASIC PRIMITIVE
//
static void R_MapPlane(int y, int x1, int x2)
{
    fixed_t     distance;
    int         dx, dy;

    if (y == centery)
        return;

    distance = FixedMul(planeheight, yslope[y]);

    dx = x1 - centerx;
    dy = ABS(centery - y);
    ds_xstep = FixedMul(viewsin, planeheight) / dy;
    ds_ystep = FixedMul(viewcos, planeheight) / dy;

    ds_xfrac = viewx + xoffs + FixedMul(viewcos, distance) + dx * ds_xstep;
    ds_yfrac = -viewy + yoffs - FixedMul(viewsin, distance) + dx * ds_ystep;

    if (fixedcolormap)
        ds_colormap = fixedcolormap;
    else
        ds_colormap = planezlight[BETWEEN(0, distance >> LIGHTZSHIFT, MAXLIGHTZ - 1)];

    ds_y = y;
    ds_x1 = x1;
    ds_x2 = x2;

    spanfunc();
}

//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes(void)
{
    int i;

    // opening / clipping determination
    for (i = 0; i < viewwidth; i++)
    {
        floorclip[i] = viewheight;
        ceilingclip[i] = -1;
    }

    for (i = 0; i < MAXVISPLANES; i++)  // new code -- killough
        for (*freehead = visplanes[i], visplanes[i] = NULL; *freehead;)
            freehead = &(*freehead)->next;

    lastopening = openings;
}

// New function, by Lee Killough
static visplane_t *new_visplane(unsigned hash)
{
    visplane_t  *check = freetail;

    if (!check)
        check = calloc(1, sizeof(*check));
    else if (!(freetail = freetail->next))
        freehead = &freetail;
    check->next = visplanes[hash];
    visplanes[hash] = check;
    return check;
}

//
// R_FindPlane
//
visplane_t *R_FindPlane(fixed_t height, int picnum, int lightlevel, fixed_t xoffs, fixed_t yoffs)
{
    visplane_t          *check;
    unsigned int        hash;           // killough

    if (picnum == skyflatnum)
        height = lightlevel = 0;        // all skys map together

    // New visplane algorithm uses hash table -- killough
    hash = visplane_hash(picnum, lightlevel, height);

    for (check = visplanes[hash]; check; check = check->next)   // killough
        if (height == check->height && picnum == check->picnum && lightlevel == check->lightlevel
            && xoffs == check->xoffs && yoffs == check->yoffs)
            return check;

    check = new_visplane(hash);         // killough

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = viewwidth;
    check->maxx = -1;
    check->xoffs = xoffs;               // killough 2/28/98: Save offsets
    check->yoffs = yoffs;

    memset(check->top, SHRT_MAX, sizeof(check->top));

    return check;
}

//
// R_CheckPlane
//
visplane_t *R_CheckPlane(visplane_t *pl, int start, int stop)
{
    int intrl;
    int intrh;
    int unionl;
    int unionh;
    int x;

    if (start < pl->minx)
    {
        intrl = pl->minx;
        unionl = start;
    }
    else
    {
        unionl = pl->minx;
        intrl = start;
    }

    if (stop > pl->maxx)
    {
        intrh = pl->maxx;
        unionh = stop;
    }
    else
    {
        unionh = pl->maxx;
        intrh = stop;
    }

    for (x = intrl; x <= intrh && pl->top[x] == SHRT_MAX; x++);

    // [crispy] fix HOM if ceilingplane and floorplane are the same
    // visplane (e.g. both skies)
    if (!(pl == floorplane && markceiling && floorplane == ceilingplane) && x > intrh)
    {
        pl->minx = unionl;
        pl->maxx = unionh;
    }
    else
    {
        unsigned int    hash = visplane_hash(pl->picnum, pl->lightlevel, pl->height);
        visplane_t      *new_pl = new_visplane(hash);

        new_pl->height = pl->height;
        new_pl->picnum = pl->picnum;
        new_pl->lightlevel = pl->lightlevel;
        new_pl->sector = pl->sector;
        new_pl->xoffs = pl->xoffs;      // killough 2/28/98
        new_pl->yoffs = pl->yoffs;
        pl = new_pl;
        pl->minx = start;
        pl->maxx = stop;
        memset(pl->top, SHRT_MAX, sizeof(pl->top));
    }

    return pl;
}

//
// R_MakeSpans
//
static void R_MakeSpans(int x, unsigned int t1, unsigned int b1, unsigned int t2, unsigned int b2)
{
    for (; t1 < t2 && t1 <= b1; t1++)
        R_MapPlane(t1, spanstart[t1], x - 1);
    for (; b1 > b2 && b1 >= t1; b1--)
        R_MapPlane(b1, spanstart[b1], x - 1);
    while (t2 < t1 && t2 <= b2)
        spanstart[t2++] = x;
    while (b2 > b1 && b2 >= t2)
        spanstart[b2--] = x;
}

// Ripple Effect from Eternity Engine (r_ripple.cpp) by Simon Howard
#define AMP             2
#define AMP2            2
#define SPEED           40

// swirl factors determine the number of waves per flat width
// 1 cycle per 64 units
#define SWIRLFACTOR     (8192 / 64)

// 1 cycle per 32 units (2 in 64)
#define SWIRLFACTOR2    (8192 / 32)

char    *normalflat;
char    distortedflat[4096];

//
// R_DistortedFlat
//
// Generates a distorted flat from a normal one using a two-dimensional
// sine wave pattern.
//
char *R_DistortedFlat(int flatnum)
{
    static int  swirltic = -1;
    static int  offset[4096];
    int         i;
    int         leveltic = I_GetTime();

    // built this tic?
    if (gametic != swirltic)
    {
        int     x, y;

        for (x = 0; x < 64; x++)
            for (y = 0; y < 64; y++)
            {
                int     x1, y1;
                int     sinvalue, sinvalue2;

                sinvalue = (y * SWIRLFACTOR + leveltic * SPEED * 5 + 900) & 8191;
                sinvalue2 = (x * SWIRLFACTOR2 + leveltic * SPEED * 4 + 300) & 8191;
                x1 = x + 128 + ((finesine[sinvalue] * AMP) >> FRACBITS)
                    + ((finesine[sinvalue2] * AMP2) >> FRACBITS);

                sinvalue = (x * SWIRLFACTOR + leveltic * SPEED * 3 + 700) & 8191;
                sinvalue2 = (y * SWIRLFACTOR2 + leveltic * SPEED * 4 + 1200) & 8191;
                y1 = y + 128 + ((finesine[sinvalue] * AMP) >> FRACBITS)
                    + ((finesine[sinvalue2] * AMP2) >> FRACBITS);

                x1 &= 63;
                y1 &= 63;

                offset[(y << 6) + x] = (y1 << 6) + x1;
            }

        swirltic = gametic;
    }

    normalflat = W_CacheLumpNum(firstflat + flatnum, PU_STATIC);

    for (i = 0; i < 4096; i++)
        distortedflat[i] = normalflat[offset[i]];

    // free the original
    Z_ChangeTag(normalflat, PU_CACHE);

    return distortedflat;
}

//
// R_DrawPlanes
// At the end of each frame.
//
void R_DrawPlanes(void)
{
    int i;

    for (i = 0; i < MAXVISPLANES; i++)
    {
        visplane_t      *pl;

        for (pl = visplanes[i]; pl; pl = pl->next)
        {
            if (pl->minx <= pl->maxx)
            {
                // sky flat
                if (pl->picnum == skyflatnum)
                {
                    int x;
                    
                    dc_iscale = pspriteiscale;

                    // Sky is always drawn full bright,
                    //  i.e. colormaps[0] is used.
                    // Because of this hack, sky is not affected
                    //  by INVUL inverse mapping.
                    dc_colormap = (fixedcolormap ? fixedcolormap : fullcolormap);
                    dc_texturemid = skytexturemid;
                    dc_texheight = textureheight[skytexture] >> FRACBITS;
                    for (x = pl->minx; x <= pl->maxx; x++)
                    {
                        dc_yl = pl->top[x];
                        dc_yh = pl->bottom[x];

                        if (dc_yl != SHRT_MAX && dc_yl <= dc_yh)
                        {
                            dc_x = x;
                            dc_source = R_GetColumn(skytexture,
                                (viewangle + xtoviewangle[x]) >> ANGLETOSKYSHIFT);
                            skycolfunc();
                        }
                    }
                }
                else
                {
                    // regular flat
                    int         picnum = pl->picnum;
                    boolean     liquid = isliquid[picnum];
                    boolean     swirling = (liquid && swirlingliquid);
                    int         lumpnum = firstflat + flattranslation[picnum];
                    int         light = (pl->lightlevel >> LIGHTSEGSHIFT) + extralight * LIGHTBRIGHT;
                    int         stop = pl->maxx + 1;
                    int         x;

                    ds_source = (swirling ? R_DistortedFlat(picnum) :
                        W_CacheLumpNum(lumpnum, PU_STATIC));

                    xoffs = pl->xoffs;  // killough 2/28/98: Add offsets
                    yoffs = pl->yoffs;
                    planeheight = ABS(pl->height - viewz);

                    if (liquid && pl->sector && pl->sector->animate != INT_MAX)
                        planeheight -= pl->sector->animate;

                    planezlight = zlight[BETWEEN(0, light, LIGHTLEVELS - 1)];

                    pl->top[pl->minx - 1] = pl->top[stop] = SHRT_MAX;

                    for (x = pl->minx; x <= stop; x++)
                        R_MakeSpans(x, pl->top[x - 1], pl->bottom[x - 1], pl->top[x], pl->bottom[x]);

                    if (!swirling)
                        W_ReleaseLumpNum(lumpnum);
                }
            }
        }
    }
}
