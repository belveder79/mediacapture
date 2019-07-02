//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#pragma once
#include "Defs.h"
#include <queue>
#include <array>

namespace Vizario 
{ 
	namespace MediaSink
	{

		public ref class RawImageCapturedArgs sealed
		{
		public:
			
			property Platform::Array<float>^ ProjectionMatrix
			{
				Platform::Array<float>^ get() {
					return _projectionMatrix;
				}
			}
			property Platform::Array<float>^ ViewMatrix
			{
				Platform::Array<float>^ get() {
					return _viewMatrix;
				}
			}
			property Platform::Array<float>^ World2CameraMatrix
			{
				Platform::Array<float>^ get() {
					return _world2cameraMatrix;
				}
			}
			property int FrameNum
			{
				int get() {
					return _frameNum;
				}
			}
		internal:
			RawImageCapturedArgs(
				ABI::Windows::Foundation::Numerics::Matrix4x4 viewMatrix,
				ABI::Windows::Foundation::Numerics::Matrix4x4 projectionMatrix,
				Windows::Foundation::Numerics::float4x4 world2camMatrix,
				int fn);

		private:
			Platform::Array<float>^ _projectionMatrix;
			Platform::Array<float>^ _viewMatrix;
			Platform::Array<float>^ _world2cameraMatrix;
			int _frameNum;
		};

		interface class ISinkCallback
		{
			void FireRawImageCaptured(RawImageCapturedArgs ^args);
			Windows::Perception::Spatial::SpatialCoordinateSystem^ GetSpatialCoordinateSystem();
			void OnShutdown();
		};
    
		public ref class VizarioMediaSinkProxy sealed
		{
		public:
			VizarioMediaSinkProxy();
			virtual ~VizarioMediaSinkProxy();

			Windows::Media::IMediaExtension ^GetMFExtensions();

			Windows::Foundation::IAsyncOperation<Windows::Media::IMediaExtension^>^ InitializeAsync(
				Windows::Media::MediaProperties::IMediaEncodingProperties ^videoEncodingProperties,
				Windows::Media::MediaProperties::IMediaEncodingProperties ^audioEncodingProperties
				);

			void SetSpatialCoordinateSystem(Windows::Perception::Spatial::SpatialCoordinateSystem^ spcsptr)
			{
				m_spCurrentCoordinateSystem = spcsptr;
				// sanity test...
				//Platform::IBox<Windows::Foundation::Numerics::float4x4>^ t =
				//	m_spCurrentCoordinateSystem->TryGetTransformTo(m_spCurrentCoordinateSystem);
			}
			Windows::Perception::Spatial::SpatialCoordinateSystem^ GetSpatialCoordinateSystem()
			{
				return m_spCurrentCoordinateSystem;
			}
			/*
			// this one does not get called!
			void SetSpatialCoordinateSystem2(IntPtr spcsptr)
			{
				Windows::Perception::Spatial::ISpatialCoordinateSystem^ r = 
					reinterpret_cast<Windows::Perception::Spatial::ISpatialCoordinateSystem^>(&spcsptr);
				m_spCurrentCoordinateSystem = safe_cast<Windows::Perception::Spatial::SpatialCoordinateSystem^>
					(reinterpret_cast<Object^>(&r));
			}
			*/

			event Windows::Foundation::EventHandler<Object^>^ RawImageCapturedEvent;

		internal:

			void SetMediaStreamProperties(
				Windows::Media::Capture::MediaStreamType MediaStreamType,
				_In_opt_ Windows::Media::MediaProperties::IMediaEncodingProperties ^mediaEncodingProperties
				);

		private:
			void FireRawImageCaptured(RawImageCapturedArgs ^args);
			void OnShutdown();

			ref class RawMediaSinkCallback sealed: ISinkCallback
			{
			public:
				virtual void FireRawImageCaptured(RawImageCapturedArgs ^args)
				{
					_parent->FireRawImageCaptured(args);
				}

				virtual void OnShutdown()
				{
					_parent->OnShutdown();
				}

				virtual Windows::Perception::Spatial::SpatialCoordinateSystem^ GetSpatialCoordinateSystem()
				{
					return _parent->GetSpatialCoordinateSystem();
				}

			internal:
				RawMediaSinkCallback(VizarioMediaSinkProxy ^parent)
					: _parent(parent)
				{
				}

			private:
				VizarioMediaSinkProxy ^_parent;
			};

			void CheckShutdown()
			{
				if (_fShutdown)
				{
					Throw(MF_E_SHUTDOWN);
				}
			}

		private:
			CritSec _critSec;
			ComPtr<IMFMediaSink> _spMediaSink;
			bool _fShutdown;
			Windows::Perception::Spatial::SpatialCoordinateSystem^ m_spCurrentCoordinateSystem;
		};
	} 
} // namespace Vizario.RawSink
