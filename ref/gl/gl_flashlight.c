/*
gl_flashlight.c - fixed-function projected flashlight
Copyright (C) 2026

This file is part of Xash3D FWGS.
*/

#include "gl_local.h"
#include "xash3d_mathlib.h"
#include <math.h>

#ifndef GL_BLEND_DST
#define GL_BLEND_DST 0x0BE0
#endif
#ifndef GL_BLEND_SRC
#define GL_BLEND_SRC 0x0BE1
#endif
#ifndef GL_CURRENT_COLOR
#define GL_CURRENT_COLOR 0x0B00
#endif
#ifndef GL_DEPTH_FUNC
#define GL_DEPTH_FUNC 0x0B74
#endif

#define FLASHLIGHT_COOKIE_SIZE 128
#define FLASHLIGHT_FALLOFF_WIDTH 256
#define FLASHLIGHT_NEAR_Z 4.0f

CVAR_DEFINE_AUTO( r_flashlight, "0", FCVAR_GLCONFIG, "enable the unshadowed projected flashlight" );
CVAR_DEFINE_AUTO( r_flashlight_fov, "45", FCVAR_GLCONFIG, "projected flashlight field of view" );
CVAR_DEFINE_AUTO( r_flashlight_far, "750", FCVAR_GLCONFIG, "projected flashlight maximum distance" );
CVAR_DEFINE_AUTO( r_flashlight_intensity, "1.25", FCVAR_GLCONFIG, "projected flashlight intensity" );

typedef struct
{
    vec3_t origin;
    vec3_t forward;
    vec3_t right;
    vec3_t up;
    float tanHalfFovX;
    float tanHalfFovY;
    float farZ;
    float intensity;
    int cookieTexture;
    int falloffTexture;
    qboolean active;
    GLint textureEnvMode[3];
    GLint depthFunc;
    GLint blendSrc;
    GLint blendDst;
    GLint depthMask;
    qboolean blendEnabled;
    qboolean alphaTestEnabled;
    GLfloat currentColor[4];
} flashlight_state_t;

static flashlight_state_t g_flashlight;
static qboolean g_flashlightWarnedUnits;

static float FlashlightSmoothStep( float edge0, float edge1, float value )
{
    float t = bound( 0.0f, ( value - edge0 ) / ( edge1 - edge0 ), 1.0f );
    return t * t * ( 3.0f - 2.0f * t );
}

static qboolean R_FlashlightEnsureTextures( void )
{
    byte cookie[FLASHLIGHT_COOKIE_SIZE * FLASHLIGHT_COOKIE_SIZE * 4];
    byte falloff[FLASHLIGHT_FALLOFF_WIDTH * 4];

    g_flashlight.cookieTexture = GL_FindTexture( "*xash_flashlight_cookie" );
    g_flashlight.falloffTexture = GL_FindTexture( "*xash_flashlight_falloff" );

    if( !g_flashlight.cookieTexture )
    {
        for( int y = 0; y < FLASHLIGHT_COOKIE_SIZE; y++ )
        {
            for( int x = 0; x < FLASHLIGHT_COOKIE_SIZE; x++ )
            {
                float u = ( ( x + 0.5f ) / FLASHLIGHT_COOKIE_SIZE ) * 2.0f - 1.0f;
                float v = ( ( y + 0.5f ) / FLASHLIGHT_COOKIE_SIZE ) * 2.0f - 1.0f;
                float radius = sqrtf( u * u + v * v );
                float edge = FlashlightSmoothStep( 0.72f, 1.0f, radius );
                float value = 1.0f - edge;
                value *= 1.0f - 0.12f * radius * radius;

                int intensity = (int)( bound( 0.0f, value, 1.0f ) * 255.0f + 0.5f );
                byte *pixel = &cookie[( y * FLASHLIGHT_COOKIE_SIZE + x ) * 4];
                pixel[0] = pixel[1] = pixel[2] = (byte)intensity;
                pixel[3] = 255;
            }
        }

        g_flashlight.cookieTexture = GL_CreateTexture(
            "*xash_flashlight_cookie",
            FLASHLIGHT_COOKIE_SIZE,
            FLASHLIGHT_COOKIE_SIZE,
            cookie,
            TF_NOMIPMAP | TF_CLAMP );
    }

    if( !g_flashlight.falloffTexture )
    {
        for( int x = 0; x < FLASHLIGHT_FALLOFF_WIDTH; x++ )
        {
            float distance = (float)x / ( FLASHLIGHT_FALLOFF_WIDTH - 1 );
            float fadeIn = FlashlightSmoothStep( 0.0f, 0.02f, distance );
            float fadeOut = 1.0f - distance;
            float value = fadeIn * fadeOut;
            value = value * sqrtf( value );

            int intensity = (int)( bound( 0.0f, value, 1.0f ) * 255.0f + 0.5f );
            byte *pixel = &falloff[x * 4];
            pixel[0] = pixel[1] = pixel[2] = (byte)intensity;
            pixel[3] = 255;
        }

        /* The first and last texels are black. This makes negative depth and
         * coordinates beyond the far plane fade to zero with edge clamping. */
        memset( falloff, 0, 4 );
        memset( &falloff[( FLASHLIGHT_FALLOFF_WIDTH - 1 ) * 4], 0, 4 );

        g_flashlight.falloffTexture = GL_CreateTexture(
            "*xash_flashlight_falloff",
            FLASHLIGHT_FALLOFF_WIDTH,
            1,
            falloff,
            TF_NOMIPMAP | TF_CLAMP );
    }

    return g_flashlight.cookieTexture != 0 && g_flashlight.falloffTexture != 0;
}

