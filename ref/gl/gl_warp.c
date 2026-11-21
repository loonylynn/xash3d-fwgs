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

#if !XASH_GLES && !XASH_GL_STATIC && !XASH_GL4ES
static void R_FreeWaterEnvironmentMap( void );
#endif

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
#if !XASH_GLES && !XASH_GL_STATIC && !XASH_GL4ES
	R_FreeWaterEnvironmentMap();
#endif
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
/*
================
R_SampleRippleImpact

Evaluate one outgoing circular gravity-wave packet from a contact point.

The radial direction is the circular-wave direction used by GPU Gems:
D = (P - C) / |P - C|. Deep-water dispersion gives omega = sqrt(g*k);
the packet envelope travels at the group velocity (half the phase velocity).
The 1/sqrt(r) factor models cylindrical energy spreading.
================
*/
static void R_SampleRippleImpact( const water_ripple_impact_t *impact, float worldX, float worldY,
	double time, float waveHeight, float waveSpeed, float ringSize, float ringWidth,
	float *height, float *gradientX, float *gradientY, float *highlight )
{
	const float gravity = 386.09f; // GoldSrc units per second squared
	const float twoPi = 6.28318530718f;
	const float halfPi = 1.57079632679f;
	const float wavelength = 144.0f * ringSize;
	const float k = twoPi / wavelength;
	const float omega = sqrtf( gravity * k ) * waveSpeed;
	const float phaseSpeed = omega / k;
	const float groupSpeed = 0.5f * phaseSpeed;
	const float sourceRadius = 0.25f * wavelength;
	const float envelopeSigma = 0.55f * wavelength * ringWidth;
	const float age = (float)( time - impact->startTime );
	const float dx = worldX - impact->origin[0];
	const float dy = worldY - impact->origin[1];
	float distance, radialDistance, packetRadius, delta;
	float gaussian, radialAttenuation, radialDecay, timeAttenuation, envelope;
	float phase, sine, cosine, amplitude, wave, radialGradient;
	float positiveCrest, ringHighlight;

	if( !impact->active || age < 0.0f || age > 3.5f )
		return;

	distance = sqrtf( dx * dx + dy * dy );
	packetRadius = sourceRadius + groupSpeed * age;
	delta = distance - packetRadius;
	if( delta < -envelopeSigma * 3.0f || delta > envelopeSigma * 3.0f )
		return;

	// A Gaussian packet gives the impact a finite train of concentric crests.
	gaussian = expf( -( delta * delta ) / ( 2.0f * envelopeSigma * envelopeSigma ));

	// Cylindrical waves lose amplitude as their circumference grows.
	// Clamp the reference radius at the source to avoid a singularity at r=0.
	radialDistance = ( distance > sourceRadius ) ? distance : sourceRadius;
	radialAttenuation = sqrtf( sourceRadius / radialDistance );
	radialDecay = ( distance > sourceRadius ) ? 0.5f / radialDistance : 0.0f;
	timeAttenuation = expf( -age / 2.4f );
	envelope = gaussian * radialAttenuation * timeAttenuation;

	// Radial phase is k*r - omega*t. A quarter-cycle starts the packet
	// with an outward crest at its source radius.
	phase = k * ( distance - sourceRadius ) - omega * age + halfPi;
	sine = sinf( phase );
	cosine = cosf( phase );
	amplitude = 4.0f * impact->strength * waveHeight;
	wave = amplitude * envelope * sine;

	// Differentiate the complete radial height function, including both the
	// Gaussian packet envelope and the cylindrical 1/sqrt(r) spreading term.
	radialGradient = amplitude * envelope *
		( k * cosine - sine * ( delta / ( envelopeSigma * envelopeSigma ) +
		  radialDecay ));

	if( height )
		*height = wave;

	if( gradientX )
		*gradientX = ( distance > 0.001f ) ? radialGradient * dx / distance : 0.0f;
	if( gradientY )
		*gradientY = ( distance > 0.001f ) ? radialGradient * dy / distance : 0.0f;

	// Highlight the same positive crests used by the displacement field.
	// A cubic crest response gives the white reflection a slightly
	// thinner profile while retaining enough width for the 12-unit mesh.
	if( highlight )
	{
		positiveCrest = ( sine > 0.0f ) ? sine : 0.0f;
		ringHighlight = positiveCrest * positiveCrest * positiveCrest * envelope *
			impact->strength * r_ripple_ringbrightness.value;
		if( ringHighlight > *highlight )
			*highlight = bound( 0.0f, ringHighlight, 1.0f );
	}
}

void R_SampleRipple( float worldX, float worldY, float *height, float *gradientX, float *gradientY, float *offsetX, float *offsetY, float *highlight )
{
	const float waveHeight = bound( 0.0f, r_ripple_waveheight.value, 4.0f );
	const float waveSpeed = bound( 0.1f, r_ripple_wavespeed.value, 4.0f );
	const float ringSize = bound( 0.5f, r_ripple_ringsize.value, 1.75f );
	const float ringWidth = bound( 0.5f, r_ripple_ringwidth.value, 1.25f );
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
	if( highlight ) *highlight = 0.0f;

	if( r_ripple.value < 2.0f || !height || !offsetX || !offsetY )
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
		const float horizontal = ( waveHeight * steepness / ( k * numWaves )) * cs;

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

	// Contact impacts are outgoing circular gravity-wave packets. The helper
	// shares the exact same dispersion, envelope, and crest profile with the
	// white-highlight pass below.
	for( int i = 0; i < MAX_WATER_RIPPLE_IMPACTS; i++ )
	{
		float impactHeight = 0.0f, impactGradientX = 0.0f, impactGradientY = 0.0f;

		R_SampleRippleImpact( &g_ripple.impacts[i], worldX, worldY, time,
			waveHeight, waveSpeed, ringSize, ringWidth, &impactHeight,
			&impactGradientX, &impactGradientY, highlight );

		h += impactHeight;
		gx += impactGradientX;
		gy += impactGradientY;
	}

	*height = h;
	if( gradientX ) *gradientX = gx;
	if( gradientY ) *gradientY = gy;
	*offsetX = ox;
	*offsetY = oy;
	if( highlight ) *highlight = bound( 0.0f, *highlight, 1.0f );
}

/*
================
R_SampleRippleHighlight

Evaluate only the contact-wave crest mask for the white overlay pass. Keeping
this separate avoids recomputing the ambient Gerstner and detail-wave spectrum.
================
*/
/*
================
R_HasActiveRippleImpacts
================
*/
qboolean R_HasActiveRippleImpacts( void )
{
	for( int i = 0; i < MAX_WATER_RIPPLE_IMPACTS; i++ )
	{
		if( g_ripple.impacts[i].active )
			return true;
	}

	return false;
}

