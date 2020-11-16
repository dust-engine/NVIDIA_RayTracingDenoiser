/*
Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "BindingBridge.hlsl"

NRI_RESOURCE( cbuffer, globalConstants, b, 0, 0 )
{
    float4x4 gViewToClip;
    float4 gFrustum;
    float2 gInvScreenSize;
    float2 gScreenSize;
    float gMetersToUnits;
    float gIsOrtho;
    float gUnproject;
    float gDebug;
    float gInf;
    float gReference;
    uint gFrameIndex;
    uint gWorldSpaceMotion;

    float4x4 gWorldToClipPrev;
    float4x4 gViewToWorld;
    float4x4 gWorldToClip;
    float4 gSpecScalingParams;
    float3 gCameraDelta;
    float gAntilag;
    float2 gMotionVectorScale;
    float2 gAntilagRadianceThreshold;
};

#include "NRD_Common.hlsl"

// Inputs
NRI_RESOURCE( Texture2D<float4>, gIn_Normal_Roughness, t, 0, 0 );
NRI_RESOURCE( Texture2D<float>, gIn_ViewZ, t, 1, 0 );
NRI_RESOURCE( Texture2D<float3>, gIn_ObjectMotion, t, 2, 0 );
NRI_RESOURCE( Texture2D<float3>, gIn_InternalData, t, 3, 0 );
NRI_RESOURCE( Texture2D<float4>, gIn_SpecHistory, t, 4, 0 );
NRI_RESOURCE( Texture2D<float4>, gIn_SpecSignal, t, 5, 0 );

// Outputs
NRI_RESOURCE( RWTexture2D<uint2>, gOut_ViewZ_Normal_Roughness_AccumSpeeds, u, 0, 0 );
NRI_RESOURCE( RWTexture2D<float4>, gOut_SpecSignal, u, 1, 0 );
NRI_RESOURCE( RWTexture2D<float4>, gOut_SpecSignalCopy, u, 2, 0 );

groupshared float4 s_SpecSignal[ BUFFER_Y ][ BUFFER_X ];
groupshared float4 s_Normal_ViewZ[ BUFFER_Y ][ BUFFER_X ];
groupshared float s_Roughness[ BUFFER_Y ][ BUFFER_X ];

void Preload( int2 sharedId, int2 globalId )
{
    // TODO: use w = 0 if outside of the screen or use SampleLevel with Clamp sampler
    float4 t = _NRD_FrontEnd_UnpackNormalAndRoughness( gIn_Normal_Roughness[ globalId ] );
    float z = gIn_ViewZ[ globalId ];

    s_SpecSignal[ sharedId.y ][ sharedId.x ] = gIn_SpecSignal[ globalId ];
    s_Normal_ViewZ[ sharedId.y ][ sharedId.x ] = float4( t.xyz, z );
    s_Roughness[ sharedId.y ][ sharedId.x ] = t.w;
}

[numthreads( GROUP_X, GROUP_Y, 1 )]
void main( int2 threadId : SV_GroupThreadId, int2 pixelPos : SV_DispatchThreadId, uint threadIndex : SV_GroupIndex )
{
    float2 pixelUv = float2( pixelPos + 0.5 ) * gInvScreenSize;

    // Debug
    #if ( SHOW_MIPS )
        float4 s = gIn_SpecSignal[ pixelPos ];
        gOut_SpecSignal[ pixelPos ] = s;
        gOut_SpecSignalCopy[ pixelPos ] = s;
        return;
    #endif

    // Rename the 16x16 group into a 18x14 group + some idle threads in the end
    float linearId = ( threadIndex + 0.5 ) / BUFFER_X;
    int2 newId = int2( frac( linearId ) * BUFFER_X, linearId );
    int2 groupBase = pixelPos - threadId - BORDER;

    // Preload into shared memory
    if ( newId.y < RENAMED_GROUP_Y )
        Preload( newId, groupBase + newId );

    newId.y += RENAMED_GROUP_Y;

    if ( newId.y < BUFFER_Y )
        Preload( newId, groupBase + newId );

    GroupMemoryBarrierWithGroupSync( );

    // Early out
    int2 pos = threadId + BORDER;
    float4 t = s_Normal_ViewZ[ pos.y ][ pos.x ];
    float centerZ = t.w;

    [branch]
    if ( abs( centerZ ) > gInf )
    {
        #if( BLACK_OUT_INF_PIXELS == 1 )
            gOut_SpecSignal[ pixelPos ] = 0;
            gOut_SpecSignalCopy[ pixelPos ] = 0;
        #endif
        gOut_ViewZ_Normal_Roughness_AccumSpeeds[ pixelPos ] = PackViewZNormalRoughnessAccumSpeeds( INF, 0.0, float3( 0, 0, 1 ), 1.0, 0.0 );
        return;
    }

    // Normal and roughness
    float3 N = t.xyz;
    float roughness = s_Roughness[ pos.y ][ pos.x ];

    // Internal data
    float virtualMotionAmount;
    float3 specInternalData = UnpackSpecInternalData( gIn_InternalData[ pixelPos ], roughness, virtualMotionAmount );
    float specAccumSpeed = specInternalData.z;

    // Position
    float3 Xv = STL::Geometry::ReconstructViewPosition( pixelUv, gFrustum, centerZ, gIsOrtho );
    float3 X = STL::Geometry::AffineTransform( gViewToWorld, Xv );
    float invDistToPoint = STL::Math::Rsqrt( STL::Math::LengthSquared( Xv ) );
    float3 V = STL::Geometry::RotateVector( gViewToWorld, -Xv ) * invDistToPoint;

    // Local variance
    float sum = 0;
    float4 specM1 = 0;
    float4 specM2 = 0;
    float4 specInput = 0;
    float4 specMaxInput = -INF;
    float4 specMinInput = INF;

    float2 specNormalParams = GetNormalWeightParamsRoughEstimate( roughness );

    [unroll]
    for( int dy = 0; dy <= BORDER * 2; dy++ )
    {
        [unroll]
        for( int dx = 0; dx <= BORDER * 2; dx++ )
        {
            int2 pos = threadId + int2( dx, dy );
            float4 signal = s_SpecSignal[ pos.y ][ pos.x ];
            t = s_Normal_ViewZ[ pos.y ][ pos.x ];

            float w = 1.0;
            if( dx == BORDER && dy == BORDER )
                specInput = signal;
            else
            {
                w = GetBilateralWeight( t.w, centerZ );
                w *= GetNormalWeight( specNormalParams, N, t.xyz ); // TODO: add roughness weight?

                float a = float( w == 0.0 ) * INF;
                specMaxInput = max( signal - a, specMaxInput );
                specMinInput = min( signal + a, specMinInput );
            }

            specM1 += signal * w;
            specM2 += signal * signal * w;
            sum += w;
        }
    }

    float invSum = STL::Math::PositiveRcp( sum );
    specM1 *= invSum;
    specM2 *= invSum;

    float4 specSigma = GetVariance( specM1, specM2 );

    // Apply RCRS
    float rcrsWeight = ( sum - 1.0 ) / ( ( BORDER * 2 + 1 ) * ( BORDER * 2 + 1 ) - 1.0 );
    rcrsWeight = STL::Math::Sqrt01( rcrsWeight );
    rcrsWeight *= 1.0 - gReference;

    float4 rcrsResult = min( specInput, specMaxInput );
    rcrsResult = max( rcrsResult, specMinInput );
    specInput = lerp( specInput, rcrsResult, rcrsWeight );

    // Compute previous position
    float3 motionVector = gIn_ObjectMotion[ pixelPos ] * gMotionVectorScale.xyy;
    float2 pixelUvPrev = STL::Geometry::GetPrevUvFromMotion( pixelUv, X, gWorldToClipPrev, motionVector, gWorldSpaceMotion );
    float isInScreen = float( all( saturate( pixelUvPrev ) == pixelUvPrev ) );
    float3 Xprev = X + motionVector * float( gWorldSpaceMotion != 0 );

    // Virtual position
    float hitDist = GetHitDistance( specInput.w, centerZ, gSpecScalingParams, roughness );
    float4 D = STL::ImportanceSampling::GetSpecularDominantDirection( N, V, roughness, SPEC_DOMINANT_DIRECTION );
    float3 Xvirtual = X - V * hitDist * D.w;

    // Sample history ( surface motion )
    float2 pixelPosPrev = saturate( pixelUvPrev ) * gScreenSize;
    float4 historySurface = BicubicFilterNoCorners( gIn_SpecHistory, gLinearClamp, pixelPosPrev, gInvScreenSize );

    // Sample history ( virtual motion )
    float2 pixelUvVirtualPrev = STL::Geometry::GetScreenUv( gWorldToClipPrev, Xvirtual );
    float4 historyVirtual = gIn_SpecHistory.SampleLevel( gLinearClamp, pixelUvVirtualPrev, 0.0 );

    // Mix histories
    float4 specHistory = lerp( historySurface, historyVirtual, virtualMotionAmount );

    // Compute parallax
    float parallax = ComputeParallax( pixelUv, 1.0, Xprev, gCameraDelta, gWorldToClip );
    float parallaxMod = parallax * ( 1.0 - virtualMotionAmount );

    // History weight
    float motionLength = length( pixelUvPrev - pixelUv );
    float2 specTemporalAccumulationParams = GetTemporalAccumulationParams( isInScreen, specAccumSpeed, motionLength, parallaxMod, roughness );

    // Antilag
    float antiLag = 1.0;
    if( gAntilag != 0.0 && USE_ANTILAG == 1 )
    {
        // TODO: if compression is used delta.x needs to be decompressed, but it doesn't affect the behavior, because heavily compressed values do not lag
        float2 delta = abs( specHistory.xw - specInput.xw ) - specSigma.xw * 2.0;
        delta = STL::Math::LinearStep( float2( gAntilagRadianceThreshold.y, 0.1 ), float2( gAntilagRadianceThreshold.x, 0.01 ), delta );
        delta = STL::Math::Pow01( delta, float2( 2.0, 8.0 ) );

        float fade = specAccumSpeed / ( 1.0 + specAccumSpeed );
        fade *= specTemporalAccumulationParams.x;
        fade *= rcrsWeight;

        antiLag = min( delta.x, delta.y );
        antiLag = lerp( 1.0, antiLag, fade );
    }

    // Clamp history and combine with the current frame
    float4 specMin = specM1 - specSigma * specTemporalAccumulationParams.y;
    float4 specMax = specM1 + specSigma * specTemporalAccumulationParams.y;
    specHistory = clamp( specHistory, specMin, specMax );

    float historyWeight = TS_MAX_HISTORY_WEIGHT * antiLag;
    float4 specResult = lerp( specInput, specHistory, historyWeight * specTemporalAccumulationParams.x );

    // Dither
    STL::Rng::Initialize( pixelPos, gFrameIndex + 2 );
    float2 rnd = STL::Rng::GetFloat2( );
    float dither = 1.0 + ( rnd.y * 2.0 - 1.0 ) * DITHERING_AMPLITUDE;
    specResult *= dither;

    // Get rid of possible negative values
    specResult.xyz = _NRD_YCoCgToLinear( specResult.xyz );
    specResult.w = max( specResult.w, 0.0 );
    specResult.xyz = _NRD_LinearToYCoCg( specResult.xyz );

    // Output
    specAccumSpeed *= antiLag;

    gOut_ViewZ_Normal_Roughness_AccumSpeeds[ pixelPos ] = PackViewZNormalRoughnessAccumSpeeds( centerZ, 0.0, N, roughness, specAccumSpeed );
    gOut_SpecSignal[ pixelPos ] = specResult;

    #if( SHOW_ACCUM_SPEED == 1 )
        specResult.w = saturate( specAccumSpeed / MAX_ACCUM_FRAME_NUM );
    #elif( SHOW_ANTILAG == 1 )
        specResult.w = antiLag;
    #endif

    gOut_SpecSignalCopy[ pixelPos ] = specResult;
}
