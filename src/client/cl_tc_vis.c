/*
 * Ported from ETe (src/client/cl_tc_vis.c), which credits "breadsticks" for guidance and
 * "XPC32 and breadsticks" for the original technique. ETJump exposes this same engine feature
 * via cvar unlockers (etj_drawClips/etj_drawTriggers/etj_drawSlicks) but does not implement it
 * itself - the actual brush-visualization logic lives entirely in the engine, as it does here.
 *
 * Extended with a SKY_BRUSH category (r_drawSkySurfaces) to visualize SURF_SKY collision brushes -
 * including invisible/nodraw "caulk sky" brushes that seal a level but never render a polygon,
 * which is exactly the case that makes missiles appear to silently pass through solid-looking
 * walls (see CG_PredictRiflenadeTrajectory's sky-passthrough handling).
 */

#include "client.h"
#include "../qcommon/cm_local.h"
#include <stdlib.h>
#include <assert.h>
#include <math.h>

// if you dare to exceed this...
#define MAX_FACE_VERTS 64
#define MIN_WALK_NORMAL 0.7f

typedef enum { TRIGGER_BRUSH = 0, CLIP_BRUSH, SLICK_BRUSH, SKY_BRUSH } visBrushType_t;

typedef struct
{
	int numVerts;
	polyVert_t *verts;
	vec3_t mins;
	vec3_t maxs;
} visFace_t;

typedef struct visBrushNode_s
{
	int numFaces;
	visFace_t *faces;

	struct visBrushNode_s *next;
} visBrushNode_t;

static void add_triggers(void);
static void add_clips(void);
static void add_slicks(void);
static void add_sky(void);
static void gen_visible_brush(int brushnum, const vec3_t origin, visBrushType_t type, const vec4_t color);
static qboolean intersect_planes(const cplane_t *p1, const cplane_t *p2, const cplane_t *p3, vec3_t p);
static qboolean point_in_brush(const vec3_t point, const cbrush_t *brush);
static int winding_cmp(const void *a, const void *b);
static void add_vert_to_face(visFace_t *face, const vec3_t vert, const vec4_t color, const vec2_t tex_coords);
static float *get_uv_coords(vec2_t uv, const vec3_t vert, const vec3_t normal);
static void free_vis_brushes(visBrushNode_t *brushes);
static void draw(visBrushNode_t *brush, qhandle_t shader);

static visBrushNode_t *trigger_head = NULL;
static visBrushNode_t *clip_head    = NULL;
static visBrushNode_t *slick_head   = NULL;
static visBrushNode_t *sky_head     = NULL;

/* needed for winding_cmp */
static vec3_t w_center, w_normal, w_ref_vec;
static float w_ref_vec_len;

static cvar_t *triggers_draw;
static cvar_t *clips_draw;
static cvar_t *slicks_draw;
static cvar_t *sky_draw;

static qhandle_t tc_render_shader;

// alpha kept well below 255 - the shader now uses normal alpha blending (not additive), so a
// fully opaque alpha would render these as solid, view-blocking shapes instead of an overlay
static const vec4_t trigger_color        = { 0, 128, 0, 140 };
static const vec4_t clip_color           = { 128, 0, 0, 140 };
static const vec4_t clipweap_color       = { 96, 0, 96, 140 };
static const vec4_t clipweap_metal_color = { 37, 58, 61, 140 };
static const vec4_t clipweap_wood_color  = { 33, 33, 0, 140 };
static const vec4_t slick_color          = { 0, 64, 128, 140 };
static const vec4_t sky_color            = { 255, 140, 0, 140 };

/**
 * @brief tc_vis_init
 * Rebuilds the visualization brush lists for the currently loaded map. Must be called after
 * CM_LoadMap.
 */
