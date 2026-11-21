/*
gl_warp.c - sky and water polygons
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
#include "wadfile.h"

#define SKYCLOUDS_QUALITY	12
#define MAX_CLIP_VERTS	128 // skybox clip vertices

static const vec3_t skyclip[SKYBOX_MAX_SIDES] =
{
{  1,  1,  0 },
{  1, -1,  0 },
{  0, -1,  1 },
{  0,  1,  1 },
{  1,  0,  1 },
{ -1,  0,  1 }
};

// 1 = s, 2 = t, 3 = 2048
static const int st_to_vec[SKYBOX_MAX_SIDES][3] =
{
{  3, -1,  2 },
{ -3,  1,  2 },
{  1,  3,  2 },
{ -1, -3,  2 },
{ -2, -1,  3 },  // 0 degrees yaw, look straight up
{  2, -1, -3 }   // look straight down
};

#define RIPPLES_CACHEWIDTH_BITS 7
#define RIPPLES_CACHEWIDTH ( 1 << RIPPLES_CACHEWIDTH_BITS )
#define RIPPLES_CACHEWIDTH_MASK (( RIPPLES_CACHEWIDTH ) - 1 )
#define RIPPLES_TEXSIZE ( RIPPLES_CACHEWIDTH * RIPPLES_CACHEWIDTH )

STATIC_ASSERT( RIPPLES_TEXSIZE == 0x4000, "fix the algorithm to work with custom resolution" );

typedef struct ripple_field_s
{
	float height[RIPPLES_TEXSIZE];
	float velocityX[RIPPLES_TEXSIZE];
	float velocityY[RIPPLES_TEXSIZE];
} ripple_field_t;

#define MAX_WATER_RIPPLE_IMPACTS 8

typedef struct water_ripple_impact_s
{
	vec3_t origin;
	double startTime;
	float strength;
	qboolean active;
} water_ripple_impact_t;

static struct
{
	double time;
	double accumulator;

	ripple_field_t state[2];
	int current;
	qboolean update;

	water_ripple_impact_t impacts[MAX_WATER_RIPPLE_IMPACTS];
	unsigned int nextImpact;

	uint32_t texture[RIPPLES_TEXSIZE];
} g_ripple;


static void DrawSkyPolygon( int nump, vec3_t vecs )
{
	// decide which face it maps to
	vec3_t v;
	VectorClear( v );

	float *vp = vecs;
	for( int i = 0; i < nump; i++, vp += 3 )
		VectorAdd( vp, v, v );

	const int axis = R_SkyboxAxisFromDir( v );

	// project new texture coords
	for( int i = 0; i < nump; i++, vecs += 3 )
	{
		float s, t;

		if( !R_SkyboxProject( vecs, axis, &s, &t ))
			continue;

		if( s < RI.skyMins[0][axis] ) RI.skyMins[0][axis] = s;
		if( t < RI.skyMins[1][axis] ) RI.skyMins[1][axis] = t;
		if( s > RI.skyMaxs[0][axis] ) RI.skyMaxs[0][axis] = s;
		if( t > RI.skyMaxs[1][axis] ) RI.skyMaxs[1][axis] = t;
	}
}

/*
==============
ClipSkyPolygon
==============
*/
static void ClipSkyPolygon( int nump, vec3_t vecs, int stage )
{
	const float	*norm;
	float		*v, d, e;
	qboolean		front, back;
	float		dists[MAX_CLIP_VERTS + 1];
	int		sides[MAX_CLIP_VERTS + 1];
	vec3_t		newv[2][MAX_CLIP_VERTS + 1];
	int		newc[2];
	int		i, j;

	if( nump > MAX_CLIP_VERTS )
		gEngfuncs.Host_Error( "%s: MAX_CLIP_VERTS\n", __func__ );
loc1:
	if( stage == 6 )
	{
		// fully clipped, so draw it
		DrawSkyPolygon( nump, vecs );
		return;
	}

	front = back = false;
	norm = skyclip[stage];
	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		d = DotProduct( v, norm );
		if( d > ON_EPSILON )
		{
			front = true;
			sides[i] = SIDE_FRONT;
		}
		else if( d < -ON_EPSILON )
		{
			back = true;
			sides[i] = SIDE_BACK;
		}
		else
		{
			sides[i] = SIDE_ON;
		}
		dists[i] = d;
	}

	if( !front || !back )
	{
		// not clipped
		stage++;
		goto loc1;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy( vecs, ( vecs + ( i * 3 )));
	newc[0] = newc[1] = 0;

	for( i = 0, v = vecs; i < nump; i++, v += 3 )
	{
		switch( sides[i] )
		{
		case SIDE_FRONT:
			VectorCopy( v, newv[0][newc[0]] );
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy( v, newv[1][newc[1]] );
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy( v, newv[0][newc[0]] );
			newc[0]++;
			VectorCopy( v, newv[1][newc[1]] );
			newc[1]++;
			break;
		}

		if( sides[i] == SIDE_ON || sides[i+1] == SIDE_ON || sides[i+1] == sides[i] )
			continue;

		d = dists[i] / ( dists[i] - dists[i+1] );
		for( j = 0; j < 3; j++ )
		{
			e = v[j] + d * ( v[j+3] - v[j] );
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	ClipSkyPolygon( newc[0], newv[0][0], stage + 1 );
	ClipSkyPolygon( newc[1], newv[1][0], stage + 1 );
}

static void MakeSkyVec( float s, float t, int axis )
{
	int farclip = RI.farClip;

	vec3_t b;
	b[0] = s * (farclip >> 1);
	b[1] = t * (farclip >> 1);
	b[2] = (farclip >> 1);

	vec3_t v;
	for( int j = 0; j < 3; j++ )
	{
		int k = st_to_vec[axis][j];
		v[j] = (k < 0) ? -b[-k-1] : b[k-1];
		v[j] += RI.cullorigin[j];
	}

	// avoid bilerp seam
	s = (s + 1.0f) * 0.5f;
	t = (t + 1.0f) * 0.5f;

	if( GL_Support( GL_CLAMPTOEDGE_EXT ))
	{
		s = bound( 0.0f, s, 1.0f );
		t = bound( 0.0f, t, 1.0f );
	}
	else
	{
		s = bound( 1.0f / 512.0f, s, 511.0f / 512.0f );
		t = bound( 1.0f / 512.0f, t, 511.0f / 512.0f );
	}

	t = 1.0f - t;

	pglTexCoord2f( s, t );
	pglVertex3fv( v );
}

/*
==============
R_ClearSkyBox
==============
*/
void R_ClearSkyBox( void )
{
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		RI.skyMins[0][i] = RI.skyMins[1][i] = 9999999.0f;
		RI.skyMaxs[0][i] = RI.skyMaxs[1][i] = -9999999.0f;
	}
}

/*
=================
R_AddSkyBoxSurface
=================
*/
void R_AddSkyBoxSurface( msurface_t *fa )
{
	if( FBitSet( tr.world->flags, FWORLD_SKYSPHERE ) && fa->polys && !FBitSet( tr.world->flags, FWORLD_CUSTOM_SKYBOX ))
	{
		glpoly2_t *p = fa->polys;

		// draw the sky poly
		pglBegin( GL_POLYGON );
		float *v = p->verts[0];
		for( int i = 0; i < p->numverts; i++, v += VERTEXSIZE )
		{
			pglTexCoord2f( v[3], v[4] );
			pglVertex3fv( v );
		}
		pglEnd ();
	}

	// calculate vertex values for sky box
	vec3_t verts[MAX_CLIP_VERTS];
	for( glpoly2_t *p = fa->polys; p; p = p->next )
	{
		for( int i = 0; i < p->numverts; i++ )
			VectorSubtract( p->verts[i], RI.cullorigin, verts[i] );
		ClipSkyPolygon( p->numverts, verts[0], 0 );
	}
}

/*
==============
R_UnloadSkybox

Unload previous skybox
==============
*/
void R_UnloadSkybox( void )
{
	// release old skybox
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( !tr.skyboxTextures[i] ) continue;
		GL_FreeTexture( tr.skyboxTextures[i] );
	}

	tr.skyboxbasenum = SKYBOX_BASE_NUM;	// set skybox base (to let some mods load hi-res skyboxes)

	memset( tr.skyboxTextures, 0, sizeof( tr.skyboxTextures ));
	ClearBits( tr.world->flags, FWORLD_CUSTOM_SKYBOX );
}

