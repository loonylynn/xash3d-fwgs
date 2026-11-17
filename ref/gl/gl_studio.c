/*
gl_studio.c - studio model renderer
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "gl_local.h"
#include "xash3d_mathlib.h"
#include "const.h"
#include "r_studioint.h"
#include "triangleapi.h"
#include "studio.h"
#include "pm_local.h"
#include <math.h>

#define EVENT_CLIENT	5000	// less than this value it's a server-side studio events
#define MAX_LOCALLIGHTS	4

typedef struct
{
	char		name[MAX_OSPATH];
	char		modelname[MAX_OSPATH];
	model_t		*model;
} player_model_t;

// never gonna change, just shut up const warning
CVAR_DEFINE_AUTO( r_shadows, "0", 0, "draw ugly shadows" );

static const vec3_t hullcolor[8] =
{
{ 1.0f, 1.0f, 1.0f },
{ 1.0f, 0.5f, 0.5f },
{ 0.5f, 1.0f, 0.5f },
{ 1.0f, 1.0f, 0.5f },
{ 0.5f, 0.5f, 1.0f },
{ 1.0f, 0.5f, 1.0f },
{ 0.5f, 1.0f, 1.0f },
{ 1.0f, 1.0f, 1.0f },
};

typedef struct sortedmesh_s
{
	mstudiomesh_t	*mesh;
	int		flags;			// face flags
} sortedmesh_t;

typedef struct
{
	double		time;
	double		frametime;
	int		framecount;		// studio framecount
	qboolean		interpolate;
	int		rendermode;
	float		blend;			// blend value

	// bones
	matrix3x4		rotationmatrix;
	matrix3x4		bonestransform[MAXSTUDIOBONES];
	matrix3x4		lighttransform[MAXSTUDIOBONES];

	// boneweighting stuff
	matrix3x4		worldtransform[MAXSTUDIOBONES];

	// cached bones
	matrix3x4		cached_bonestransform[MAXSTUDIOBONES];
	matrix3x4		cached_lighttransform[MAXSTUDIOBONES];
	char		cached_bonenames[MAXSTUDIOBONES][32];
	int		cached_numbones;		// number of bones in cache

	sortedmesh_t	meshes[MAXSTUDIOMESHES];	// sorted meshes
	vec3_t		verts[MAXSTUDIOVERTS];
	vec3_t		norms[MAXSTUDIOVERTS];

	// lighting state
	float		ambientlight;
	float		shadelight;
	vec3_t		lightvec;			// averaging light direction
	vec3_t		lightspot;		// shadow spot
	vec3_t		lightcolor;		// averaging lightcolor
	vec3_t		blightvec[MAXSTUDIOBONES];	// bone light vecs
	vec3_t		lightvalues[MAXSTUDIOVERTS];	// precomputed lightvalues per each shared vertex of submodel

	// chrome stuff
	vec3_t		chrome_origin;
	vec2_t		chrome[MAXSTUDIOVERTS];	// texture coords for surface normals
	vec3_t		chromeright[MAXSTUDIOBONES];	// chrome vector "right" in bone reference frames
	vec3_t		chromeup[MAXSTUDIOBONES];	// chrome vector "up" in bone reference frames
	int		chromeage[MAXSTUDIOBONES];	// last time chrome vectors were updated

	// glowshell stuff
	int		normaltable[MAXSTUDIOVERTS];	// glowshell uses this

	// elights cache
	int		numlocallights;
	int		lightage[MAXSTUDIOBONES];
	dlight_t		*locallight[MAX_LOCALLIGHTS];
	int		locallightcolor[MAX_LOCALLIGHTS][3];
	vec4_t		lightpos[MAXSTUDIOVERTS][MAX_LOCALLIGHTS];
	vec3_t		lightbonepos[MAXSTUDIOBONES][MAX_LOCALLIGHTS];
	float		locallightR2[MAX_LOCALLIGHTS];

	// playermodels
	player_model_t  player_models[MAX_CLIENTS];

	// drawelements renderer
	vec3_t			arrayverts[MAXSTUDIOVERTS];
	vec2_t			arraycoord[MAXSTUDIOVERTS];
	unsigned short	arrayelems[MAXSTUDIOVERTS*6];
	GLubyte			arraycolor[MAXSTUDIOVERTS][4];
	uint			numverts;
	uint			numelems;
} studio_draw_state_t;

// studio-related cvars
CVAR_DEFINE_AUTO( r_studio_sort_textures, "0", FCVAR_GLCONFIG, "change draw order for additive meshes" );
CVAR_DEFINE_AUTO( r_studio_drawelements, "1", FCVAR_GLCONFIG, "use glDrawElements for studiomodels" );
CVAR_DEFINE_AUTO( r_studio_builtin_renderer, "0", 0, "use built-in studio model renderer instead of the one provided by client library (debugging)" );
CVAR_DEFINE_AUTO( r_studio_ambient_budget, "12", FCVAR_GLCONFIG, "maximum directional lightmap rays used by studio ambient sampling per frame" );
CVAR_DEFINE_AUTO( r_studio_ambient_far_dist, "2048", FCVAR_GLCONFIG, "distance beyond which studio ambient sampling uses one downward light sample" );
CVAR_DEFINE_AUTO( r_studio_ambient_static_interval, "2.0", FCVAR_GLCONFIG, "refresh interval for stationary studio ambient caches" );
CVAR_DEFINE_AUTO( r_studio_lighting_debug, "0", FCVAR_CHEAT, "draw studio ambient sample rays and print scheduler statistics" );
static cvar_t			*cl_righthand = NULL;

static r_studio_interface_t	*pStudioDraw;
static studio_draw_state_t	g_studio;		// global studio state

// global variables
static qboolean m_fDoRemap;
static mstudiomodel_t *m_pSubModel;
static mstudiobodyparts_t *m_pBodyPart;
static player_info_t *m_pPlayerInfo;
static studiohdr_t *m_pStudioHeader;
static float m_flGaitMovement;
static int g_nTopColor, g_nBottomColor;	// remap colors
static int g_nFaceFlags, g_nForceFaceFlags;

#define STUDIO_AMBIENT_CACHE_COUNT      (MAX_EDICTS + 1)
#define STUDIO_AMBIENT_VISIBLE_COUNT    (MAX_VISIBLE_PACKET * 2 + 1)
#define STUDIO_AMBIENT_TOPK_MAX         64
#define STUDIO_AMBIENT_AXES             6
#define STUDIO_AMBIENT_VALID            BIT( 0 )
#define STUDIO_AMBIENT_ANIMATED         BIT( 1 )
#define STUDIO_AMBIENT_STATIC            BIT( 2 )
#define STUDIO_AMBIENT_TELEPORT          BIT( 3 )

typedef struct cached_ambient_cube_s
{
	vec3_t cube[STUDIO_AMBIENT_AXES];
	vec3_t fallbackColor;
	vec3_t sampleOrigin;
	vec3_t entityOrigin;
	vec3_t lastSeenOrigin;
	vec3_t lastSeenAngles;
	float motionSpeed;
	float rayLength;
	float rayOffset;
	float boundsRadius;
	double sampleTime;
	double firstVisibleTime;
	double lastSeenTime;
	double lastMotionTime;
	double axisSampleTime[STUDIO_AMBIENT_AXES];
	model_t *model;
	uint generation;
	uint lastSeenFrame;
	int modelIndex;
	int nextAxis;
	uint validMask;
	uint animatedMask;
	uint flags;
	qboolean seenMotion;
	qboolean fallbackAnimated;
} cached_ambient_cube_t;

typedef struct studio_ambient_candidate_s
{
	cl_entity_t *entity;
	int cacheIndex;
	float distance;
	float radius;
	float speed;
	float score;
	qboolean farLod;
} studio_ambient_candidate_t;

typedef struct studio_ambient_selection_s
{
	int candidateIndex;
	float score;
} studio_ambient_selection_t;

static const vec3_t g_studioAmbientDirections[STUDIO_AMBIENT_AXES] =
{
	{ 1.0f, 0.0f, 0.0f },
	{ -1.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f },
	{ 0.0f, -1.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f },
	{ 0.0f, 0.0f, -1.0f }
};

static cached_ambient_cube_t g_studioAmbientCache[STUDIO_AMBIENT_CACHE_COUNT];
static studio_ambient_candidate_t g_studioAmbientVisible[STUDIO_AMBIENT_VISIBLE_COUNT];
static int g_studioAmbientVisibleCount;
static int g_studioAmbientTested;
static int g_studioAmbientCulled;
static int g_studioAmbientStaticSkipped;
static int g_studioAmbientFarLod;
static int g_studioAmbientTraces;
static int g_studioAmbientSampled;
static qboolean g_studioAmbientCollecting;
static double g_studioAmbientNow;

/*
 * Renderer-owned studio decals. The client submits only the original shot
 * ray and the Jolt contact. Projection is performed against g_studio.verts,
 * the exact skinned vertices used by this renderer, and the clipped polygons
 * are stored in barycentric form so they follow every subsequent pose.
 */
#define MAX_RAGDOLL_STUDIO_DECALS       96
#define MAX_RAGDOLL_STUDIO_FRAGMENTS    64
#define MAX_RAGDOLL_STUDIO_POLY_VERTS   12

typedef struct ragdoll_studio_decal_vertex_s
{
	float barycentric[3];
	float texcoord[2];
} ragdoll_studio_decal_vertex_t;

typedef struct ragdoll_studio_decal_fragment_s
{
	int bodypartIndex;
	int submodelIndex;
	int meshIndex;
	int vertexIndex[3];
	int numVerts;
	ragdoll_studio_decal_vertex_t verts[MAX_RAGDOLL_STUDIO_POLY_VERTS];
} ragdoll_studio_decal_fragment_t;

typedef struct ragdoll_studio_decal_s
{
	qboolean used;
	qboolean projected;
	int entityIndex;
	int ragdollID;
	int boneIndex;
	int bloodColor;
	uint age;
	model_t *model;
	float rayStart[3];
	float impactPosition[3];
	float anchorPosition[3];
	float radius;
	float normal[3];
	int fragmentCount;
	ragdoll_studio_decal_fragment_t fragments[MAX_RAGDOLL_STUDIO_FRAGMENTS];
} ragdoll_studio_decal_t;

static ragdoll_studio_decal_t g_RagdollStudioDecals[MAX_RAGDOLL_STUDIO_DECALS];
static uint g_RagdollStudioDecalClock;
static int m_nCurrentBodyPartIndex = -1;
static int m_nCurrentSubModelIndex = -1;
static int g_RagdollStudioDecalTexture;


/*
====================
R_StudioInit

====================
*/
void R_StudioInit( void )
{
	R_StudioAmbientReset();
	memset( g_RagdollStudioDecals, 0, sizeof( g_RagdollStudioDecals ));
	g_RagdollStudioDecalClock = 0;
	g_RagdollStudioDecalTexture = 0;

#if XASH_PSVITA
	// don't do the same array-building work twice since that's what our FFP shim does anyway
	gEngfuncs.Cvar_FullSet( "r_studio_drawelements", "0", FCVAR_READ_ONLY );
#endif

	Matrix3x4_LoadIdentity( g_studio.rotationmatrix );

	g_studio.interpolate = true;
	g_studio.framecount = 0;
	m_fDoRemap = false;
}

/*
================
R_StudioSetupTimings

init current time for a given model
================
*/
static void R_StudioSetupTimings( void )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		// synchronize with server time
		g_studio.time = gp_cl->time;
		g_studio.frametime = gp_cl->time - gp_cl->oldtime;
	}
	else
	{
		// menu stuff
		g_studio.time = gp_host->realtime;
		g_studio.frametime = gp_host->frametime;
	}
}

/*
================
R_AllowFlipViewModel

should a flip the viewmodel if cl_righthand is set to 1
================
*/
static qboolean R_AllowFlipViewModel( cl_entity_t *e )
{
	if( cl_righthand && cl_righthand->value > 0 )
	{
		if( e == tr.viewent )
			return true;
	}

	return false;
}

