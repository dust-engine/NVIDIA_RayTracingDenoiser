/*
Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

size_t DenoiserImpl::AddMethod_NrdSpecular(uint16_t w, uint16_t h)
{
    DispatchDesc desc = {};

    enum class Permanent
    {
        PREV_VIEWZ_NORMAL_ROUGHNESS_ACCUMSPEEDS = PERMANENT_POOL_START,
        HISTORY,
        STABILIZED_HISTORY_1,
        STABILIZED_HISTORY_2,
    };

    m_PermanentPool.push_back( {Format::RG32_UINT, w, h, 1} );
    m_PermanentPool.push_back( {Format::RGBA16_SFLOAT, w, h, 1} );
    m_PermanentPool.push_back( {Format::RGBA16_SFLOAT, w, h, 1} );
    m_PermanentPool.push_back( {Format::RGBA16_SFLOAT, w, h, 1} );

    enum class Transient
    {
        INTERNAL_DATA = TRANSIENT_POOL_START,
        ACCUMULATED,
        SCALED_VIEWZ,
    };

    m_TransientPool.push_back( {Format::RGBA8_UNORM, w, h, 1} );
    m_TransientPool.push_back( {Format::RGBA16_SFLOAT, w, h, 5} );
    m_TransientPool.push_back( {Format::R16_SFLOAT, w, h, 5} );

    // Tricks to save memory
    #define TEMP AsUint(Permanent::STABILIZED_HISTORY_1), 0, 1, AsUint(Permanent::STABILIZED_HISTORY_2)

    PushPass("Specular - pre-blur");
    {
        PushInput( AsUint(ResourceType::IN_NORMAL_ROUGHNESS) );
        PushInput( AsUint(ResourceType::IN_VIEWZ) );
        PushInput( AsUint(ResourceType::IN_SPEC_HIT) );

        PushOutput( TEMP );
        PushOutput( AsUint(Transient::SCALED_VIEWZ) );

        desc.constantBufferDataSize = SumConstants(1, 3, 0, 1);

        AddDispatch(desc, NRD_Specular_PreBlur, w, h);
    }

    PushPass("Specular - temporal accumulation");
    {
        PushInput( AsUint(ResourceType::IN_NORMAL_ROUGHNESS) );
        PushInput( AsUint(ResourceType::IN_VIEWZ) );
        PushInput( AsUint(ResourceType::IN_MV) );
        PushInput( AsUint(Permanent::HISTORY) ); // TODO: STABILIZED_HISTORY can be used here - it looks better, but adds lag because TS uses wide variance clamping
        PushInput( TEMP );
        PushInput( AsUint(Permanent::PREV_VIEWZ_NORMAL_ROUGHNESS_ACCUMSPEEDS) );

        PushOutput( AsUint(Transient::ACCUMULATED) );
        PushOutput( AsUint(Transient::INTERNAL_DATA) );

        desc.constantBufferDataSize = SumConstants(4, 4, 1, 4);

        AddDispatch(desc, NRD_Specular_TemporalAccumulation, w, h);
    }

    PushPass("Specular - mip generation");
    {
        PushInput( AsUint(Transient::ACCUMULATED) );
        PushInput( AsUint(Transient::SCALED_VIEWZ) );

        PushOutput( AsUint(Transient::ACCUMULATED), 1, 1 );
        PushOutput( AsUint(Transient::SCALED_VIEWZ), 1, 1 );
        PushOutput( AsUint(Transient::ACCUMULATED), 2, 1 );
        PushOutput( AsUint(Transient::SCALED_VIEWZ), 2, 1 );
        PushOutput( AsUint(Transient::ACCUMULATED), 3, 1 );
        PushOutput( AsUint(Transient::SCALED_VIEWZ), 3, 1 );
        PushOutput( AsUint(Transient::ACCUMULATED), 4, 1 );
        PushOutput( AsUint(Transient::SCALED_VIEWZ), 4, 1 );

        desc.constantBufferDataSize = SumConstants(0, 0, 0, 0);

        AddDispatchWithExplicitCTASize(desc, NRD_Specular_Mips, DivideUp(w, 2), DivideUp(h, 2), 16, 16);
    }

    PushPass("Specular - history fix");
    {
        PushInput( AsUint(Transient::INTERNAL_DATA) );
        PushInput( AsUint(ResourceType::IN_NORMAL_ROUGHNESS) );
        PushInput( AsUint(Transient::ACCUMULATED), 1, 4 );
        PushInput( AsUint(Transient::SCALED_VIEWZ), 0, 5 );

        PushOutput( AsUint(Transient::ACCUMULATED) );

        desc.constantBufferDataSize = SumConstants(0, 0, 1, 0);

        AddDispatch(desc, NRD_Specular_HistoryFix, w, h);
    }

    PushPass("Specular - blur");
    {
        PushInput( AsUint(ResourceType::IN_NORMAL_ROUGHNESS) );
        PushInput( AsUint(Transient::INTERNAL_DATA) );
        PushInput( AsUint(Transient::ACCUMULATED) );
        PushInput( AsUint(Transient::SCALED_VIEWZ) );

        PushOutput( TEMP );

        desc.constantBufferDataSize = SumConstants(1, 3, 0, 0);

        AddDispatch(desc, NRD_Specular_Blur, w, h);
    }

    PushPass("Specular - post-blur");
    {
        PushInput( AsUint(ResourceType::IN_NORMAL_ROUGHNESS) );
        PushInput( AsUint(Transient::INTERNAL_DATA) );
        PushInput( TEMP );
        PushInput( AsUint(Transient::SCALED_VIEWZ) );
        PushInput( AsUint(Transient::ACCUMULATED) );

        PushOutput( AsUint(Permanent::HISTORY) );

        desc.constantBufferDataSize = SumConstants(1, 3, 0, 1);

        AddDispatch(desc, NRD_Specular_PostBlur, w, h);
    }

    PushPass("Specular - temporal stabilization");
    {
        PushInput( AsUint(ResourceType::IN_NORMAL_ROUGHNESS) );
        PushInput( AsUint(ResourceType::IN_VIEWZ) );
        PushInput( AsUint(ResourceType::IN_MV) );
        PushInput( AsUint(Transient::INTERNAL_DATA) );
        PushInput( AsUint(Permanent::STABILIZED_HISTORY_2), 0, 1, AsUint(Permanent::STABILIZED_HISTORY_1) );
        PushInput( AsUint(Permanent::HISTORY) );

        PushOutput( AsUint(Permanent::PREV_VIEWZ_NORMAL_ROUGHNESS_ACCUMSPEEDS) );
        PushOutput( AsUint(Permanent::STABILIZED_HISTORY_1), 0, 1, AsUint(Permanent::STABILIZED_HISTORY_2) );
        PushOutput( AsUint(ResourceType::OUT_SPEC_HIT) );

        desc.constantBufferDataSize = SumConstants(3, 2, 2, 0);

        AddDispatch(desc, NRD_Specular_TemporalStabilization, w, h);
    }

    #undef TEMP

    return sizeof(NrdSpecularSettings);
}

void DenoiserImpl::UpdateMethod_NrdSpecular(const MethodData& methodData)
{
    enum class Dispatch
    {
        PRE_BLUR,
        TEMPORAL_ACCUMULATION,
        MIP_GENERATION,
        HISTORY_FIX,
        BLUR,
        POST_BLUR,
        TEMPORAL_STABILIZATION,
   };

    const NrdSpecularSettings& settings = methodData.settings.specular;

    float maxAccumulatedFrameNum = float( Min(settings.maxAccumulatedFrameNum, NRD_SPECULAR_MAX_HISTORY_FRAME_NUM) );
    float blurRadius = settings.blurRadius;
    float disocclusionThreshold = settings.disocclusionThreshold;
    bool useAntilag = !m_CommonSettings.forceReferenceAccumulation && settings.antilagSettings.enable;

    if (m_CommonSettings.forceReferenceAccumulation)
    {
        maxAccumulatedFrameNum = settings.maxAccumulatedFrameNum == 0 ? 0.0f : NRD_SPECULAR_MAX_HISTORY_FRAME_NUM;
        blurRadius = 0.0f;
        disocclusionThreshold = 0.005f;
    }

    float4 scalingParams = float4(settings.hitDistanceParameters.A, settings.hitDistanceParameters.B, settings.hitDistanceParameters.C, settings.hitDistanceParameters.D) * m_CommonSettings.metersToUnitsMultiplier;
    float4 trimmingParams_and_blurRadius = float4(settings.lobeTrimmingParameters.A, settings.lobeTrimmingParameters.B, settings.lobeTrimmingParameters.C, blurRadius);
    float4 trimmingParams_and_checkerboardResolveAccumSpeed = float4(settings.lobeTrimmingParameters.A, settings.lobeTrimmingParameters.B, settings.lobeTrimmingParameters.C, m_CheckerboardResolveAccumSpeed);
    uint32_t checkerboard = ((uint32_t)settings.checkerboardMode + 2) % 3;

    // PRE_BLUR
    Constant* data = PushDispatch(methodData, AsUint(Dispatch::PRE_BLUR));
    AddSharedConstants(methodData, data);
    AddFloat4x4(data, m_WorldToView);
    AddFloat4(data, m_Rotator[0]);
    AddFloat4(data, scalingParams);
    AddFloat4(data, trimmingParams_and_blurRadius);
    AddUint(data, checkerboard);
    ValidateConstants(data);

    // TEMPORAL_ACCUMULATION
    data = PushDispatch(methodData, AsUint(Dispatch::TEMPORAL_ACCUMULATION));
    AddSharedConstants(methodData, data);
    AddFloat4x4(data, m_WorldToViewPrev);
    AddFloat4x4(data, m_WorldToClipPrev);
    AddFloat4x4(data, m_ViewToWorld);
    AddFloat4x4(data, m_WorldToClip);
    AddFloat4(data, m_FrustumPrev);
    AddFloat4(data, float4( m_CameraDelta.x, m_CameraDelta.y, m_CameraDelta.z, m_IsOrthoPrev ) );
    AddFloat4(data, scalingParams);
    AddFloat4(data, trimmingParams_and_checkerboardResolveAccumSpeed);
    AddFloat2(data, m_CommonSettings.motionVectorScale[0], m_CommonSettings.motionVectorScale[1]);
    AddFloat(data, disocclusionThreshold);
    AddFloat(data, m_JitterDelta );
    AddFloat(data, float(maxAccumulatedFrameNum));
    AddUint(data, checkerboard);
    ValidateConstants(data);

    // MIP_GENERATION
    data = PushDispatch(methodData, AsUint(Dispatch::MIP_GENERATION));
    AddSharedConstants(methodData, data);
    ValidateConstants(data);

    // HISTORY_FIX
    data = PushDispatch(methodData, AsUint(Dispatch::HISTORY_FIX));
    AddSharedConstants(methodData, data);
    AddUint2(data, methodData.desc.fullResolutionWidth, methodData.desc.fullResolutionHeight);
    ValidateConstants(data);

    // BLUR
    data = PushDispatch(methodData, AsUint(Dispatch::BLUR));
    AddSharedConstants(methodData, data);
    AddFloat4x4(data, m_WorldToView);
    AddFloat4(data, m_Rotator[1]);
    AddFloat4(data, scalingParams);
    AddFloat4(data, trimmingParams_and_blurRadius);
    ValidateConstants(data);

    // POST_BLUR
    data = PushDispatch(methodData, AsUint(Dispatch::POST_BLUR));
    AddSharedConstants(methodData, data);
    AddFloat4x4(data, m_WorldToView);
    AddFloat4(data, m_Rotator[2]);
    AddFloat4(data, scalingParams);
    AddFloat4(data, trimmingParams_and_blurRadius);
    AddFloat(data, settings.postBlurMaxAdaptiveRadiusScale);
    ValidateConstants(data);

    // TEMPORAL_STABILIZATION
    data = PushDispatch(methodData, AsUint(Dispatch::TEMPORAL_STABILIZATION));
    AddSharedConstants(methodData, data);
    AddFloat4x4(data, m_WorldToClipPrev);
    AddFloat4x4(data, m_ViewToWorld);
    AddFloat4x4(data, m_WorldToClip);
    AddFloat4(data, scalingParams);
    AddFloat4(data, float4( m_CameraDelta.x, m_CameraDelta.y, m_CameraDelta.z, useAntilag ? 1.0f : 0.0f ) );
    AddFloat2(data, m_CommonSettings.motionVectorScale[0], m_CommonSettings.motionVectorScale[1]);
    AddFloat2(data, settings.antilagSettings.intensityThresholdMin, settings.antilagSettings.intensityThresholdMax);
    ValidateConstants(data);
}