/*
==============
R_DrawSkybox
==============
*/
void R_DrawSkyBox( void )
{
	// don't fogging skybox (this fix old Half-Life bug)
	if( !RI.fogSkybox ) R_AllowFog( false );

	if( RI.fogEnabled )
		pglFogf( GL_FOG_DENSITY, RI.fogDensity * 0.5f );

	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( RI.skyMins[0][i] >= RI.skyMaxs[0][i] || RI.skyMins[1][i] >= RI.skyMaxs[1][i] )
			continue;

		if( tr.skyboxTextures[r_skyTexOrder[i]] )
			GL_Bind( XASH_TEXTURE0, tr.skyboxTextures[r_skyTexOrder[i]] );
		else GL_Bind( XASH_TEXTURE0, tr.grayTexture ); // stub

		pglBegin( GL_QUADS );
		MakeSkyVec( RI.skyMins[0][i], RI.skyMins[1][i], i );
		MakeSkyVec( RI.skyMins[0][i], RI.skyMaxs[1][i], i );
		MakeSkyVec( RI.skyMaxs[0][i], RI.skyMaxs[1][i], i );
		MakeSkyVec( RI.skyMaxs[0][i], RI.skyMins[1][i], i );
		pglEnd();
	}

	if( !RI.fogSkybox )
		R_AllowFog( true );

	if( RI.fogEnabled )
		pglFogf( GL_FOG_DENSITY, RI.fogDensity );

	R_LoadIdentity();
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================
/*
==============
R_CloudVertex
==============
*/
static void R_CloudVertex( float s, float t, int axis, vec3_t v )
{
	int farclip = RI.farClip;

	vec3_t b;
	b[0] = s * (farclip >> 1);
	b[1] = t * (farclip >> 1);
	b[2] = (farclip >> 1);

	for( int j = 0; j < 3; j++ )
	{
		int k = st_to_vec[axis][j];
		v[j] = (k < 0) ? -b[-k-1] : b[k-1];
		v[j] += RI.cullorigin[j];
	}
}

