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

#define _USE_MATH_DEFINES

#include <math.h>

#include "c_console.h"
#include "d_net.h"
#include "doomstat.h"
#include "i_timer.h"
#include "m_config.h"
#include "m_menu.h"
#include "p_local.h"
#include "r_sky.h"
#include "v_video.h"

// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW     2048

// increment every time a check is made
int                     validcount = 1;

lighttable_t            *fixedcolormap;
extern lighttable_t     **walllights;

int                     centerx;
int                     centery;

fixed_t                 centerxfrac;
fixed_t                 centeryfrac;
fixed_t                 viewheightfrac;
fixed_t                 projection;
fixed_t                 projectiony;

fixed_t                 viewx;
fixed_t                 viewy;
fixed_t                 viewz;

angle_t                 viewangle;

fixed_t                 viewcos;
fixed_t                 viewsin;

player_t                *viewplayer;

// [AM] Fractional part of the current tic, in the half-open
//      range of [0.0, 1.0).  Used for interpolation.
fixed_t                 fractionaltic;

//
// precalculated math tables
//
angle_t                 clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X.
int                     viewangletox[FINEANGLES / 2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
angle_t                 xtoviewangle[SCREENWIDTH + 1];

fixed_t                 *finecosine = &finesine[FINEANGLES / 4];

// killough 3/20/98: Support dynamic colormaps, e.g. deep water
// killough 4/4/98: support dynamic number of them as well
int                     numcolormaps = 1;
lighttable_t            *(*c_scalelight)[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t            *(*c_zlight)[LIGHTLEVELS][MAXLIGHTZ];
lighttable_t            *(*scalelight)[MAXLIGHTSCALE];
lighttable_t            *(*zlight)[MAXLIGHTZ];
lighttable_t            *fullcolormap;
lighttable_t            **colormaps;

// bumped light from gun blasts
int                     extralight;

boolean                 translucency = TRANSLUCENCY_DEFAULT;

boolean                 homindicator = HOMINDICATOR_DEFAULT;

int                     r_frame_count;

extern int              viewheight2;
extern int              gametic;
extern boolean          canmodify;

void (*colfunc)(void);
void (*wallcolfunc)(void);
void (*fbwallcolfunc)(void);
void (*basecolfunc)(void);
void (*fuzzcolfunc)(void);
void (*tlcolfunc)(void);
void (*tl50colfunc)(void);
void (*tl33colfunc)(void);
void (*tlgreencolfunc)(void);
void (*tlredcolfunc)(void);
void (*tlredwhitecolfunc)(void);
void (*tlredwhite50colfunc)(void);
void (*tlbluecolfunc)(void);
void (*tlgreen50colfunc)(void);
void (*tlred50colfunc)(void);
void (*tlblue50colfunc)(void);
void (*redtobluecolfunc)(void);
void (*transcolfunc)(void);
void (*spanfunc)(void);
void (*skycolfunc)(void);
void (*redtogreencolfunc)(void);
void (*tlredtoblue33colfunc)(void);
void (*tlredtogreen33colfunc)(void);
void (*psprcolfunc)(void);
void (*bloodsplatcolfunc)(void);
void (*megaspherecolfunc)(void);

//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
int R_PointOnSide(fixed_t x, fixed_t y, const node_t *node)
{
    return ((int64_t)(y - node->y) * node->dx + (int64_t)(node->x - x) * node->dy >= 0);
}

int R_PointOnSegSide(fixed_t x, fixed_t y, seg_t *line)
{
    return ((int64_t)(line->v2->x - line->v1->x) * (y - line->v1->y)
            - (int64_t)(line->v2->y - line->v1->y) * (x - line->v1->x) >= 0);
}

int SlopeDiv(unsigned int num, unsigned int den)
{
    unsigned int        ans;

    if (den < 512)
        return SLOPERANGE;

    ans = (num << 3) / (den >> 8);
    return (ans <= SLOPERANGE ? ans : SLOPERANGE);
}

//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
// the coordinates are flipped until they are in the first octant of
// the coordinate system, then the y (<=x) is scaled and divided by x
// to get a tangent (slope) value which is looked up in the
// tantoangle[] table.

// Point (x2,y2) to point (x1,y1) angle.
angle_t R_PointToAngle2(fixed_t x2, fixed_t y2, fixed_t x1, fixed_t y1)
{
    x1 -= x2;
    y1 -= y2;

    if (!x1 && !y1)
        return 0;

    if (x1 > INT_MAX / 4 || x1 < -INT_MAX / 4 || y1 > INT_MAX / 4 || y1 < -INT_MAX / 4)
        return (int)(atan2(y1, x1) * ANG180 / M_PI);

    if (x1 >= 0)
    {
        if (y1 >= 0)
            return (x1 > y1 ? tantoangle[SlopeDiv(y1, x1)] :
                ANG90 - 1 - tantoangle[SlopeDiv(x1, y1)]);
        else
        {
            y1 = -y1;
            return (x1 > y1 ? -(int)tantoangle[SlopeDiv(y1, x1)] :
                ANG270 + tantoangle[SlopeDiv(x1, y1)]);
        }
    }
    else
    {
        x1 = -x1;
        if (y1 >= 0)
            return (x1 > y1 ? ANG180 - 1 - tantoangle[SlopeDiv(y1, x1)] :
                ANG90 + tantoangle[SlopeDiv(x1, y1)]);
        else
        {
            y1 = -y1;
            return (x1 > y1 ? ANG180 + tantoangle[SlopeDiv(y1, x1)] :
                ANG270 - 1 - tantoangle[SlopeDiv(x1, y1)]);
        }
    }
}

// Point of view (viewx, viewy) to point (x1, y1) angle.
angle_t R_PointToAngle(fixed_t x, fixed_t y)
{
    x -= viewx;
    y -= viewy;

    if (!x && !y)
        return 0;

    if (x > INT_MAX / 4 || x < -INT_MAX / 4 || y > INT_MAX / 4 || y < -INT_MAX / 4)
        return (int)(atan2(y, x) * ANG180 / M_PI);

    if (x >= 0)
    {
        if (y >= 0)
            return (x > y ? tantoangle[SlopeDiv(y, x)] :
                ANG90 - 1 - tantoangle[SlopeDiv(x, y)]);
        else
        {
            y = -y;
            return (x > y ? -(int)tantoangle[SlopeDiv(y, x)] :
                ANG270 + tantoangle[SlopeDiv(x, y)]);
        }
    }
    else
    {
        x = -x;
        if (y >= 0)
            return (x > y ? ANG180 - 1 - tantoangle[SlopeDiv(y, x)] :
                ANG90 + tantoangle[SlopeDiv(x, y)]);
        else
        {
            y = -y;
            return (x > y ? ANG180 + tantoangle[SlopeDiv(y, x)] :
                ANG270 - 1 - tantoangle[SlopeDiv(x, y)]);
        }
    }
}

fixed_t R_PointToDist(fixed_t x, fixed_t y)
{
    fixed_t     dx = ABS(x - viewx);
    fixed_t     dy = ABS(y - viewy);

    if (dy > dx)
    {
        fixed_t t = dx;

        dx = dy;
        dy = t;
    }

    if (!dy)
        return dx;

    return (dx ? FixedDiv(dx, finesine[(tantoangle[FixedDiv(dy, dx) >> DBITS]
        + ANG90) >> ANGLETOFINESHIFT]) : 0);
}

// [AM] Interpolate between two angles.
angle_t R_InterpolateAngle(angle_t oangle, angle_t nangle, fixed_t scale)
{
    if (nangle == oangle)
        return nangle;
    else if (nangle > oangle)
    {
        if (nangle - oangle < ANG270)
            return oangle + (angle_t)((nangle - oangle) * FIXED2DOUBLE(scale));
        else // Wrapped around
            return oangle - (angle_t)((oangle - nangle) * FIXED2DOUBLE(scale));
    }
    else // nangle < oangle
    {
        if (oangle - nangle < ANG270)
            return oangle - (angle_t)((oangle - nangle) * FIXED2DOUBLE(scale));
        else // Wrapped around
            return oangle + (angle_t)((nangle - oangle) * FIXED2DOUBLE(scale));
    }
}

//
// R_InitTables
//
static void R_InitTables(void)
{
    int                 i;
    const double        pimul = M_PI * 2 / FINEANGLES;

    // viewangle tangent table
    finetangent[0] = (fixed_t)(FRACUNIT * tan((0.5 - FINEANGLES / 4) * pimul) + 0.5);
    for (i = 1; i < FINEANGLES / 2; i++)
        finetangent[i] = (fixed_t)(FRACUNIT * tan((i - FINEANGLES / 4) * pimul) + 0.5);

    // finesine table
    for (i = 0; i < FINEANGLES / 4; i++)
        finesine[i] = (fixed_t)(FRACUNIT * sin(i * pimul));
    for (i = 0; i < FINEANGLES / 4; i++)
        finesine[i + FINEANGLES / 4] = finesine[FINEANGLES / 4 - 1 - i];
    for (i = 0; i < FINEANGLES / 2; i++)
        finesine[i + FINEANGLES / 2] = -finesine[i];
    finesine[FINEANGLES / 4] = FRACUNIT;
    finesine[FINEANGLES * 3 / 4] = -FRACUNIT;
    memcpy(&finesine[FINEANGLES], &finesine[0], sizeof(angle_t) * FINEANGLES / 4);
}

static void R_InitPointToAngle(void)
{
    int         i;

    // slope (tangent) to angle lookup
    for (i = 0; i <= SLOPERANGE; i++)
    {
        float   f = atanf((float)i / SLOPERANGE) / ((float)M_PI * 2);
        long    t = (long)(0xffffffff * f);

        // this used to have PI (as defined above) written out longhand
        tantoangle[i] = t;
    }
}

// e6y: caching
angle_t R_GetVertexViewAngle(vertex_t *v)
{
    if (v->angletime != r_frame_count)
    {
        v->angletime = r_frame_count;
        v->viewangle = R_PointToAngle(v->x, v->y);
    }
    return v->viewangle;
}

//
// R_InitTextureMapping
//
void R_InitTextureMapping(void)
{
    int         i;
    int         x;
    int         t;
    fixed_t     focallength;

    // Use tangent table to generate viewangletox:
    //  viewangletox will give the next greatest x
    //  after the view angle.

    const fixed_t       hitan = finetangent[FINEANGLES / 4 + FIELDOFVIEW / 2];
    const fixed_t       lotan = finetangent[FINEANGLES / 4 - FIELDOFVIEW / 2];
    const int           highend = viewwidth + 1;

    // Calc focallength
    //  so FIELDOFVIEW angles covers SCREENWIDTH.
    focallength = FixedDiv(centerxfrac, hitan);

    for (i = 0; i < FINEANGLES / 2; i++)
    {
        fixed_t tangent = finetangent[i];

        if (tangent > hitan)
            t = -1;
        else if (tangent < lotan)
            t = highend;
        else
        {
            t = (centerxfrac - FixedMul(tangent, focallength) + FRACUNIT - 1) >> FRACBITS;
            t = BETWEEN(-1, t, highend);
        }
        viewangletox[i] = t;
    }

    // Scan viewangletox[] to generate xtoviewangle[]:
    //  xtoviewangle will give the smallest view angle
    //  that maps to x.
    for (x = 0; x <= viewwidth; x++)
    {
        for (i = 0; viewangletox[i] > x; i++);
        xtoviewangle[x] = (i << ANGLETOFINESHIFT) - ANG90;
    }

    // Take out the fencepost cases from viewangletox.
    for (i = 0; i < FINEANGLES / 2; i++)
    {
        if (viewangletox[i] == -1)
            viewangletox[i] = 0;
        else if (viewangletox[i] == highend)
            viewangletox[i]--;
    }

    clipangle = xtoviewangle[0];
}

//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//
#define DISTMAP 2

void R_InitLightTables(void)
{
    int i;

    // killough 4/4/98: dynamic colormaps
    c_zlight = malloc(sizeof(*c_zlight) * numcolormaps);
    c_scalelight = malloc(sizeof(*c_scalelight) * numcolormaps);

    // Calculate the light levels to use
    //  for each level / distance combination.
    for (i = 0; i < LIGHTLEVELS; i++)
    {
        int j, startmap = ((LIGHTLEVELS - LIGHTBRIGHT - i) * 2) * NUMCOLORMAPS / LIGHTLEVELS;

        for (j = 0; j < MAXLIGHTZ; j++)
        {
            int scale = FixedDiv(SCREENWIDTH / 2 * FRACUNIT, (j + 1) << LIGHTZSHIFT);
            int t, level = BETWEEN(0, startmap - (scale >>= LIGHTSCALESHIFT) / DISTMAP,
                NUMCOLORMAPS - 1) * 256;

            // killough 3/20/98: Initialize multiple colormaps
            for (t = 0; t < numcolormaps; t++)
                c_zlight[t][i][j] = colormaps[t] + level;
        }
    }
}

//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//
boolean setsizeneeded;
int     setblocks;

void R_SetViewSize(int blocks)
{
    setsizeneeded = true;
    setblocks = blocks + 3;
}

//
// R_ExecuteSetViewSize
//
void R_ExecuteSetViewSize(void)
{
    int i;
    int j;

    setsizeneeded = false;

    if (setblocks == 11)
    {
        scaledviewwidth = SCREENWIDTH;
        viewheight = SCREENHEIGHT;
        viewheight2 = SCREENHEIGHT;
    }
    else
    {
        scaledviewwidth = setblocks * SCREENWIDTH / 10;
        viewheight = (setblocks * (SCREENHEIGHT - SBARHEIGHT) / 10) & ~7;
        viewheight2 = SCREENHEIGHT - SBARHEIGHT;
    }

    viewwidth = scaledviewwidth;
    viewheightfrac = viewheight << FRACBITS;

    centery = viewheight / 2;
    centerx = viewwidth / 2;
    centerxfrac = centerx << FRACBITS;
    centeryfrac = centery << FRACBITS;
    projection = centerxfrac;
    projectiony = ((SCREENHEIGHT * centerx * ORIGINALWIDTH) / ORIGINALHEIGHT) / SCREENWIDTH * FRACUNIT;

    R_InitBuffer(scaledviewwidth, viewheight);

    R_InitTextureMapping();

    // psprite scales
    pspritexscale = (centerx << FRACBITS) / (ORIGINALWIDTH / 2);
    pspriteyscale = (((SCREENHEIGHT * viewwidth) / SCREENWIDTH) << FRACBITS) / ORIGINALHEIGHT;
    pspriteiscale = FixedDiv(FRACUNIT, pspritexscale);

    // thing clipping
    for (i = 0; i < viewwidth; i++)
        screenheightarray[i] = viewheight;

    // planes
    for (i = 0; i < viewheight; i++)
    {
        fixed_t dy = ABS(((i - viewheight / 2) << FRACBITS) + FRACUNIT / 2);

        yslope[i] = FixedDiv(projectiony, dy);
    }

    for (i = 0; i < viewwidth; i++)
    {
        fixed_t cosadj = ABS(finecosine[xtoviewangle[i] >> ANGLETOFINESHIFT]);

        distscale[i] = FixedDiv(FRACUNIT, cosadj);
    }

    // Calculate the light levels to use
    //  for each level / scale combination.
    for (i = 0; i < LIGHTLEVELS; i++)
    {
        int startmap = ((LIGHTLEVELS - LIGHTBRIGHT - i) * 2) * NUMCOLORMAPS / LIGHTLEVELS;

        for (j = 0; j < MAXLIGHTSCALE; j++)
        {
            int t, level = BETWEEN(0, startmap - j * SCREENWIDTH / (viewwidth * DISTMAP),
                NUMCOLORMAPS - 1) * 256;

            // killough 3/20/98: initialize multiple colormaps
            for (t = 0; t < numcolormaps; t++)     // killough 4/4/98
                c_scalelight[t][i][j] = colormaps[t] + level;
        }
    }
}

void R_InitColumnFunctions(void)
{
    int i;

    colfunc = basecolfunc = R_DrawColumn;
    fuzzcolfunc = R_DrawFuzzColumn;
    transcolfunc = R_DrawTranslatedColumn;

    if (translucency)
    {
        tlcolfunc = R_DrawTranslucentColumn;
        tl50colfunc = R_DrawTranslucent50Column;
        tl33colfunc = R_DrawTranslucent33Column;
        tlgreencolfunc = R_DrawTranslucentGreenColumn;
        tlredcolfunc = R_DrawTranslucentRedColumn;
        tlredwhitecolfunc = R_DrawTranslucentRedWhiteColumn;
        tlredwhite50colfunc = R_DrawTranslucentRedWhite50Column;
        tlbluecolfunc = R_DrawTranslucentBlueColumn;
        tlgreen50colfunc = R_DrawTranslucentGreen50Column;
        tlred50colfunc = R_DrawTranslucentRed50Column;
        tlblue50colfunc = R_DrawTranslucentBlue50Column;
        tlredtoblue33colfunc = R_DrawTranslucentRedToBlue33Column;
        tlredtogreen33colfunc = R_DrawTranslucentRedToGreen33Column;
        bloodsplatcolfunc = R_DrawBloodSplatColumn;
        megaspherecolfunc = R_DrawMegaSphereColumn;
    }
    else
    {
        tlcolfunc = R_DrawColumn;
        tl50colfunc = R_DrawColumn;
        tl33colfunc = R_DrawColumn;
        tlgreencolfunc = R_DrawColumn;
        tlredcolfunc = R_DrawColumn;
        tlredwhitecolfunc = R_DrawColumn;
        tlredwhite50colfunc = R_DrawColumn;
        tlbluecolfunc = R_DrawColumn;
        tlgreen50colfunc = R_DrawColumn;
        tlred50colfunc = R_DrawColumn;
        tlblue50colfunc = R_DrawColumn;
        tlredtoblue33colfunc = R_DrawRedToBlueColumn;
        tlredtogreen33colfunc = R_DrawRedToGreenColumn;
        bloodsplatcolfunc = R_DrawSolidBloodSplatColumn;
        megaspherecolfunc = R_DrawSolidMegaSphereColumn;
    }

    spanfunc = R_DrawSpan;
    redtobluecolfunc = R_DrawRedToBlueColumn;
    redtogreencolfunc = R_DrawRedToGreenColumn;
    wallcolfunc = R_DrawWallColumn;
    fbwallcolfunc = R_DrawFullbrightWallColumn;
    psprcolfunc = R_DrawPlayerSpriteColumn;

    for (i = 0; i < NUMMOBJTYPES; i++)
    {
        mobjinfo_t      *info = &mobjinfo[i];
        int             flags2 = info->flags2;

        if (flags2 & MF2_TRANSLUCENT)
            info->colfunc = tlcolfunc;
        else if (info->doomednum == MegaSphere && !hacx)
            info->colfunc = megaspherecolfunc;
        else if (info->flags & MF_FUZZ)
            info->colfunc = fuzzcolfunc;
        else if (flags2 & MF2_TRANSLUCENT_REDONLY)
            info->colfunc = tlredcolfunc;
        else if (flags2 & MF2_TRANSLUCENT_GREENONLY)
            info->colfunc = tlgreencolfunc;
        else if (flags2 & MF2_TRANSLUCENT_BLUEONLY)
            info->colfunc = tlbluecolfunc;
        else if (flags2 & MF2_TRANSLUCENT_33)
            info->colfunc = tl33colfunc;
        else if (flags2 & MF2_TRANSLUCENT_50)
            info->colfunc = tl50colfunc;
        else if (flags2 & MF2_TRANSLUCENT_REDWHITEONLY)
            info->colfunc = tlredwhitecolfunc;
        else if (flags2 & MF2_TRANSLUCENT_REDTOGREEN_33)
            info->colfunc = tlredtogreen33colfunc;
        else if (flags2 & MF2_TRANSLUCENT_REDTOBLUE_33)
            info->colfunc = tlredtoblue33colfunc;
        else if (flags2 & MF2_REDTOGREEN)
            info->colfunc = redtogreencolfunc;
        else if (flags2 & MF2_REDTOBLUE)
            info->colfunc = redtobluecolfunc;
        else
            info->colfunc = basecolfunc;
    }

    if (chex)
        mobjinfo[MT_BLOOD].blood = GREENBLOOD;
}

//
// R_Init
//
void R_Init(void)
{
    R_InitData();
    R_InitPointToAngle();
    R_InitTables();

    R_SetViewSize(screensize);
    R_InitLightTables();
    R_InitSkyMap();
    R_InitTranslationTables();
    R_InitColumnFunctions();
}

//
// R_PointInSubsector
//
subsector_t *R_PointInSubsector(fixed_t x, fixed_t y)
{
    int nodenum = numnodes - 1;

    while (!(nodenum & NF_SUBSECTOR))
        nodenum = nodes[nodenum].children[R_PointOnSide(x, y, nodes + nodenum)];

    return &subsectors[nodenum & ~NF_SUBSECTOR];
}

//
// R_SetupFrame
//
void R_SetupFrame(player_t *player)
{
    int cm;

    viewplayer = player;

    // [AM] Interpolate the player camera if the feature is enabled.

    // Figure out how far into the current tic we're in as a fixed_t
    if (!capfps)
        fractionaltic = I_GetTimeMS() * TICRATE % 1000 * FRACUNIT / 1000;

    if (!capfps
        // Don't interpolate on the first tic of a level, otherwise
        // oldviewz might be garbage.
        && leveltime > 1
        // Don't interpolate if the player did something 
        // that would necessitate turning it off for a tic.
        && player->mo->interp
        // Don't interpolate during a paused state
        && !paused && !menuactive && !consoleactive)
    {
        // Interpolate player camera from their old position to their current one.
        viewx = player->mo->oldx + FixedMul(player->mo->x - player->mo->oldx, fractionaltic);
        viewy = player->mo->oldy + FixedMul(player->mo->y - player->mo->oldy, fractionaltic);
        viewz = player->oldviewz + FixedMul(player->viewz - player->oldviewz, fractionaltic);
        viewangle = R_InterpolateAngle(player->mo->oldangle, player->mo->angle, fractionaltic);
    }
    else
    {
        viewx = player->mo->x;
        viewy = player->mo->y;
        viewz = player->viewz;
        viewangle = player->mo->angle;
    }

    extralight = player->extralight << 1;

    viewsin = finesine[viewangle >> ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle >> ANGLETOFINESHIFT];

    // killough 3/20/98, 4/4/98: select colormap based on player status
    if (player->mo->subsector->sector->heightsec != -1)
    {
        const sector_t  *s = player->mo->subsector->sector->heightsec + sectors;

        cm = (viewz < s->interpfloorheight ? s->bottommap : (viewz > s->interpceilingheight ?
            s->topmap : s->midmap));
        if (cm < 0 || cm > numcolormaps)
            cm = 0;
    }
    else
        cm = 0;

    fullcolormap = colormaps[cm];
    zlight = c_zlight[cm];
    scalelight = c_scalelight[cm];

    if (player->fixedcolormap)
    {
        // killough 3/20/98: localize scalelightfixed (readability/optimization)
        static lighttable_t     *scalelightfixed[MAXLIGHTSCALE];
        int                     i;

        fixedcolormap = fullcolormap   // killough 3/20/98: use fullcolormap
            + player->fixedcolormap * 256 * sizeof(lighttable_t);

        walllights = scalelightfixed;

        for (i = 0; i<MAXLIGHTSCALE; i++)
            scalelightfixed[i] = fixedcolormap;
    }
    else
        fixedcolormap = 0;

    validcount++;
}

//
// R_RenderView
//
void R_RenderPlayerView(player_t *player)
{
    r_frame_count++;

    R_SetupFrame(player);

    // Clear buffers.
    R_ClearClipSegs();
    R_ClearDrawSegs();
    R_ClearPlanes();
    R_ClearSprites();

    if (automapactive)
    {
        // The head node is the last node output.
        R_RenderBSPNode(numnodes - 1);
    }
    else
    {
        if (player->cheats & CF_NOCLIP)
            V_FillRect(0, viewwindowx, viewwindowy, viewwidth, viewheight, 0);
        else if (homindicator)
            V_FillRect(0, viewwindowx, viewwindowy, viewwidth, viewheight,
                ((gametic % 20) < 9 && !consoleactive && !menuactive && !paused ? 176 : 0));

        // The head node is the last node output.
        R_RenderBSPNode(numnodes - 1);

        R_DrawPlanes();
        R_DrawMasked();
    }
}