void R_SampleRippleHighlight( float worldX, float worldY, float *highlight )
{
	const float waveHeight = bound( 0.0f, r_ripple_waveheight.value, 4.0f );
	const float waveSpeed = bound( 0.1f, r_ripple_wavespeed.value, 4.0f );
	const float ringSize = bound( 0.5f, r_ripple_ringsize.value, 1.75f );
	const float ringWidth = bound( 0.5f, r_ripple_ringwidth.value, 1.25f );
	const double time = gp_cl->time;

	if( !highlight )
		return;

	*highlight = 0.0f;
	if( r_ripple.value < 2.0f )
		return;

	for( int i = 0; i < MAX_WATER_RIPPLE_IMPACTS; i++ )
		R_SampleRippleImpact( &g_ripple.impacts[i], worldX, worldY, time,
			waveHeight, waveSpeed, ringSize, ringWidth, NULL, NULL, NULL, highlight );
}


/*
=============================================================================

    EARLY-2000s WATER MATERIAL SHADER

The water shader keeps the existing GoldSrc polygon path and texture material,
but replaces flat per-vertex lighting with analytic per-pixel wave normals,
skybox environment reflections and a restrained specular response.

It is optional: renderers without desktop GLSL 1.20 support continue using
the fixed-function water path. Contact impacts are evaluated from the same
analytic packet model used for CPU vertex displacement.

=============================================================================
*/

#if !XASH_GLES && !XASH_GL_STATIC && !XASH_GL4ES
#include <stdlib.h>

static GLhandleARB s_waterShaderProgram;
static qboolean s_waterShaderTried;
static GLuint s_waterEnvironmentCube;
static int s_waterSkyTextureIndices[SKYBOX_MAX_SIDES] = { -1, -1, -1, -1, -1, -1 };
static int s_waterEnvironmentSize;
static qboolean s_waterCubeBound;
static GLint s_waterPreviousCubeMap;
static int s_waterPreviousCubeTMU;

static GLuint s_waterSceneTexture;
static int s_waterSceneTextureWidth, s_waterSceneTextureHeight;
static qboolean s_waterSceneBound;
static GLint s_waterPreviousSceneTexture;
static int s_waterPreviousSceneTMU;
static qboolean s_waterPlanarBound;
static GLint s_waterPreviousPlanarTexture;
static int s_waterPreviousPlanarTMU;
static GLint s_waterSceneViewport[4];
static GLfloat s_waterSceneScale[2];
static GLfloat s_waterSceneTexel[2];
static int s_waterSceneCaptureFrame = -1;
static GLint s_waterSceneCaptureViewport[4];
static vec3_t s_waterSceneCaptureOrigin;
static vec3_t s_waterSceneCaptureForward;
static GLboolean s_waterBlendWasEnabled;
static GLint s_waterBlendSource, s_waterBlendDestination;
static qboolean s_waterBlendStateSaved;

static GLint s_waterUniformTexture = -1;
static GLint s_waterUniformEnvironment = -1;
static GLint s_waterUniformHasEnvironment = -1;
static GLint s_waterUniformPlanarReflection = -1;
static GLint s_waterUniformHasPlanarReflection = -1;
static GLint s_waterUniformPlanarReflectionTexel = -1;
static GLint s_waterUniformReflectionMatrix = -1;
static GLint s_waterUniformSceneTexture = -1;
static GLint s_waterUniformHasScene = -1;
static GLint s_waterUniformSceneScale = -1;
static GLint s_waterUniformSceneTexel = -1;
static GLint s_waterUniformSceneViewport = -1;
static GLint s_waterUniformRefraction = -1;
static GLint s_waterUniformOpacity = -1;
static GLint s_waterUniformTime = -1;
static GLint s_waterUniformWaveHeight = -1;
static GLint s_waterUniformWaveSpeed = -1;
static GLint s_waterUniformRingSize = -1;
static GLint s_waterUniformRingWidth = -1;
static GLint s_waterUniformNormalScale = -1;
static GLint s_waterUniformReflection = -1;
static GLint s_waterUniformSpecular = -1;
static GLint s_waterUniformFogEnabled = -1;
static GLint s_waterUniformFogMode = -1;
static GLint s_waterUniformFogParams = -1;
static GLint s_waterUniformFogColor = -1;
static GLint s_waterUniformCamera = -1;
static GLint s_waterUniformImpacts = -1;

static const char s_waterVertexShader[] =
	"#version 110\n"
	"varying vec3 vWaterPosition;\n"
	"varying vec3 vEyePosition;\n"
	"varying vec2 vWaterTexCoord;\n"
	"varying vec4 vWaterColor;\n"
	"uniform mat4 uReflectionMatrix;\n"
	"varying vec4 vReflectionCoord;\n"
	"void main(void)\n"
	"{\n"
	"    vec4 eyePosition = gl_ModelViewMatrix * gl_Vertex;\n"
	"    vReflectionCoord = uReflectionMatrix * gl_Vertex;\n"
	"    vWaterPosition = gl_Vertex.xyz;\n"
	"    vEyePosition = eyePosition.xyz;\n"
	"    vWaterTexCoord = gl_MultiTexCoord0.st;\n"
	"    vWaterColor = gl_Color;\n"
	"    gl_FogFragCoord = abs(eyePosition.z);\n"
	"    gl_Position = gl_ProjectionMatrix * eyePosition;\n"
	"}\n";