/*
=============
R_CloudTexCoord
=============
*/
static void R_CloudTexCoord( const vec3_t v, float speed, float *s, float *t )
{
	float speedscale = gp_cl->time * speed;
	speedscale -= (int)speedscale & ~127;

	vec3_t dir;
	VectorSubtract( v, RI.rvp.vieworigin, dir );
	dir[2] *= 3.0f; // flatten the sphere

	float length = VectorLength( dir );
	length = 6.0f * 63.0f / length;

	*s = ( speedscale + dir[0] * length ) * (1.0f / 128.0f);
	*t = ( speedscale + dir[1] * length ) * (1.0f / 128.0f);
}

/*
===============
R_CloudDrawPoly
===============
*/
static void R_CloudDrawPoly( const float *verts )
{
	GL_SetRenderMode( kRenderNormal );
	GL_Bind( XASH_TEXTURE0, tr.solidskyTexture );

	pglBegin( GL_QUADS );
	const float *v = verts;
	for( int i = 0; i < 4; i++, v += VERTEXSIZE )
	{
		float s, t;
		R_CloudTexCoord( v, 8.0f, &s, &t );
		pglTexCoord2f( s, t );
		pglVertex3fv( v );
	}
	pglEnd();

	GL_SetRenderMode( kRenderTransTexture );
	GL_Bind( XASH_TEXTURE0, tr.alphaskyTexture );

	pglBegin( GL_QUADS );
	v = verts;
	for( int i = 0; i < 4; i++, v += VERTEXSIZE )
	{
		float s, t;
		R_CloudTexCoord( v, 16.0f, &s, &t );
		pglTexCoord2f( s, t );
		pglVertex3fv( v );
	}
	pglEnd();

	pglDisable( GL_BLEND );
}