/*
================
R_StudioComputeBBox

Compute a full bounding box for current sequence
================
*/
static qboolean R_StudioComputeBBox( vec3_t bbox[8] )
{
	cl_entity_t	*e = RI.currententity;

	if( !m_pStudioHeader )
		return false;

	vec3_t mins, maxs;
	// check if we have valid mins\maxs
	if( !VectorIsNull( RI.currentmodel->mins ) && !VectorIsNull( RI.currentmodel->maxs ))
	{
		// clipping bounding box
		VectorCopy( RI.currentmodel->mins, mins );
		VectorCopy( RI.currentmodel->maxs, maxs );
	}
	else
	{
		ClearBounds( mins, maxs );
	}

	// check sequence range
	if( e->curstate.sequence < 0 || e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;

	// add sequence box to the model box
	AddPointToBounds( pseqdesc->bbmin, mins, maxs );
	AddPointToBounds( pseqdesc->bbmax, mins, maxs );
	vec3_t studio_mins, studio_maxs;
	ClearBounds( studio_mins, studio_maxs );

	// compute a full bounding box
	for( int i = 0; i < 8; i++ )
	{
		vec3_t p1, p2;
  		p1[0] = ( i & 1 ) ? mins[0] : maxs[0];
  		p1[1] = ( i & 2 ) ? mins[1] : maxs[1];
  		p1[2] = ( i & 4 ) ? mins[2] : maxs[2];

		Matrix3x4_VectorTransform( g_studio.rotationmatrix, p1, p2 );
		AddPointToBounds( p2, studio_mins, studio_maxs );
		if( bbox ) VectorCopy( p2, bbox[i] );
	}

	if( !bbox && R_CullModel( e, studio_mins, studio_maxs ))
		return false; // model culled
	return true; // visible
}

static float R_StudioAmbientClamp( float value, float minimum, float maximum )
{
	if( value < minimum ) return minimum;
	if( value > maximum ) return maximum;
	return value;
}

static float R_StudioAmbientAngleDelta( float a, float b )
{
	float delta = fabsf( a - b );
	if( delta > 180.0f ) delta = 360.0f - delta;
	return delta;
}

static void R_StudioAmbientToLinear( const colorVec color, vec3_t out )
{
	const float inverse255 = 1.0f / 255.0f;
	float value;

	value = color.r * inverse255;
	out[0] = Q_max( value * value, 0.001f );
	value = color.g * inverse255;
	out[1] = Q_max( value * value, 0.001f );
	value = color.b * inverse255;
	out[2] = Q_max( value * value, 0.001f );
}

static void R_StudioAmbientPointCube( cached_ambient_cube_t *cache, const vec3_t origin, double now, qboolean snap )
{
	qboolean animated = false;
	colorVec point = R_LightPointEx( origin, &animated );
	vec3_t target;
	R_StudioAmbientToLinear( point, target );

	VectorCopy( target, cache->fallbackColor );
	cache->fallbackAnimated = animated ? true : false;
	cache->validMask = 0;
	cache->animatedMask = animated ? ((1U << STUDIO_AMBIENT_AXES) - 1U) : 0;
	for( int axis = 0; axis < STUDIO_AMBIENT_AXES; axis++ )
	{
		if( snap || !( cache->flags & STUDIO_AMBIENT_VALID ))
			VectorCopy( target, cache->cube[axis] );
		else
		{
			float dt = (float)( now - cache->axisSampleTime[axis] );
			float blend = 1.0f - expf( -8.0f * R_StudioAmbientClamp( dt, 0.0f, 1.0f ));
			vec3_t result;
			VectorLerp( cache->cube[axis], blend, target, result );
			VectorCopy( result, cache->cube[axis] );
		}
		cache->axisSampleTime[axis] = now;
	}
	cache->flags |= STUDIO_AMBIENT_VALID;
	if( cache->animatedMask )
		cache->flags |= STUDIO_AMBIENT_ANIMATED;
	else
		cache->flags &= ~STUDIO_AMBIENT_ANIMATED;
	cache->sampleTime = now;
}

static int R_StudioAmbientCacheIndex( const cl_entity_t *entity )
{
	if( !entity )
		return -1;
	if( entity == tr.viewent )
		return MAX_EDICTS;
	if( entity->index < 0 || entity->index >= MAX_EDICTS )
		return -1;
	return entity->index;
}

static void R_StudioAmbientEntityBounds( const cl_entity_t *entity, vec3_t mins, vec3_t maxs, float *radius )
{
	VectorCopy( entity->curstate.mins, mins );
	VectorCopy( entity->curstate.maxs, maxs );
	if( VectorIsNull( mins ) && VectorIsNull( maxs ) && entity->model )
	{
		VectorCopy( entity->model->mins, mins );
		VectorCopy( entity->model->maxs, maxs );
	}
	vec3_t size;
	VectorSubtract( maxs, mins, size );
	*radius = Q_max( VectorLength( size ) * 0.5f, 1.0f );
}

static void R_StudioAmbientCollect( cl_entity_t *entity )
{
	if( !g_studioAmbientCollecting || !entity || g_studioAmbientVisibleCount >= STUDIO_AMBIENT_VISIBLE_COUNT )
		return;

	const int index = R_StudioAmbientCacheIndex( entity );
	if( index < 0 )
		return;

	cached_ambient_cube_t *cache = &g_studioAmbientCache[index];
	if( cache->lastSeenFrame == (uint)tr.framecount )
		return;

	vec3_t entityOrigin, sampleOrigin, mins, maxs, centerOffset;
	float radius;
	R_StudioAmbientEntityBounds( entity, mins, maxs, &radius );
	VectorCopy( entity->origin, entityOrigin );
	VectorAdd( mins, maxs, centerOffset );
	VectorScale( centerOffset, 0.5f, centerOffset );
	VectorAdd( entityOrigin, centerOffset, sampleOrigin );

	// The viewmodel is sampled from the camera, not its weapon-model origin.
	if( entity == tr.viewent )
	{
		VectorCopy( RI.rvp.vieworigin, entityOrigin );
		VectorCopy( RI.rvp.vieworigin, sampleOrigin );
	}

	const int modelIndex = entity->curstate.modelindex;
	if( cache->model != entity->model ||
		cache->modelIndex != modelIndex ||
		cache->generation != entity->renderGeneration )
	{
		memset( cache, 0, sizeof( *cache ));
		cache->model = entity->model;
		cache->modelIndex = modelIndex;
		cache->generation = entity->renderGeneration;
	}

	float speed = 0.0f;
	if( cache->seenMotion )
	{
		vec3_t movement;
		VectorSubtract( entityOrigin, cache->lastSeenOrigin, movement );
		float moved = VectorLength( movement );
		float dt = (float)( g_studioAmbientNow - cache->lastSeenTime );
		dt = Q_max( dt, 0.001f );
		float angleDelta = Q_max(
			Q_max( R_StudioAmbientAngleDelta( entity->angles[0], cache->lastSeenAngles[0] ),
				R_StudioAmbientAngleDelta( entity->angles[1], cache->lastSeenAngles[1] )),
			R_StudioAmbientAngleDelta( entity->angles[2], cache->lastSeenAngles[2] ));
		float moveThreshold = Q_max( radius * 0.001f, 0.05f );

		if( moved > moveThreshold || angleDelta > 0.1f )
		{
			cache->motionSpeed = moved / dt;
			cache->lastMotionTime = g_studioAmbientNow;
			cache->flags &= ~STUDIO_AMBIENT_STATIC;
			if( moved > radius * 4.0f + 1.0f )
				cache->flags |= STUDIO_AMBIENT_TELEPORT;
		}
		else
		{
			cache->motionSpeed *= expf( -4.0f * R_StudioAmbientClamp( dt, 0.0f, 1.0f ));
			if( g_studioAmbientNow - cache->lastMotionTime > 0.25 )
				cache->flags |= STUDIO_AMBIENT_STATIC;
		}
	}
	else
	{
		cache->firstVisibleTime = g_studioAmbientNow;
		cache->lastMotionTime = g_studioAmbientNow;
		cache->seenMotion = true;
		cache->flags &= ~STUDIO_AMBIENT_STATIC;
	}
	speed = cache->motionSpeed;
	VectorCopy( entityOrigin, cache->lastSeenOrigin );
	VectorCopy( entity->angles, cache->lastSeenAngles );
	cache->lastSeenTime = g_studioAmbientNow;
	cache->lastSeenFrame = tr.framecount;

	cache->boundsRadius = radius;
	cache->rayOffset = Q_max( radius * 0.08f, 0.1f );
	cache->rayLength = Q_max( radius * 6.0f, cache->rayOffset * 2.0f );
	VectorCopy( entityOrigin, cache->entityOrigin );
	VectorCopy( sampleOrigin, cache->sampleOrigin );

	vec3_t toViewer;
	VectorSubtract( sampleOrigin, RI.rvp.vieworigin, toViewer );
	float distance = VectorLength( toViewer );
	float farDistance = r_studio_ambient_far_dist.value;
	qboolean farLod = farDistance > 0.0f && distance > farDistance;
	if( farLod )
		g_studioAmbientFarLod++;

	studio_ambient_candidate_t *candidate = &g_studioAmbientVisible[g_studioAmbientVisibleCount++];
	candidate->entity = entity;
	candidate->cacheIndex = index;
	candidate->distance = distance;
	candidate->radius = radius;
	candidate->speed = Q_max( speed, VectorLength( entity->curstate.velocity ));
	candidate->farLod = farLod;

	float age = ( cache->flags & STUDIO_AMBIENT_VALID ) ?
		(float)( g_studioAmbientNow - cache->sampleTime ) :
		(float)( g_studioAmbientNow - cache->firstVisibleTime ) + 8.0f;
	float screenFactor = radius / ( distance + radius + 1.0f );
	float velocityFactor = R_StudioAmbientClamp( candidate->speed / 200.0f, 0.0f, 3.0f );
	// The age multiplier has a nonzero floor, so bounded screen/velocity
	// factors can never permanently starve a visible cache entry.
	candidate->score = age * ( 0.25f + screenFactor * 4.0f ) * ( 1.0f + velocityFactor );
	if( farLod )
		candidate->score *= 0.15f;
}

static int R_StudioAmbientTraceAxis( cached_ambient_cube_t *cache, int axis, vec3_t color, qboolean *animated )
{
	vec3_t start;
	VectorMA( cache->sampleOrigin, cache->rayOffset, g_studioAmbientDirections[axis], start );
	if( animated )
		*animated = false;

	if( tr.worldmodel && tr.worldmodel->nodes )
	{
		mleaf_t *leaf = gEngfuncs.Mod_PointInLeaf( start, tr.worldmodel->nodes, tr.worldmodel );
		if( leaf && leaf->contents == CONTENTS_SOLID )
		{
			vec3_t towardOrigin;
			VectorSubtract( cache->entityOrigin, start, towardOrigin );
			float length = VectorNormalizeLength2( towardOrigin, towardOrigin );
			if( length > 0.0f )
			{
				VectorMA( start, Q_min( length, cache->boundsRadius * 0.5f ), towardOrigin, start );
				leaf = gEngfuncs.Mod_PointInLeaf( start, tr.worldmodel->nodes, tr.worldmodel );
			}
			if( leaf && leaf->contents == CONTENTS_SOLID )
			{
				qboolean fallbackAnimated = false;
				colorVec point = R_LightPointEx( cache->sampleOrigin, &fallbackAnimated );
				R_StudioAmbientToLinear( point, color );
				if( animated ) *animated = fallbackAnimated;
				return 2; // point fallback, not a valid directional ray
			}
		}
	}

	colorVec sample;
	if( R_LightSampleDir( start, g_studioAmbientDirections[axis], cache->rayLength, &sample, animated ))
	{
		R_StudioAmbientToLinear( sample, color );
		return 1;
	}
	return 0;
}

static void R_StudioAmbientAverageValid( const cached_ambient_cube_t *cache, int excludedAxis, vec3_t average )
{
	VectorClear( average );
	int count = 0;
	for( int axis = 0; axis < STUDIO_AMBIENT_AXES; axis++ )
	{
		if( axis == excludedAxis || !( cache->validMask & BIT( axis )))
			continue;
		VectorAdd( average, cache->cube[axis], average );
		count++;
	}
	if( count )
		VectorScale( average, 1.0f / count, average );
	else
		VectorCopy( cache->fallbackColor, average );
}

static void R_StudioAmbientSampleAxis( cached_ambient_cube_t *cache, int axis, double now, qboolean snap )
{
	vec3_t fresh, target, result;
	qboolean animated = false;
	int status = R_StudioAmbientTraceAxis( cache, axis, fresh, &animated );

	if( status == 2 )
	{
		VectorCopy( fresh, cache->fallbackColor );
		cache->fallbackAnimated = animated;
	}

	if( status == 1 )
	{
		cache->validMask |= BIT( axis );
		VectorCopy( fresh, target );
		if( animated )
			cache->animatedMask |= BIT( axis );
		else
			cache->animatedMask &= ~BIT( axis );
	}
	else
	{
		cache->validMask &= ~BIT( axis );
		R_StudioAmbientAverageValid( cache, axis, target );
		if( cache->fallbackAnimated && !( cache->validMask & BIT( axis )))
			cache->animatedMask |= BIT( axis );
		else
			cache->animatedMask &= ~BIT( axis );
	}

	float dt = R_StudioAmbientClamp( (float)( now - cache->axisSampleTime[axis] ), 0.0f, 1.0f );
	float blend = snap ? 1.0f : 1.0f - expf( -8.0f * dt );
	VectorLerp( cache->cube[axis], blend, target, result );
	VectorCopy( result, cache->cube[axis] );
	cache->axisSampleTime[axis] = now;
	cache->sampleTime = now;
	if( cache->animatedMask )
		cache->flags |= STUDIO_AMBIENT_ANIMATED;
	else
		cache->flags &= ~STUDIO_AMBIENT_ANIMATED;
}

static void R_StudioAmbientSampleAll( cached_ambient_cube_t *cache, double now, qboolean snap )
{
	vec3_t fresh[STUDIO_AMBIENT_AXES];
	int status[STUDIO_AMBIENT_AXES];
	uint validMask = 0;
	uint animatedMask = 0;

	for( int axis = 0; axis < STUDIO_AMBIENT_AXES; axis++ )
	{
		qboolean animated = false;
		status[axis] = R_StudioAmbientTraceAxis( cache, axis, fresh[axis], &animated );
		if( status[axis] == 1 )
		{
			validMask |= BIT( axis );
			if( animated ) animatedMask |= BIT( axis );
		}
		else if( status[axis] == 2 )
		{
			VectorCopy( fresh[axis], cache->fallbackColor );
			cache->fallbackAnimated = animated;
		}
	}

	vec3_t average = { 0.0f, 0.0f, 0.0f };
	int validCount = 0;
	for( int axis = 0; axis < STUDIO_AMBIENT_AXES; axis++ )
	{
		if( validMask & BIT( axis ))
		{
			VectorAdd( average, fresh[axis], average );
			validCount++;
		}
	}
	if( validCount )
		VectorScale( average, 1.0f / validCount, average );
	else
		VectorCopy( cache->fallbackColor, average );

	for( int axis = 0; axis < STUDIO_AMBIENT_AXES; axis++ )
	{
		vec3_t target, result;
		if( validMask & BIT( axis ))
			VectorCopy( fresh[axis], target );
		else
		{
			VectorCopy( average, target );
			if( cache->fallbackAnimated )
				animatedMask |= BIT( axis );
		}
		float dt = R_StudioAmbientClamp( (float)( now - cache->axisSampleTime[axis] ), 0.0f, 1.0f );
		float blend = snap ? 1.0f : 1.0f - expf( -8.0f * dt );
		VectorLerp( cache->cube[axis], blend, target, result );
		VectorCopy( result, cache->cube[axis] );
		cache->axisSampleTime[axis] = now;
	}
	cache->validMask = validMask;
	cache->animatedMask = animatedMask;
	cache->sampleTime = now;
	cache->flags |= STUDIO_AMBIENT_VALID;
	if( animatedMask )
		cache->flags |= STUDIO_AMBIENT_ANIMATED;
	else
		cache->flags &= ~STUDIO_AMBIENT_ANIMATED;
}

void R_StudioAmbientReset( void )
{
	memset( g_studioAmbientCache, 0, sizeof( g_studioAmbientCache ));
	g_studioAmbientVisibleCount = 0;
	g_studioAmbientTested = 0;
	g_studioAmbientCulled = 0;
	g_studioAmbientStaticSkipped = 0;
	g_studioAmbientFarLod = 0;
	g_studioAmbientTraces = 0;
	g_studioAmbientSampled = 0;
	g_studioAmbientCollecting = false;
	g_studioAmbientNow = 0.0;
}

void R_StudioAmbientBeginFrame( void )
{
	g_studioAmbientVisibleCount = 0;
	g_studioAmbientTested = 0;
	g_studioAmbientCulled = 0;
	g_studioAmbientStaticSkipped = 0;
	g_studioAmbientFarLod = 0;
	g_studioAmbientTraces = 0;
	g_studioAmbientSampled = 0;
	g_studioAmbientNow = FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) ? gp_cl->time : gp_host->realtime;
	g_studioAmbientCollecting = true;
}

static void R_StudioAmbientDrawDebug( void )
{
	if( !r_studio_lighting_debug.value || !g_studioAmbientVisibleCount )
		return;

	TriBegin( TRI_LINES );
	for( int i = 0; i < g_studioAmbientVisibleCount; i++ )
	{
		const studio_ambient_candidate_t *candidate = &g_studioAmbientVisible[i];
		const cached_ambient_cube_t *cache = &g_studioAmbientCache[candidate->cacheIndex];
		if( !( cache->flags & STUDIO_AMBIENT_VALID ))
			continue;
		for( int axis = 0; axis < STUDIO_AMBIENT_AXES; axis++ )
		{
			vec3_t start, end;
			VectorMA( cache->sampleOrigin, cache->rayOffset, g_studioAmbientDirections[axis], start );
			VectorMA( cache->sampleOrigin, cache->rayLength, g_studioAmbientDirections[axis], end );
			TriColor4f(
				sqrtf( cache->cube[axis][0] ),
				sqrtf( cache->cube[axis][1] ),
				sqrtf( cache->cube[axis][2] ),
				1.0f
			);
			TriVertex3fv( start );
			TriVertex3fv( end );
		}
	}
	TriEnd();
}

void R_StudioAmbientUpdate( void )
{
	g_studioAmbientCollecting = false;
	int budget = (int)R_StudioAmbientClamp( r_studio_ambient_budget.value, 0.0f, 64.0f );
	float staticInterval = Q_max( r_studio_ambient_static_interval.value, 0.1f );
	float staleThreshold = Q_max( 2.0f, staticInterval * 3.0f );
	studio_ambient_selection_t selected[STUDIO_AMBIENT_TOPK_MAX];
	int selectedCount = 0;
	int topK = Q_min( budget, STUDIO_AMBIENT_TOPK_MAX );

	for( int i = 0; i < g_studioAmbientVisibleCount; i++ )
	{
		studio_ambient_candidate_t *candidate = &g_studioAmbientVisible[i];
		cached_ambient_cube_t *cache = &g_studioAmbientCache[candidate->cacheIndex];
		float age = ( cache->flags & STUDIO_AMBIENT_VALID ) ?
			(float)( g_studioAmbientNow - cache->sampleTime ) :
			(float)( g_studioAmbientNow - cache->firstVisibleTime ) + 8.0f;

		if(( cache->flags & STUDIO_AMBIENT_VALID ) &&
			( cache->flags & STUDIO_AMBIENT_STATIC ) &&
			!( cache->flags & STUDIO_AMBIENT_ANIMATED ) &&
			age < staticInterval )
		{
			g_studioAmbientStaticSkipped++;
			continue;
		}
		if( candidate->farLod && ( cache->flags & STUDIO_AMBIENT_VALID ) &&
			age < Q_max( 1.0f, staticInterval ))
			continue;
		if( topK <= 0 )
			continue;

		int insertAt = selectedCount;
		if( insertAt >= topK )
		{
			if( candidate->score <= selected[topK - 1].score )
				continue;
			insertAt = topK - 1;
		}
		else selectedCount++;

		while( insertAt > 0 && selected[insertAt - 1].score < candidate->score )
		{
			if( insertAt < topK )
				selected[insertAt] = selected[insertAt - 1];
			insertAt--;
		}
		selected[insertAt].candidateIndex = i;
		selected[insertAt].score = candidate->score;
	}

	int fullFills = 0;
	for( int i = 0; i < selectedCount && budget > 0; i++ )
	{
		studio_ambient_candidate_t *candidate =
			&g_studioAmbientVisible[selected[i].candidateIndex];
		cached_ambient_cube_t *cache = &g_studioAmbientCache[candidate->cacheIndex];
		float age = ( cache->flags & STUDIO_AMBIENT_VALID ) ?
			(float)( g_studioAmbientNow - cache->sampleTime ) :
			(float)( g_studioAmbientNow - cache->firstVisibleTime ) + 8.0f;
		qboolean snap = !( cache->flags & STUDIO_AMBIENT_VALID ) ||
			( cache->flags & STUDIO_AMBIENT_TELEPORT ) || age > staleThreshold;

		if( candidate->farLod )
		{
			if( snap )
			{
				if( budget < 1 ) continue;
				R_StudioAmbientPointCube( cache, cache->sampleOrigin, g_studioAmbientNow, true );
				budget--;
				g_studioAmbientTraces++;
				g_studioAmbientSampled++;
				cache->flags &= ~STUDIO_AMBIENT_TELEPORT;
			}
			else if( age >= Q_max( 1.0f, staticInterval ))
			{
				if( budget < 1 ) continue;
				R_StudioAmbientPointCube( cache, cache->sampleOrigin, g_studioAmbientNow, false );
				budget--;
				g_studioAmbientTraces++;
				g_studioAmbientSampled++;
			}
			continue;
		}

		if( snap )
		{
			if( budget < 1 ) continue;
			R_StudioAmbientPointCube( cache, cache->sampleOrigin, g_studioAmbientNow, true );
			budget--;
			g_studioAmbientTraces++;
			if( budget >= 6 && fullFills < 2 )
			{
				R_StudioAmbientSampleAll( cache, g_studioAmbientNow, true );
				budget -= 6;
				g_studioAmbientTraces += 6;
				fullFills++;
			}
			g_studioAmbientSampled++;
			cache->flags &= ~STUDIO_AMBIENT_TELEPORT;
			continue;
		}

		if(( cache->flags & STUDIO_AMBIENT_STATIC ) &&
			age >= staticInterval && budget >= 6 && fullFills < 2 )
		{
			R_StudioAmbientSampleAll( cache, g_studioAmbientNow, false );
			budget -= 6;
			g_studioAmbientTraces += 6;
			fullFills++;
			g_studioAmbientSampled++;
			continue;
		}

		int axis = cache->nextAxis;
		R_StudioAmbientSampleAxis( cache, axis, g_studioAmbientNow, false );
		cache->nextAxis = ( axis + 1 ) % STUDIO_AMBIENT_AXES;
		budget--;
		g_studioAmbientTraces++;
		g_studioAmbientSampled++;
	}

	g_studioAmbientCulled = Q_max( g_studioAmbientTested - g_studioAmbientVisibleCount, 0 );
	if( r_studio_lighting_debug.value )
	{
		gEngfuncs.Con_Printf(
			"[studio ambient] traces %d/%d, visible %d, culled %d, static-skipped %d, far-LOD %d, sampled %d\n",
			g_studioAmbientTraces,
			(int)r_studio_ambient_budget.value,
			g_studioAmbientVisibleCount,
			g_studioAmbientCulled,
			g_studioAmbientStaticSkipped,
			g_studioAmbientFarLod,
			g_studioAmbientSampled
		);
		R_StudioAmbientDrawDebug();
	}
}

static void R_StudioComputeSkinMatrix( mstudioboneweight_t *boneweights, matrix3x4 result )
{
	int	numbones = 0;

	for( int i = 0; i < MAXSTUDIOBONEWEIGHTS; i++ )
	{
		if( boneweights->bone[i] != -1 )
			numbones++;
	}

	if( numbones == 4 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		vec4_t *boneMat3 = (vec4_t *)g_studio.worldtransform[boneweights->bone[3]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flWeight2 = boneweights->weight[2] / 255.0f;
		float flWeight3 = boneweights->weight[3] / 255.0f;
		float flTotal = flWeight0 + flWeight1 + flWeight2 + flWeight3;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;	// compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2 + boneMat3[0][0] * flWeight3;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2 + boneMat3[0][1] * flWeight3;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2 + boneMat3[0][2] * flWeight3;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2 + boneMat3[0][3] * flWeight3;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2 + boneMat3[1][0] * flWeight3;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2 + boneMat3[1][1] * flWeight3;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2 + boneMat3[1][2] * flWeight3;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2 + boneMat3[1][3] * flWeight3;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2 + boneMat3[2][0] * flWeight3;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2 + boneMat3[2][1] * flWeight3;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2 + boneMat3[2][2] * flWeight3;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2 + boneMat3[2][3] * flWeight3;
	}
	else if( numbones == 3 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		vec4_t *boneMat2 = (vec4_t *)g_studio.worldtransform[boneweights->bone[2]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flWeight2 = boneweights->weight[2] / 255.0f;
		float flTotal = flWeight0 + flWeight1 + flWeight2;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;	// compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1 + boneMat2[0][0] * flWeight2;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1 + boneMat2[0][1] * flWeight2;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1 + boneMat2[0][2] * flWeight2;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1 + boneMat2[0][3] * flWeight2;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1 + boneMat2[1][0] * flWeight2;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1 + boneMat2[1][1] * flWeight2;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1 + boneMat2[1][2] * flWeight2;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1 + boneMat2[1][3] * flWeight2;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1 + boneMat2[2][0] * flWeight2;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1 + boneMat2[2][1] * flWeight2;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1 + boneMat2[2][2] * flWeight2;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1 + boneMat2[2][3] * flWeight2;
	}
	else if( numbones == 2 )
	{
		vec4_t *boneMat0 = (vec4_t *)g_studio.worldtransform[boneweights->bone[0]];
		vec4_t *boneMat1 = (vec4_t *)g_studio.worldtransform[boneweights->bone[1]];
		float flWeight0 = boneweights->weight[0] / 255.0f;
		float flWeight1 = boneweights->weight[1] / 255.0f;
		float flTotal = flWeight0 + flWeight1;

		if( flTotal < 1.0f ) flWeight0 += 1.0f - flTotal;	// compensate rounding error

		result[0][0] = boneMat0[0][0] * flWeight0 + boneMat1[0][0] * flWeight1;
		result[0][1] = boneMat0[0][1] * flWeight0 + boneMat1[0][1] * flWeight1;
		result[0][2] = boneMat0[0][2] * flWeight0 + boneMat1[0][2] * flWeight1;
		result[0][3] = boneMat0[0][3] * flWeight0 + boneMat1[0][3] * flWeight1;
		result[1][0] = boneMat0[1][0] * flWeight0 + boneMat1[1][0] * flWeight1;
		result[1][1] = boneMat0[1][1] * flWeight0 + boneMat1[1][1] * flWeight1;
		result[1][2] = boneMat0[1][2] * flWeight0 + boneMat1[1][2] * flWeight1;
		result[1][3] = boneMat0[1][3] * flWeight0 + boneMat1[1][3] * flWeight1;
		result[2][0] = boneMat0[2][0] * flWeight0 + boneMat1[2][0] * flWeight1;
		result[2][1] = boneMat0[2][1] * flWeight0 + boneMat1[2][1] * flWeight1;
		result[2][2] = boneMat0[2][2] * flWeight0 + boneMat1[2][2] * flWeight1;
		result[2][3] = boneMat0[2][3] * flWeight0 + boneMat1[2][3] * flWeight1;
	}
	else
	{
		Matrix3x4_Copy( result, g_studio.worldtransform[boneweights->bone[0]] );
	}
}

/*
===============
pfnGetCurrentEntity

===============
*/
static cl_entity_t *pfnGetCurrentEntity( void )
{
	return RI.currententity;
}

/*
===============
pfnPlayerInfo

===============
*/
player_info_t *pfnPlayerInfo( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		index = -1;

	return gEngfuncs.pfnPlayerInfo( index );
}

/*
===============
pfnGetPlayerState

===============
*/
static entity_state_t *R_StudioGetPlayerState( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) && RI.currententity )
		return &RI.currententity->curstate;

	return gEngfuncs.pfnGetPlayerState( index );
}