qboolean R_FlashlightBeginWorldPass( void )
{
    float fov;
    float viewportWidth, viewportHeight, aspect;

    g_flashlight.active = false;

    if( !r_flashlight.value )
        return false;

    if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) ||
        FBitSet( RI.rvp.flags, RF_ONLY_CLIENTDRAW | RF_DRAW_OVERVIEW | RF_DRAW_CUBEMAP ))
        return false;

    if( !GL_Support( GL_ARB_MULTITEXTURE ) || glConfig.max_texture_units < 3 )
    {
        if( !g_flashlightWarnedUnits )
        {
            gEngfuncs.Con_Printf( S_WARN "r_flashlight requires fixed-function multitexture with at least 3 texture units\n" );
            g_flashlightWarnedUnits = true;
        }
        return false;
    }

    if( !R_FlashlightEnsureTextures() )
    {
        gEngfuncs.Con_Printf( S_WARN "r_flashlight: unable to create projected-light textures\n" );
        return false;
    }

    VectorCopy( RI.rvp.vieworigin, g_flashlight.origin );
    VectorCopy( RI.vforward, g_flashlight.forward );
    VectorCopy( RI.vright, g_flashlight.right );
    VectorCopy( RI.vup, g_flashlight.up );

    VectorNormalize( g_flashlight.forward );
    VectorNormalize( g_flashlight.right );
    VectorNormalize( g_flashlight.up );

    fov = bound( 15.0f, r_flashlight_fov.value, 100.0f );
    g_flashlight.tanHalfFovX = tanf( fov * 0.008726646259971648f );

    viewportWidth = RI.rvp.viewport[2] > 0 ? (float)RI.rvp.viewport[2] : (float)gpGlobals->width;
    viewportHeight = RI.rvp.viewport[3] > 0 ? (float)RI.rvp.viewport[3] : (float)gpGlobals->height;
    aspect = viewportWidth / Q_max( viewportHeight, 1.0f );
    aspect = bound( 0.25f, aspect, 4.0f );
    g_flashlight.tanHalfFovY = g_flashlight.tanHalfFovX / aspect;

    g_flashlight.farZ = bound( 64.0f, r_flashlight_far.value, 4096.0f );
    g_flashlight.intensity = bound( 0.0f, r_flashlight_intensity.value, 4.0f );

    if( g_flashlight.tanHalfFovX <= 0.0f || g_flashlight.tanHalfFovY <= 0.0f || g_flashlight.intensity <= 0.0f )
        return false;

    /* Use three fixed-function units:
     * 0 = surface diffuse texture
     * 1 = soft projected spotlight cookie
     * 2 = per-pixel distance falloff ramp
     */
    pglGetIntegerv( GL_DEPTH_FUNC, &g_flashlight.depthFunc );
    pglGetIntegerv( GL_BLEND_SRC, &g_flashlight.blendSrc );
    pglGetIntegerv( GL_BLEND_DST, &g_flashlight.blendDst );
    pglGetIntegerv( GL_DEPTH_WRITEMASK, &g_flashlight.depthMask );
    g_flashlight.blendEnabled = pglIsEnabled( GL_BLEND ) ? true : false;
    g_flashlight.alphaTestEnabled = pglIsEnabled( GL_ALPHA_TEST ) ? true : false;
    pglGetFloatv( GL_CURRENT_COLOR, g_flashlight.currentColor );

    GL_SelectTexture( 0 );
    pglGetTexEnviv( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &g_flashlight.textureEnvMode[0] );
    pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

    GL_SelectTexture( 1 );
    pglGetTexEnviv( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &g_flashlight.textureEnvMode[1] );
    GL_LoadIdentityTexMatrix();
    GL_Bind( 1, g_flashlight.cookieTexture );
    pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

    GL_SelectTexture( 2 );
    pglGetTexEnviv( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &g_flashlight.textureEnvMode[2] );
    GL_LoadIdentityTexMatrix();
    GL_Bind( 2, g_flashlight.falloffTexture );
    pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

    pglDisable( GL_ALPHA_TEST );
    pglEnable( GL_BLEND );
    pglBlendFunc( GL_ONE, GL_ONE );
    pglDepthFunc( GL_LEQUAL );
    pglDepthMask( GL_FALSE );
    GL_PushPolygonOffset( -1.0f, -1.0f );
    pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

    g_flashlight.active = true;
    return true;
}