/*
==============
R_CloudRenderSide
==============
*/
static void R_CloudRenderSide( int axis )
{
	vec3_t verts[4];
	R_CloudVertex( -1.0f, -1.0f, axis, verts[0] );
	R_CloudVertex( -1.0f,  1.0f, axis, verts[1] );
	R_CloudVertex(  1.0f,  1.0f, axis, verts[2] );
	R_CloudVertex(  1.0f, -1.0f, axis, verts[3] );

	vec3_t vup, vright;
	VectorSubtract( verts[2], verts[3], vup );
	VectorSubtract( verts[2], verts[1], vright );

	float di = SKYCLOUDS_QUALITY;
	float qi = 1.0f / di;
	float dj = (axis < 4) ? di * 2 : di; //subdivide vertically more than horizontally on skybox sides
	float qj = 1.0f / dj;

	for( int i = 0; i < di; i++ )
	{
		for( int j = 0; j < dj; j++ )
		{
			if( i * qi < RI.skyMins[0][axis] / 2 + 0.5f - qi
			 || i * qi > RI.skyMaxs[0][axis] / 2 + 0.5f
			 || j * qj < RI.skyMins[1][axis] / 2 + 0.5f - qj
			 || j * qj > RI.skyMaxs[1][axis] / 2 + 0.5f )
				continue;

			vec3_t temp, temp2;
			VectorScale( vright, qi * i, temp );
			VectorScale( vup, qj * j, temp2 );
			VectorAdd( temp, temp2, temp );
			float final_verts[4][VERTEXSIZE];
			VectorAdd( verts[0], temp, final_verts[0] );

			VectorScale( vup, qj, temp );
			VectorAdd( final_verts[0], temp, final_verts[1] );

			VectorScale( vright, qi, temp );
			VectorAdd( final_verts[1], temp, final_verts[2] );

			VectorAdd( final_verts[0], temp, final_verts[3] );

			R_CloudDrawPoly( final_verts[0] );
		}
	}
}

/*
==============
R_DrawClouds

Quake-style clouds
==============
*/
void R_DrawClouds( void )
{
	if( RI.fogEnabled )
		pglFogf( GL_FOG_DENSITY, RI.fogDensity * 0.25f );
	pglDepthFunc( GL_GEQUAL );
	pglDepthMask( GL_FALSE );

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		if( RI.skyMins[0][i] >= RI.skyMaxs[0][i] || RI.skyMins[1][i] >= RI.skyMaxs[1][i] )
			continue;
		R_CloudRenderSide( i );
	}

	pglDepthFunc( GL_LEQUAL );
	pglDepthMask( GL_TRUE );

	if( RI.fogEnabled )
		pglFogf( GL_FOG_DENSITY, RI.fogDensity );
}

/*
============================================================

	HALF-LIFE SOFTWARE WATER

https://web.archive.org/web/20160418004149/http://freespace.virgin.net/hugo.elias/graphics/x_water.htm

============================================================
*/
void R_ResetRipples( void )
{
	g_ripple.current = 0;
	g_ripple.time = gp_cl->time - 0.1;
	g_ripple.accumulator = 0.0;
	g_ripple.update = false;
	g_ripple.nextImpact = 0;
	memset( g_ripple.state, 0, sizeof( g_ripple.state ));
	memset( g_ripple.impacts, 0, sizeof( g_ripple.impacts ));
}