/*
===============
pfnGetEngineTimes

===============
*/
static void pfnGetEngineTimes( int *framecount, double *current, double *old )
{
	if( framecount ) *framecount = tr.realframecount;
	if( current ) *current = gp_cl->time;
	if( old ) *old =   gp_cl->oldtime;
}

/*
===============
pfnGetViewInfo

===============
*/
static void pfnGetViewInfo( float *origin, float *upv, float *rightv, float *forwardv )
{
	if( origin ) VectorCopy( RI.rvp.vieworigin, origin );
	if( forwardv ) VectorCopy( RI.vforward, forwardv );
	if( rightv ) VectorCopy( RI.vright, rightv );
	if( upv ) VectorCopy( RI.vup, upv );
}

/*
===============
pfnGetModelCounters

===============
*/
static void pfnGetModelCounters( int **s, int **a )
{
	*s = &g_studio.framecount;
	*a = &r_stats.c_studio_models_drawn;
}

/*
===============
pfnStudioGetBoneTransform

===============
*/
static float ****pfnStudioGetBoneTransform( void )
{
	return (float ****)g_studio.bonestransform;
}

/*
===============
pfnStudioGetLightTransform

===============
*/
static float ****pfnStudioGetLightTransform( void )
{
	return (float ****)g_studio.lighttransform;
}

/*
===============
pfnStudioGetRotationMatrix

===============
*/
static float ***pfnStudioGetRotationMatrix( void )
{
	return (float ***)g_studio.rotationmatrix;
}

/*
====================
StudioPlayerBlend

====================
*/
static void R_StudioPlayerBlend( mstudioseqdesc_t *pseqdesc, int *pBlend, float *pPitch )
{
	// calc up/down pointing
	*pBlend = (*pPitch * 3.0f);

	if( *pBlend < pseqdesc->blendstart[0] )
	{
		*pPitch -= pseqdesc->blendstart[0] / 3.0f;
		*pBlend = 0;
	}
	else if( *pBlend > pseqdesc->blendend[0] )
	{
		*pPitch -= pseqdesc->blendend[0] / 3.0f;
		*pBlend = 255;
	}
	else
	{
		if( pseqdesc->blendend[0] - pseqdesc->blendstart[0] < 0.1f ) // catch qc error
			*pBlend = 127;
		else *pBlend = 255 * (*pBlend - pseqdesc->blendstart[0]) / (pseqdesc->blendend[0] - pseqdesc->blendstart[0]);
		*pPitch = 0.0f;
	}
}

/*
====================
R_StudioLerpMovement

====================
*/
void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	float	f = 1.0f;

	// don't do it if the goalstarttime hasn't updated in a while.
	// NOTE: Because we need to interpolate multiplayer characters, the interpolation time limit
	// was increased to 1.0 s., which is 2x the max lag we are accounting for.
	if( g_studio.interpolate && ( time < e->curstate.animtime + 1.0f ) && ( e->curstate.animtime != e->latched.prevanimtime ))
		f = ( time - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime );

	// Con_Printf( "%4.2f %.2f %.2f\n", f, e->curstate.animtime, g_studio.time );
	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ))
	{
		vec4_t	q, q1, q2;
		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, angles );
	}
	else VectorCopy( e->curstate.angles, angles );
}

/*
====================
StudioSetUpTransform

====================
*/
static void R_StudioSetUpTransform( cl_entity_t *e )
{
	vec3_t origin = Vec3( e->origin );
	vec3_t angles = Vec3( e->angles );

	// interpolate monsters position (moved into UpdateEntityFields by user request)
	if( e->curstate.movetype == MOVETYPE_STEP && !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ))
	{
		R_StudioLerpMovement( e, g_studio.time, origin, angles );
	}

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ))
		angles[PITCH] = -angles[PITCH]; // stupid quake bug

	// don't rotate clients, only aim
	if( e->player ) angles[PITCH] = 0.0f;

	Matrix3x4_CreateFromEntity( g_studio.rotationmatrix, angles, origin, 1.0f );

	if( tr.fFlipViewModel )
	{
		g_studio.rotationmatrix[0][1] = -g_studio.rotationmatrix[0][1];
		g_studio.rotationmatrix[1][1] = -g_studio.rotationmatrix[1][1];
		g_studio.rotationmatrix[2][1] = -g_studio.rotationmatrix[2][1];
	}
}

/*
====================
StudioEstimateFrame

====================
*/
float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	double	dfdt, f;

	if( g_studio.interpolate )
	{
		if( time < e->curstate.animtime ) dfdt = 0.0;
		else dfdt = (time - e->curstate.animtime) * e->curstate.framerate * pseqdesc->fps;
	}
	else dfdt = 0;

	if( pseqdesc->numframes <= 1 ) f = 0.0;
	else f = (e->curstate.frame * (pseqdesc->numframes - 1)) / 256.0f;

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)(f / (pseqdesc->numframes - 1)) *  (pseqdesc->numframes - 1);
		if( f < 0 ) f += (pseqdesc->numframes - 1);
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001 )
			f = pseqdesc->numframes - 1.001;
		if( f < 0.0 )  f = 0.0;
	}
	return f;
}

/*
====================
StudioEstimateInterpolant

====================
*/
static float R_StudioEstimateInterpolant( cl_entity_t *e )
{
	float	dadt = 1.0f;

	if( g_studio.interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ))
	{
		dadt = ( g_studio.time - e->curstate.animtime ) / 0.1f;
		if( dadt > 2.0f ) dadt = 2.0f;
	}

	return dadt;
}

/*
====================
StudioFxTransform

====================
*/
static void R_StudioFxTransform( cl_entity_t *ent, matrix3x4 transform )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int	axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 ) axis = 2; // choose between x & z
			VectorScale( transform[axis], gEngfuncs.COM_RandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( !gEngfuncs.COM_RandomLong( 0, 49 ))
		{
			int	axis = gEngfuncs.COM_RandomLong( 0, 1 );

			if( axis == 1 ) axis = 2; // choose between x & z
			float offset = gEngfuncs.COM_RandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.COM_RandomLong( 0, 2 )][3] += offset;
		}
		break;
	case kRenderFxExplode:
		{
			float scale = 1.0f + ( g_studio.time - ent->curstate.animtime ) * 10.0f;
			if( scale > 2.0f ) scale = 2.0f; // don't blow up more than 200%

			transform[0][1] *= scale;
			transform[1][1] *= scale;
			transform[2][1] *= scale;
		}
		break;
	}
}

/*
====================
StudioCalcBoneAdj

====================
*/
static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	mstudiobonecontroller_t	*pbonecontroller = (mstudiobonecontroller_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bonecontrollerindex);

	for( int j = 0; j < m_pStudioHeader->numbonecontrollers; j++ )
	{
		float value = 0.0f;
		int i = pbonecontroller[j].index;

		if( i == STUDIO_MOUTH )
		{
			// mouth hardcoded at controller 4
			value = (float)mouthopen / 64.0f;
			value = bound( 0.0f, value, 1.0f );
			value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
		}
		else if( i < 4 )
		{
			// check for 360% wrapping
			if( FBitSet( pbonecontroller[j].type, STUDIO_RLOOP ))
			{
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128 )
				{
					int a = (pcontroller1[i] + 128) % 256;
					int b = (pcontroller2[i] + 128) % 256;
					value = (( a * dadt ) + ( b * ( 1.0f - dadt )) - 128) * (360.0f / 256.0f) + pbonecontroller[j].start;
				}
				else
				{
					value = ((pcontroller1[i] * dadt + (pcontroller2[i]) * (1.0f - dadt))) * (360.0f / 256.0f) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = (pcontroller1[i] * dadt + pcontroller2[i] * (1.0f - dadt)) / 255.0f;
				value = bound( 0.0f, value, 1.0f );
				value = (1.0f - value) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			}
		}

		switch( pbonecontroller[j].type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = DEG2RAD( value );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

/*
====================
StudioCalcRotations

====================
*/
static void R_StudioCalcRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	float adj[MAXSTUDIOCONTROLLERS];

	// bah, fix this bug with changing sequences too fast
	if( f > pseqdesc->numframes - 1 )
	{
		f = 0.0f;
	}
	else if( f < -0.01f )
	{
		// BUG ( somewhere else ) but this code should validate this data.
		// This could cause a crash if the frame # is negative, so we'll go ahead
		// and clamp it here
		f = -0.01f;
	}

	int frame = (int)f;
	float dadt = R_StudioEstimateInterpolant( e );
	float s = (f - frame);

	// add in programtic controllers
	mstudiobone_t *pbone = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	R_StudioCalcBoneAdj( dadt, adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

	for( int i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );

	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

/*
====================
StudioMergeBones

====================
*/
static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	static vec4_t	q[MAXSTUDIOBONES];
	static float	pos[MAXSTUDIOBONES][3];

	if( e->curstate.sequence >=  m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, m_pSubModel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		int j;
		for( j = 0; j < g_studio.cached_numbones; j++ )
		{
			if( !Q_stricmp( pbones[i].name, g_studio.cached_bonenames[j] ))
			{
				Matrix3x4_Copy( g_studio.bonestransform[i], g_studio.cached_bonestransform[j] );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.cached_lighttransform[j] );
				break;
			}
		}

		if( j >= g_studio.cached_numbones )
		{
			matrix3x4 bonematrix;
			Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );
			if( pbones[i].parent == -1 )
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

				// apply client-side effects to the transformation matrix
				R_StudioFxTransform( e, g_studio.bonestransform[i] );
			}
			else
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
				Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
			}
		}
	}
}

/*
====================
StudioSetupBones

====================
*/
static void R_StudioSetupBones( cl_entity_t *e )
{
	static vec3_t	pos[MAXSTUDIOBONES];
	static vec4_t	q[MAXSTUDIOBONES];
	static vec3_t	pos2[MAXSTUDIOBONES];
	static vec4_t	q2[MAXSTUDIOBONES];
	static vec3_t	pos3[MAXSTUDIOBONES];
	static vec4_t	q3[MAXSTUDIOBONES];
	static vec3_t	pos4[MAXSTUDIOBONES];
	static vec4_t	q4[MAXSTUDIOBONES];

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );

	if( pseqdesc->numblends > 1 )
	{
		panim += m_pStudioHeader->numbones;
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, f );

		float dadt = R_StudioEstimateInterpolant( e );
		float s = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;

		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q2, pos2, s );

		if( pseqdesc->numblends == 4 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, f );

			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, f );

			s = (e->curstate.blending[0] * dadt + e->latched.prevblending[0] * (1.0f - dadt)) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

			s = (e->curstate.blending[1] * dadt + e->latched.prevblending[1] * (1.0f - dadt)) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q3, pos3, s );
		}
	}

	if( g_studio.interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > g_studio.time ) && ( e->latched.prevsequence < m_pStudioHeader->numseq ))
	{
		// blend from last sequence
		static vec3_t	pos1b[MAXSTUDIOBONES];
		static vec4_t	q1b[MAXSTUDIOBONES];

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->latched.prevsequence;
		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );

		// clip prevframe
		R_StudioCalcRotations( e, pos1b, q1b, pseqdesc, panim, e->latched.prevframe );

		if( pseqdesc->numblends > 1 )
		{
			panim += m_pStudioHeader->numbones;
			R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, e->latched.prevframe );

			float s = (e->latched.prevseqblending[0]) / 255.0f;
			R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q2, pos2, s );

			if( pseqdesc->numblends == 4 )
			{
				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos3, q3, pseqdesc, panim, e->latched.prevframe );

				panim += m_pStudioHeader->numbones;
				R_StudioCalcRotations( e, pos4, q4, pseqdesc, panim, e->latched.prevframe );

				s = (e->latched.prevseqblending[0]) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q3, pos3, q4, pos4, s );

				s = (e->latched.prevseqblending[1]) / 255.0f;
				R_StudioSlerpBones( m_pStudioHeader->numbones, q1b, pos1b, q3, pos3, s );
			}
		}

		float s = 1.0f - ( g_studio.time - e->latched.sequencetime ) / 0.2f;
		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q1b, pos1b, s );
	}
	else
	{
		// store prevframe otherwise
		e->latched.prevframe = f;
	}

	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	// calc gait animation
	if( m_pPlayerInfo && m_pPlayerInfo->gaitsequence != 0 )
	{
		qboolean	copy_bones = true;

		if( m_pPlayerInfo->gaitsequence >= m_pStudioHeader->numseq )
			m_pPlayerInfo->gaitsequence = 0;

		pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + m_pPlayerInfo->gaitsequence;

		panim = gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, m_pPlayerInfo->gaitframe );

		for( int i = 0; i < m_pStudioHeader->numbones; i++ )
		{
			if( !Q_strcmp( pbones[i].name, "Bip01 Spine" ))
				copy_bones = false;
			else if( !Q_strcmp( pbones[pbones[i].parent].name, "Bip01 Pelvis" ))
				copy_bones = true;

			if( !copy_bones ) continue;

			VectorCopy( pos2[i], pos[i] );
			Vector4Copy( q2[i], q[i] );
		}
	}

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		matrix3x4 bonematrix;
		Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );

		if( pbones[i].parent == -1 )
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
			Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );

			// apply client-side effects to the transformation matrix
			R_StudioFxTransform( e, g_studio.bonestransform[i] );
		}
		else
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
			Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
		}
	}
}

/*
====================
StudioSaveBones

====================
*/
static void R_StudioSaveBones( void )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	g_studio.cached_numbones = m_pStudioHeader->numbones;

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_Copy( g_studio.cached_bonestransform[i], g_studio.bonestransform[i] );
		Matrix3x4_Copy( g_studio.cached_lighttransform[i], g_studio.lighttransform[i] );
		Q_strncpy( g_studio.cached_bonenames[i], pbones[i].name, 32 );
	}
}

/*
====================
StudioBuildNormalTable

NOTE: m_pSubModel must be set
====================
*/
static void R_StudioBuildNormalTable( void )
{
	cl_entity_t	*e = RI.currententity;

	Assert( m_pSubModel != NULL );

	// reset chrome cache
	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
		g_studio.chromeage[i] = 0;

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		g_studio.normaltable[i] = -1;

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 ) i = -i;

			for( ; i > 0; i--, ptricmds += 4 )
			{
				if( g_studio.normaltable[ptricmds[0]] < 0 )
					g_studio.normaltable[ptricmds[0]] = ptricmds[1];
			}
		}
	}

	g_studio.chrome_origin[0] = cos( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[1] = sin( r_glowshellfreq->value * g_studio.time ) * 4000.0f;
	g_studio.chrome_origin[2] = cos( r_glowshellfreq->value * g_studio.time * 0.33f ) * 4000.0f;

	if( e->curstate.rendercolor.r || e->curstate.rendercolor.g || e->curstate.rendercolor.b )
		TriColor4ub( e->curstate.rendercolor.r, e->curstate.rendercolor.g, e->curstate.rendercolor.b, 255 );
	else TriColor4ub( 255, 255, 255, 255 );
}

/*
====================
StudioGenerateNormals

NOTE: m_pSubModel must be set
g_studio.verts must be computed
====================
*/
static void R_StudioGenerateNormals( void )
{
	Assert( m_pSubModel != NULL );

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorClear( g_studio.norms[i] );

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
			{
				i = -i;

				if( i > 2 )
				{
					vec3_t e0, e1, norm;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						v1 = v2;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
			else
			{
				if( i > 2 )
				{
					qboolean	odd = false;
					vec3_t e0, e1, norm;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( i -= 2; i > 0; i--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];

						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						if( odd ) v1 = v2;
						else v0 = v2;

						odd = !odd;
					}
				}
				else
				{
					ptricmds += i;
				}
			}
		}
	}

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorNormalize( g_studio.norms[i] );
}

/*
====================
StudioSetupChrome

====================
*/
static void R_StudioSetupChrome( float *pchrome, int bone, vec3_t normal )
{
	if( g_studio.chromeage[bone] != g_studio.framecount )
	{
		// calculate vectors from the viewer to the bone. This roughly adjusts for position
		vec3_t	chromeupvec;	// g_studio.chrome t vector in world reference frame
		vec3_t	chromerightvec;	// g_studio.chrome s vector in world reference frame
		vec3_t	tmp;		// vector pointing at bone in world reference frame

		VectorNegate( g_studio.chrome_origin, tmp );
		tmp[0] += g_studio.lighttransform[bone][0][3];
		tmp[1] += g_studio.lighttransform[bone][1][3];
		tmp[2] += g_studio.lighttransform[bone][2][3];

		VectorNormalize( tmp );
		CrossProduct( tmp, RI.vright, chromeupvec );
		VectorNormalize( chromeupvec );
		CrossProduct( chromeupvec, tmp, chromerightvec );
		VectorNormalize( chromerightvec );

		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromeupvec, g_studio.chromeup[bone] );
		Matrix3x4_VectorIRotate( g_studio.lighttransform[bone], chromerightvec, g_studio.chromeright[bone] );

		g_studio.chromeage[bone] = g_studio.framecount;
	}

	// calc s coord
	float n = DotProduct( normal, g_studio.chromeright[bone] );
	pchrome[0] = (n + 1.0f) * 32.0f;

	// calc t coord
	n = DotProduct( normal, g_studio.chromeup[bone] );
	pchrome[1] = (n + 1.0f) * 32.0f;
}

/*
====================
StudioCalcAttachments

====================
*/
static void R_StudioCalcAttachments( void )
{
	// calculate attachment points
	mstudioattachment_t *pAtt = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);

	for( int i = 0; i < Q_min( MAXSTUDIOATTACHMENTS, m_pStudioHeader->numattachments ); i++ )
	{
		Matrix3x4_VectorTransform( g_studio.lighttransform[pAtt[i].bone], pAtt[i].org, RI.currententity->attachment[i] );
	}
}

/*
===============
pfnStudioSetupModel

===============
*/
static void R_StudioSetupModel( int bodypart, void **ppbodypart, void **ppsubmodel )
{
	if( bodypart < 0 || bodypart >= m_pStudioHeader->numbodyparts )
		bodypart = 0;

	m_nCurrentBodyPartIndex = bodypart;
	m_pBodyPart = (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex) + bodypart;

	if( m_pBodyPart->nummodels <= 0 || m_pBodyPart->base <= 0 )
	{
		m_pSubModel = NULL;
		m_nCurrentSubModelIndex = -1;
		if( ppbodypart ) *ppbodypart = m_pBodyPart;
		if( ppsubmodel ) *ppsubmodel = NULL;
		return;
	}

	int index = RI.currententity->curstate.body / m_pBodyPart->base;
	index = index % m_pBodyPart->nummodels;
	m_nCurrentSubModelIndex = index;

	m_pSubModel = (mstudiomodel_t *)((byte *)m_pStudioHeader + m_pBodyPart->modelindex) + index;

	if( ppbodypart ) *ppbodypart = m_pBodyPart;
	if( ppsubmodel ) *ppsubmodel = m_pSubModel;
}

typedef void (*ragdoll_studio_triangle_callback_t)(
	int meshIndex, int i0, int i1, int i2, void *context );

typedef struct ragdoll_studio_clip_vertex_s
{
	float barycentric[3];
	float u, v, depth;
} ragdoll_studio_clip_vertex_t;

static float R_StudioDecalDot3( const float *a, const float *b )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void R_StudioDecalCross3( const float *a, const float *b, float *out )
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static qboolean R_StudioDecalFinite3( const float *v )
{
	return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] &&
		fabsf( v[0] ) < 1.0e20f && fabsf( v[1] ) < 1.0e20f &&
		fabsf( v[2] ) < 1.0e20f;
}

static float R_StudioDecalNormalize3( float *v )
{
	float length = sqrtf( R_StudioDecalDot3( v, v ));
	if( length > 0.000001f )
	{
		float inverse = 1.0f / length;
		v[0] *= inverse;
		v[1] *= inverse;
		v[2] *= inverse;
	}
	return length;
}

/*
 * Walk exactly the same four-short studio command records as the renderer.
 * The vertex indices are therefore the ones used by R_StudioSubmitMesh.
 */