void tc_vis_init(void)
{
	free_vis_brushes(trigger_head);
	free_vis_brushes(clip_head);
	free_vis_brushes(slick_head);
	free_vis_brushes(sky_head);
	trigger_head = NULL;
	clip_head    = NULL;
	slick_head   = NULL;
	sky_head     = NULL;

	triggers_draw = Cvar_Get("r_drawTriggers", "0", CVAR_ARCHIVE_ND | CVAR_CHEAT);
	clips_draw    = Cvar_Get("r_drawClips", "0", CVAR_ARCHIVE_ND | CVAR_CHEAT);
	slicks_draw   = Cvar_Get("r_drawSlicks", "0", CVAR_ARCHIVE_ND | CVAR_CHEAT);
	sky_draw      = Cvar_Get("r_drawSkySurfaces", "0", CVAR_ARCHIVE_ND | CVAR_CHEAT);

	if (!re.GetTcRenderShader)
	{
		return; // renderer backend doesn't implement the internal visualization shader
	}

	tc_render_shader = re.GetTcRenderShader();
	if (!tc_render_shader)
	{
		return;
	}

	add_triggers();
	add_clips();
	add_slicks();
	add_sky();
}

/**
 * @brief tc_vis_render
 * Draws whichever brush categories are currently cvar-enabled. Called once per rendered frame.
 */
void tc_vis_render(void)
{
	if (cls.glconfig.vidWidth == 0 || (Key_GetCatcher() & KEYCATCH_UI))
	{
		return;
	}

	if (!cls.rendererStarted || !tc_render_shader)
	{
		return;
	}

	if (triggers_draw->integer)
	{
		draw(trigger_head, tc_render_shader);
	}
	if (clips_draw->integer)
	{
		draw(clip_head, tc_render_shader);
	}
	if (slicks_draw->integer)
	{
		draw(slick_head, tc_render_shader);
	}
	if (sky_draw->integer)
	{
		draw(sky_head, tc_render_shader);
	}
}

// ripped from breadsticks (via ETe)
static void add_triggers(void)
{
	char *entities = cm.entityString;

	for (;; )
	{
		qboolean is_trigger = qfalse;
		int      model      = -1;
		vec3_t   origin     = { 0 };
		char     *token;

		token = COM_Parse(&entities);
		if (!entities)
		{
			break;
		}

		if (token[0] != '{')
		{
			Com_Error(ERR_DROP, "map is borked\n");
		}

		for (;;)
		{
			token = COM_Parse(&entities);

			if (token[0] == '}')
			{
				break;
			}

			if (!Q_stricmp(token, "model"))
			{
				token = COM_Parse(&entities);
				if (token[0] == '*')
				{
					model = atoi(token + 1);
				}
			}

			if (!Q_stricmp(token, "classname"))
			{
				token      = COM_Parse(&entities);
				is_trigger = !!Q_stristr(token, "trigger");
			}

			if (!Q_stricmp(token, "origin"))
			{
				token = COM_Parse(&entities);
				sscanf(token, "%f %f %f", &origin[0], &origin[1], &origin[2]);
			}
		}

		if (is_trigger && model > 0)
		{
			const cLeaf_t *leaf = &cm.cmodels[model].leaf;
			int           i;

			for (i = 0; i < leaf->numLeafBrushes; i++)
			{
				gen_visible_brush(cm.leafbrushes[leaf->firstLeafBrush + i], origin, TRIGGER_BRUSH, trigger_color);
			}
		}
	}
}