/*
================
R_AddRippleImpulse

Inject a soft, radially symmetric footstep depression into the world-space
surface field. A compensating outer ring keeps the total displacement near
zero, avoiding a gradual drift of the periodic simulation.
================
*/
void R_AddRippleImpulse( const vec3_t origin, float strength )
{
	water_ripple_impact_t *impact;

	if( r_ripple.value < 2.0f || !origin || strength <= 0.01f )
		return;

	// Store a compact contact event. The renderer turns it into an expanding
	// circular wave packet when it samples the surface, rather than injecting
	// one static disturbance into a repeating simulation grid.
	impact = &g_ripple.impacts[g_ripple.nextImpact++ & ( MAX_WATER_RIPPLE_IMPACTS - 1 )];
	VectorCopy( origin, impact->origin );
	impact->startTime = gp_cl->time;
	impact->strength = bound( 0.0f, strength / 320.0f, 1.0f );
	impact->active = true;
}
/*
================
R_RunShallowWaterStep

Linearized 2D shallow-water equations on a periodic grid:

	height_t = -divergence(velocity)
	velocity_t = -gravity * gradient(height) + viscosity * laplacian(velocity)

The velocity field is updated first, then height uses the new velocity
(symplectic Euler). This is a small, fixed-cost surface-wave approximation;
it is not a full 3D fluid or free-surface solver.
================
*/
static void R_RunShallowWaterStep( float dt )
{
	const ripple_field_t *src = &g_ripple.state[g_ripple.current];
	ripple_field_t *dst = &g_ripple.state[g_ripple.current ^ 1];
	float gravity = r_ripple_gravity.value;
	float viscosity = r_ripple_viscosity.value;
	float dampingRate = r_ripple_damping.value;
	float damping;

	if( gravity < 0.0f ) gravity = 0.0f;
	if( gravity > 128.0f ) gravity = 128.0f;
	if( viscosity < 0.0f ) viscosity = 0.0f;
	if( viscosity > 1.0f ) viscosity = 1.0f;
	if( dampingRate < 0.0f ) dampingRate = 0.0f;
	if( dampingRate > 10.0f ) dampingRate = 10.0f;

	damping = 1.0f / ( 1.0f + dampingRate * dt );

	// First pass: update horizontal velocity from the surface-height gradient.
	for( int y = 0; y < RIPPLES_CACHEWIDTH; y++ )
	{
		const int row = y * RIPPLES_CACHEWIDTH;
		const int upRow = (( y - 1 ) & RIPPLES_CACHEWIDTH_MASK) * RIPPLES_CACHEWIDTH;
		const int downRow = (( y + 1 ) & RIPPLES_CACHEWIDTH_MASK) * RIPPLES_CACHEWIDTH;

		for( int x = 0; x < RIPPLES_CACHEWIDTH; x++ )
		{
			const int index = row + x;
			const int left = row + (( x - 1 ) & RIPPLES_CACHEWIDTH_MASK);
			const int right = row + (( x + 1 ) & RIPPLES_CACHEWIDTH_MASK);
			const int up = upRow + x;
			const int down = downRow + x;

			const float heightGradientX = ( src->height[right] - src->height[left] ) * 0.5f;
			const float heightGradientY = ( src->height[down] - src->height[up] ) * 0.5f;

			const float laplacianX =
				src->velocityX[left] + src->velocityX[right] +
				src->velocityX[up] + src->velocityX[down] -
				4.0f * src->velocityX[index];
			const float laplacianY =
				src->velocityY[left] + src->velocityY[right] +
				src->velocityY[up] + src->velocityY[down] -
				4.0f * src->velocityY[index];

			dst->velocityX[index] = ( src->velocityX[index] -
				gravity * dt * heightGradientX + viscosity * dt * laplacianX ) * damping;
			dst->velocityY[index] = ( src->velocityY[index] -
				gravity * dt * heightGradientY + viscosity * dt * laplacianY ) * damping;
		}
	}

	// Second pass: update surface height from the new velocity divergence.
	for( int y = 0; y < RIPPLES_CACHEWIDTH; y++ )
	{
		const int row = y * RIPPLES_CACHEWIDTH;
		const int upRow = (( y - 1 ) & RIPPLES_CACHEWIDTH_MASK) * RIPPLES_CACHEWIDTH;
		const int downRow = (( y + 1 ) & RIPPLES_CACHEWIDTH_MASK) * RIPPLES_CACHEWIDTH;

		for( int x = 0; x < RIPPLES_CACHEWIDTH; x++ )
		{
			const int index = row + x;
			const int left = row + (( x - 1 ) & RIPPLES_CACHEWIDTH_MASK);
			const int right = row + (( x + 1 ) & RIPPLES_CACHEWIDTH_MASK);
			const int up = upRow + x;
			const int down = downRow + x;

			const float divergence =
				( dst->velocityX[right] - dst->velocityX[left] +
				  dst->velocityY[down] - dst->velocityY[up] ) * 0.5f;

			dst->height[index] = src->height[index] - dt * divergence;
		}
	}

	g_ripple.current ^= 1;
}