static void R_StudioForEachSubModelTriangle(
	ragdoll_studio_triangle_callback_t callback, void *context )
{
	if( !callback || !m_pStudioHeader || !m_pSubModel )
		return;

	const int headerLength = m_pStudioHeader->length;
	if( headerLength <= 0 ||
		m_pSubModel->numverts <= 0 ||
		m_pSubModel->numverts > MAXSTUDIOVERTS ||
		m_pSubModel->nummesh <= 0 ||
		m_pSubModel->nummesh > MAXSTUDIOMESHES ||
		m_pSubModel->meshindex <= 0 ||
		m_pSubModel->meshindex > headerLength ||
		m_pSubModel->nummesh >
			( headerLength - m_pSubModel->meshindex ) / (int)sizeof( mstudiomesh_t ) )
		return;

	const byte *headerBytes = (const byte *)m_pStudioHeader;
	const mstudiomesh_t *meshes =
		(const mstudiomesh_t *)( headerBytes + m_pSubModel->meshindex );

	for( int meshIndex = 0; meshIndex < m_pSubModel->nummesh; ++meshIndex )
	{
		const mstudiomesh_t *mesh = &meshes[meshIndex];
		if( mesh->numtris <= 0 || mesh->numtris > MAXSTUDIOTRIANGLES ||
			mesh->triindex <= 0 || mesh->triindex > headerLength - (int)sizeof( short ) )
			continue;

		const short *command = (const short *)( headerBytes + mesh->triindex );
		const short *commandEnd =
			(const short *)( headerBytes + headerLength );
		int trianglesVisited = 0;

		while( command < commandEnd && trianglesVisited < mesh->numtris )
		{
			int signedVertexCount = *command++;
			if( signedVertexCount == 0 )
				break;

			qboolean isFan = signedVertexCount < 0;
			int vertexCount = isFan ? -signedVertexCount : signedVertexCount;
			if( vertexCount < 3 || vertexCount > MAXSTUDIOVERTS ||
				commandEnd - command < vertexCount * 4 )
				break;

			for( int vertex = 2;
				vertex < vertexCount && trianglesVisited < mesh->numtris;
				++vertex )
			{
				int a, b, c;
				if( isFan )
				{
					a = 0;
					b = vertex - 1;
					c = vertex;
				}
				else if( vertex & 1 )
				{
					a = vertex - 1;
					b = vertex - 2;
					c = vertex;
				}
				else
				{
					a = vertex - 2;
					b = vertex - 1;
					c = vertex;
				}

				int i0 = command[a * 4];
				int i1 = command[b * 4];
				int i2 = command[c * 4];
				if( i0 >= 0 && i0 < m_pSubModel->numverts &&
					i1 >= 0 && i1 < m_pSubModel->numverts &&
					i2 >= 0 && i2 < m_pSubModel->numverts )
					callback( meshIndex, i0, i1, i2, context );

				++trianglesVisited;
			}

			command += vertexCount * 4;
		}
	}
}

static qboolean R_StudioDecalRayTriangle(
	const float origin[3], const float direction[3],
	const float a[3], const float b[3], const float c[3],
	float *distance )
{
	float edge1[3], edge2[3], p[3], tvec[3], q[3];
	for( int i = 0; i < 3; ++i )
	{
		edge1[i] = b[i] - a[i];
		edge2[i] = c[i] - a[i];
	}
	R_StudioDecalCross3( direction, edge2, p );
	float determinant = R_StudioDecalDot3( edge1, p );
	if( fabsf( determinant ) < 0.00001f )
		return false;

	float inverse = 1.0f / determinant;
	for( int i = 0; i < 3; ++i )
		tvec[i] = origin[i] - a[i];
	float u = R_StudioDecalDot3( tvec, p ) * inverse;
	if( u < 0.0f || u > 1.0f )
		return false;

	R_StudioDecalCross3( tvec, edge1, q );
	float v = R_StudioDecalDot3( direction, q ) * inverse;
	if( v < 0.0f || u + v > 1.0f )
		return false;

	float t = R_StudioDecalDot3( edge2, q ) * inverse;
	if( t < 0.0f )
		return false;

	*distance = t;
	return true;
}

static void R_StudioDecalClosestPointOnSegment(
	const float p[3], const float a[3], const float b[3],
	const float baryA[3], const float baryB[3],
	float closest[3], float bary[3] )
{
	float ab[3], ap[3];
	for( int i = 0; i < 3; ++i )
	{
		ab[i] = b[i] - a[i];
		ap[i] = p[i] - a[i];
	}
	float denominator = R_StudioDecalDot3( ab, ab );
	float t = denominator > 0.000001f ?
		R_StudioDecalDot3( ap, ab ) / denominator : 0.0f;
	if( t < 0.0f ) t = 0.0f;
	if( t > 1.0f ) t = 1.0f;
	for( int i = 0; i < 3; ++i )
	{
		closest[i] = a[i] + ab[i] * t;
		bary[i] = baryA[i] + ( baryB[i] - baryA[i] ) * t;
	}
}

/* Closest point and barycentrics, from the Voronoi-region triangle test. */
static void R_StudioDecalClosestPoint(
	const float p[3], const float a[3], const float b[3], const float c[3],
	float closest[3], float bary[3] )
{
	float ab[3], ac[3], ap[3];
	for( int i = 0; i < 3; ++i )
	{
		ab[i] = b[i] - a[i];
		ac[i] = c[i] - a[i];
		ap[i] = p[i] - a[i];
	}

	float areaNormal[3];
	R_StudioDecalCross3( ab, ac, areaNormal );
	if( R_StudioDecalDot3( areaNormal, areaNormal ) < 0.00000001f )
	{
		const float baryA[3] = { 1.0f, 0.0f, 0.0f };
		const float baryB[3] = { 0.0f, 1.0f, 0.0f };
		const float baryC[3] = { 0.0f, 0.0f, 1.0f };
		float candidate[3], candidateBary[3], delta[3];
		float bestDistance = 1.0e30f;

		R_StudioDecalClosestPointOnSegment(
			p, a, b, baryA, baryB, candidate, candidateBary );
		for( int i = 0; i < 3; ++i ) delta[i] = candidate[i] - p[i];
		bestDistance = R_StudioDecalDot3( delta, delta );
		for( int i = 0; i < 3; ++i )
		{
			closest[i] = candidate[i];
			bary[i] = candidateBary[i];
		}

		R_StudioDecalClosestPointOnSegment(
			p, b, c, baryB, baryC, candidate, candidateBary );
		for( int i = 0; i < 3; ++i ) delta[i] = candidate[i] - p[i];
		float distance = R_StudioDecalDot3( delta, delta );
		if( distance < bestDistance )
		{
			bestDistance = distance;
			for( int i = 0; i < 3; ++i )
			{
				closest[i] = candidate[i];
				bary[i] = candidateBary[i];
			}
		}

		R_StudioDecalClosestPointOnSegment(
			p, c, a, baryC, baryA, candidate, candidateBary );
		for( int i = 0; i < 3; ++i ) delta[i] = candidate[i] - p[i];
		distance = R_StudioDecalDot3( delta, delta );
		if( distance < bestDistance )
		{
			for( int i = 0; i < 3; ++i )
			{
				closest[i] = candidate[i];
				bary[i] = candidateBary[i];
			}
		}
		return;
	}
	float d1 = R_StudioDecalDot3( ab, ap );
	float d2 = R_StudioDecalDot3( ac, ap );
	if( d1 <= 0.0f && d2 <= 0.0f )
	{
		bary[0] = 1.0f; bary[1] = bary[2] = 0.0f;
		goto finish;
	}

	float bp[3];
	for( int i = 0; i < 3; ++i ) bp[i] = p[i] - b[i];
	float d3 = R_StudioDecalDot3( ab, bp );
	float d4 = R_StudioDecalDot3( ac, bp );
	if( d3 >= 0.0f && d4 <= d3 )
	{
		bary[0] = 0.0f; bary[1] = 1.0f; bary[2] = 0.0f;
		goto finish;
	}

	float vc = d1 * d4 - d3 * d2;
	if( vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f )
	{
		float v = d1 / ( d1 - d3 );
		bary[0] = 1.0f - v; bary[1] = v; bary[2] = 0.0f;
		goto finish;
	}

	float cp[3];
	for( int i = 0; i < 3; ++i ) cp[i] = p[i] - c[i];
	float d5 = R_StudioDecalDot3( ab, cp );
	float d6 = R_StudioDecalDot3( ac, cp );
	if( d6 >= 0.0f && d5 <= d6 )
	{
		bary[0] = 0.0f; bary[1] = 0.0f; bary[2] = 1.0f;
		goto finish;
	}

	float vb = d5 * d2 - d1 * d6;
	if( vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f )
	{
		float w = d2 / ( d2 - d6 );
		bary[0] = 1.0f - w; bary[1] = 0.0f; bary[2] = w;
		goto finish;
	}

	float va = d3 * d6 - d5 * d4;
	if( va <= 0.0f && ( d4 - d3 ) >= 0.0f && ( d5 - d6 ) >= 0.0f )
	{
		float w = ( d4 - d3 ) / (( d4 - d3 ) + ( d5 - d6 ));
		bary[0] = 0.0f; bary[1] = 1.0f - w; bary[2] = w;
		goto finish;
	}

	float denominator = 1.0f / ( va + vb + vc );
	bary[1] = vb * denominator;
	bary[2] = vc * denominator;
	bary[0] = 1.0f - bary[1] - bary[2];

finish:
	for( int i = 0; i < 3; ++i )
		closest[i] = bary[0] * a[i] + bary[1] * b[i] + bary[2] * c[i];
}

typedef struct ragdoll_studio_anchor_context_s
{
	ragdoll_studio_decal_t *decal;
	float direction[3];
	float rayLength;
	float bestRayDistance;
	qboolean rayFound;
	float bestClosestDistanceSquared;
	float closestPoint[3];
	float closestNormal[3];
	int hitBoneIndex;
} ragdoll_studio_anchor_context_t;