static const char s_waterFragmentShader[] =
	"#version 110\n"
	"uniform sampler2D uWaterTexture;\n"
	"uniform samplerCube uEnvironment;\n"
	"uniform sampler2D uPlanarReflection;\n"
	"uniform sampler2D uSceneTexture;\n"
	"uniform float uHasEnvironment;\n"
	"uniform float uHasPlanarReflection;\n"
	"uniform vec2 uPlanarReflectionTexel;\n"
	"uniform float uHasScene;\n"
	"uniform vec2 uSceneScale;\n"
	"uniform vec2 uSceneTexel;\n"
	"uniform vec4 uSceneViewport;\n"
	"uniform float uRefraction;\n"
	"uniform float uWaterOpacity;\n"
	"uniform float uTime;\n"
	"uniform float uWaveHeight;\n"
	"uniform float uWaveSpeed;\n"
	"uniform float uRingSize;\n"
	"uniform float uRingWidth;\n"
	"uniform float uNormalScale;\n"
	"uniform float uReflection;\n"
	"uniform float uSpecular;\n"
	"uniform float uFogEnabled;\n"
	"uniform int uFogMode;\n"
	"uniform vec4 uFogParams;\n"
	"uniform vec4 uFogColor;\n"
	"uniform vec3 uCameraPosition;\n"
	"uniform vec4 uImpact[8];\n"
	"varying vec3 vWaterPosition;\n"
	"varying vec3 vEyePosition;\n"
	"varying vec2 vWaterTexCoord;\n"
	"varying vec4 vWaterColor;\n"
	"varying vec4 vReflectionCoord;\n"
	"const float WATER_PI = 3.14159265359;\n"
	"const float WATER_TWO_PI = 6.28318530718;\n"
	"const float WATER_GRAVITY = 386.09;\n"
	"void AddWaterWave(in vec2 p, in vec2 direction, in float wavelength,\n"
	"                  in float amplitude, in float phaseOffset,\n"
	"                  in float speedScale, inout vec2 gradient)\n"
	"{\n"
	"    float k = WATER_TWO_PI / wavelength;\n"
	"    float omega = sqrt(WATER_GRAVITY * k) * uWaveSpeed * speedScale;\n"
	"    float phase = k * dot(p, direction) - omega * uTime + phaseOffset;\n"
	"    gradient += amplitude * uWaveHeight * k * direction * cos(phase);\n"
	"}\n"
	"void main(void)\n"
	"{\n"
	"    vec2 p = vWaterPosition.xy;\n"
	"    vec2 gradient = vec2(0.0);\n"
	"    AddWaterWave(p, vec2( 0.92,  0.39), 384.0, 2.80, 0.0, 1.0, gradient);\n"
	"    AddWaterWave(p, vec2( 0.31, -0.95), 288.0, 1.90, 1.7, 1.0, gradient);\n"
	"    AddWaterWave(p, vec2(-0.77,  0.64), 216.0, 1.25, 3.1, 1.0, gradient);\n"
	"    AddWaterWave(p, vec2( 0.58,  0.81), 480.0, 1.60, 4.4, 1.0, gradient);\n"
	"    AddWaterWave(p, vec2( 0.80,  0.60), 56.0, 0.25, 0.2, 1.25, gradient);\n"
	"    AddWaterWave(p, vec2(-0.60,  0.80), 42.0, 0.18, 2.3, 1.25, gradient);\n"
	"    AddWaterWave(p, vec2( 0.28, -0.96), 34.0, 0.14, 1.1, 1.25, gradient);\n"
	"    AddWaterWave(p, vec2(-0.94, -0.34), 72.0, 0.22, 4.0, 1.25, gradient);\n"
	"    AddWaterWave(p, vec2( 0.66, -0.75), 28.0, 0.10, 5.2, 1.25, gradient);\n"
	"    float ringWavelength = 144.0 * uRingSize;\n"
	"    float ringK = WATER_TWO_PI / ringWavelength;\n"
	"    float ringOmega = sqrt(WATER_GRAVITY * ringK) * uWaveSpeed;\n"
	"    float ringGroupSpeed = 0.5 * ringOmega / ringK;\n"
	"    float sourceRadius = 0.25 * ringWavelength;\n"
	"    float envelopeSigma = 0.55 * ringWavelength * uRingWidth;\n"
	"    for (int i = 0; i < 8; i++)\n"
	"    {\n"
	"        vec4 impact = uImpact[i];\n"
	"        if (impact.w > 0.001)\n"
	"        {\n"
	"            float age = uTime - impact.z;\n"
	"            if (age >= 0.0 && age <= 3.5)\n"
	"            {\n"
	"                vec2 deltaPosition = p - impact.xy;\n"
	"                float distanceFromImpact = length(deltaPosition);\n"
	"                float packetRadius = sourceRadius + ringGroupSpeed * age;\n"
	"                float delta = distanceFromImpact - packetRadius;\n"
	"                if (abs(delta) <= envelopeSigma * 3.0)\n"
	"                {\n"
	"                    float gaussian = exp(-(delta * delta) /\n"
	"                        (2.0 * envelopeSigma * envelopeSigma));\n"
	"                    float radialDistance = max(distanceFromImpact, sourceRadius);\n"
	"                    float radialAttenuation = sqrt(sourceRadius / radialDistance);\n"
	"                    float radialDecay = distanceFromImpact > sourceRadius ?\n"
	"                        0.5 / radialDistance : 0.0;\n"
	"                    float timeAttenuation = exp(-age / 2.4);\n"
	"                    float envelope = gaussian * radialAttenuation * timeAttenuation;\n"
	"                    float phase = ringK * (distanceFromImpact - sourceRadius) -\n"
	"                        ringOmega * age + 1.57079632679;\n"
	"                    float sine = sin(phase);\n"
	"                    float cosine = cos(phase);\n"
	"                    float amplitude = 4.0 * impact.w * uWaveHeight;\n"
	"                    float radialGradient = amplitude * envelope *\n"
	"                        (ringK * cosine - sine * (delta /\n"
	"                         (envelopeSigma * envelopeSigma) + radialDecay));\n"
	"                    if (distanceFromImpact > 0.001)\n"
	"                        gradient += radialGradient * deltaPosition / distanceFromImpact;\n"
	"                }\n"
	"            }\n"
	"        }\n"
	"    }\n"
	"    vec3 normal = normalize(vec3(-gradient * uNormalScale, 1.0));\n"
	"    if (!gl_FrontFacing)\n"
	"        normal = -normal;\n"
	"    vec3 viewDirection = normalize(uCameraPosition - vWaterPosition);\n"
	"    vec3 lightDirection = normalize(vec3(-0.28, -0.34, 0.90));\n"
	"    vec3 halfDirection = normalize(lightDirection + viewDirection);\n"
	"    float diffuse = 0.72 + 0.28 * max(dot(normal, lightDirection), 0.0);\n"
	"    vec2 materialOffset = gradient * 0.0016;\n"
	"    vec4 waterSample = texture2D(uWaterTexture, vWaterTexCoord + materialOffset);\n"
	"    vec3 reflectedDirection = reflect(-viewDirection, normal);\n"
	"    vec3 environmentColor = mix(vec3(0.12, 0.27, 0.30),\n"
	"                                vec3(0.42, 0.57, 0.68),\n"
	"                                clamp(reflectedDirection.z * 0.5 + 0.5, 0.0, 1.0));\n"
	"    if (uHasEnvironment > 0.5)\n"
	"        environmentColor = textureCube(uEnvironment, reflectedDirection).rgb;\n"
	"    float fresnel = 0.02 + 0.98 * pow(1.0 -\n"
	"        max(dot(normal, viewDirection), 0.0), 5.0);\n"
	"    float sunSpecular = pow(max(dot(normal, halfDirection), 0.0), 64.0);\n"
	"    float broadSpecular = pow(max(dot(normal, halfDirection), 0.0), 16.0);\n"
	"    float fogFactor = 1.0;\n"
	"    if (uFogEnabled > 0.5 && uFogMode == 0x2601)\n"
	"        fogFactor = clamp((uFogParams.z - gl_FogFragCoord) * uFogParams.w, 0.0, 1.0);\n"
	"    else if (uFogEnabled > 0.5 && uFogMode == 0x0800)\n"
	"        fogFactor = clamp(exp(-uFogParams.x * gl_FogFragCoord), 0.0, 1.0);\n"
	"    else if (uFogEnabled > 0.5 && uFogMode == 0x0801)\n"
	"        fogFactor = clamp(exp(-uFogParams.x * uFogParams.x *\n"
	"                               gl_FogFragCoord * gl_FogFragCoord), 0.0, 1.0);\n"
	"    vec3 materialColor = mix(uFogColor.rgb, waterSample.rgb * diffuse, fogFactor);\n"
	"    vec3 waterColor = materialColor;\n"
	"    if (uHasScene > 0.5)\n"
	"    {\n"
	"        vec2 localUV = (gl_FragCoord.xy - uSceneViewport.xy) / uSceneViewport.zw;\n"
	"        vec2 sceneUV = localUV * uSceneScale;\n"
	"        vec2 sceneMin = uSceneTexel * 0.5;\n"
	"        vec2 sceneMax = uSceneScale - sceneMin;\n"
	"        vec2 refractOffset = normal.xy * uRefraction * uSceneTexel;\n"
	"        sceneUV = clamp(sceneUV + refractOffset, sceneMin, sceneMax);\n"
	"        vec3 sceneColor = texture2D(uSceneTexture, sceneUV).rgb;\n"
	"        float tintOpacity = clamp(uWaterOpacity * vWaterColor.a * waterSample.a, 0.0, 1.0);\n"
	"        waterColor = mix(sceneColor, materialColor, tintOpacity);\n"
	"    }\n"
	"    float reflectionAmount = clamp(fresnel * uReflection, 0.0, 0.85);\n"
	"    if (uHasPlanarReflection > 0.5)\n"
	"    {\n"
	"        if (vReflectionCoord.w > 0.0001)\n"
	"        {\n"
	"            vec2 reflectionUV = vReflectionCoord.xy / vReflectionCoord.w * 0.5 + 0.5;\n"
	"            float reflectionDepth = vReflectionCoord.z / vReflectionCoord.w;\n"
	"            if (reflectionUV.x > 0.0 && reflectionUV.x < 1.0 &&\n"
	"                reflectionUV.y > 0.0 && reflectionUV.y < 1.0 &&\n"
	"                reflectionDepth > -1.0 && reflectionDepth < 1.0)\n"
	"            {\n"
	"                vec2 reflectionOffset = normal.xy * uPlanarReflectionTexel * 2.0;\n"
	"                reflectionUV = clamp(reflectionUV + reflectionOffset,\n"
	"                    uPlanarReflectionTexel, vec2(1.0) - uPlanarReflectionTexel);\n"
	"                vec3 planarColor = texture2D(uPlanarReflection, reflectionUV).rgb;\n"
	"                waterColor = mix(waterColor, planarColor, reflectionAmount);\n"
	"            }\n"
	"            else\n"
	"                waterColor = mix(waterColor, environmentColor, reflectionAmount);\n"
	"        }\n"
	"        else\n"
	"            waterColor = mix(waterColor, environmentColor, reflectionAmount);\n"
	"    }\n"
	"    else\n"
	"        waterColor = mix(waterColor, environmentColor, reflectionAmount);\n"
	"    waterColor += vec3((sunSpecular + broadSpecular * 0.08) * uSpecular);\n"
	"    gl_FragColor = vec4(waterColor * vWaterColor.rgb,\n"
	"                        (uHasScene > 0.5) ? 1.0 : waterSample.a * vWaterColor.a);\n"	"}\n";