/*
================
R_SampleRipple

Sample the simulated surface in world space. Unlike the legacy upload path,
this keeps the wave field anchored to the map instead of repeating once per
water texture tile.
================
*/
typedef struct water_wave_s
{
	float directionX;
	float directionY;
	float wavelength;
	float amplitude;
	float phase;
} water_wave_t;

// Four broad geometric waves, following the GPU Gems water model. Their
// wavelengths remain comfortably larger than the GoldSrc water mesh spacing.
static const water_wave_t g_waterWaves[] =
{
	{  0.92f,  0.39f, 384.0f, 2.8f, 0.0f },
	{  0.31f, -0.95f, 288.0f, 1.9f, 1.7f },
	{ -0.77f,  0.64f, 216.0f, 1.25f, 3.1f },
	{  0.58f,  0.81f, 480.0f, 1.6f, 4.4f }
};

// Finer waves contribute surface slope and texture distortion, not geometry.
// This is the fixed-function approximation of the chapter's dynamic normal map.
static const water_wave_t g_waterDetailWaves[] =
{
	{  0.80f,  0.60f, 56.0f, 0.25f, 0.2f },
	{ -0.60f,  0.80f, 42.0f, 0.18f, 2.3f },
	{  0.28f, -0.96f, 34.0f, 0.14f, 1.1f },
	{ -0.94f, -0.34f, 72.0f, 0.22f, 4.0f },
	{  0.66f, -0.75f, 28.0f, 0.10f, 5.2f }
};