static qboolean R_StudioDecalTriangleTouchesBone(
	int hitBoneIndex, int i0, int i1, int i2 )
{
	if( hitBoneIndex < 0 || hitBoneIndex >= m_pStudioHeader->numbones ||
		m_pStudioHeader->numbones <= 0 ||
		m_pStudioHeader->numbones > MAXSTUDIOBONES )
		return false;

	const int headerLength = m_pStudioHeader->length;
	if( m_pStudioHeader->boneindex <= 0 ||
		m_pStudioHeader->boneindex > headerLength ||
		m_pStudioHeader->numbones >
			( headerLength - m_pStudioHeader->boneindex ) / (int)sizeof( mstudiobone_t ) ||
		m_pSubModel->vertinfoindex <= 0 ||
		m_pSubModel->vertinfoindex > headerLength ||
		m_pSubModel->numverts > headerLength - m_pSubModel->vertinfoindex )
		return false;

	const mstudiobone_t *bones =
		(const mstudiobone_t *)((const byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	const byte *vertexBones =
		(const byte *)m_pStudioHeader + m_pSubModel->vertinfoindex;
	const int hitParent = bones[hitBoneIndex].parent;
	const int indices[3] = { i0, i1, i2 };

	for( int corner = 0; corner < 3; ++corner )
	{
		const int vertexBone = vertexBones[indices[corner]];
		if( vertexBone >= m_pStudioHeader->numbones )
			continue;
		if( vertexBone == hitBoneIndex || vertexBone == hitParent ||
			bones[vertexBone].parent == hitBoneIndex )
			return true;
	}

	return false;
}

static void R_StudioFindDecalAnchorTriangle(
	int meshIndex, int i0, int i1, int i2, void *opaque )
{
	ragdoll_studio_anchor_context_t *context =
		(ragdoll_studio_anchor_context_t *)opaque;
	const float *a = g_studio.verts[i0];
	const float *b = g_studio.verts[i1];
	const float *c = g_studio.verts[i2];
	if( !R_StudioDecalFinite3( a ) || !R_StudioDecalFinite3( b ) ||
		!R_StudioDecalFinite3( c ) )
		return;

	float distance;
	if( R_StudioDecalRayTriangle(
			context->decal->rayStart, context->direction,
			a, b, c, &distance ) &&
		distance <= context->rayLength &&
		( !context->rayFound || distance < context->bestRayDistance ) )
	{
		context->rayFound = true;
		context->bestRayDistance = distance;
	}

	// Ray intersections above are authoritative regardless of skinning
	// ownership. The coarse-hull fallback is restricted to the hit bone and
	// its immediate hierarchy so a nearby overlapping body part cannot steal it.
	if( !R_StudioDecalTriangleTouchesBone( context->hitBoneIndex, i0, i1, i2 ) )
		return;

	float closest[3], bary[3], delta[3], edge1[3], edge2[3], normal[3];
	R_StudioDecalClosestPoint(
		context->decal->impactPosition, a, b, c, closest, bary );
	for( int i = 0; i < 3; ++i )
	{
		delta[i] = closest[i] - context->decal->impactPosition[i];
		edge1[i] = b[i] - a[i];
		edge2[i] = c[i] - a[i];
	}
	R_StudioDecalCross3( edge1, edge2, normal );
	if( R_StudioDecalNormalize3( normal ) < 0.0001f )
		return;

	float distanceSquared = R_StudioDecalDot3( delta, delta );
	if( distanceSquared < context->bestClosestDistanceSquared )
	{
		context->bestClosestDistanceSquared = distanceSquared;
		for( int i = 0; i < 3; ++i )
		{
			context->closestPoint[i] = closest[i];
			context->closestNormal[i] = normal[i];
		}
	}
}

static qboolean R_StudioFindDecalAnchor( ragdoll_studio_decal_t *decal )
{
	ragdoll_studio_anchor_context_t context;
	memset( &context, 0, sizeof( context ));
	context.decal = decal;
	context.hitBoneIndex = decal->boneIndex;
	context.bestRayDistance = 1.0e30f;
	context.bestClosestDistanceSquared = 1.0e30f;

	float direction[3], rayLengthSquared = 0.0f;
	for( int i = 0; i < 3; ++i )
	{
		direction[i] = decal->impactPosition[i] - decal->rayStart[i];
		rayLengthSquared += direction[i] * direction[i];
	}
	if( rayLengthSquared < 0.0001f )
		return false;
	context.rayLength = sqrtf( rayLengthSquared );
	for( int i = 0; i < 3; ++i )
		context.direction[i] = direction[i] / context.rayLength;
	context.rayLength += Q_max( decal->radius * 2.0f, 8.0f );

	R_StudioForEachSubModelTriangle( R_StudioFindDecalAnchorTriangle, &context );

	if( context.rayFound )
	{
		for( int i = 0; i < 3; ++i )
		{
			decal->anchorPosition[i] =
				decal->rayStart[i] + context.direction[i] * context.bestRayDistance;
			decal->normal[i] = -context.direction[i];
		}
		return R_StudioDecalNormalize3( decal->normal ) > 0.0001f;
	}

	const float maximumDistance = Q_max( decal->radius * 2.5f, 10.0f );
	if( context.bestClosestDistanceSquared > maximumDistance * maximumDistance )
		return false;

	for( int i = 0; i < 3; ++i )
	{
		decal->anchorPosition[i] = context.closestPoint[i];
		decal->normal[i] = context.closestNormal[i];
	}
	float toShooter[3];
	for( int i = 0; i < 3; ++i )
		toShooter[i] = decal->rayStart[i] - decal->anchorPosition[i];
	if( R_StudioDecalDot3( decal->normal, toShooter ) < 0.0f )
		for( int i = 0; i < 3; ++i )
			decal->normal[i] = -decal->normal[i];
	return R_StudioDecalNormalize3( decal->normal ) > 0.0001f;
}

static float R_StudioDecalClipDistance(
	const ragdoll_studio_clip_vertex_t *vertex,
	int plane, float radius, float depthTolerance )
{
	switch( plane )
	{
	case 0: return vertex->u + radius;
	case 1: return radius - vertex->u;
	case 2: return vertex->v + radius;
	case 3: return radius - vertex->v;
	case 4: return vertex->depth + depthTolerance;
	default: return depthTolerance - vertex->depth;
	}
}

static ragdoll_studio_clip_vertex_t R_StudioDecalInterpolateClipVertex(
	const ragdoll_studio_clip_vertex_t *a,
	const ragdoll_studio_clip_vertex_t *b, float fraction )
{
	ragdoll_studio_clip_vertex_t result;
	for( int i = 0; i < 3; ++i )
		result.barycentric[i] =
			a->barycentric[i] + ( b->barycentric[i] - a->barycentric[i] ) * fraction;
	result.u = a->u + ( b->u - a->u ) * fraction;
	result.v = a->v + ( b->v - a->v ) * fraction;
	result.depth = a->depth + ( b->depth - a->depth ) * fraction;
	return result;
}

typedef struct ragdoll_studio_project_context_s
{
	ragdoll_studio_decal_t *decal;
	float tangent[3];
	float bitangent[3];
	float depthTolerance;
	int bodypartIndex;
	int submodelIndex;
} ragdoll_studio_project_context_t;

static void R_StudioProjectDecalTriangle(
	int meshIndex, int i0, int i1, int i2, void *opaque )
{
	ragdoll_studio_project_context_t *context =
		(ragdoll_studio_project_context_t *)opaque;
	ragdoll_studio_decal_t *decal = context->decal;
	if( decal->fragmentCount >= MAX_RAGDOLL_STUDIO_FRAGMENTS )
		return;

	const int indices[3] = { i0, i1, i2 };
	ragdoll_studio_clip_vertex_t polygon[MAX_RAGDOLL_STUDIO_POLY_VERTS];
	ragdoll_studio_clip_vertex_t clipped[MAX_RAGDOLL_STUDIO_POLY_VERTS];
	for( int corner = 0; corner < 3; ++corner )
	{
		const float *position = g_studio.verts[indices[corner]];
		if( !R_StudioDecalFinite3( position ) )
			return;

		float relative[3];
		for( int axis = 0; axis < 3; ++axis )
			relative[axis] = position[axis] - decal->anchorPosition[axis];
		polygon[corner].u = R_StudioDecalDot3( relative, context->tangent );
		polygon[corner].v = R_StudioDecalDot3( relative, context->bitangent );
		polygon[corner].depth = R_StudioDecalDot3( relative, decal->normal );
		polygon[corner].barycentric[0] = corner == 0 ? 1.0f : 0.0f;
		polygon[corner].barycentric[1] = corner == 1 ? 1.0f : 0.0f;
		polygon[corner].barycentric[2] = corner == 2 ? 1.0f : 0.0f;
	}

	int polygonCount = 3;
	for( int plane = 0; plane < 6 && polygonCount >= 3; ++plane )
	{
		int clippedCount = 0;
		const ragdoll_studio_clip_vertex_t *previous = &polygon[polygonCount - 1];
		float previousDistance = R_StudioDecalClipDistance(
			previous, plane, decal->radius, context->depthTolerance );
		qboolean previousInside = previousDistance >= -0.00001f;

		for( int vertex = 0; vertex < polygonCount; ++vertex )
		{
			const ragdoll_studio_clip_vertex_t *current = &polygon[vertex];
			float currentDistance = R_StudioDecalClipDistance(
				current, plane, decal->radius, context->depthTolerance );
			qboolean currentInside = currentDistance >= -0.00001f;

			if( currentInside != previousInside &&
				clippedCount < MAX_RAGDOLL_STUDIO_POLY_VERTS )
			{
				float denominator = previousDistance - currentDistance;
				if( fabsf( denominator ) > 0.000001f )
				{
					float fraction = previousDistance / denominator;
					if( fraction < 0.0f ) fraction = 0.0f;
					if( fraction > 1.0f ) fraction = 1.0f;
					clipped[clippedCount++] =
						R_StudioDecalInterpolateClipVertex( previous, current, fraction );
				}
			}
			if( currentInside && clippedCount < MAX_RAGDOLL_STUDIO_POLY_VERTS )
				clipped[clippedCount++] = *current;

			previous = current;
			previousDistance = currentDistance;
			previousInside = currentInside;
		}
		polygonCount = clippedCount;
		for( int i = 0; i < polygonCount; ++i )
			polygon[i] = clipped[i];
	}

	if( polygonCount < 3 || polygonCount > MAX_RAGDOLL_STUDIO_POLY_VERTS )
		return;

	ragdoll_studio_decal_fragment_t *fragment =
		&decal->fragments[decal->fragmentCount];
	memset( fragment, 0, sizeof( *fragment ));
	fragment->bodypartIndex = context->bodypartIndex;
	fragment->submodelIndex = context->submodelIndex;
	fragment->meshIndex = meshIndex;
	fragment->vertexIndex[0] = i0;
	fragment->vertexIndex[1] = i1;
	fragment->vertexIndex[2] = i2;
	fragment->numVerts = polygonCount;

	for( int vertex = 0; vertex < polygonCount; ++vertex )
	{
		for( int corner = 0; corner < 3; ++corner )
			fragment->verts[vertex].barycentric[corner] =
				polygon[vertex].barycentric[corner];
		fragment->verts[vertex].texcoord[0] =
			0.5f + polygon[vertex].u / ( 2.0f * decal->radius );
		fragment->verts[vertex].texcoord[1] =
			0.5f + polygon[vertex].v / ( 2.0f * decal->radius );
	}
	++decal->fragmentCount;
}

static qboolean R_StudioBuildDecalFragments( ragdoll_studio_decal_t *decal )
{
	ragdoll_studio_project_context_t context;
	memset( &context, 0, sizeof( context ));
	context.decal = decal;
	context.bodypartIndex = m_nCurrentBodyPartIndex;
	context.submodelIndex = m_nCurrentSubModelIndex;
	context.depthTolerance = decal->radius * 0.65f + 0.5f;

	float reference[3] = { 0.0f, 0.0f, 1.0f };
	if( fabsf( decal->normal[2] ) > 0.90f )
	{
		reference[0] = 0.0f;
		reference[1] = 1.0f;
		reference[2] = 0.0f;
	}
	R_StudioDecalCross3( reference, decal->normal, context.tangent );
	if( R_StudioDecalNormalize3( context.tangent ) < 0.0001f )
		return false;
	R_StudioDecalCross3( decal->normal, context.tangent, context.bitangent );
	if( R_StudioDecalNormalize3( context.bitangent ) < 0.0001f )
		return false;

	decal->fragmentCount = 0;
	R_StudioForEachSubModelTriangle( R_StudioProjectDecalTriangle, &context );
	return decal->fragmentCount > 0;
}

static void R_StudioProjectPendingRagdollDecals( void )
{
	if( !RI.currententity || !m_pStudioHeader || !m_pSubModel ||
		RI.currententity->index <= 0 || !RI.currententity->model )
		return;

	for( int i = 0; i < MAX_RAGDOLL_STUDIO_DECALS; ++i )
	{
		ragdoll_studio_decal_t *decal = &g_RagdollStudioDecals[i];
		if( !decal->used || decal->projected ||
			decal->entityIndex != RI.currententity->index )
			continue;

		if( decal->model && decal->model != RI.currententity->model )
		{
			decal->used = false;
			continue;
		}

		if( !R_StudioFindDecalAnchor( decal ) )
			continue;
		if( R_StudioBuildDecalFragments( decal ) )
		{
			decal->model = RI.currententity->model;
			decal->projected = true;
			decal->age = ++g_RagdollStudioDecalClock;
		}
	}
}

static int R_StudioGetRagdollDecalTexture( void )
{
	static const char textureName[] = "ragdoll_studio_bloodmark_v1";
	static byte pixels[64 * 64 * 4];
	static qboolean pixelsBuilt;

	// Resolve by name every time: numeric GL texture IDs can change after
	// a video restart or renderer resource-table rebuild.
	g_RagdollStudioDecalTexture = GL_FindTexture( textureName );
	if( g_RagdollStudioDecalTexture > 0 )
		return g_RagdollStudioDecalTexture;

	if( !pixelsBuilt )
	{
		for( int y = 0; y < 64; ++y )
		{
			for( int x = 0; x < 64; ++x )
			{
				float nx = ( (float)x + 0.5f - 32.0f ) / 32.0f;
				float ny = ( (float)y + 0.5f - 32.0f ) / 32.0f;
				float radius = sqrtf( nx * nx + ny * ny );
				float angle = atan2f( ny, nx );
				float edge = 0.77f + 0.10f * sinf( angle * 5.0f ) +
					0.055f * cosf( angle * 11.0f ) +
					0.035f * sinf( angle * 17.0f );
				float alpha = ( edge - radius ) * 14.0f;
				if( alpha < 0.0f ) alpha = 0.0f;
				if( alpha > 1.0f ) alpha = 1.0f;
				alpha = alpha * alpha * ( 3.0f - 2.0f * alpha );
				int offset = ( y * 64 + x ) * 4;
				pixels[offset + 0] = 255;
				pixels[offset + 1] = 255;
				pixels[offset + 2] = 255;
				pixels[offset + 3] = (byte)( alpha * 235.0f + 0.5f );
			}
		}
		pixelsBuilt = true;
	}

	g_RagdollStudioDecalTexture = GL_CreateTexture(
		textureName, 64, 64, pixels, TF_CLAMP | TF_NOMIPMAP | TF_HAS_ALPHA );
	return g_RagdollStudioDecalTexture;
}

static void R_StudioDrawRagdollDecalsForMesh( mstudiomesh_t *mesh )
{
	if( !RI.currententity || !mesh ||
		RI.currententity->curstate.rendermode != kRenderNormal )
		return;

	mstudiomesh_t *firstMesh =
		(mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);
	int meshIndex = (int)( mesh - firstMesh );
	if( meshIndex < 0 || meshIndex >= m_pSubModel->nummesh )
		return;

	int texture = R_StudioGetRagdollDecalTexture();
	if( texture <= 0 )
		return;

	qboolean drawing = false;
	for( int i = 0; i < MAX_RAGDOLL_STUDIO_DECALS; ++i )
	{
		const ragdoll_studio_decal_t *decal = &g_RagdollStudioDecals[i];
		if( !decal->used || !decal->projected ||
			decal->entityIndex != RI.currententity->index ||
			decal->model != RI.currententity->model ||
			decal->fragmentCount <= 0 )
			continue;

		for( int f = 0; f < decal->fragmentCount; ++f )
		{
			const ragdoll_studio_decal_fragment_t *fragment = &decal->fragments[f];
			if( fragment->bodypartIndex != m_nCurrentBodyPartIndex ||
				fragment->submodelIndex != m_nCurrentSubModelIndex ||
				fragment->meshIndex != meshIndex ||
				fragment->numVerts < 3 ||
				fragment->numVerts > MAX_RAGDOLL_STUDIO_POLY_VERTS )
				continue;

			if( !drawing )
			{
				GL_Bind( XASH_TEXTURE0, texture );
				pglEnable( GL_TEXTURE_2D );
				pglEnable( GL_BLEND );
				pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				pglDepthMask( GL_FALSE );
				drawing = true;
			}

			if( decal->bloodColor == 195 )
				pglColor4f( 0.82f, 0.68f, 0.12f, 0.90f );
			else if( decal->bloodColor == 30 )
				pglColor4f( 0.12f, 0.42f, 0.08f, 0.90f );
			else
				pglColor4f( 0.58f, 0.018f, 0.012f, 0.92f );

			pglBegin( GL_POLYGON );
			qboolean fragmentValid = true;
			for( int corner = 0; corner < 3; ++corner )
			{
				int vertexIndex = fragment->vertexIndex[corner];
				if( vertexIndex < 0 || vertexIndex >= m_pSubModel->numverts ||
					vertexIndex >= MAXSTUDIOVERTS )
					fragmentValid = false;
			}
			if( !fragmentValid )
				continue;

			pglBegin( GL_POLYGON );
			for( int vertex = 0; vertex < fragment->numVerts; ++vertex )
			{
				const ragdoll_studio_decal_vertex_t *decalVertex =
					&fragment->verts[vertex];
				float position[3] = { 0.0f, 0.0f, 0.0f };
				for( int corner = 0; corner < 3; ++corner )
				{
					int vertexIndex = fragment->vertexIndex[corner];
					for( int axis = 0; axis < 3; ++axis )
						position[axis] +=
							decalVertex->barycentric[corner] *
							g_studio.verts[vertexIndex][axis];
				}
				for( int axis = 0; axis < 3; ++axis )
					position[axis] += decal->normal[axis] * 0.045f;
				pglTexCoord2f(
					decalVertex->texcoord[0], decalVertex->texcoord[1] );
				pglVertex3fv( position );
			}
			pglEnd();
		}
	}

	if( drawing )
	{
		pglDepthMask( GL_TRUE );
		pglDisable( GL_BLEND );
		pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	}
}

static int R_StudioDecalShoot(
	int entityIndex, int ragdollID, int boneIndex,
	const float *rayStart, const float *impactPosition,
	float radius, int bloodColor )
{
	if( entityIndex <= 0 || ragdollID <= 0 || !rayStart || !impactPosition ||
		boneIndex < 0 || boneIndex >= MAXSTUDIOBONES ||
		!( radius >= 0.5f && radius <= 32.0f ) ||
		!R_StudioDecalFinite3( rayStart ) ||
		!R_StudioDecalFinite3( impactPosition ))
		return 0;

	for( int i = 0; i < MAX_RAGDOLL_STUDIO_DECALS; ++i )
	{
		ragdoll_studio_decal_t *decal = &g_RagdollStudioDecals[i];
		if( !decal->used || decal->entityIndex != entityIndex )
			continue;

		if( decal->ragdollID != ragdollID )
		{
			decal->used = false;
			continue;
		}

		qboolean duplicate = decal->bloodColor == bloodColor &&
			fabsf( decal->radius - radius ) < 0.05f;
		for( int axis = 0; axis < 3 && duplicate; ++axis )
			if( fabsf( decal->rayStart[axis] - rayStart[axis] ) > 0.15f ||
				fabsf( decal->impactPosition[axis] - impactPosition[axis] ) > 0.15f )
				duplicate = false;
		if( duplicate )
			return 1;
	}

	int slot = -1;
	uint oldestAge = 0xFFFFFFFFu;
	for( int i = 0; i < MAX_RAGDOLL_STUDIO_DECALS; ++i )
	{
		if( !g_RagdollStudioDecals[i].used )
		{
			slot = i;
			break;
		}
		if( g_RagdollStudioDecals[i].age < oldestAge )
		{
			oldestAge = g_RagdollStudioDecals[i].age;
			slot = i;
		}
	}
	if( slot < 0 )
		return 0;

	ragdoll_studio_decal_t *decal = &g_RagdollStudioDecals[slot];
	memset( decal, 0, sizeof( *decal ));
	decal->used = true;
	decal->entityIndex = entityIndex;
	decal->ragdollID = ragdollID;
	decal->boneIndex = boneIndex;
	decal->bloodColor = bloodColor;
	decal->radius = bound( 1.0f, radius, 12.0f );
	decal->age = ++g_RagdollStudioDecalClock;
	for( int axis = 0; axis < 3; ++axis )
	{
		decal->rayStart[axis] = rayStart[axis];
		decal->impactPosition[axis] = impactPosition[axis];
	}
	return 1;
}

static void R_StudioClearRagdollDecals( int entityIndex )
{
	if( entityIndex <= 0 )
	{
		memset( g_RagdollStudioDecals, 0, sizeof( g_RagdollStudioDecals ));
		g_RagdollStudioDecalClock = 0;
		return;
	}

	for( int i = 0; i < MAX_RAGDOLL_STUDIO_DECALS; ++i )
	{
		if( g_RagdollStudioDecals[i].used &&
			g_RagdollStudioDecals[i].entityIndex == entityIndex )
			memset( &g_RagdollStudioDecals[i], 0, sizeof( g_RagdollStudioDecals[i] ));
	}
}



/*
===============
R_StudioCheckBBox

===============
*/
static int R_StudioCheckBBox( void )
{
	if( !RI.currententity || !RI.currentmodel )
		return false;

	qboolean visible = R_StudioComputeBBox( NULL );
	if( g_studioAmbientCollecting )
	{
		g_studioAmbientTested++;
		if( visible )
			R_StudioAmbientCollect( RI.currententity );
	}
	return visible;
}

/*
===============
pfnStudioEntityLight

===============
*/
static void R_StudioEntityLight( alight_t *lightinfo )
{
	float		lstrength[MAX_LOCALLIGHTS];
	cl_entity_t	*ent = RI.currententity;

	g_studio.numlocallights = 0;

	if( !ent || !r_dynamic->value )
		return;

	for( int i = 0; i < MAX_LOCALLIGHTS; i++ )
		lstrength[i] = 0;

	vec3_t origin;
	Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );
	float dist2 = 1000000.0f;
	int k = 0;

	for( int lnum = 0; lnum < MAX_ELIGHTS; lnum++ )
	{
		dlight_t *el = &tr.elights[lnum];

		if( el->die < g_studio.time || el->radius <= 0.0f )
			continue;

		if(( el->key & 0xFFF ) == ent->index )
		{
			int	att = (el->key >> 12) & 0xF;

			if( att ) VectorCopy( ent->attachment[att], el->origin );
			else VectorCopy( ent->origin, el->origin );
		}

		vec3_t mid;
		VectorSubtract( origin, el->origin, mid );

		float f = DotProduct( mid, mid );
		float r2 = el->radius * el->radius;

		float minstrength;
		if( f > r2 ) minstrength = r2 / f;
		else minstrength = 1.0f;

		if( minstrength > 0.05f )
		{
			if( g_studio.numlocallights >= MAX_LOCALLIGHTS )
			{
				k = -1;
				for( int j = 0; j < g_studio.numlocallights; j++ )
				{
					if( lstrength[j] < dist2 && lstrength[j] < minstrength )
					{
						dist2 = lstrength[j];
						k = j;
					}
				}
			}
			else k = g_studio.numlocallights;

			if( k != -1 )
			{
				g_studio.locallightcolor[k][0] = LinearGammaTable( el->color.r << 2 );
				g_studio.locallightcolor[k][1] = LinearGammaTable( el->color.g << 2 );
				g_studio.locallightcolor[k][2] = LinearGammaTable( el->color.b << 2 );
				g_studio.locallightR2[k] = r2;
				g_studio.locallight[k] = el;
				lstrength[k] = minstrength;

				if( k >= g_studio.numlocallights )
					g_studio.numlocallights = k + 1;
			}
		}
	}
}

/*
===============
R_StudioSetupLighting

===============
*/
static void R_StudioSetupLighting( alight_t *plight )
{
	float	scale = 1.0f;

	if( !m_pStudioHeader || !plight )
		return;

	if( RI.currententity != NULL )
		scale = RI.currententity->curstate.scale;

	g_studio.ambientlight = plight->ambientlight;
	g_studio.shadelight = plight->shadelight;
	VectorCopy( plight->plightvec, g_studio.lightvec );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_VectorIRotate( g_studio.lighttransform[i], plight->plightvec, g_studio.blightvec[i] );
		if( scale > 1.0f ) VectorNormalize( g_studio.blightvec[i] ); // in case model may be scaled
	}

	VectorCopy( plight->color, g_studio.lightcolor );
}

/*
===============
R_StudioLighting

===============
*/
static void R_StudioLighting( float *lv, int bone, int flags, vec3_t normal )
{
	if( FBitSet( flags, STUDIO_NF_FULLBRIGHT ))
	{
		*lv = 1.0f;
		return;
	}

	float illum = g_studio.ambientlight;

	if( FBitSet( flags, STUDIO_NF_FLATSHADE ))
	{
		illum += g_studio.shadelight * 0.8f;
	}
	else
	{
		float lightcos;
		if( bone != -1 ) lightcos = DotProduct( normal, g_studio.blightvec[bone] );
		else lightcos = DotProduct( normal, g_studio.lightvec ); // -1 colinear, 1 opposite
		if( lightcos > 1.0f ) lightcos = 1.0f;

		illum += g_studio.shadelight;

		float r = SHADE_LAMBERT;

 		// do modified hemispherical lighting
		if( r <= 1.0f )
		{
			r += 1.0f;
			lightcos = (( r - 1.0f ) - lightcos) / r;
			if( lightcos > 0.0f )
				illum += g_studio.shadelight * lightcos;
		}
		else
		{
			lightcos = (lightcos + ( r - 1.0f )) / r;
			if( lightcos > 0.0f )
				illum -= g_studio.shadelight * lightcos;
		}

		illum = Q_max( illum, 0.0f );
	}

	illum = Q_min( illum, 255.0f );

	*lv = LightToTexGamma( illum * 4 ) / 1023.0f;
}

/*
====================
R_LightLambert

====================
*/
static void R_LightLambert( vec4_t light[MAX_LOCALLIGHTS], const vec3_t normal, const vec3_t color, byte *out )
{
	if( !g_studio.numlocallights )
	{
		VectorScale( color, 255.0f, out );
		return;
	}

	vec3_t finalLight = { 0, 0, 0 };

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		float r = DotProduct( normal, light[i] );
		if( likely( !tr.fFlipViewModel ))
			r = -r;

		if( r > 0.0f )
		{
			if( light[i][3] == 0.0f )
			{
				float r2 = DotProduct( light[i], light[i] );

				if( r2 > 0.0f )
					light[i][3] = g_studio.locallightR2[i] / ( r2 * sqrt( r2 ));
				else light[i][3] = 0.0001f;
			}

			float temp = r * light[i][3];

			vec3_t localLight;
			VectorAddScalar( g_studio.locallightcolor[i], temp, localLight );
			VectorAdd( finalLight, localLight, finalLight );
		}
	}

	if( !VectorIsNull( finalLight ))
	{
		for( int i = 0; i < 3; i++ )
		{
			float c = finalLight[i] + LinearGammaTable( color[i] * 1023.0f );

			if( c > 1023.0f )
				out[i] = 255;
			else
				out[i] = ScreenGammaTable( c ) >> 2;
		}
	}
	else
	{
		VectorScale( color, 255.0f, out );
	}
}

static void R_StudioSetColorArray( short *ptricmds, vec3_t *pstudionorms, byte *color )
{
	float	*lv = (float *)g_studio.lightvalues[ptricmds[1]];

	color[3] = tr.blend * 255;
	R_LightLambert( g_studio.lightpos[ptricmds[0]], pstudionorms[ptricmds[1]], lv, color );
}

static void R_StudioSetColorBegin( short *ptricmds, vec3_t *pstudionorms )
{
	rgba_t color;

	R_StudioSetColorArray( ptricmds, pstudionorms, color );
	pglColor4ubv( color );
}

/*
====================
R_LightStrength

====================
*/
static void R_LightStrength( int bone, vec3_t localpos, vec4_t light[MAX_LOCALLIGHTS] )
{
	if( g_studio.lightage[bone] != g_studio.framecount )
	{
		for( int i = 0; i < g_studio.numlocallights; i++ )
		{
			dlight_t *el = g_studio.locallight[i];
			Matrix3x4_VectorITransform( g_studio.lighttransform[bone], el->origin, g_studio.lightbonepos[bone][i] );
		}

		g_studio.lightage[bone] = g_studio.framecount;
	}

	for( int i = 0; i < g_studio.numlocallights; i++ )
	{
		VectorSubtract( localpos, g_studio.lightbonepos[bone][i], light[i] );
		light[i][3] = 0.0f;
	}
}

/*
===============
R_StudioSetupSkin

===============
*/
static void R_StudioSetupSkin( studiohdr_t *ptexturehdr, int index )
{
	mstudiotexture_t	*ptexture = NULL;

	if( FBitSet( g_nForceFaceFlags, STUDIO_NF_CHROME ))
		return;

	if( ptexturehdr == NULL )
		return;

	// NOTE: user may ignore to call StudioRemapColors and remap_info will be unavailable
	if( m_fDoRemap ) ptexture = gEngfuncs.CL_GetRemapInfoForEntity( RI.currententity )->ptexture;
	if( !ptexture ) ptexture = (mstudiotexture_t *)((byte *)ptexturehdr + ptexturehdr->textureindex); // fallback

	if( r_lightmap->value && !r_fullbright->value )
		GL_Bind( XASH_TEXTURE0, tr.whiteTexture );
	else GL_Bind( XASH_TEXTURE0, ptexture[index].index );
}