static GLhandleARB R_CompileWaterShader( GLenum type, const char *source )
{
	GLhandleARB shader = pglCreateShaderObjectARB( type );
	GLint status = 0;

	if( !shader )
		return 0;

	pglShaderSourceARB( shader, 1, (const GLcharARB **)&source, NULL );
	pglCompileShaderARB( shader );
	pglGetObjectParameterivARB( shader, GL_OBJECT_COMPILE_STATUS_ARB, &status );

	if( !status )
	{
		char log[1024];
		GLint length = 0;
		GLsizei written = 0;

		pglGetObjectParameterivARB( shader, GL_OBJECT_INFO_LOG_LENGTH_ARB, &length );
		if( length > 0 )
		{
			pglGetInfoLogARB( shader, sizeof( log ) - 1, &written, log );
			if( written < 0 ) written = 0;
			if( written >= (GLsizei)sizeof( log )) written = sizeof( log ) - 1;
			log[written] = '\0';
			gEngfuncs.Con_Printf( S_WARN "water shader compile failed: %s\n", log );
		}
		pglDeleteObjectARB( shader );
		return 0;
	}

	return shader;
}

static qboolean R_InitWaterShader( void )
{
	GLhandleARB vertexShader, fragmentShader;
	GLint linked = 0;

	if( s_waterShaderProgram )
		return true;

	if( s_waterShaderTried )
		return false;

	s_waterShaderTried = true;

	if( !GL_Support( GL_SHADER_OBJECTS_EXT ) ||
		!GL_Support( GL_SHADER_GLSL100_EXT ) ||
		!GL_Support( GL_ARB_MULTITEXTURE ) ||
		!GL_Support( GL_TEXTURE_CUBEMAP_EXT ) ||
		glConfig.max_texture_units < 4 ||
		glConfig.max_teximage_units < 4 ||
		glConfig.context == CONTEXT_TYPE_GL_CORE )
		return false;

	vertexShader = R_CompileWaterShader( GL_VERTEX_SHADER_ARB, s_waterVertexShader );
	if( !vertexShader )
		return false;

	fragmentShader = R_CompileWaterShader( GL_FRAGMENT_SHADER_ARB, s_waterFragmentShader );
	if( !fragmentShader )
	{
		pglDeleteObjectARB( vertexShader );
		return false;
	}

	s_waterShaderProgram = pglCreateProgramObjectARB();
	if( !s_waterShaderProgram )
	{
		pglDeleteObjectARB( vertexShader );
		pglDeleteObjectARB( fragmentShader );
		return false;
	}

	pglAttachObjectARB( s_waterShaderProgram, vertexShader );
	pglAttachObjectARB( s_waterShaderProgram, fragmentShader );
	pglLinkProgramARB( s_waterShaderProgram );
	pglGetObjectParameterivARB( s_waterShaderProgram, GL_OBJECT_LINK_STATUS_ARB, &linked );

	pglDeleteObjectARB( vertexShader );
	pglDeleteObjectARB( fragmentShader );

	if( !linked )
	{
		char log[1024];
		GLint length = 0;
		GLsizei written = 0;

		pglGetObjectParameterivARB( s_waterShaderProgram, GL_OBJECT_INFO_LOG_LENGTH_ARB, &length );
		if( length > 0 )
		{
			pglGetInfoLogARB( s_waterShaderProgram, sizeof( log ) - 1, &written, log );
			if( written < 0 ) written = 0;
			if( written >= (GLsizei)sizeof( log )) written = sizeof( log ) - 1;
			log[written] = '\0';
			gEngfuncs.Con_Printf( S_WARN "water shader link failed: %s\n", log );
		}
		pglDeleteObjectARB( s_waterShaderProgram );
		s_waterShaderProgram = 0;
		return false;
	}

	s_waterUniformTexture = pglGetUniformLocationARB( s_waterShaderProgram, "uWaterTexture" );
	s_waterUniformEnvironment = pglGetUniformLocationARB( s_waterShaderProgram, "uEnvironment" );
	s_waterUniformHasEnvironment = pglGetUniformLocationARB( s_waterShaderProgram, "uHasEnvironment" );
	s_waterUniformPlanarReflection = pglGetUniformLocationARB( s_waterShaderProgram, "uPlanarReflection" );
	s_waterUniformHasPlanarReflection = pglGetUniformLocationARB( s_waterShaderProgram, "uHasPlanarReflection" );
	s_waterUniformPlanarReflectionTexel = pglGetUniformLocationARB( s_waterShaderProgram, "uPlanarReflectionTexel" );
	s_waterUniformReflectionMatrix = pglGetUniformLocationARB( s_waterShaderProgram, "uReflectionMatrix" );
	s_waterUniformSceneTexture = pglGetUniformLocationARB( s_waterShaderProgram, "uSceneTexture" );
	s_waterUniformHasScene = pglGetUniformLocationARB( s_waterShaderProgram, "uHasScene" );
	s_waterUniformSceneScale = pglGetUniformLocationARB( s_waterShaderProgram, "uSceneScale" );
	s_waterUniformSceneTexel = pglGetUniformLocationARB( s_waterShaderProgram, "uSceneTexel" );
	s_waterUniformSceneViewport = pglGetUniformLocationARB( s_waterShaderProgram, "uSceneViewport" );
	s_waterUniformRefraction = pglGetUniformLocationARB( s_waterShaderProgram, "uRefraction" );
	s_waterUniformOpacity = pglGetUniformLocationARB( s_waterShaderProgram, "uWaterOpacity" );
	s_waterUniformTime = pglGetUniformLocationARB( s_waterShaderProgram, "uTime" );
	s_waterUniformWaveHeight = pglGetUniformLocationARB( s_waterShaderProgram, "uWaveHeight" );
	s_waterUniformWaveSpeed = pglGetUniformLocationARB( s_waterShaderProgram, "uWaveSpeed" );
	s_waterUniformRingSize = pglGetUniformLocationARB( s_waterShaderProgram, "uRingSize" );
	s_waterUniformRingWidth = pglGetUniformLocationARB( s_waterShaderProgram, "uRingWidth" );
	s_waterUniformNormalScale = pglGetUniformLocationARB( s_waterShaderProgram, "uNormalScale" );
	s_waterUniformReflection = pglGetUniformLocationARB( s_waterShaderProgram, "uReflection" );
	s_waterUniformSpecular = pglGetUniformLocationARB( s_waterShaderProgram, "uSpecular" );
	s_waterUniformFogEnabled = pglGetUniformLocationARB( s_waterShaderProgram, "uFogEnabled" );
	s_waterUniformFogMode = pglGetUniformLocationARB( s_waterShaderProgram, "uFogMode" );
	s_waterUniformFogParams = pglGetUniformLocationARB( s_waterShaderProgram, "uFogParams" );
	s_waterUniformFogColor = pglGetUniformLocationARB( s_waterShaderProgram, "uFogColor" );
	s_waterUniformCamera = pglGetUniformLocationARB( s_waterShaderProgram, "uCameraPosition" );
	s_waterUniformImpacts = pglGetUniformLocationARB( s_waterShaderProgram, "uImpact[0]" );

	gEngfuncs.Con_Printf( S_NOTE "GPU water material shader initialized\n" );
	return true;
}