static void add_clips(void)
{
	int i, s;

	for (i = 0; i < cm.numBrushes; i++)
	{
		const cbrush_t *brush = &cm.brushes[i];

		// need to exclude ladders here explicitly since common/ladder is also CONTENTS_PLAYERCLIP
		if ((brush->contents & CONTENTS_PLAYERCLIP) && !(brush->sides->surfaceFlags & SURF_LADDER))
		{
			gen_visible_brush(i, vec3_origin, CLIP_BRUSH, clip_color);
		}

		// this should match weaponclips, although it might also match other shaders.
		// weaponclips don't have a specific content/surfaceparm, so in reality this is just a hack
		// to match parameters used in weaponclip shaders
		for (s = 0; s < brush->numsides; s++)
		{
			const cbrushside_t *side = &brush->sides[s];

			if (!(brush->contents & CONTENTS_SOLID) || (side->surfaceFlags & SURF_SLICK))
			{
				continue;
			}
			if ((side->surfaceFlags & SURF_NODRAW) && (side->surfaceFlags & SURF_NOMARKS) && (brush->contents & CONTENTS_TRANSLUCENT))
			{
				// weaponclip found, see if its metal, wood or other
				if (side->surfaceFlags & SURF_METAL)
				{
					gen_visible_brush(i, vec3_origin, CLIP_BRUSH, clipweap_metal_color);
					break;
				}
				else if (side->surfaceFlags & SURF_WOOD)
				{
					gen_visible_brush(i, vec3_origin, CLIP_BRUSH, clipweap_wood_color);
					break;
				}
				else
				{
					gen_visible_brush(i, vec3_origin, CLIP_BRUSH, clipweap_color);
					break;
				}
			}
		}
	}
}

static ID_INLINE qboolean angleSlick(const cplane_t *plane)
{
	return (plane->normal[2] < MIN_WALK_NORMAL && plane->normal[2] > 0);
}

static void add_slicks(void)
{
	int i, s;

	for (i = 0; i < cm.numBrushes; i++)
	{
		const cbrush_t *brush = &cm.brushes[i];

		if (!(brush->contents & CONTENTS_SOLID))
		{
			continue;
		}
		for (s = 0; s < brush->numsides; s++)
		{
			const cbrushside_t *side = &brush->sides[s];

			if ((side->surfaceFlags & SURF_SLICK) ||
			    (!(side->surfaceFlags & SURF_NODRAW) && !(side->surfaceFlags & SURF_SKY) && angleSlick(side->plane)))
			{
				gen_visible_brush(i, vec3_origin, SLICK_BRUSH, slick_color);
				break;
			}
		}
	}
}

/**
 * @brief add_sky
 * Finds every solid collision brush that has a SURF_SKY side - including ones with no visible
 * render surface at all (e.g. a "caulk sky" brush used to seal a leak), which is exactly what
 * r_debugShaderSurfaceFlags cannot show, since it only tints polygons that actually get drawn.
 */
static void add_sky(void)
{
	int i, s;

	for (i = 0; i < cm.numBrushes; i++)
	{
		const cbrush_t *brush = &cm.brushes[i];

		if (!(brush->contents & CONTENTS_SOLID))
		{
			continue;
		}
		for (s = 0; s < brush->numsides; s++)
		{
			const cbrushside_t *side = &brush->sides[s];

			if (side->surfaceFlags & SURF_SKY)
			{
				gen_visible_brush(i, vec3_origin, SKY_BRUSH, sky_color);
				break;
			}
		}
	}
}