/*
===============
R_StudioGetTexture

Doesn't changes studio global state at all
===============
*/
mstudiotexture_t *R_StudioGetTexture( cl_entity_t *e )
{
	studiohdr_t *phdr;

	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, e->model )) == NULL )
		return NULL;

	studiohdr_t *thdr = m_pStudioHeader;
	if( !thdr ) return NULL;

	mstudiotexture_t *ptexture;
	if( m_fDoRemap ) ptexture = gEngfuncs.CL_GetRemapInfoForEntity( e )->ptexture;
	else ptexture = (mstudiotexture_t *)((byte *)thdr + thdr->textureindex);

	return ptexture;
}

static void R_StudioSetRenderamt( int iRenderamt )
{
	if( !RI.currententity ) return;

	RI.currententity->curstate.renderamt = iRenderamt;
	tr.blend = CL_FxBlend( RI.currententity ) / 255.0f;
}

/*
===============
R_StudioSetCullState

sets true for enable backculling (for left-hand viewmodel)
===============
*/
static void R_StudioSetCullState( int iCull )
{
	// This function intentionally does nothing
}

/*
===============
R_StudioRenderShadow

just a prefab for render shadow
===============
*/
static void R_StudioRenderShadow( int iSprite, float *p1, float *p2, float *p3, float *p4 )
{
	if( !p1 || !p2 || !p3 || !p4 )
		return;

	if( TriSpriteTexture( CL_ModelHandle( iSprite ), 0 ))
	{
		TriRenderMode( kRenderTransAlpha );
		TriColor4f( 0.0f, 0.0f, 0.0f, 1.0f );

		pglBegin( GL_QUADS );
			pglTexCoord2f( 0.0f, 0.0f );
			pglVertex3fv( p1 );
			pglTexCoord2f( 0.0f, 1.0f );
			pglVertex3fv( p2 );
			pglTexCoord2f( 1.0f, 1.0f );
			pglVertex3fv( p3 );
			pglTexCoord2f( 1.0f, 0.0f );
			pglVertex3fv( p4 );
		pglEnd();

		TriRenderMode( kRenderNormal );
	}
}

/*
===============
R_StudioMeshCompare

Sorting opaque entities by model type
===============
*/
static int R_StudioMeshCompare( const void *a, const void *b )
{
	if( FBitSet( ((const sortedmesh_t*)a)->flags, STUDIO_NF_ADDITIVE ))
		return 1;

	if( FBitSet( ((const sortedmesh_t*)a)->flags, STUDIO_NF_MASKED ))
		return -1;

	return 0;
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawNormalMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t )
{
	int	i;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			pglBegin( GL_TRIANGLE_FAN );
			i = -i;
		}
		else pglBegin( GL_TRIANGLE_STRIP );

		for( ; i > 0; i--, ptricmds += 4 )
		{
			R_StudioSetColorBegin( ptricmds, pstudionorms );

			pglTexCoord2f( ptricmds[2] * s, ptricmds[3] * t );
			pglVertex3fv( g_studio.verts[ptricmds[0]] );
		}

		pglEnd();
	}
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawFloatMesh( short *ptricmds, vec3_t *pstudionorms )
{
	int	i;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			pglBegin( GL_TRIANGLE_FAN );
			i = -i;
		}
		else pglBegin( GL_TRIANGLE_STRIP );

		for( ; i > 0; i--, ptricmds += 4 )
		{
			R_StudioSetColorBegin( ptricmds, pstudionorms );
			pglTexCoord2f( HalfToFloat( ptricmds[2] ), HalfToFloat( ptricmds[3] ));
			pglVertex3fv( g_studio.verts[ptricmds[0]] );
		}

		pglEnd();
	}
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioDrawChromeMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float scale )
{
	int	i;
	qboolean	glowShell = (scale > 0.0f) ? true : false;

	while(( i = *( ptricmds++ )))
	{
		if( i < 0 )
		{
			pglBegin( GL_TRIANGLE_FAN );
			i = -i;
		}
		else pglBegin( GL_TRIANGLE_STRIP );

		for( ; i > 0; i--, ptricmds += 4 )
		{
			int idx;
			if( glowShell )
			{
				color24 *clr = &RI.currententity->curstate.rendercolor;

				idx = g_studio.normaltable[ptricmds[0]];
				float *av = g_studio.verts[ptricmds[0]];
				float *lv = g_studio.norms[ptricmds[0]];
				vec3_t vert;
				VectorMA( av, scale, lv, vert );
				pglColor4ub( clr->r, clr->g, clr->b, 255 );
				pglTexCoord2f( g_studio.chrome[idx][0] * s, g_studio.chrome[idx][1] * t );
				pglVertex3fv( vert );
			}
			else
			{
				idx = ptricmds[1];
				R_StudioSetColorBegin( ptricmds, pstudionorms );
				pglTexCoord2f( g_studio.chrome[idx][0] * s, g_studio.chrome[idx][1] * t );
				pglVertex3fv( g_studio.verts[ptricmds[0]] );
			}
		}

		pglEnd();
	}
}


static int R_StudioBuildIndices( qboolean tri_strip, int vertexState )
{
	// build in indices
	if( vertexState++ < 3 )
	{
		g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts;
	}
	else if( tri_strip )
	{
		// flip triangles between clockwise and counter clockwise
		if( vertexState & 1 )
		{
			// draw triangle [n-2 n-1 n]
			g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts - 2;
			g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts - 1;
			g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts;
		}
		else
		{
			// draw triangle [n-1 n-2 n]
			g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts - 1;
			g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts - 2;
			g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts;
		}
	}
	else
	{
		// draw triangle fan [0 n-1 n]
		g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts - ( vertexState - 1 );
		g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts - 1;
		g_studio.arrayelems[g_studio.numelems++] = g_studio.numverts;
	}

	return vertexState;
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioBuildArrayNormalMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t )
{
	int	i;

	while(( i = *( ptricmds++ )))
	{
		int vertexState = 0;
		qboolean tri_strip = true;

		if( i < 0 )
		{
			tri_strip = false;
			i = -i;
		}

		for( ; i > 0; i--, ptricmds += 4 )
		{
			GLubyte *cl = g_studio.arraycolor[g_studio.numverts];

			vertexState = R_StudioBuildIndices( tri_strip, vertexState );

			R_StudioSetColorArray( ptricmds, pstudionorms, cl );

			g_studio.arraycoord[g_studio.numverts][0] = ptricmds[2] * s;
			g_studio.arraycoord[g_studio.numverts][1] = ptricmds[3] * t;

			VectorCopy( g_studio.verts[ptricmds[0]], g_studio.arrayverts[g_studio.numverts] );
			g_studio.numverts++;
		}
	}
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioBuildArrayFloatMesh( short *ptricmds, vec3_t *pstudionorms )
{
	int	i;

	while(( i = *( ptricmds++ )))
	{
		int vertexState = 0;
		qboolean tri_strip = true;

		if( i < 0 )
		{
			tri_strip = false;
			i = -i;
		}

		for( ; i > 0; i--, ptricmds += 4 )
		{
			GLubyte *cl = g_studio.arraycolor[g_studio.numverts];

			vertexState = R_StudioBuildIndices( tri_strip, vertexState );

			R_StudioSetColorArray( ptricmds, pstudionorms, cl );

			g_studio.arraycoord[g_studio.numverts][0] = HalfToFloat( ptricmds[2] );
			g_studio.arraycoord[g_studio.numverts][1] = HalfToFloat( ptricmds[3] );

			VectorCopy( g_studio.verts[ptricmds[0]], g_studio.arrayverts[g_studio.numverts] );
			g_studio.numverts++;
		}
	}
}

/*
===============
R_StudioDrawNormalMesh

generic path
===============
*/
static void R_StudioBuildArrayChromeMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float scale )
{
	int	i;
	qboolean	glowShell = (scale > 0.0f) ? true : false;

	while(( i = *( ptricmds++ )))
	{
		int vertexState = 0;
		qboolean tri_strip = true;

		if( i < 0 )
		{
			tri_strip = false;
			i = -i;
		}

		for( ; i > 0; i--, ptricmds += 4 )
		{
			GLubyte *cl = g_studio.arraycolor[g_studio.numverts];

			vertexState = R_StudioBuildIndices( tri_strip, vertexState );

			int idx;
			if( glowShell )
			{
				idx = g_studio.normaltable[ptricmds[0]];
				float *av = g_studio.verts[ptricmds[0]];
				float *lv = g_studio.norms[ptricmds[0]];

				cl[0] = RI.currententity->curstate.rendercolor.r;
				cl[1] = RI.currententity->curstate.rendercolor.g;
				cl[2] = RI.currententity->curstate.rendercolor.b;
				cl[3] = 255;

				vec3_t vert;
				VectorMA( av, scale, lv, vert );
				VectorCopy( vert, g_studio.arrayverts[g_studio.numverts] );
			}
			else
			{
				idx = ptricmds[1];
				R_StudioSetColorArray( ptricmds, pstudionorms, cl );

				VectorCopy( g_studio.verts[ptricmds[0]], g_studio.arrayverts[g_studio.numverts] );
			}

			g_studio.arraycoord[g_studio.numverts][0] = g_studio.chrome[idx][0] * s;
			g_studio.arraycoord[g_studio.numverts][1] = g_studio.chrome[idx][1] * t;

			g_studio.numverts++;
		}
	}
}

static void R_StudioDrawArrays( uint startverts, uint startelems )
{
	if( g_studio.numelems == startelems )
		return;

	pglEnableClientState( GL_VERTEX_ARRAY );
	pglVertexPointer( 3, GL_FLOAT, 12, g_studio.arrayverts );

	pglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	pglTexCoordPointer( 2, GL_FLOAT, 0, g_studio.arraycoord );

	if( !( g_nForceFaceFlags & STUDIO_NF_CHROME ) )
	{
		pglEnableClientState( GL_COLOR_ARRAY );
		pglColorPointer( 4, GL_UNSIGNED_BYTE, 0, g_studio.arraycolor );
	}

	GL_DrawRangeElements( GL_TRIANGLES, startverts, g_studio.numverts - 1, g_studio.numelems - startelems, GL_UNSIGNED_SHORT, &g_studio.arrayelems[startelems] );
	pglDisableClientState( GL_VERTEX_ARRAY );
	pglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	if( !( g_nForceFaceFlags & STUDIO_NF_CHROME ) )
		pglDisableClientState( GL_COLOR_ARRAY );
}

/*
===============
R_StudioSubmitMesh

===============
*/
static void R_StudioSubmitMesh( short *ptricmds, vec3_t *pstudionorms, float s, float t, float shellscale, int tesslevel )
{
	if( tesslevel > 0 )
	{
		// R_StudioDrawTruformMesh( ptricmds, pstudionorms, s, t, shellscale, tesslevel );
	}
	else if( r_studio_drawelements.value )
	{
		uint startArrayVerts = g_studio.numverts;
		uint startArrayElems = g_studio.numelems;

		if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
			R_StudioBuildArrayChromeMesh( ptricmds, pstudionorms, s, t, shellscale );
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_UV_COORDS ))
			R_StudioBuildArrayFloatMesh( ptricmds, pstudionorms );
		else
			R_StudioBuildArrayNormalMesh( ptricmds, pstudionorms, s, t );

		R_StudioDrawArrays( startArrayVerts, startArrayElems );
	}
	else
	{
		if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
			R_StudioDrawChromeMesh( ptricmds, pstudionorms, s, t, shellscale );
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_UV_COORDS ))
			R_StudioDrawFloatMesh( ptricmds, pstudionorms );
		else
			R_StudioDrawNormalMesh( ptricmds, pstudionorms, s, t );
	}
}

/*
===============
R_StudioDrawPoints

===============
*/
static void R_StudioDrawPoints( void )
{
	float		shellscale = 0.0f;
	qboolean		need_sort = false;

	if( !m_pStudioHeader || !m_pSubModel ) return;


	g_studio.numverts = g_studio.numelems = 0;

	int m_skinnum = RI.currententity->curstate.skin;
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)m_pStudioHeader + m_pStudioHeader->textureindex);
	byte *pvertbone = ((byte *)m_pStudioHeader + m_pSubModel->vertinfoindex);
	byte *pnormbone = ((byte *)m_pStudioHeader + m_pSubModel->norminfoindex);

	mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);
	vec3_t *pstudioverts = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->vertindex);
	vec3_t *pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);

	short *pskinref = (short *)((byte *)m_pStudioHeader + m_pStudioHeader->skinindex);
	if( m_skinnum > 0 && m_skinnum < m_pStudioHeader->numskinfamilies )
		pskinref += (m_skinnum * m_pStudioHeader->numskinref);

	const int numverts = m_pSubModel->numverts;
	const int numnorms = m_pSubModel->numnorms;

	if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ) && m_pSubModel->blendvertinfoindex != 0 && m_pSubModel->blendnorminfoindex != 0 )
	{
		mstudioboneweight_t	*pvertweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendvertinfoindex);
		mstudioboneweight_t	*pnormweight = (mstudioboneweight_t *)((byte *)m_pStudioHeader + m_pSubModel->blendnorminfoindex);
		matrix3x4		skinMat;

		for( int i = 0; i < numverts; i++ )
		{
			R_StudioComputeSkinMatrix( &pvertweight[i], skinMat );
			Matrix3x4_VectorTransform( skinMat, pstudioverts[i], g_studio.verts[i] );
			R_LightStrength( pvertbone[i], pstudioverts[i], g_studio.lightpos[i] );
		}

		for( int i = 0; i < numnorms; i++ )
		{
			R_StudioComputeSkinMatrix( &pnormweight[i], skinMat );
			Matrix3x4_VectorRotate( skinMat, pstudionorms[i], g_studio.norms[i] );
		}
	}
	else
	{
		for( int i = 0; i < numverts; i++ )
			Matrix3x4_VectorTransform( g_studio.bonestransform[pvertbone[i]], pstudioverts[i], g_studio.verts[i] );

		for( int i = 0; i < numverts; i++ )
			R_LightStrength( pvertbone[i], pstudioverts[i], g_studio.lightpos[i] );
	}

	// generate shared normals for properly scaling glowing shell
	if( RI.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		float factor = (1.0f / 128.0f);
		shellscale = Q_max( factor, RI.currententity->curstate.renderamt * factor );
		R_StudioBuildNormalTable();
		R_StudioGenerateNormals();
	}

	int k = 0;
	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		g_nFaceFlags = ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags;

		// fill in sortedmesh info
		g_studio.meshes[j].flags = g_nFaceFlags;
		g_studio.meshes[j].mesh = &pmesh[j];

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED|STUDIO_NF_ADDITIVE ))
			need_sort = true;

		if( RI.currententity->curstate.rendermode == kRenderTransAdd )
		{
			for( int i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorSet( g_studio.lightvalues[k], tr.blend, tr.blend, tr.blend );
			}
		}
		else
		{
			for( int i = 0; i < pmesh[j].numnorms; i++, k++, pstudionorms++, pnormbone++ )
			{
				float lv_tmp;
				if( FBitSet( m_pStudioHeader->flags, STUDIO_HAS_BONEWEIGHTS ))
					R_StudioLighting( &lv_tmp, -1, g_nFaceFlags, g_studio.norms[k] );
				else R_StudioLighting( &lv_tmp, *pnormbone, g_nFaceFlags, (float *)pstudionorms );

				if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ))
					R_StudioSetupChrome( g_studio.chrome[k], *pnormbone, (float *)pstudionorms );
				VectorScale( g_studio.lightcolor, lv_tmp, g_studio.lightvalues[k] );
			}
		}
	}

	if( r_studio_sort_textures.value && need_sort )
	{
		// resort opaque and translucent meshes draw order
		qsort( g_studio.meshes, m_pSubModel->nummesh, sizeof( sortedmesh_t ), R_StudioMeshCompare );
	}

	// NOTE: rewind normals at start
	pstudionorms = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->normindex);

	// backface culling for left-handed weapons
	//
	// there is a bug in GoldSrc that sets backface culling to GL_FRONT
	// but doesn't enable OpenGL GL_CULL_FACE. This allows mod developers to disable
	// backface culling through TriAPI call
	//
	// see https://github.com/FWGS/xash3d-fwgs/issues/2517
	if( glState.faceCull != GL_NONE )
	{
		if( R_AllowFlipViewModel( RI.currententity ))
		{
			tr.fFlipViewModel = true;
			GL_Cull( GL_NONE );
		}
		else
		{
			tr.fFlipViewModel = false;
			GL_Cull( GL_FRONT );
		}
	}

	R_StudioProjectPendingRagdollDecals();

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		float	oldblend = tr.blend;

		pmesh = g_studio.meshes[j].mesh;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		g_nFaceFlags = ptexture[pskinref[pmesh->skinref]].flags | g_nForceFaceFlags;

		float s = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].width;
		float t = 1.0f / (float)ptexture[pskinref[pmesh->skinref]].height;

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			pglEnable( GL_ALPHA_TEST );
			pglAlphaFunc( GL_GREATER, 0.5f );
			pglDepthMask( GL_TRUE );
			if( R_ModelOpaque( RI.currententity->curstate.rendermode ))
				tr.blend = 1.0f;
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ))
		{
			if( R_ModelOpaque( RI.currententity->curstate.rendermode ))
			{
				pglBlendFunc( GL_ONE, GL_ONE );
				pglDepthMask( GL_FALSE );
				pglEnable( GL_BLEND );
				R_AllowFog( false );
			}
			else pglBlendFunc( GL_SRC_ALPHA, GL_ONE );
		}

		R_StudioSetupSkin( m_pStudioHeader, pskinref[pmesh->skinref] );

		R_StudioSubmitMesh( ptricmds, pstudionorms, s, t, shellscale, 0 );

		if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ))
		{
			pglAlphaFunc( GL_GREATER, DEFAULT_ALPHATEST );
			pglDisable( GL_ALPHA_TEST );
		}
		else if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) && R_ModelOpaque( RI.currententity->curstate.rendermode ))
		{
			pglDepthMask( GL_TRUE );
			pglDisable( GL_BLEND );
			R_AllowFog( true );
		}

		r_stats.c_studio_polys += pmesh->numtris;
		tr.blend = oldblend;
		R_StudioDrawRagdollDecalsForMesh( pmesh );
		if( RI.currententity->curstate.rendermode == kRenderNormal )
			R_StudioSetupSkin( m_pStudioHeader, pskinref[pmesh->skinref] );
	}
}

/*
===============
R_StudioDrawHulls

===============
*/
static void R_StudioDrawHulls( void )
{
	float alpha = ( r_drawentities->value == 4 ) ? 0.5f : 1.0f;

	GL_Bind( XASH_TEXTURE0, tr.whiteTexture );
	pglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	for( int i = 0; i < m_pStudioHeader->numhitboxes; i++ )
	{
		mstudiobbox_t	*pbbox = (mstudiobbox_t *)((byte *)m_pStudioHeader + m_pStudioHeader->hitboxindex);
		vec3_t		tmp, p[8];

		for( int j = 0; j < 8; j++ )
		{
			tmp[0] = (j & 1) ? pbbox[i].bbmin[0] : pbbox[i].bbmax[0];
			tmp[1] = (j & 2) ? pbbox[i].bbmin[1] : pbbox[i].bbmax[1];
			tmp[2] = (j & 4) ? pbbox[i].bbmin[2] : pbbox[i].bbmax[2];

			Matrix3x4_VectorTransform( g_studio.bonestransform[pbbox[i].bone], tmp, p[j] );
		}

		int j = (pbbox[i].group % 8);

		TriBegin( TRI_QUADS );
		TriColor4f( hullcolor[j][0], hullcolor[j][1], hullcolor[j][2], alpha );

		for( j = 0; j < 6; j++ )
		{
			float lv;
			VectorClear( tmp );
			tmp[j % 3] = (j < 3) ? 1.0f : -1.0f;
			R_StudioLighting( &lv, pbbox[i].bone, 0, tmp );

			TriBrightness( lv );
			TriVertex3fv( p[boxpnt[j][0]] );
			TriVertex3fv( p[boxpnt[j][1]] );
			TriVertex3fv( p[boxpnt[j][2]] );
			TriVertex3fv( p[boxpnt[j][3]] );
		}
		TriEnd();
	}
}