static void R_FreeWaterEnvironmentMap( void )
{
	if( s_waterEnvironmentCube )
	{
		pglDeleteTextures( 1, &s_waterEnvironmentCube );
		s_waterEnvironmentCube = 0;
	}

	s_waterEnvironmentSize = 0;
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
		s_waterSkyTextureIndices[i] = -1;
}

static qboolean R_BuildWaterEnvironmentMap( int baseTexture )
{
	static const GLenum cubeTargets[6] =
	{
		GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB,
		GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB,
		GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB
	};
	byte *sourcePixels[SKYBOX_MAX_SIDES] = { NULL, NULL, NULL, NULL, NULL, NULL };
	int sourceWidths[SKYBOX_MAX_SIDES];
	int sourceHeights[SKYBOX_MAX_SIDES];
	int sourceIndices[SKYBOX_MAX_SIDES];
	int minDimension = 4096;
	int cubeSize = 128;
	byte *cubePixels = NULL;
	GLint previousCubeMap = 0;
	qboolean unchanged = true;

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		sourceIndices[i] = tr.skyboxTextures[i];
		if( !sourceIndices[i] )
		{
			R_FreeWaterEnvironmentMap();
			GL_Bind( XASH_TEXTURE0, baseTexture );
			return false;
		}

		gl_texture_t *texture = R_GetTexture( sourceIndices[i] );
		if( !texture || !texture->texnum || texture->target != GL_TEXTURE_2D ||
			!texture->width || !texture->height )
		{
			R_FreeWaterEnvironmentMap();
			GL_Bind( XASH_TEXTURE0, baseTexture );
			return false;
		}

		sourceWidths[i] = texture->width;
		sourceHeights[i] = texture->height;
		if( sourceWidths[i] < minDimension ) minDimension = sourceWidths[i];
		if( sourceHeights[i] < minDimension ) minDimension = sourceHeights[i];

		if( s_waterSkyTextureIndices[i] != sourceIndices[i] )
			unchanged = false;
	}

	if( unchanged && s_waterEnvironmentCube && s_waterEnvironmentSize > 0 )
		return true;

	R_FreeWaterEnvironmentMap();

	while( cubeSize > minDimension )
		cubeSize >>= 1;

	if( cubeSize < 16 )
	{
		GL_Bind( XASH_TEXTURE0, baseTexture );
		return false;
	}

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		size_t bytes = (size_t)sourceWidths[i] * sourceHeights[i] * 4;
		sourcePixels[i] = (byte *)malloc( bytes );
		if( !sourcePixels[i] )
			goto failed;

		GL_Bind( XASH_TEXTURE0, sourceIndices[i] );
		pglGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, sourcePixels[i] );
	}

	cubePixels = (byte *)malloc( (size_t)cubeSize * cubeSize * 4 );
	if( !cubePixels )
		goto failed;

	pglActiveTextureARB( GL_TEXTURE1_ARB );
	pglGetIntegerv( GL_TEXTURE_BINDING_CUBE_MAP_ARB, &previousCubeMap );
	pglGenTextures( 1, &s_waterEnvironmentCube );
	if( !s_waterEnvironmentCube )
	{
		pglBindTexture( GL_TEXTURE_CUBE_MAP_ARB, (GLuint)previousCubeMap );
		pglActiveTextureARB( GL_TEXTURE0_ARB );
		goto failed;
	}
	pglBindTexture( GL_TEXTURE_CUBE_MAP_ARB, s_waterEnvironmentCube );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_CUBE_MAP_ARB, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE );

	for( int face = 0; face < 6; face++ )
	{
		for( int y = 0; y < cubeSize; y++ )
		{
			for( int x = 0; x < cubeSize; x++ )
			{
				const float sc = 2.0f * ((float)x + 0.5f) / cubeSize - 1.0f;
				const float tc = 2.0f * ((float)y + 0.5f) / cubeSize - 1.0f;
				vec3_t direction;
				float skyS, skyT;
				int axis, sourceIndex, sx, sy;
				const byte *pixel;

				switch( face )
				{
				case 0: VectorSet( direction,  1.0f, -tc,  -sc ); break;
				case 1: VectorSet( direction, -1.0f, -tc,   sc ); break;
				case 2: VectorSet( direction,   sc, 1.0f,   tc ); break;
				case 3: VectorSet( direction,   sc,-1.0f,  -tc ); break;
				case 4: VectorSet( direction,   sc, -tc,  1.0f ); break;
				default:VectorSet( direction,  -sc, -tc, -1.0f ); break;
				}

				axis = R_SkyboxAxisFromDir( direction );
				if( !R_SkyboxProject( direction, axis, &skyS, &skyT ))
					continue;

				sourceIndex = r_skyTexOrder[axis];
				sx = (int)(((skyS + 1.0f) * 0.5f) * sourceWidths[sourceIndex]);
				sy = (int)(((1.0f - skyT) * 0.5f) * sourceHeights[sourceIndex]);
				if( sx < 0 ) sx = 0;
				if( sy < 0 ) sy = 0;
				if( sx >= sourceWidths[sourceIndex] ) sx = sourceWidths[sourceIndex] - 1;
				if( sy >= sourceHeights[sourceIndex] ) sy = sourceHeights[sourceIndex] - 1;

				pixel = sourcePixels[sourceIndex] +
					((size_t)sy * sourceWidths[sourceIndex] + sx) * 4;
				memcpy( cubePixels + ((size_t)y * cubeSize + x) * 4, pixel, 4 );
			}
		}

		pglTexImage2D( cubeTargets[face], 0, GL_RGBA, cubeSize, cubeSize,
			0, GL_RGBA, GL_UNSIGNED_BYTE, cubePixels );
	}

	pglBindTexture( GL_TEXTURE_CUBE_MAP_ARB, (GLuint)previousCubeMap );
	pglActiveTextureARB( GL_TEXTURE0_ARB );

	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
	{
		s_waterSkyTextureIndices[i] = sourceIndices[i];
		free( sourcePixels[i] );
	}
	free( cubePixels );
	s_waterEnvironmentSize = cubeSize;
	GL_Bind( XASH_TEXTURE0, baseTexture );
	return true;