static qboolean R_FlashlightSurfaceInFrustum( const msurface_t *surf, vec3_t centroid, int *vertexCount )
{
    qboolean outside[6] = { true, true, true, true, true, true };
    int count = 0;

    VectorClear( centroid );

    for( glpoly2_t *poly = surf->polys; poly; poly = poly->next )
    {
        for( int i = 0; i < poly->numverts; i++ )
        {
            vec3_t relative;
            const float *vertex = poly->verts[i];
            float depth, right, up;
            float planes[6];

            VectorSubtract( vertex, g_flashlight.origin, relative );
            depth = DotProduct( relative, g_flashlight.forward );
            right = DotProduct( relative, g_flashlight.right );
            up = DotProduct( relative, g_flashlight.up );

            planes[0] = depth - FLASHLIGHT_NEAR_Z;
            planes[1] = g_flashlight.farZ - depth;
            planes[2] = depth * g_flashlight.tanHalfFovX - right;
            planes[3] = depth * g_flashlight.tanHalfFovX + right;
            planes[4] = depth * g_flashlight.tanHalfFovY - up;
            planes[5] = depth * g_flashlight.tanHalfFovY + up;

            for( int p = 0; p < 6; p++ )
            {
                if( planes[p] >= 0.0f )
                    outside[p] = false;
            }

            VectorAdd( vertex, centroid, centroid );
            count++;
        }
    }

    if( count == 0 )
        return false;

    VectorScale( centroid, 1.0f / count, centroid );
    *vertexCount = count;

    for( int p = 0; p < 6; p++ )
    {
        if( outside[p] )
            return false;
    }

    return true;
}