/*
===============
R_StudioDrawAbsBBox

===============
*/
static void R_StudioDrawAbsBBox( void )
{
	vec3_t	p[8], tmp;

	// looks ugly, skip
	if( RI.currententity == tr.viewent )
		return;

	if( !R_StudioComputeBBox( p ))
		return;

	GL_Bind( XASH_TEXTURE0, tr.whiteTexture );
	TriColor4f( 0.5f, 0.5f, 1.0f, 0.5f );
	TriRenderMode( kRenderTransAdd );

	TriBegin( TRI_QUADS );
	for( int i = 0; i < 6; i++ )
	{
		float lv;
		VectorClear( tmp );
		tmp[i % 3] = (i < 3) ? 1.0f : -1.0f;
		R_StudioLighting( &lv, -1, 0, tmp );

		TriBrightness( lv );
		TriVertex3fv( p[boxpnt[i][0]] );
		TriVertex3fv( p[boxpnt[i][1]] );
		TriVertex3fv( p[boxpnt[i][2]] );
		TriVertex3fv( p[boxpnt[i][3]] );
	}
	TriEnd();
	TriRenderMode( kRenderNormal );
}

/*
===============
R_StudioDrawBones

===============
*/
static void R_StudioDrawBones( void )
{
	mstudiobone_t	*pbones = (mstudiobone_t *) ((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	vec3_t		point;

	pglDisable( GL_TEXTURE_2D );

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		if( pbones[i].parent >= 0 )
		{
			pglPointSize( 3.0f );
			pglColor3f( 1, 0.7f, 0 );
			pglBegin( GL_LINES );

			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[pbones[i].parent], point );
			pglVertex3fv( point );
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
			pglVertex3fv( point );

			pglEnd();

			pglColor3f( 0, 0, 0.8f );
			pglBegin( GL_POINTS );
			if( pbones[pbones[i].parent].parent != -1 )
			{
				Matrix3x4_OriginFromMatrix( g_studio.bonestransform[pbones[i].parent], point );
				pglVertex3fv( point );
			}
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
			pglVertex3fv( point );
			pglEnd();
		}
		else
		{
			// draw parent bone node
			pglPointSize( 5.0f );
			pglColor3f( 0.8f, 0, 0 );
			pglBegin( GL_POINTS );
			Matrix3x4_OriginFromMatrix( g_studio.bonestransform[i], point );
			pglVertex3fv( point );
			pglEnd();
		}
	}

	pglPointSize( 1.0f );
	pglEnable( GL_TEXTURE_2D );
}

static void R_StudioDrawAttachments( void )
{
	pglDisable( GL_TEXTURE_2D );
	pglDisable( GL_DEPTH_TEST );

	for( int i = 0; i < m_pStudioHeader->numattachments; i++ )
	{
		vec3_t		v[4];

		mstudioattachment_t *pattachments = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].org, v[0] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[0], v[1] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[1], v[2] );
		Matrix3x4_VectorTransform( g_studio.bonestransform[pattachments[i].bone], pattachments[i].vectors[2], v[3] );

		pglBegin( GL_LINES );
		pglColor3f( 1, 0, 0 );
		pglVertex3fv( v[0] );
		pglColor3f( 1, 1, 1 );
		pglVertex3fv (v[1] );
		pglColor3f( 1, 0, 0 );
		pglVertex3fv (v[0] );
		pglColor3f( 1, 1, 1 );
		pglVertex3fv (v[2] );
		pglColor3f( 1, 0, 0 );
		pglVertex3fv (v[0] );
		pglColor3f( 1, 1, 1 );
		pglVertex3fv( v[3] );
		pglEnd();

		pglPointSize( 5.0f );
		pglColor3f( 0, 1, 0 );
		pglBegin( GL_POINTS );
		pglVertex3fv( v[0] );
		pglEnd();
		pglPointSize( 1.0f );
	}

	pglEnable( GL_TEXTURE_2D );
	pglEnable( GL_DEPTH_TEST );
}

/*
===============
R_StudioSetRemapColors

===============
*/
static void R_StudioSetRemapColors( int newTop, int newBottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors( RI.currententity, RI.currentmodel, newTop, newBottom ))
		m_fDoRemap = true;
}

void R_StudioResetPlayerModels( void )
{
	memset( g_studio.player_models, 0, sizeof( g_studio.player_models ));
}

/*
===============
R_StudioSetupPlayerModel

===============
*/
static model_t *R_StudioSetupPlayerModel( int index )
{
	player_info_t  *info = gEngfuncs.pfnPlayerInfo( index );

	if( index < 0 || index >= gp_cl->maxclients )
		return NULL;

	player_model_t *state = &g_studio.player_models[index];

	// g-cont: force for "dev-mode", non-local games and menu preview
	if(( gpGlobals->developer || !ENGINE_GET_PARM( PARM_SINGLEPLAYER_GAME ) || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) ) && info->model[0] )
	{
		if( Q_strcmp( state->name, info->model ))
		{
			Q_strncpy( state->name, info->model, sizeof( state->name ));
			state->name[sizeof( state->name ) - 1] = 0;

			Q_snprintf( state->modelname, sizeof( state->modelname ), "models/player/%s/%s.mdl", info->model, info->model );

			if( gEngfuncs.fsapi->FileExists( state->modelname, false ))
				state->model = gEngfuncs.Mod_ForName( state->modelname, false, true );
			else
				state->model = NULL;

			if( !state->model )
				state->model = RI.currententity->model;
		}
	}
	else
	{
		if( state->model != RI.currententity->model )
			state->model = RI.currententity->model;
		state->name[0] = 0;
	}

	return state->model;
}

/*
================
R_GetEntityRenderMode

check for texture flags
================
*/
int R_GetEntityRenderMode( cl_entity_t *ent )
{
	model_t *model = NULL;

	cl_entity_t *oldent = RI.currententity;
	RI.currententity = ent;

	if( ent->player ) // check it for real playermodel
		model = R_StudioSetupPlayerModel( ent->curstate.number - 1 );

	if( !model )
		model = ent->model;

	RI.currententity = oldent;

	studiohdr_t *phdr;
	if(( phdr = gEngfuncs.Mod_Extradata( mod_studio, model )) == NULL )
	{
		if( R_ModelOpaque( ent->curstate.rendermode ))
		{
			// forcing to choose right sorting type
			if(( model && model->type == mod_brush ) && FBitSet( model->flags, MODEL_TRANSPARENT ))
				return kRenderTransAlpha;
		}
		return ent->curstate.rendermode;
	}
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)phdr + phdr->textureindex);

	int opaque = 0, trans = 0;
	for( int i = 0; i < phdr->numtextures; i++, ptexture++ )
	{
		// ignore chrome & additive it's just a specular-like effect
		if( FBitSet( ptexture->flags, STUDIO_NF_ADDITIVE ) && !FBitSet( ptexture->flags, STUDIO_NF_CHROME ))
			trans++;
		else opaque++;
	}

	// if model is more additive than opaque
	if( trans > opaque )
		return kRenderTransAdd;
	return ent->curstate.rendermode;
}

/*
===============
R_StudioClientEvents

===============
*/
static void R_StudioClientEvents( void )
{
	cl_entity_t	*e = RI.currententity;

	if( g_studio.frametime == 0.0 )
		return; // gamepaused

	// fill attachments with interpolated origin
	if( m_pStudioHeader->numattachments <= 0 )
	{
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[0] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[1] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[2] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[3] );
	}

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ))
	{
		dlight_t	*el = gEngfuncs.CL_AllocElight( 0 );

		ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
		VectorCopy( e->attachment[0], el->origin );
		el->die = gp_cl->time + 0.05f;
		el->color.r = 255;
		el->color.g = 192;
		el->color.b = 64;
		el->decay = 320;
		el->radius = 24;
	}

	int sequence = bound( 0, e->curstate.sequence, m_pStudioHeader->numseq - 1 );
	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + sequence;

	// no events for this animation
	if( pseqdesc->numevents == 0 )
		return;

	float end = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );
	float start = end - e->curstate.framerate * gp_host->frametime * pseqdesc->fps;
	mstudioevent_t *pevent = (mstudioevent_t *)((byte *)m_pStudioHeader + pseqdesc->eventindex);

	if( e->latched.sequencetime == e->curstate.animtime )
	{
		if( !FBitSet( pseqdesc->flags, STUDIO_LOOPING ))
			start = -0.01f;
	}

	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		// ignore all non-client-side events
		if( pevent[i].event < EVENT_CLIENT )
			continue;

		if( (float)pevent[i].frame > start && pevent[i].frame <= end )
			gEngfuncs.pfnStudioEvent( &pevent[i], e );
	}
}

/*
===============
R_StudioGetForceFaceFlags

===============
*/
static int R_StudioGetForceFaceFlags( void )
{
	return g_nForceFaceFlags;
}

/*
===============
R_StudioSetForceFaceFlags

===============
*/
static void R_StudioSetForceFaceFlags( int flags )
{
	g_nForceFaceFlags = flags;
}

/*
===============
pfnStudioSetHeader

===============
*/
static void R_StudioSetHeader( studiohdr_t *pheader )
{
	m_pStudioHeader = pheader;
	m_fDoRemap = false;
}

studiohdr_t *R_StudioGetHeader( void )
{
	return m_pStudioHeader;
}

/*
===============
R_StudioSetRenderModel

===============
*/
static void R_StudioSetRenderModel( model_t *model )
{
	RI.currentmodel = model;
}

/*
===============
R_StudioSetupRenderer

===============
*/
static void R_StudioSetupRenderer( int rendermode )
{
	studiohdr_t	*phdr = m_pStudioHeader;

	if( rendermode > kRenderTransAdd ) rendermode = 0;
	g_studio.rendermode = bound( 0, rendermode, kRenderTransAdd );

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( false );

	pglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	pglDisable( GL_ALPHA_TEST );
	pglShadeModel( GL_SMOOTH );

	// a point to setup local to world transform for boneweighted models
	if( phdr && FBitSet( phdr->flags, STUDIO_HAS_BONEINFO ))
	{
		// NOTE: extended boneinfo goes immediately after bones
		mstudioboneinfo_t *boneinfo = (mstudioboneinfo_t *)((byte *)phdr + phdr->boneindex + phdr->numbones * sizeof( mstudiobone_t ));

		for( int i = 0; i < phdr->numbones; i++ )
			Matrix3x4_ConcatTransforms( g_studio.worldtransform[i], g_studio.bonestransform[i], boneinfo[i].poseToBone );
	}
}

/*
===============
R_StudioRestoreRenderer

===============
*/
static void R_StudioRestoreRenderer( void )
{
	if( g_studio.rendermode != kRenderNormal )
		pglDisable( GL_BLEND );

	if( g_studio.rendermode == kRenderTransAdd || g_studio.rendermode == kRenderGlow )
		R_AllowFog( true );

	pglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	pglShadeModel( GL_FLAT );
	m_fDoRemap = false;
}

/*
===============
R_StudioSetChromeOrigin

===============
*/
static void R_StudioSetChromeOrigin( void )
{
	VectorCopy( RI.rvp.vieworigin, g_studio.chrome_origin );
}

/*
===============
R_StudioDrawPointsShadow

===============
*/
static void R_StudioDrawPointsShadow( void )
{
	vec3_t		point;

	if( FBitSet( RI.currententity->curstate.effects, EF_NOSHADOW ))
		return;

	if( glState.stencilEnabled )
		pglEnable( GL_STENCIL_TEST );

	float vec_x = -g_studio.lightvec[0] * 8.0f;
	float vec_y = -g_studio.lightvec[1] * 8.0f;

	for( int k = 0; k < m_pSubModel->nummesh; k++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + k;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

		r_stats.c_studio_polys += pmesh->numtris;

		int i;
		while(( i = *( ptricmds++ )))
		{
			if( i < 0 )
			{
				pglBegin( GL_TRIANGLE_FAN );
				i = -i;
			}
			else
			{
				pglBegin( GL_TRIANGLE_STRIP );
			}


			for( ; i > 0; i--, ptricmds += 4 )
			{
				float *av = g_studio.verts[ptricmds[0]];
				point[0] = av[0] - (vec_x * ( av[2] - g_studio.lightspot[2] ));
				point[1] = av[1] - (vec_y * ( av[2] - g_studio.lightspot[2] ));
				point[2] = g_studio.lightspot[2] + 1.0f;

				pglVertex3fv( point );
			}

			pglEnd();
		}
	}

	if( glState.stencilEnabled )
		pglDisable( GL_STENCIL_TEST );
}

/*
===============
GL_StudioSetRenderMode

set rendermode for studiomodel
===============
*/
static void GL_StudioSetRenderMode( int rendermode )
{
	switch( rendermode )
	{
	case kRenderNormal:
		break;
	case kRenderTransColor:
		pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		pglEnable( GL_BLEND );
		break;
	case kRenderTransAdd:
		pglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		pglColor4f( tr.blend, tr.blend, tr.blend, 1.0f );
		pglBlendFunc( GL_ONE, GL_ONE );
		pglDepthMask( GL_FALSE );
		pglEnable( GL_BLEND );
		break;
	default:
		pglTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		pglColor4f( 1.0f, 1.0f, 1.0f, tr.blend );
		pglDepthMask( GL_TRUE );
		pglEnable( GL_BLEND );
		break;
	}
}

/*
===============
GL_StudioDrawShadow

g-cont: don't modify this code it's 100% matched with
original GoldSrc code and used in some mods to enable
studio shadows with some asm tricks
===============
*/
static void GL_StudioDrawShadow( void )
{
	pglDepthMask( GL_TRUE );

	if( r_shadows.value && g_studio.rendermode != kRenderTransAdd && !FBitSet( RI.currentmodel->flags, STUDIO_AMBIENT_LIGHT ))
	{
		float	color = 1.0f - (tr.blend * 0.5f);

		pglDisable( GL_TEXTURE_2D );
		pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		pglEnable( GL_BLEND );
		pglColor4f( 0.0f, 0.0f, 0.0f, 1.0f - color );

		pglDepthFunc( GL_LESS );
		R_StudioDrawPointsShadow();
		pglDepthFunc( GL_LEQUAL );

		pglEnable( GL_TEXTURE_2D );
		pglDisable( GL_BLEND );
		pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		pglShadeModel( GL_SMOOTH );
	}
}

/*
====================
StudioRenderFinal

====================
*/
static void R_StudioRenderFinal( void )
{
	int rendermode = R_StudioGetForceFaceFlags() ? kRenderTransAdd : RI.currententity->curstate.rendermode;
	R_StudioSetupRenderer( rendermode );

	if( r_drawentities->value == 2 )
	{
		R_StudioDrawBones();
	}
	else if( r_drawentities->value == 3 )
	{
		R_StudioDrawHulls();
	}
	else
	{
		for( int i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			R_StudioSetupModel( i, (void**)&m_pBodyPart, (void**)&m_pSubModel );

			GL_StudioSetRenderMode( rendermode );
			R_StudioDrawPoints();
			GL_StudioDrawShadow();
		}
	}

	if( r_drawentities->value == 4 )
	{
		TriRenderMode( kRenderTransAdd );
		R_StudioDrawHulls( );
		TriRenderMode( kRenderNormal );
	}

	if( r_drawentities->value == 5 )
	{
		R_StudioDrawAbsBBox( );
	}

	if( r_drawentities->value == 6 )
	{
		R_StudioDrawAttachments();
	}

	if( r_drawentities->value == 7 )
	{
		vec3_t	origin;

		pglDisable( GL_TEXTURE_2D );
		pglDisable( GL_DEPTH_TEST );

		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, origin );

		pglBegin( GL_LINES );
		pglColor3f( 1, 0.5, 0 );
		pglVertex3fv( origin );
		pglVertex3fv( g_studio.lightspot );
		pglEnd();

		pglBegin( GL_LINES );
		pglColor3f( 0, 0.5, 1 );
		VectorMA( g_studio.lightspot, -64.0f, g_studio.lightvec, origin );
		pglVertex3fv( g_studio.lightspot );
		pglVertex3fv( origin );
		pglEnd();

		pglPointSize( 5.0f );
		pglColor3f( 1, 0, 0 );
		pglBegin( GL_POINTS );
		pglVertex3fv( g_studio.lightspot );
		pglEnd();
		pglPointSize( 1.0f );

		pglEnable( GL_DEPTH_TEST );
		pglEnable( GL_TEXTURE_2D );
	}

	R_StudioRestoreRenderer();
}

/*
====================
StudioRenderModel

====================
*/
static void R_StudioRenderModel( void )
{
	R_StudioSetChromeOrigin();
	R_StudioSetForceFaceFlags( 0 );

	if( RI.currententity->curstate.renderfx == kRenderFxGlowShell )
	{
		RI.currententity->curstate.renderfx = kRenderFxNone;

		R_StudioRenderFinal( );

		R_StudioSetForceFaceFlags( STUDIO_NF_CHROME );
		TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_CHROME_SPRITE ), 0 );
		RI.currententity->curstate.renderfx = kRenderFxGlowShell;

		R_StudioRenderFinal( );
	}
	else
	{
		R_StudioRenderFinal( );
	}
}

/*
====================
StudioEstimateGait

====================
*/
static void R_StudioEstimateGait( entity_state_t *pplayer )
{
	vec3_t	est_velocity;
	float dt = bound( 0.0f, g_studio.frametime, 1.0f );

	if( dt == 0.0f || m_pPlayerInfo->renderframe == tr.realframecount )
	{
		m_flGaitMovement = 0;
		return;
	}

	VectorSubtract( RI.currententity->origin, m_pPlayerInfo->prevgaitorigin, est_velocity );
	VectorCopy( RI.currententity->origin, m_pPlayerInfo->prevgaitorigin );
	m_flGaitMovement = VectorLength( est_velocity );

	if( dt <= 0.0f || m_flGaitMovement / dt < 5.0f )
	{
		m_flGaitMovement = 0.0f;
		est_velocity[0] = 0.0f;
		est_velocity[1] = 0.0f;
	}

	if( est_velocity[1] == 0.0f && est_velocity[0] == 0.0f )
	{
		float	flYawDiff = RI.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;

		flYawDiff = flYawDiff - (int)(flYawDiff / 360) * 360;
		if( flYawDiff > 180.0f ) flYawDiff -= 360.0f;
		if( flYawDiff < -180.0f ) flYawDiff += 360.0f;

		if( dt < 0.25f )
			flYawDiff *= dt * 4.0f;
		else flYawDiff *= dt;

		m_pPlayerInfo->gaityaw += flYawDiff;
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - (int)(m_pPlayerInfo->gaityaw / 360) * 360;

		m_flGaitMovement = 0.0f;
	}
	else
	{
		m_pPlayerInfo->gaityaw = ( atan2( est_velocity[1], est_velocity[0] ) * 180 / M_PI_F );
		if( m_pPlayerInfo->gaityaw > 180.0f ) m_pPlayerInfo->gaityaw = 180.0f;
		if( m_pPlayerInfo->gaityaw < -180.0f ) m_pPlayerInfo->gaityaw = -180.0f;
	}

}