static void gen_visible_brush(int brushnum, const vec3_t origin, visBrushType_t type, const vec4_t color)
{
	int             i;
	cbrush_t        *brush = &cm.brushes[brushnum];
	visBrushNode_t  *node;
	visBrushNode_t  **head = NULL;

	if (!brush->numsides)
	{
		return;
	}

	node           = malloc(sizeof(visBrushNode_t));
	node->numFaces = brush->numsides;
	node->faces    = malloc(node->numFaces * sizeof(visFace_t));
	for (i = 0; i < node->numFaces; i++)
	{
		node->faces[i].numVerts = 0;
		node->faces[i].verts    = malloc(MAX_FACE_VERTS * sizeof(polyVert_t));
	}

	for (i = 0; i < brush->numsides; i++)
	{
		cplane_t *p1 = brush->sides[i].plane;
		int      j;

		for (j = i + 1; j < brush->numsides; j++)
		{
			cplane_t *p2 = brush->sides[j].plane;
			int      k;

			for (k = j + 1; k < brush->numsides; k++)
			{
				vec3_t   p, v1, v2, v3;
				vec2_t   uv;
				cplane_t *p3 = brush->sides[k].plane;

				if (!intersect_planes(p1, p2, p3, p))
				{
					continue;
				}

				if (!point_in_brush(p, brush))
				{
					continue;
				}

				// translate point to be relative to provided origin
				VectorAdd(p, origin, p);

				// fix z-fighting by slightly moving vertices outwards
				VectorScale(p1->normal, .0625f, v1);
				VectorScale(p2->normal, .0625f, v2);
				VectorScale(p3->normal, .0625f, v3);
				VectorAdd(p, v1, v1);
				VectorAdd(p, v2, v2);
				VectorAdd(p, v3, v3);

				if (type != SLICK_BRUSH || (brush->sides->surfaceFlags & SURF_SLICK) || angleSlick(p1))
				{
					add_vert_to_face(&node->faces[i], v1, color, get_uv_coords(uv, p, p1->normal));
				}
				if (type != SLICK_BRUSH || (brush->sides->surfaceFlags & SURF_SLICK) || angleSlick(p2))
				{
					add_vert_to_face(&node->faces[j], v2, color, get_uv_coords(uv, p, p2->normal));
				}
				if (type != SLICK_BRUSH || (brush->sides->surfaceFlags & SURF_SLICK) || angleSlick(p3))
				{
					add_vert_to_face(&node->faces[k], v3, color, get_uv_coords(uv, p, p3->normal));
				}
			}
		}
	}

	// winding
	for (i = 0; i < brush->numsides; i++)
	{
		int       j;
		visFace_t *face = &node->faces[i];

		if (!face->numVerts)
		{
			continue;
		}
		VectorCopy(brush->sides[i].plane->normal, w_normal);
		VectorClear(w_center);
		ClearBounds(face->mins, face->maxs);
		for (j = 0; j < face->numVerts; j++)
		{
			VectorAdd(w_center, face->verts[j].xyz, w_center);
			AddPointToBounds(face->verts[j].xyz, face->mins, face->maxs);
		}
		VectorScale(w_center, 1.0f / face->numVerts, w_center);
		VectorSubtract(face->verts[0].xyz, w_center, w_ref_vec);
		w_ref_vec_len = VectorLength(w_ref_vec);
		if (face->numVerts >= 2)
		{
			qsort(face->verts, face->numVerts, sizeof(face->verts[0]), winding_cmp);
		}
	}

	switch (type)
	{
	case TRIGGER_BRUSH:
		head = &trigger_head;
		break;
	case CLIP_BRUSH:
		head = &clip_head;
		break;
	case SLICK_BRUSH:
		head = &slick_head;
		break;
	case SKY_BRUSH:
		head = &sky_head;
		break;
	}
	assert(head);
	node->next = *head;
	*head      = node;
}

static qboolean intersect_planes(const cplane_t *p1, const cplane_t *p2, const cplane_t *p3, vec3_t p)
{
	// thanks Real-Time Collision Detection
	int    i;
	vec3_t u, v;
	float  denom;

	CrossProduct(p2->normal, p3->normal, u);
	denom = DotProduct(p1->normal, u);

	// brushes with non-AA planes + AA bevel planes create invalid intersections
	// EPSILON 1e-5 too small => 1e-3
	if (fabsf(denom) < 1e-3f)
	{
		return qfalse;
	}

	for (i = 0; i < 3; i++)
	{
		p[i] = p3->dist * p2->normal[i] - p2->dist * p3->normal[i];
	}

	CrossProduct(p1->normal, p, v);
	VectorMA(v, p1->dist, u, p);
	VectorScale(p, 1.0f / denom, p);
	return qtrue;
}