/*
================
R_SampleRipple

Evaluate a GPU Gems-inspired water surface in world space: broad directional
Gerstner-style waves for geometry, finer wave slopes for texture detail, and
short-lived circular wave packets for local water contacts.
================
*/
void R_SampleRipple( float worldX, float worldY, float *height, float *gradientX, float *gradientY, float *offsetX, float *offsetY )
{
	const float waveHeight = bound( 0.0f, r_ripple_waveheight.value, 4.0f );
	const float waveSpeed = bound( 0.1f, r_ripple_wavespeed.value, 4.0f );
	const float steepness = bound( 0.0f, r_ripple_steepness.value, 1.0f );
	const float gravity = 386.09f; // approximate GoldSrc units per second squared
	const float twoPi = 6.28318530718f;
	const float time = gp_cl->time;
	float h = 0.0f, gx = 0.0f, gy = 0.0f;
	float ox = 0.0f, oy = 0.0f;
	const int numWaves = sizeof( g_waterWaves ) / sizeof( g_waterWaves[0] );

	if( height ) *height = 0.0f;
	if( gradientX ) *gradientX = 0.0f;
	if( gradientY ) *gradientY = 0.0f;
	if( offsetX ) *offsetX = 0.0f;
	if( offsetY ) *offsetY = 0.0f;

	if( r_ripple.value < 2.0f || !height || !gradientX || !gradientY || !offsetX || !offsetY )
		return;

	// Broad geometric waves. The dispersion relation omega=sqrt(g*k) gives
	// longer waves a naturally faster crest speed than short waves.
	for( int i = 0; i < numWaves; i++ )
	{
		const water_wave_t *wave = &g_waterWaves[i];
		const float k = twoPi / wave->wavelength;
		const float omega = sqrtf( gravity * k ) * waveSpeed;
		const float phase = k * ( wave->directionX * worldX + wave->directionY * worldY ) -
			omega * time + wave->phase;
		const float sn = sinf( phase );
		const float cs = cosf( phase );
		const float amplitude = wave->amplitude * waveHeight;
		const float horizontal = ( steepness / ( k * numWaves )) * cs;

		h += amplitude * sn;
		gx += amplitude * k * wave->directionX * cs;
		gy += amplitude * k * wave->directionY * cs;
		ox += horizontal * wave->directionX;
		oy += horizontal * wave->directionY;
	}

	// Fine wave spectrum: affect the slope (and therefore the animated
	// material coordinates) without asking the coarse water mesh to represent it.
	for( int i = 0; i < (int)( sizeof( g_waterDetailWaves ) / sizeof( g_waterDetailWaves[0] )); i++ )
	{
		const water_wave_t *wave = &g_waterDetailWaves[i];
		const float k = twoPi / wave->wavelength;
		const float omega = sqrtf( gravity * k ) * waveSpeed * 1.25f;
		const float phase = k * ( wave->directionX * worldX + wave->directionY * worldY ) -
			omega * time + wave->phase;
		const float cs = cosf( phase );
		const float amplitude = wave->amplitude * waveHeight;

		gx += amplitude * k * wave->directionX * cs;
		gy += amplitude * k * wave->directionY * cs;
	}

	// Footsteps add expanding circular packets. Each packet is a few
	// concentric crests travelling away from the contact point.
	for( int i = 0; i < MAX_WATER_RIPPLE_IMPACTS; i++ )
	{
		const water_ripple_impact_t *impact = &g_ripple.impacts[i];
		const float age = (float)( time - impact->startTime );
		const float dx = worldX - impact->origin[0];
		const float dy = worldY - impact->origin[1];
		const float distanceSquared = dx * dx + dy * dy;
		float distance, ringSpeed, delta, phase, envelope, amplitude;
		float wave, radialGradient;

		if( !impact->active || age < 0.0f || age > 3.5f )
			continue;

		ringSpeed = 96.0f * waveSpeed;
		delta = sqrtf( distanceSquared ) - ringSpeed * age;
		if( delta < -112.0f || delta > 112.0f )
			continue;

		distance = sqrtf( distanceSquared );
		phase = ( twoPi / 44.0f ) * delta;
		envelope = expf( -( delta * delta ) / ( 2.0f * 56.0f * 56.0f )) * expf( -age / 2.4f );
		amplitude = 2.5f * impact->strength * waveHeight;
		wave = amplitude * envelope * sinf( phase );
		radialGradient = amplitude * envelope *
			(( twoPi / 44.0f ) * cosf( phase ) - delta / ( 56.0f * 56.0f ) * sinf( phase ));

		h += wave;
		if( distance > 0.001f )
		{
			gx += radialGradient * dx / distance;
			gy += radialGradient * dy / distance;
		}
	}

	*height = h;
	*gradientX = gx;
	*gradientY = gy;
	*offsetX = ox;
	*offsetY = oy;
}

void R_AnimateRipples( void )
{
	double frametime = gp_cl->time - g_ripple.time;
	float stepTime = r_ripple_updatetime.value;
	int steps = 0;

	g_ripple.update = false;
	g_ripple.time = gp_cl->time;

	if( r_ripple.value >= 2.0f )
	{
		for( int i = 0; i < MAX_WATER_RIPPLE_IMPACTS; i++ )
		{
			if( g_ripple.impacts[i].active && gp_cl->time - g_ripple.impacts[i].startTime > 3.5 )
				g_ripple.impacts[i].active = false;
		}
		return;
	}

	if( frametime < 0.0 )
	{
		g_ripple.accumulator = 0.0;
		return;
	}

	if( !r_ripple.value )
	{
		g_ripple.accumulator = 0.0;
		return;
	}

	// Keep the simulation timestep fixed and within the stable CFL range.
	if( stepTime < 0.01f ) stepTime = 0.01f;
	if( stepTime > 0.05f ) stepTime = 0.05f;

	// Avoid an expensive catch-up after a pause or a slow frame.
	if( frametime > 0.2 )
		frametime = 0.2;

	g_ripple.accumulator += frametime;
	if( g_ripple.accumulator > stepTime * 8.0 )
		g_ripple.accumulator = stepTime * 8.0;

	while( g_ripple.accumulator >= stepTime && steps < 8 )
	{
		R_RunShallowWaterStep( stepTime );
		g_ripple.accumulator -= stepTime;
		steps++;
	}

	if( steps > 0 )
		g_ripple.update = true;
}