void R_FlashlightDrawWorldSurface( msurface_t *surf, texture_t *texture )
{
    vec3_t centroid, normal, toLight;
    int vertexCount;
    float distance;

    if( !g_flashlight.active || !surf || !texture || !surf->plane || !surf->polys )
        return;

    if( FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_TRANSPARENT ))
        return;

    if( !R_FlashlightSurfaceInFrustum( surf, centroid, &vertexCount ))
        return;

    VectorCopy( surf->plane->normal, normal );
    if( FBitSet( surf->flags, SURF_PLANEBACK ))
        VectorScale( normal, -1.0f, normal );

    VectorSubtract( g_flashlight.origin, centroid, toLight );
    distance = sqrtf( DotProduct( toLight, toLight ) );

    if( distance <= 0.01f )
        return;

    VectorScale( toLight, 1.0f / distance, toLight );

    if( DotProduct( normal, toLight ) <= 0.0f )
        return;

    GL_SelectTexture( 0 );
    GL_Bind( 0, texture->gl_texturenum );

    for( glpoly2_t *poly = surf->polys; poly; poly = poly->next )
    {
        pglBegin( GL_POLYGON );

        for( int i = 0; i < poly->numverts; i++ )
        {
            const float *vertex = poly->verts[i];
            vec3_t relative, vertexToLight;
            float depth, right, up;
            float s, t, q;
            float diffuse, vertexDistance, radialAttenuation;

            VectorSubtract( vertex, g_flashlight.origin, relative );
            depth = DotProduct( relative, g_flashlight.forward );
            right = DotProduct( relative, g_flashlight.right );
            up = DotProduct( relative, g_flashlight.up );

            /* Keep homogeneous projection continuous across the near plane.
             * The falloff ramp suppresses points at or behind FLASHLIGHT_NEAR_Z;
             * clamping q avoids singular projective coordinates near the light. */
            float projectedDepth = Q_max( depth, FLASHLIGHT_NEAR_Z );
            s = 0.5f * projectedDepth + 0.5f * right / g_flashlight.tanHalfFovX;
            t = 0.5f * projectedDepth - 0.5f * up / g_flashlight.tanHalfFovY;
            q = projectedDepth;

            VectorSubtract( g_flashlight.origin, vertex, vertexToLight );
            vertexDistance = sqrtf( DotProduct( vertexToLight, vertexToLight ) );

            if( vertexDistance > 0.0001f )
                VectorScale( vertexToLight, 1.0f / vertexDistance, vertexToLight );
            else
                VectorClear( vertexToLight );

            diffuse = Q_max( 0.0f, DotProduct( normal, vertexToLight ));

            /* A mild radial term complements the depth ramp without replacing
             * its per-pixel falloff. */
            radialAttenuation = 1.0f / ( 1.0f + vertexDistance * 0.00035f );
            diffuse *= g_flashlight.intensity * radialAttenuation;

            pglColor4f( diffuse, diffuse, diffuse, 1.0f );
            pglMultiTexCoord2f( GL_TEXTURE0_ARB, vertex[3], vertex[4] );
            pglMultiTexCoord4f( GL_TEXTURE1_ARB, s, t, 0.0f, q );
            pglMultiTexCoord2f( GL_TEXTURE2_ARB,
                ( depth - FLASHLIGHT_NEAR_Z ) / ( g_flashlight.farZ - FLASHLIGHT_NEAR_Z ), 0.5f );
            pglVertex3fv( vertex );
        }

        pglEnd();
    }
}

void R_FlashlightEndWorldPass( void )
{
    if( !g_flashlight.active )
        return;

    GL_SelectTexture( 2 );
    pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, g_flashlight.textureEnvMode[2] );
    GL_SelectTexture( 1 );
    pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, g_flashlight.textureEnvMode[1] );
    GL_SelectTexture( 0 );
    pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, g_flashlight.textureEnvMode[0] );

    GL_PopPolygonOffset();
    pglDepthMask( g_flashlight.depthMask ? GL_TRUE : GL_FALSE );
    pglDepthFunc( g_flashlight.depthFunc );
    pglBlendFunc( g_flashlight.blendSrc, g_flashlight.blendDst );
    if( g_flashlight.blendEnabled ) pglEnable( GL_BLEND );
    else pglDisable( GL_BLEND );
    if( g_flashlight.alphaTestEnabled ) pglEnable( GL_ALPHA_TEST );
    else pglDisable( GL_ALPHA_TEST );
    pglColor4fv( g_flashlight.currentColor );

    GL_SelectTexture( 2 );
    GL_CleanUpTextureUnits( 1 );
    GL_SelectTexture( 0 );

    g_flashlight.active = false;
}