failed:
	for( int i = 0; i < SKYBOX_MAX_SIDES; i++ )
		free( sourcePixels[i] );
	free( cubePixels );
	if( s_waterEnvironmentCube )
	{
		pglActiveTextureARB( GL_TEXTURE1_ARB );
		pglBindTexture( GL_TEXTURE_CUBE_MAP_ARB, (GLuint)previousCubeMap );
		pglActiveTextureARB( GL_TEXTURE0_ARB );
		R_FreeWaterEnvironmentMap();
	}
	GL_Bind( XASH_TEXTURE0, baseTexture );
	return false;
}

static int R_WaterNextPowerOfTwo( int value )
{
	int result = 1;

	while( result < value && result < 32768 )
		result <<= 1;

	return result;
}

static qboolean R_WaterSceneCaptureMatches( const GLint *viewport )
{
	if( s_waterSceneCaptureFrame != tr.framecount ||
		memcmp( s_waterSceneCaptureViewport, viewport, sizeof( s_waterSceneCaptureViewport )))
		return false;

	for( int i = 0; i < 3; i++ )
	{
		if( fabsf( s_waterSceneCaptureOrigin[i] - RI.rvp.vieworigin[i] ) > 0.01f ||
			fabsf( s_waterSceneCaptureForward[i] - RI.vforward[i] ) > 0.001f )
			return false;
	}

	return true;
}

/*
 * Capture the opaque scene once per view, before the first water surface.
 * Source uses a separate clipped refraction view; this initial Xash3D pass
 * uses the current view as a cheaper fallback until water-plane clipping
 * and the dedicated refraction pass are implemented.
 */