/*
====================
StudioProcessGait

====================
*/
static void R_StudioProcessGait( entity_state_t *pplayer )
{
	if( RI.currententity->curstate.sequence >= m_pStudioHeader->numseq )
		RI.currententity->curstate.sequence = 0;

	float dt = bound( 0.0f, g_studio.frametime, 1.0f );

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + RI.currententity->curstate.sequence;

	int iBlend;
	R_StudioPlayerBlend( pseqdesc, &iBlend, &RI.currententity->angles[PITCH] );

	RI.currententity->latched.prevangles[PITCH] = RI.currententity->angles[PITCH];
	RI.currententity->curstate.blending[0] = iBlend;
	RI.currententity->latched.prevblending[0] = RI.currententity->curstate.blending[0];
	RI.currententity->latched.prevseqblending[0] = RI.currententity->curstate.blending[0];
	R_StudioEstimateGait( pplayer );

	// calc side to side turning
	float flYaw = RI.currententity->angles[YAW] - m_pPlayerInfo->gaityaw;
	flYaw = flYaw - (int)(flYaw / 360) * 360;
	if( flYaw < -180.0f ) flYaw = flYaw + 360.0f;
	if( flYaw > 180.0f ) flYaw = flYaw - 360.0f;

	if( flYaw > 120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw - 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw + 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw + 180.0f;
	}

	// adjust torso
	RI.currententity->curstate.controller[0] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[1] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[2] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->curstate.controller[3] = ((flYaw / 4.0f) + 30.0f) / (60.0f / 255.0f);
	RI.currententity->latched.prevcontroller[0] = RI.currententity->curstate.controller[0];
	RI.currententity->latched.prevcontroller[1] = RI.currententity->curstate.controller[1];
	RI.currententity->latched.prevcontroller[2] = RI.currententity->curstate.controller[2];
	RI.currententity->latched.prevcontroller[3] = RI.currententity->curstate.controller[3];

	RI.currententity->angles[YAW] = m_pPlayerInfo->gaityaw;
	if( RI.currententity->angles[YAW] < -0 ) RI.currententity->angles[YAW] += 360.0f;
	RI.currententity->latched.prevangles[YAW] = RI.currententity->angles[YAW];

	if( pplayer->gaitsequence >= m_pStudioHeader->numseq )
		pplayer->gaitsequence = 0;

	pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + pplayer->gaitsequence;

	// calc gait frame
	if( pseqdesc->linearmovement[0] > 0 )
		m_pPlayerInfo->gaitframe += (m_flGaitMovement / pseqdesc->linearmovement[0]) * pseqdesc->numframes;
	else m_pPlayerInfo->gaitframe += pseqdesc->fps * dt;

	// do modulo
	m_pPlayerInfo->gaitframe = m_pPlayerInfo->gaitframe - (int)(m_pPlayerInfo->gaitframe / pseqdesc->numframes) * pseqdesc->numframes;
	if( m_pPlayerInfo->gaitframe < 0 ) m_pPlayerInfo->gaitframe += pseqdesc->numframes;
}

/*
===============
R_StudioDrawPlayer

===============
*/
static int R_StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	alight_t	lighting;
	vec3_t	dir;

	int m_nPlayerIndex = pplayer->number - 1;

	if( m_nPlayerIndex < 0 || m_nPlayerIndex >= gp_cl->maxclients )
		return 0;

	RI.currentmodel = R_StudioSetupPlayerModel( m_nPlayerIndex );
	if( RI.currentmodel == NULL )
		return 0;

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel ));

	if( pplayer->gaitsequence )
	{
		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		vec3_t orig_angles = Vec3( RI.currententity->angles );

		R_StudioProcessGait( pplayer );

		m_pPlayerInfo->gaitsequence = pplayer->gaitsequence;
		m_pPlayerInfo = NULL;

		R_StudioSetUpTransform( RI.currententity );
		VectorCopy( orig_angles, RI.currententity->angles );
	}
	else
	{
		RI.currententity->curstate.controller[0] = 127;
		RI.currententity->curstate.controller[1] = 127;
		RI.currententity->curstate.controller[2] = 127;
		RI.currententity->curstate.controller[3] = 127;
		RI.currententity->latched.prevcontroller[0] = RI.currententity->curstate.controller[0];
		RI.currententity->latched.prevcontroller[1] = RI.currententity->curstate.controller[1];
		RI.currententity->latched.prevcontroller[2] = RI.currententity->curstate.controller[2];
		RI.currententity->latched.prevcontroller[3] = RI.currententity->curstate.controller[3];

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
		m_pPlayerInfo->gaitsequence = 0;

		R_StudioSetUpTransform( RI.currententity );
	}

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !R_StudioCheckBBox( ))
			return 0;

		r_stats.c_studio_models_drawn++;
		g_studio.framecount++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );
	R_StudioSetupBones( RI.currententity );
	R_StudioSaveBones( );

	m_pPlayerInfo->renderframe = tr.realframecount;
	m_pPlayerInfo = NULL;

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		// copy attachments into global entity array
		if( RI.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
			memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		// change body if it's a menu entity
		if( cl_himodels->value && ( RI.currentmodel != RI.currententity->model || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD )))
		{
			// show highest resolution multiplayer model
			RI.currententity->curstate.body = 255;
		}

		if( !( !gpGlobals->developer && gp_cl->maxclients == 1 ) && ( RI.currentmodel == RI.currententity->model ))
			RI.currententity->curstate.body = 1; // force helmet

		lighting.plightvec = dir;
		R_EntityDynamicLight( RI.currententity, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		// model and frame independant
		R_StudioSetupLighting( &lighting );

		m_pPlayerInfo = pfnPlayerInfo( m_nPlayerIndex );

		// get remap colors
		g_nTopColor = m_pPlayerInfo->topcolor;
		g_nBottomColor = m_pPlayerInfo->bottomcolor;

		if( g_nTopColor < 0 ) g_nTopColor = 0;
		if( g_nTopColor > 360 ) g_nTopColor = 360;
		if( g_nBottomColor < 0 ) g_nBottomColor = 0;
		if( g_nBottomColor > 360 ) g_nBottomColor = 360;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

		R_StudioRenderModel( );
		m_pPlayerInfo = NULL;

		if( pplayer->weaponmodel )
		{
			cl_entity_t	saveent = *RI.currententity;
			model_t		*pweaponmodel = CL_ModelHandle( pplayer->weaponmodel );

			m_pStudioHeader = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, pweaponmodel );

			R_StudioMergeBones( RI.currententity, pweaponmodel );
			R_StudioSetupLighting( &lighting );
			R_StudioRenderModel( );
			R_StudioCalcAttachments( );

			*RI.currententity = saveent;
		}
	}

	return 1;
}

/*
===============
R_StudioDrawModel

===============
*/
static int R_StudioDrawModel( int flags )
{
	alight_t	lighting;
	vec3_t	dir;

	if( RI.currententity->curstate.renderfx == kRenderFxDeadPlayer )
	{
		if( RI.currententity->curstate.renderamt <= 0 ||
			RI.currententity->curstate.renderamt > gp_cl->maxclients )
			return 0;

		// get copy of player
		entity_state_t deadplayer = *R_StudioGetPlayerState( RI.currententity->curstate.renderamt - 1 );

		// clear weapon, movement state
		deadplayer.number = RI.currententity->curstate.renderamt;
		deadplayer.weaponmodel = 0;
		deadplayer.gaitsequence = 0;

		deadplayer.movetype = MOVETYPE_NONE;
		VectorCopy( RI.currententity->curstate.angles, deadplayer.angles );
		VectorCopy( RI.currententity->curstate.origin, deadplayer.origin );

		g_studio.interpolate = false;
		int result = R_StudioDrawPlayer( flags, &deadplayer ); // draw as though it were a player
		g_studio.interpolate = true;

		return result;
	}

	R_StudioSetHeader((studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel ));

	R_StudioSetUpTransform( RI.currententity );

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !R_StudioCheckBBox( ))
			return 0;

		r_stats.c_studio_models_drawn++;
		g_studio.framecount++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	if( RI.currententity->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( RI.currententity, RI.currentmodel );
	else R_StudioSetupBones( RI.currententity );

	R_StudioSaveBones();

	if( flags & STUDIO_EVENTS )
	{
		R_StudioCalcAttachments( );
		R_StudioClientEvents( );

		// copy attachments into global entity array
		if( RI.currententity->index > 0 )
		{
			cl_entity_t *ent = CL_GetEntityByIndex( RI.currententity->index );
			memcpy( ent->attachment, RI.currententity->attachment, sizeof( vec3_t ) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		lighting.plightvec = dir;
		R_EntityDynamicLight( RI.currententity, &lighting, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );

		R_StudioEntityLight( &lighting );

		// model and frame independant
		R_StudioSetupLighting( &lighting );

		// get remap colors
		g_nTopColor = RI.currententity->curstate.colormap & 0xFF;
		g_nBottomColor = (RI.currententity->curstate.colormap & 0xFF00) >> 8;

		R_StudioSetRemapColors( g_nTopColor, g_nBottomColor );

		R_StudioRenderModel();
	}

	return 1;
}

/*
=================
R_StudioDrawModelInternal
=================
*/
static void R_StudioDrawModelInternal( cl_entity_t *e, int flags )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, &e->curstate );
		else R_StudioDrawModel( flags );
	}
	else if( unlikely( r_studio_builtin_renderer.value ))
	{
		if( e->player )
			R_StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else R_StudioDrawModel( flags );
	}
	else
	{
		// select the properly method
		if( e->player )
			pStudioDraw->StudioDrawPlayer( flags, R_StudioGetPlayerState( e->index - 1 ));
		else pStudioDraw->StudioDrawModel( flags );
	}
}

static cl_entity_t *R_FindParentEntity( cl_entity_t *e, cl_entity_t **entities, uint num_entities )
{
	for( uint i = 0; i < num_entities; i++ )
	{
		if( entities[i]->index == e->curstate.aiment )
			return entities[i];
	}

	return NULL;
}

/*
=================
R_DrawStudioModel
=================
*/
void R_DrawStudioModel( cl_entity_t *e )
{
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		return;

	R_StudioSetupTimings();

	if( e->player )
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
	else if( e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );

		if( !parent || !parent->model || parent->model->type != mod_studio )
			return;

		parent = R_FindParentEntity( e, tr.draw_list->solid_entities, tr.draw_list->num_solid_entities );

		if( !parent )
			parent = R_FindParentEntity( e, tr.draw_list->trans_entities, tr.draw_list->num_trans_entities );

		if( parent )
		{
			RI.currententity = parent;
			R_StudioDrawModelInternal( RI.currententity, 0 );
			VectorCopy( RI.currententity->curstate.origin, e->curstate.origin );
			VectorCopy( RI.currententity->origin, e->origin );
			RI.currententity = e;

			R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
		}
	}
	else
	{
		R_StudioDrawModelInternal( e, STUDIO_RENDER|STUDIO_EVENTS );
	}
}

/*
=================
R_RunViewmodelEvents
=================
*/
void R_RunViewmodelEvents( void )
{
	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	// ignore in thirdperson, camera view or client is died
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	RI.currententity = tr.viewent;

	if( !RI.currententity->model || RI.currententity->model->type != mod_studio )
		return;

	R_StudioSetupTimings();

	vec3_t simorg = Vec3( gp_cl->simorg );
	for( int i = 0; i < 4; i++ )
		VectorCopy( simorg, RI.currententity->attachment[i] );
	RI.currentmodel = RI.currententity->model;

	R_StudioDrawModelInternal( RI.currententity, STUDIO_EVENTS );
}

/*
=================
R_DrawViewModel
=================
*/
void R_DrawViewModel( void )
{
	cl_entity_t	*view = tr.viewent;

	R_GatherPlayerLight( view );

	if( r_drawviewmodel->value == 0 )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ))
		return;

	// ignore in thirdperson, camera view or client is died
	if( FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer())
		return;

	tr.blend = CL_FxBlend( view ) / 255.0f;
	if( !R_ModelOpaque( view->curstate.rendermode ) && tr.blend <= 0.0f )
		return; // invisible ?

	RI.currententity = view;

	if( !RI.currententity->model )
		return;

	// adjust the depth range to prevent view model from poking into walls
	pglDepthRange( gldepthmin, gldepthmin + VIEWMODEL_DEPTH_RANGE * ( gldepthmax - gldepthmin ));
	RI.currentmodel = RI.currententity->model;

	switch( RI.currententity->model->type )
	{
	case mod_alias:
		R_DrawAliasModel( RI.currententity );
		break;
	case mod_studio:
		R_StudioSetupTimings();
		R_StudioDrawModelInternal( RI.currententity, STUDIO_RENDER );
		break;
	}

	// restore depth range
	pglDepthRange( gldepthmin, gldepthmax );
}

/*
====================
R_StudioLoadTexture

load model texture with unique name
====================
*/
static void R_StudioLoadTexture( model_t *mod, studiohdr_t *phdr, mstudiotexture_t *ptexture )
{
	int		flags = 0;
	char		texname[128], name[128], mdlname[128];
	texture_t		*tx = NULL;
	qboolean load_external = false;

	if( ptexture->flags & STUDIO_NF_NORMALMAP )
		flags |= (TF_NORMALMAP);

	// store some textures for remapping
	if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ) || !Q_strnicmp( ptexture->name, "remap", 5 ))
	{
		char	val[6];

		int i = mod->numtextures;
		mod->textures = (texture_t **)Mem_Realloc( mod->mempool, mod->textures, ( i + 1 ) * sizeof( texture_t* ));
		int size = ptexture->width * ptexture->height + 768;
		tx = Mem_Calloc( mod->mempool, sizeof( *tx ) + size );
		mod->textures[i] = tx;

		// store ranges into anim_min, anim_max etc
		if( !Q_strnicmp( ptexture->name, "DM_Base", 7 ))
		{
			Q_strncpy( tx->name, "DM_Base", sizeof( tx->name ));
			tx->anim_min = PLATE_HUE_START; // topcolor start
			tx->anim_max = PLATE_HUE_END; // topcolor end
			// bottomcolor start always equal is (topcolor end + 1)
			tx->anim_total = SUIT_HUE_END;// bottomcolor end
		}
		else
		{
			Q_strncpy( tx->name, "DM_User", sizeof( tx->name )); // custom remapped
			Q_strncpy( val, ptexture->name + 7, 4 );
			tx->anim_min = bound( 0, Q_atoi( val ), 255 ); // topcolor start
			Q_strncpy( val, ptexture->name + 11, 4 );
			tx->anim_max = bound( 0, Q_atoi( val ), 255 ); // topcolor end
			// bottomcolor start always equal is (topcolor end + 1)
			Q_strncpy( val, ptexture->name + 15, 4 );
			tx->anim_total = bound( 0, Q_atoi( val ), 255 ); // bottomcolor end
		}

		tx->width = ptexture->width;
		tx->height = ptexture->height;

		// the pixels immediately follow the structures
		byte *pixels = (byte *)phdr + ptexture->index;
		memcpy( tx+1, pixels, size );

		ptexture->flags |= STUDIO_NF_COLORMAP;	// yes, this is colormap image
		flags |= TF_FORCE_COLOR;

		mod->numtextures++;	// done
	}

	Q_strncpy( mdlname, mod->name, sizeof( mdlname ));
	COM_FileBase( ptexture->name, name, sizeof( name ));
	COM_StripExtension( mdlname );

	if( FBitSet( ptexture->flags, STUDIO_NF_NOMIPS ))
		SetBits( flags, TF_NOMIPMAP );

	if( FBitSet( gp_host->features, ENGINE_IMPROVED_LINETRACE ) && FBitSet( ptexture->flags, STUDIO_NF_MASKED ))
		flags |= TF_KEEP_SOURCE; // Paranoia2 texture alpha-tracing

	// NOTE: colormaps must have the palette for properly work. Ignore them
	if( Mod_AllowMaterials( ) && !FBitSet( ptexture->flags, STUDIO_NF_COLORMAP ))
	{
		if( R_SearchForTextureReplacement( texname, sizeof( texname ), mdlname, "materials/%s/%s.tga", mdlname, name ))
		{
			int gl_texturenum = GL_LoadTexture( texname, NULL, 0, flags );
			R_TextureReplacementReport( mdlname, gl_texturenum, texname );
			if(( load_external = gl_texturenum != 0 ))
				ptexture->index = gl_texturenum;
		}
	}

	if( !load_external )
	{
		// NOTE: replace index with pointer to start of imagebuffer, ImageLib expected it
		gEngfuncs.Image_SetMDLPointer((byte *)phdr + ptexture->index);
		size_t size = sizeof( mstudiotexture_t ) + ptexture->width * ptexture->height + 768;

		// build the texname
		Q_snprintf( texname, sizeof( texname ), "#%s/%s.mdl", mdlname, name );
		ptexture->index = GL_LoadTexture( texname, (byte *)ptexture, size, flags );
	}

	if( !ptexture->index )
	{
		ptexture->index = tr.defaultTexture;
	}
	else if( tx )
	{
		// duplicate texnum for easy acess
		tx->gl_texturenum = ptexture->index;
	}
}

/*
=================
Mod_StudioLoadTextures
=================
*/
void Mod_StudioLoadTextures( model_t *mod, void *data )
{
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);
	if( phdr->textureindex > 0 )
	{
		for( int i = 0; i < phdr->numtextures; i++ )
			R_StudioLoadTexture( mod, phdr, &ptexture[i] );
	}
}

/*
=================
Mod_StudioUnloadTextures
=================
*/
void Mod_StudioUnloadTextures( void *data )
{
	studiohdr_t	*phdr = (studiohdr_t *)data;

	if( !phdr )
		return;

	mstudiotexture_t *ptexture = (mstudiotexture_t *)(((byte *)phdr) + phdr->textureindex);

	// release all textures
	for( int i = 0; i < phdr->numtextures; i++ )
	{
		if( ptexture[i].index == tr.defaultTexture )
			continue;
		GL_FreeTexture( ptexture[i].index );
	}
}

static void pfnStudioDynamicLight( cl_entity_t *ent, alight_t *plight )
{
	R_EntityDynamicLight( ent, plight, FBitSet( RI.rvp.flags, RF_DRAW_WORLD ), g_studio.time, g_studio.lightspot, g_studio.lightvec );
}

qboolean R_StudioFillAPI( engine_studio_api_t *api, r_studio_interface_t *pDefaultDraw )
{
	cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );

	api->GetCurrentEntity        = pfnGetCurrentEntity;
	api->PlayerInfo              = pfnPlayerInfo;
	api->GetPlayerState          = R_StudioGetPlayerState;
	api->GetTimes                = pfnGetEngineTimes;
	api->GetViewInfo             = pfnGetViewInfo;
	api->GetModelCounters        = pfnGetModelCounters;
	api->StudioGetBoneTransform  = pfnStudioGetBoneTransform;
	api->StudioGetLightTransform = pfnStudioGetLightTransform;
	api->StudioGetRotationMatrix = pfnStudioGetRotationMatrix;
	api->StudioSetupModel        = R_StudioSetupModel;
	api->StudioCheckBBox         = R_StudioCheckBBox;
	api->StudioDynamicLight      = pfnStudioDynamicLight;
	api->StudioEntityLight       = R_StudioEntityLight;
	api->StudioSetupLighting     = R_StudioSetupLighting;
	api->StudioDrawPoints        = R_StudioDrawPoints;
	api->StudioDrawHulls         = R_StudioDrawHulls;
	api->StudioDrawAbsBBox       = R_StudioDrawAbsBBox;
	api->StudioDrawBones         = R_StudioDrawBones;
	api->StudioSetupSkin         = (void *)R_StudioSetupSkin;
	api->StudioSetRemapColors    = R_StudioSetRemapColors;
	api->SetupPlayerModel        = R_StudioSetupPlayerModel;
	api->StudioClientEvents      = R_StudioClientEvents;
	api->GetForceFaceFlags       = R_StudioGetForceFaceFlags;
	api->SetForceFaceFlags       = R_StudioSetForceFaceFlags;
	api->StudioSetHeader         = (void *)R_StudioSetHeader;
	api->SetRenderModel          = R_StudioSetRenderModel;
	api->SetupRenderer           = R_StudioSetupRenderer;
	api->RestoreRenderer         = R_StudioRestoreRenderer;
	api->SetChromeOrigin         = R_StudioSetChromeOrigin;
	api->GL_StudioDrawShadow     = GL_StudioDrawShadow;
	api->GL_SetRenderMode        = GL_StudioSetRenderMode;
	api->StudioDecalShoot         = R_StudioDecalShoot;
	api->StudioClearDecals        = R_StudioClearRagdollDecals;
	api->StudioSetRenderamt      = R_StudioSetRenderamt;
	api->StudioSetCullState      = R_StudioSetCullState;
	api->StudioRenderShadow      = R_StudioRenderShadow;

	pDefaultDraw->version         = STUDIO_INTERFACE_VERSION;
	pDefaultDraw->StudioDrawModel  = R_StudioDrawModel;
	pDefaultDraw->StudioDrawPlayer = R_StudioDrawPlayer;

	return true;
}

void R_StudioSetDrawInterface( r_studio_interface_t *pDraw )
{
	pStudioDraw = pDraw;
}