static qboolean point_in_brush(const vec3_t point, const cbrush_t *brush)
{
	int i;

	for (i = 0; i < brush->numsides; i++)
	{
		const float d = DotProduct(point, brush->sides[i].plane->normal);

		// brushes with non-AA planes + AA bevel planes create too many intersections
		// EPSILON 1e-1 too big => 1e-3
		if (d - brush->sides[i].plane->dist > 1e-3f)
		{
			return qfalse;
		}
	}
	return qtrue;
}

// This function was initially supposed to obtain the ccw angle from w_ref_vec
// for ac and bc and compare them. However, we don't really need the exact angle.
// We just need to know which point lies further ccw relative to the ref.
// So a linear substitute is instead used to preserve the monotone decrease of acos.
static int winding_cmp(const void *a, const void *b)
{
	vec3_t ac, bc, n1, n2;

	float proj_ac, proj_bc;
	float a_diff, b_diff;

	VectorSubtract(((polyVert_t *)a)->xyz, w_center, ac);
	VectorSubtract(((polyVert_t *)b)->xyz, w_center, bc);

	proj_ac = DotProduct(ac, w_ref_vec) / VectorLength(ac);
	proj_bc = DotProduct(bc, w_ref_vec) / VectorLength(bc);

	a_diff = w_ref_vec_len - proj_ac;
	b_diff = w_ref_vec_len - proj_bc;

	CrossProduct(ac, w_ref_vec, n1);
	CrossProduct(bc, w_ref_vec, n2);

	if (DotProduct(n1, w_normal) < 0)
	{
		a_diff = 4.f * w_ref_vec_len - a_diff;
	}
	if (DotProduct(n2, w_normal) < 0)
	{
		b_diff = 4.f * w_ref_vec_len - b_diff;
	}

	if (a_diff < b_diff)
	{
		return -1;
	}
	if (a_diff > b_diff)
	{
		return 1;
	}

	return 0;
}

static void add_vert_to_face(visFace_t *face, const vec3_t vert, const vec4_t color, const vec2_t tex_coords)
{
	if (face->numVerts >= MAX_FACE_VERTS)
	{
		return;
	}

	VectorCopy(vert, face->verts[face->numVerts].xyz);
	face->verts[face->numVerts].modulate[0] = (byte) color[0];
	face->verts[face->numVerts].modulate[1] = (byte) color[1];
	face->verts[face->numVerts].modulate[2] = (byte) color[2];
	face->verts[face->numVerts].modulate[3] = (byte) color[3];
	face->verts[face->numVerts].st[0]       = tex_coords[0];
	face->verts[face->numVerts].st[1]       = tex_coords[1];
	face->numVerts++;
}

static float *get_uv_coords(vec2_t uv, const vec3_t vert, const vec3_t normal)
{
	const float x = fabsf(normal[0]), y = fabsf(normal[1]), z = fabsf(normal[2]);

	if (x >= y && x >= z)
	{
		uv[0] = -vert[1] / 32.f;
		uv[1] = -vert[2] / 32.f;
	}
	else if (y > x && y >= z)
	{
		uv[0] = -vert[0] / 32.f;
		uv[1] = -vert[2] / 32.f;
	}
	else
	{
		uv[0] = -vert[0] / 32.f;
		uv[1] = -vert[1] / 32.f;
	}

	return uv;
}

static void free_vis_brushes(visBrushNode_t *brushes)
{
	while (brushes != NULL)
	{
		visBrushNode_t *next = brushes->next;
		int            i;

		for (i = 0; i < brushes->numFaces; i++)
		{
			free(brushes->faces[i].verts);
		}
		free(brushes->faces);
		free(brushes);
		brushes = next;
	}
}

static void draw(visBrushNode_t *brush, qhandle_t shader)
{
	while (brush)
	{
		int i;

		for (i = 0; i < brush->numFaces; ++i)
		{
			if (!brush->faces[i].numVerts)
			{
				continue;
			}
			re.AddPolyToScene(shader, brush->faces[i].numVerts, brush->faces[i].verts);
		}
		brush = brush->next;
	}
}