static qboolean R_BindWaterSceneTexture( void )
{
	GLint viewport[4];
	GLint maxTextureSize = 0;
	GLint previousTexture = 0;
	int previousTMU = glState.activeTMU;
	int textureWidth, textureHeight;
	qboolean captureScene, allocateTexture = false;

	if( tr.targets.msaa.fbo || FBitSet( RI.rvp.flags, RF_DRAW_CUBEMAP ))
		return false;

	pglGetIntegerv( GL_VIEWPORT, viewport );
	if( viewport[2] <= 0 || viewport[3] <= 0 || viewport[0] < 0 || viewport[1] < 0 )
		return false;

	pglGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTextureSize );
	textureWidth = R_WaterNextPowerOfTwo( viewport[2] );
	textureHeight = R_WaterNextPowerOfTwo( viewport[3] );
	if( maxTextureSize <= 0 || textureWidth < viewport[2] || textureHeight < viewport[3] ||
		textureWidth > maxTextureSize || textureHeight > maxTextureSize )
		return false;

	captureScene = !s_waterSceneTexture || !R_WaterSceneCaptureMatches( viewport );

	GL_SelectTexture( 2 );
	pglGetIntegerv( GL_TEXTURE_BINDING_2D, &previousTexture );
	s_waterPreviousSceneTexture = previousTexture;
	s_waterPreviousSceneTMU = previousTMU;

	if( !s_waterSceneTexture || textureWidth > s_waterSceneTextureWidth ||
		textureHeight > s_waterSceneTextureHeight )
	{
		if( s_waterSceneTexture )
			pglDeleteTextures( 1, &s_waterSceneTexture );

		s_waterSceneTexture = 0;
		pglGenTextures( 1, &s_waterSceneTexture );
		if( !s_waterSceneTexture )
		{
			GL_SelectTexture( previousTMU );
			return false;
		}

		s_waterSceneTextureWidth = textureWidth;
		s_waterSceneTextureHeight = textureHeight;
		captureScene = true;
		allocateTexture = true;
	}

	pglBindTexture( GL_TEXTURE_2D, s_waterSceneTexture );

	if( captureScene )
	{
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP );
		if( allocateTexture )
		{
			pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, s_waterSceneTextureWidth,
				s_waterSceneTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		}
		pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1],
			viewport[2], viewport[3] );

		s_waterSceneCaptureFrame = tr.framecount;
		memcpy( s_waterSceneCaptureViewport, viewport, sizeof( s_waterSceneCaptureViewport ));
		VectorCopy( RI.rvp.vieworigin, s_waterSceneCaptureOrigin );
		VectorCopy( RI.vforward, s_waterSceneCaptureForward );
	}

	memcpy( s_waterSceneViewport, viewport, sizeof( s_waterSceneViewport ));
	s_waterSceneScale[0] = (float)viewport[2] / s_waterSceneTextureWidth;
	s_waterSceneScale[1] = (float)viewport[3] / s_waterSceneTextureHeight;
	s_waterSceneTexel[0] = 1.0f / s_waterSceneTextureWidth;
	s_waterSceneTexel[1] = 1.0f / s_waterSceneTextureHeight;
	s_waterSceneBound = true;

	GL_SelectTexture( previousTMU );
	return true;
}

qboolean R_BeginWaterShader( msurface_t *water )
{
	float impacts[MAX_WATER_RIPPLE_IMPACTS][4];
	qboolean hasEnvironment, hasScene, hasPlanar;
	int baseTexture, reflectionTexture = 0;
	gl_texture_t *reflectionImage = NULL;
	matrix4x4 reflectionMatrix;
	GLfloat reflectionMatrixArray[16];

	if( !r_water_shader.value || r_ripple.value < 2.0f || !water ||
		!water->texinfo || !water->texinfo->texture )
		return false;

	if( !R_InitWaterShader() )
		return false;

	baseTexture = water->texinfo->texture->gl_texturenum;
	Matrix4x4_LoadIdentity( reflectionMatrix );
	hasPlanar = false;
	if( r_water_planarreflect.value && water->plane &&
		fabsf( water->plane->normal[2] ) > 0.99f )
	{
		float waterHeight = water->plane->dist / water->plane->normal[2];
		hasPlanar = R_GetWaterReflection( waterHeight, &reflectionTexture, reflectionMatrix );
	}

	if( hasPlanar )
	{
		reflectionImage = R_GetTexture( reflectionTexture );
		if( !reflectionImage || !reflectionImage->texnum ||
			reflectionImage->width <= 0 || reflectionImage->height <= 0 )
			hasPlanar = false;
	}

	hasEnvironment = R_BuildWaterEnvironmentMap( baseTexture );
	hasScene = ( r_water_refract.value > 0.0f ) ? R_BindWaterSceneTexture() : false;
	Matrix4x4_ToArrayFloatGL( reflectionMatrix, reflectionMatrixArray );

	pglUseProgramObjectARB( s_waterShaderProgram );
	pglUniform1iARB( s_waterUniformTexture, 0 );
	pglUniform1iARB( s_waterUniformEnvironment, 1 );
	pglUniform1iARB( s_waterUniformPlanarReflection, 3 );
	pglUniform1fARB( s_waterUniformHasPlanarReflection, hasPlanar ? 1.0f : 0.0f );
	pglUniform2fARB( s_waterUniformPlanarReflectionTexel,
		hasPlanar ? 1.0f / reflectionImage->width : 1.0f,
		hasPlanar ? 1.0f / reflectionImage->height : 1.0f );
	pglUniformMatrix4fvARB( s_waterUniformReflectionMatrix, 1, GL_FALSE, reflectionMatrixArray );
	pglUniform1iARB( s_waterUniformSceneTexture, 2 );
	pglUniform1fARB( s_waterUniformHasEnvironment, hasEnvironment ? 1.0f : 0.0f );
	pglUniform1fARB( s_waterUniformHasScene, hasScene ? 1.0f : 0.0f );
	pglUniform2fARB( s_waterUniformSceneScale, hasScene ? s_waterSceneScale[0] : 1.0f,
		hasScene ? s_waterSceneScale[1] : 1.0f );
	pglUniform2fARB( s_waterUniformSceneTexel, hasScene ? s_waterSceneTexel[0] : 1.0f,
		hasScene ? s_waterSceneTexel[1] : 1.0f );
	pglUniform4fARB( s_waterUniformSceneViewport, hasScene ? (GLfloat)s_waterSceneViewport[0] : 0.0f,
		hasScene ? (GLfloat)s_waterSceneViewport[1] : 0.0f,
		hasScene ? (GLfloat)s_waterSceneViewport[2] : 1.0f,
		hasScene ? (GLfloat)s_waterSceneViewport[3] : 1.0f );
	pglUniform1fARB( s_waterUniformRefraction, bound( 0.0f, r_water_refract.value, 16.0f ));
	pglUniform1fARB( s_waterUniformOpacity, bound( 0.0f, r_water_opacity.value, 1.0f ) );
	pglUniform1fARB( s_waterUniformTime, gp_cl->time );
	pglUniform1fARB( s_waterUniformWaveHeight, bound( 0.0f, r_ripple_waveheight.value, 4.0f ));
	pglUniform1fARB( s_waterUniformWaveSpeed, bound( 0.1f, r_ripple_wavespeed.value, 4.0f ));
	pglUniform1fARB( s_waterUniformRingSize, bound( 0.5f, r_ripple_ringsize.value, 1.75f ));
	pglUniform1fARB( s_waterUniformRingWidth, bound( 0.5f, r_ripple_ringwidth.value, 1.25f ));
	pglUniform1fARB( s_waterUniformNormalScale, bound( 0.0f, r_water_normalscale.value, 8.0f ));
	pglUniform1fARB( s_waterUniformReflection, bound( 0.0f, r_water_reflect.value, 1.0f ));
	pglUniform1fARB( s_waterUniformSpecular, bound( 0.0f, r_water_specular.value, 2.0f ));
	{
		GLint fogMode = GL_EXP;
		GLfloat fogDensity = 0.0f, fogStart = 0.0f, fogEnd = 1.0f;
		GLfloat fogColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		GLfloat fogScale;

		pglGetIntegerv( GL_FOG_MODE, &fogMode );
		pglGetFloatv( GL_FOG_DENSITY, &fogDensity );
		pglGetFloatv( GL_FOG_START, &fogStart );
		pglGetFloatv( GL_FOG_END, &fogEnd );
		pglGetFloatv( GL_FOG_COLOR, fogColor );
		fogScale = ( fogEnd != fogStart ) ? 1.0f / ( fogEnd - fogStart ) : 0.0f;
		pglUniform1fARB( s_waterUniformFogEnabled, pglIsEnabled( GL_FOG ) ? 1.0f : 0.0f );
		pglUniform1iARB( s_waterUniformFogMode, fogMode );
		pglUniform4fARB( s_waterUniformFogParams, fogDensity, fogStart, fogEnd, fogScale );
		pglUniform4fARB( s_waterUniformFogColor, fogColor[0], fogColor[1], fogColor[2], fogColor[3] );
	}
	pglUniform3fARB( s_waterUniformCamera, RI.rvp.vieworigin[0],
		RI.rvp.vieworigin[1], RI.rvp.vieworigin[2] );

	memset( impacts, 0, sizeof( impacts ));
	for( int i = 0; i < MAX_WATER_RIPPLE_IMPACTS; i++ )
	{
		if( !g_ripple.impacts[i].active )
			continue;

		impacts[i][0] = g_ripple.impacts[i].origin[0];
		impacts[i][1] = g_ripple.impacts[i].origin[1];
		impacts[i][2] = (float)g_ripple.impacts[i].startTime;
		impacts[i][3] = g_ripple.impacts[i].strength;
	}
	pglUniform4fvARB( s_waterUniformImpacts, MAX_WATER_RIPPLE_IMPACTS, &impacts[0][0] );

	s_waterBlendStateSaved = false;
	if( hasScene )
	{
		s_waterBlendWasEnabled = pglIsEnabled( GL_BLEND );
		pglGetIntegerv( 0x0BE1, &s_waterBlendSource );
		pglGetIntegerv( 0x0BE0, &s_waterBlendDestination );
		pglDisable( GL_BLEND );
		s_waterBlendStateSaved = true;
	}

	s_waterPlanarBound = false;
	if( hasPlanar )
	{
		s_waterPreviousPlanarTMU = glState.activeTMU;
		GL_SelectTexture( 3 );
		pglGetIntegerv( GL_TEXTURE_BINDING_2D, &s_waterPreviousPlanarTexture );
		pglBindTexture( GL_TEXTURE_2D, reflectionImage->texnum );
		GL_SelectTexture( s_waterPreviousPlanarTMU );
		s_waterPlanarBound = true;
	}

	if( hasEnvironment )
	{
		s_waterPreviousCubeTMU = glState.activeTMU;
		GL_SelectTexture( 1 );
		pglGetIntegerv( GL_TEXTURE_BINDING_CUBE_MAP_ARB, &s_waterPreviousCubeMap );
		pglBindTexture( GL_TEXTURE_CUBE_MAP_ARB, s_waterEnvironmentCube );
		GL_SelectTexture( s_waterPreviousCubeTMU );
		s_waterCubeBound = true;
	}
	else
	{
		s_waterCubeBound = false;
	}

	return true;
}

