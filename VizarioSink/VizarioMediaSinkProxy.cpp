//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#include "pch.h"
#include "VizarioMediaSinkProxy.h"
#include "Defs.h"
#include "VizarioMediaSink.h"

using namespace Vizario::MediaSink;

RawImageCapturedArgs::RawImageCapturedArgs(
	ABI::Windows::Foundation::Numerics::Matrix4x4 viewMatrix,
	ABI::Windows::Foundation::Numerics::Matrix4x4 projectionMatrix,
	Windows::Foundation::Numerics::float4x4 world2camMatrix,
	int fn)
{
	_world2cameraMatrix = ref new Platform::Array<float>(16);
	_world2cameraMatrix[0] = world2camMatrix.m11; 	_world2cameraMatrix[1] = world2camMatrix.m12; 	_world2cameraMatrix[2] = world2camMatrix.m13; 	_world2cameraMatrix[3] = world2camMatrix.m14;
	_world2cameraMatrix[4] = world2camMatrix.m21; 	_world2cameraMatrix[5] = world2camMatrix.m22; 	_world2cameraMatrix[6] = world2camMatrix.m23; 	_world2cameraMatrix[7] = world2camMatrix.m14;
	_world2cameraMatrix[8] = world2camMatrix.m31; 	_world2cameraMatrix[9] = world2camMatrix.m32; 	_world2cameraMatrix[10] = world2camMatrix.m33; 	_world2cameraMatrix[11] = world2camMatrix.m14;
	_world2cameraMatrix[12] = world2camMatrix.m41; 	_world2cameraMatrix[13] = world2camMatrix.m42; 	_world2cameraMatrix[14] = world2camMatrix.m43; 	_world2cameraMatrix[15] = world2camMatrix.m14;
	_viewMatrix = ref new Platform::Array<float>(16);
	_viewMatrix[0] = viewMatrix.M11; 	_viewMatrix[1] = viewMatrix.M12; 	_viewMatrix[2] = viewMatrix.M13; 	_viewMatrix[3] = viewMatrix.M14;
	_viewMatrix[4] = viewMatrix.M21; 	_viewMatrix[5] = viewMatrix.M22; 	_viewMatrix[6] = viewMatrix.M23; 	_viewMatrix[7] = viewMatrix.M14;
	_viewMatrix[8] = viewMatrix.M31; 	_viewMatrix[9] = viewMatrix.M32; 	_viewMatrix[10] = viewMatrix.M33; 	_viewMatrix[11] = viewMatrix.M14;
	_viewMatrix[12] = viewMatrix.M41; 	_viewMatrix[13] = viewMatrix.M42; 	_viewMatrix[14] = viewMatrix.M43; 	_viewMatrix[15] = viewMatrix.M14;
	_projectionMatrix = ref new Platform::Array<float>(16);
	_projectionMatrix[0] = projectionMatrix.M11; 	_projectionMatrix[1] = projectionMatrix.M12; 	_projectionMatrix[2] = projectionMatrix.M13; 	_projectionMatrix[3] = projectionMatrix.M14;
	_projectionMatrix[4] = projectionMatrix.M21; 	_projectionMatrix[5] = projectionMatrix.M22; 	_projectionMatrix[6] = projectionMatrix.M23; 	_projectionMatrix[7] = projectionMatrix.M14;
	_projectionMatrix[8] = projectionMatrix.M31; 	_projectionMatrix[9] = projectionMatrix.M32; 	_projectionMatrix[10] = projectionMatrix.M33; 	_projectionMatrix[11] = projectionMatrix.M14;
	_projectionMatrix[12] = projectionMatrix.M41; 	_projectionMatrix[13] = projectionMatrix.M42; 	_projectionMatrix[14] = projectionMatrix.M43; 	_projectionMatrix[15] = projectionMatrix.M14;
	_frameNum = fn;
}

VizarioMediaSinkProxy::VizarioMediaSinkProxy()
{
}

VizarioMediaSinkProxy::~VizarioMediaSinkProxy()
{
    AutoLock lock(_critSec);

    if (_spMediaSink != nullptr)
    {
        _spMediaSink->Shutdown();
        _spMediaSink = nullptr;
    }
}

Windows::Media::IMediaExtension ^VizarioMediaSinkProxy::GetMFExtensions()
{
    AutoLock lock(_critSec);

    if (_spMediaSink == nullptr)
    {
        Throw(MF_E_NOT_INITIALIZED);
    }

    ComPtr<IInspectable> spInspectable;
    ThrowIfError(_spMediaSink.As(&spInspectable));

    return safe_cast<IMediaExtension^>(reinterpret_cast<Object^>(spInspectable.Get()));
}


Windows::Foundation::IAsyncOperation<IMediaExtension^>^ VizarioMediaSinkProxy::InitializeAsync(
    Windows::Media::MediaProperties::IMediaEncodingProperties ^audioEncodingProperties,
    Windows::Media::MediaProperties::IMediaEncodingProperties ^videoEncodingProperties
    )
{
    return concurrency::create_async([this, videoEncodingProperties, audioEncodingProperties]()
    {
        AutoLock lock(_critSec);
        CheckShutdown();

        if (_spMediaSink != nullptr)
        {
            Throw(MF_E_ALREADY_INITIALIZED);
        }

        // Prepare the MF extension
        ThrowIfError(MakeAndInitialize<CMediaSink>(&_spMediaSink, ref new RawMediaSinkCallback(this), audioEncodingProperties, videoEncodingProperties));

        ComPtr<IInspectable> spInspectable;
        ThrowIfError(_spMediaSink.As(&spInspectable));

        return safe_cast<IMediaExtension^>(reinterpret_cast<Object^>(spInspectable.Get()));
    });
}

void VizarioMediaSinkProxy::FireRawImageCaptured(RawImageCapturedArgs ^args)
{
	RawImageCapturedEvent(this, args);
}

void VizarioMediaSinkProxy::OnShutdown()
{
    AutoLock lock(_critSec);
    if (_fShutdown)
    {
        return;
    }
    _fShutdown = true;
    _spMediaSink = nullptr;
}