static void R_GetRippleTextureSize( const texture_t *image, int *width, int *height )
{
	// try to preserve aspect ratio
	if( image->width > image->height )
	{
		*width = RIPPLES_CACHEWIDTH;
		*height = (float)image->height / image->width * RIPPLES_CACHEWIDTH;
	}
	else if( image->width < image->height )
	{
		*width = (float)image->width / image->height * RIPPLES_CACHEWIDTH;
		*height = RIPPLES_CACHEWIDTH;
	}
	else
	{
		*width = *height = RIPPLES_CACHEWIDTH;
	}
}

qboolean R_UploadRipples( texture_t *image )
{
	qboolean update = g_ripple.update;

	// Mode 2 keeps the original material texture bound. The shallow-water
	// field is sampled from world coordinates by EmitWaterPolys instead.
	if( r_ripple.value >= 2.0f )
	{
		GL_Bind( XASH_TEXTURE0, image->gl_texturenum );
		return true;
	}

	if( !r_ripple.value )
	{
		GL_Bind( XASH_TEXTURE0, image->gl_texturenum );
		return false;
	}

	// discard unuseful textures
	const gl_texture_t *glt = R_GetTexture( image->gl_texturenum );
	if( !glt || !glt->original || !glt->original->buffer )
	{
		GL_Bind( XASH_TEXTURE0, image->gl_texturenum );
		return false;
	}

	int width, height;
	if( !image->fb_texturenum )
	{
		rgbdata_t pic = { 0 };
		string name;

		Q_snprintf( name, sizeof( name ), "*rippletex_%s", image->name );
		R_GetRippleTextureSize( image, &width, &height );

		pic.width = width;
		pic.height = height;
		pic.depth = 1;
		pic.flags = IMAGE_HAS_COLOR;
		pic.buffer = (byte *)g_ripple.texture;
		pic.type = PF_RGBA_32;
		pic.size = width * height * 4;
		pic.numMips = 1;
		memset( pic.buffer, 0, pic.size );

		image->fb_texturenum = GL_LoadTextureInternal( name, &pic, TF_NOMIPMAP | TF_ALLOW_NEAREST );

		update = true;
		image->dt_texturenum = ( tr.framecount - 1 ) & 0xFFFF;
	}

	GL_Bind( XASH_TEXTURE0, image->fb_texturenum );

	// no updates this frame
	if( !update || image->dt_texturenum == ( tr.framecount & 0xFFFF ))
		return true;

	// prevent rendering texture multiple times in frame
	image->dt_texturenum = tr.framecount & 0xFFFF;

	R_GetRippleTextureSize( image, &width, &height );

	int size = r_ripple.value == 1.0f ? 64 : RIPPLES_CACHEWIDTH;
	const uint32_t *pixels = (const uint32_t *)glt->original->buffer;

	for( int y = 0; y < height; y++ )
	{
		int ry = (float)y / height * size;

		for( int x = 0; x < width; x++ )
		{
			int rx = (float)x / width * size;
			int val = (int)( g_ripple.state[g_ripple.current].height[ry * RIPPLES_CACHEWIDTH + rx] / 16.0f );

			// transform it to texture space and get nice tiling effect
			int rpy = ( y - val ) % height;
			int rpx = ( x + val ) % width;

			int py = (float)rpy / height * image->height;
			int px = (float)rpx / width * image->width;

			if( py < 0 ) py = image->height + py;
			if( px < 0 ) px = image->width + px;

			g_ripple.texture[y * width + x] = pixels[py * image->width + px];
		}
	}

	pglTexImage2D( GL_TEXTURE_2D, 0, glt->format, width, height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, g_ripple.texture );

	return true;
}