void R_EndWaterShader( void )
{
	if( !s_waterShaderProgram )
		return;

	pglUseProgramObjectARB( 0 );

	if( s_waterCubeBound )
	{
		GL_SelectTexture( 1 );
		pglBindTexture( GL_TEXTURE_CUBE_MAP_ARB, (GLuint)s_waterPreviousCubeMap );
		GL_SelectTexture( s_waterPreviousCubeTMU );
	}

	if( s_waterPlanarBound )
	{
		GL_SelectTexture( 3 );
		pglBindTexture( GL_TEXTURE_2D, (GLuint)s_waterPreviousPlanarTexture );
		GL_SelectTexture( s_waterPreviousPlanarTMU );
	}

	if( s_waterSceneBound )
	{
		GL_SelectTexture( 2 );
		pglBindTexture( GL_TEXTURE_2D, (GLuint)s_waterPreviousSceneTexture );
		GL_SelectTexture( s_waterPreviousSceneTMU );
	}

	if( s_waterBlendStateSaved )
	{
		pglBlendFunc( s_waterBlendSource, s_waterBlendDestination );
		if( s_waterBlendWasEnabled )
			pglEnable( GL_BLEND );
		else
			pglDisable( GL_BLEND );
	}

	s_waterCubeBound = false;
	s_waterPreviousCubeMap = 0;
	s_waterPreviousCubeTMU = 0;
	s_waterPlanarBound = false;
	s_waterPreviousPlanarTexture = 0;
	s_waterPreviousPlanarTMU = 0;
	s_waterSceneBound = false;
	s_waterPreviousSceneTexture = 0;
	s_waterPreviousSceneTMU = 0;
	s_waterBlendStateSaved = false;
}

#else
qboolean R_BeginWaterShader( msurface_t *water )
{
	(void)water;
	return false;
}

void R_EndWaterShader( void )
{
}
#endif

void R_ShutdownWaterShader( void )
{
#if !XASH_GLES && !XASH_GL_STATIC && !XASH_GL4ES
	R_EndWaterShader();
	R_FreeWaterEnvironmentMap();
	GL_FreeWaterReflectionTarget();
	if( s_waterSceneTexture )
	{
		pglDeleteTextures( 1, &s_waterSceneTexture );
		s_waterSceneTexture = 0;
		s_waterSceneTextureWidth = s_waterSceneTextureHeight = 0;
	}
	s_waterSceneCaptureFrame = -1;
	if( s_waterShaderProgram )
	{
		pglDeleteObjectARB( s_waterShaderProgram );
		s_waterShaderProgram = 0;
	}
	s_waterShaderTried = false;
#endif
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
		g_ripple.accumulator = 0.0;
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
