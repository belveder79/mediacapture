//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#include "pch.h"
#include "VizarioStreamSink.h"
#include "VizarioMediaSink.h"
#include "MediaBufferWrapper.h"
#include "VizarioMediaSinkProxy.h"

using namespace Vizario::MediaSink;
/*
using namespace rapidjson;

// THIS IS FROM VIZARIOCORE - TODO: USE FULL TEMPLATE SET FROM THERE LATER ON...
std::basic_string<char16_t> convertUTFFormat8toV(const std::basic_string<char>& data)
{
#if _MSC_VER >= 1900
	char16_t buf[16384];
	MultiByteToWideChar(CP_ACP, 0, data.data(), -1, reinterpret_cast<wchar_t*>(buf), 16384);
	return std::basic_string<char16_t>(buf);
#else
	std::wstring_convert<deletable_facet<std::codecvt<char16_t, char, std::mbstate_t> >, char16_t> convert;
	return convert.from_bytes(data);
#endif
}
*/

#define SET_SAMPLE_FLAG(dest, destMask, pSample, flagName) \
    { \
        UINT32 unValue; \
        if (SUCCEEDED(pSample->GetUINT32(MFSampleExtension_##flagName, &unValue))) \
        { \
            dest |= (unValue != FALSE) ? StspSampleFlag_##flagName : 0; \
            destMask |= StspSampleFlag_##flagName; \
        } \
    }

CStreamSink::CStreamSink(DWORD dwIdentifier)
    : _frameCnt(0)
	, _cRef(1)
    , _dwIdentifier(dwIdentifier)
    , _state(State_TypeNotSet)
    , _IsShutdown(false)
    , _fIsVideo(false)
    , _fGetStartTimeFromSample(false)
    , _StartTime(0)
    , _WorkQueueId(0)
    , _pParent(nullptr)
#pragma warning(push)
#pragma warning(disable:4355)
    , _WorkQueueCB(this, &CStreamSink::OnDispatchWorkItem)
#pragma warning(pop)
{
    ZeroMemory(&_guiCurrentSubtype, sizeof(_guiCurrentSubtype));
}
/*
void CStreamSink::OnLocatabilityChanged(
	_In_ Windows::Perception::Spatial::SpatialLocator^ sender,
	_In_ Platform::Object^ args)
{
	switch (sender->Locatability)
	{
	case Windows::Perception::Spatial::SpatialLocatability::Unavailable:
	{
		// Holograms cannot be rendered.
#if DBG_ENABLE_INFORMATIONAL_LOGGING
		dbg::trace(
			L"SpatialPerception::OnLocatabilityChanged: warning positional tracking is %s!\n",
			sender->Locatability.ToString());
#endif // DBG_ENABLE_INFORMATIONAL_LOGGING 
	}
	break;

	// In the following three cases, it is still possible to place holograms using a
	// SpatialLocatorAttachedFrameOfReference.
	case Windows::Perception::Spatial::SpatialLocatability::PositionalTrackingActivating:
		// The system is preparing to use positional tracking.

	case Windows::Perception::Spatial::SpatialLocatability::OrientationOnly:
		// Positional tracking has not been activated.

	case Windows::Perception::Spatial::SpatialLocatability::PositionalTrackingInhibited:
		// Positional tracking is temporarily inhibited. User action may be required
		// in order to restore positional tracking.
		break;

	case Windows::Perception::Spatial::SpatialLocatability::PositionalTrackingActive:
		// Positional tracking is active. World-locked content can be rendered.
		break;
	}
}
*/
CStreamSink::~CStreamSink()
{
    assert(_IsShutdown);
}


// IUnknown methods

IFACEMETHODIMP CStreamSink::QueryInterface(REFIID riid, void **ppv)
{
    if (ppv == nullptr)
    {
        return E_POINTER;
    }
    (*ppv) = nullptr;

    HRESULT hr = S_OK;
    if (riid == IID_IUnknown || 
        riid == IID_IMFStreamSink ||
        riid == IID_IMFMediaEventGenerator)
    {
        (*ppv) = static_cast<IMFStreamSink*>(this);
        AddRef();
    }
    else if (riid == IID_IMFMediaTypeHandler)
    {
        (*ppv) = static_cast<IMFMediaTypeHandler*>(this);
        AddRef();
    }
    else
    {
        hr = E_NOINTERFACE;
    }

    if (FAILED(hr) && riid == IID_IMarshal)
    {
        if (_spFTM == nullptr)
        {
            AutoLock lock(_critSec);
            if (_spFTM == nullptr)
            {
                hr = CoCreateFreeThreadedMarshaler(static_cast<IMFStreamSink*>(this), &_spFTM);
            }
        }

        if (SUCCEEDED(hr))
        {
            if (_spFTM == nullptr)
            {
                hr = E_UNEXPECTED;
            }
            else
            {
                hr = _spFTM.Get()->QueryInterface(riid, ppv);
            }
        }
    }

    return hr;
}

IFACEMETHODIMP_(ULONG) CStreamSink::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

IFACEMETHODIMP_(ULONG) CStreamSink::Release()
{
    long cRef = InterlockedDecrement(&_cRef);
    if (cRef == 0)
    {
        delete this;
    }
    return cRef;
}

// IMFMediaEventGenerator methods.
// Note: These methods call through to the event queue helper object.

IFACEMETHODIMP CStreamSink::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState)
{
    HRESULT hr = S_OK;

    AutoLock lock(_critSec);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = _spEventQueue->BeginGetEvent(pCallback, punkState);
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CStreamSink::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent)
{
    HRESULT hr = S_OK;

    AutoLock lock(_critSec);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = _spEventQueue->EndGetEvent(pResult, ppEvent);
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CStreamSink::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent)
{
    // NOTE:
    // GetEvent can block indefinitely, so we don't hold the lock.
    // This requires some juggling with the event queue pointer.

    HRESULT hr = S_OK;

    ComPtr<IMFMediaEventQueue> spQueue;

    {
        AutoLock lock(_critSec);

        // Check shutdown
        hr = CheckShutdown();

        // Get the pointer to the event queue.
        if (SUCCEEDED(hr))
        {
            spQueue = _spEventQueue;
        }
    }

    // Now get the event.
    if (SUCCEEDED(hr))
    {
        hr = spQueue->GetEvent(dwFlags, ppEvent);
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CStreamSink::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, PROPVARIANT const *pvValue)
{
    HRESULT hr = S_OK;

    AutoLock lock(_critSec);

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = _spEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
    }

    TRACEHR_RET(hr);
}

/// IMFStreamSink methods

IFACEMETHODIMP CStreamSink::GetMediaSink(IMFMediaSink **ppMediaSink)
{
    if (ppMediaSink == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        _spSink.Get()->QueryInterface(IID_IMFMediaSink, (void**)ppMediaSink);
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CStreamSink::GetIdentifier(DWORD *pdwIdentifier)
{
    if (pdwIdentifier == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        *pdwIdentifier = _dwIdentifier;
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CStreamSink::GetMediaTypeHandler(IMFMediaTypeHandler **ppHandler)
{
    if (ppHandler == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    // This stream object acts as its own type handler, so we QI ourselves.
    if (SUCCEEDED(hr))
    {
        hr = QueryInterface(IID_IMFMediaTypeHandler, (void**)ppHandler);
    }

    TRACEHR_RET(hr);
}

// We received a sample from an upstream component
IFACEMETHODIMP CStreamSink::ProcessSample(IMFSample *pSample)
{
    if (pSample == nullptr)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;

    AutoLock lock(_critSec);

    hr = CheckShutdown();

    // Validate the operation.
    if (SUCCEEDED(hr))
    {
        hr = ValidateOperation(OpProcessSample);
    }
	/*
    if (SUCCEEDED(hr) && _fWaitingForFirstSample && !_Connected)
    {
        _spFirstVideoSample = pSample;
        _fWaitingForFirstSample = false;

        hr = QueueEvent(MEStreamSinkRequestSample, GUID_NULL, hr, nullptr);
    }
    else*/ if (SUCCEEDED(hr))
    {
        // Add the sample to the sample queue.
        if (SUCCEEDED(hr))
        {
            hr = _SampleQueue.InsertBack(pSample);
        }

        // Unless we are paused, start an async operation to dispatch the next sample.
        if (SUCCEEDED(hr))
        {
            if (_state != State_Paused)
            {
                // Queue the operation.
                hr = QueueAsyncOperation(OpProcessSample);
            }
        }
    }

    TRACEHR_RET(hr);
}

// The client can call PlaceMarker at any time. In response,
// we need to queue an MEStreamSinkMarker event, but not until
// *after *we have processed all samples that we have received
// up to this point.
//
// Also, in general you might need to handle specific marker
// types, although this sink does not.

IFACEMETHODIMP CStreamSink::PlaceMarker(
    MFSTREAMSINK_MARKER_TYPE eMarkerType,
    const PROPVARIANT *pvarMarkerValue,
    const PROPVARIANT *pvarContextValue)
{
    AutoLock lock(_critSec);

    HRESULT hr = S_OK;
    ComPtr<IMarker> spMarker;

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = ValidateOperation(OpPlaceMarker);
    }

    if (SUCCEEDED(hr))
    {
        hr = CreateMarker(eMarkerType, pvarMarkerValue, pvarContextValue, &spMarker);
    }

    if (SUCCEEDED(hr))
    {
        hr = _SampleQueue.InsertBack(spMarker.Get());
    }

    // Unless we are paused, start an async operation to dispatch the next sample/marker.
    if (SUCCEEDED(hr))
    {
        if (_state != State_Paused)
        {
            // Queue the operation.
            hr = QueueAsyncOperation(OpPlaceMarker); // Increments ref count on pOp.
        }
    }

    TRACEHR_RET(hr);
}

// Discards all samples that were not processed yet.
IFACEMETHODIMP CStreamSink::Flush()
{
    AutoLock lock(_critSec);
    HRESULT hr = S_OK;
    try
    {
        ThrowIfError(CheckShutdown());

        // Note: Even though we are flushing data, we still need to send
        // any marker events that were queued.
        DropSamplesFromQueue();
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    TRACEHR_RET(hr);
}


/// IMFMediaTypeHandler methods

// Check if a media type is supported.
IFACEMETHODIMP CStreamSink::IsMediaTypeSupported(
    /* [in] */ IMFMediaType *pMediaType,
    /* [out] */ IMFMediaType **ppMediaType)
{
    if (pMediaType == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    GUID majorType = GUID_NULL;
    UINT cbSize = 0;

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = pMediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
    }

    // First make sure it's video or audio type.
    if (SUCCEEDED(hr))
    {
        if (majorType != MFMediaType_Video && majorType != MFMediaType_Audio)
        {
            hr = MF_E_INVALIDTYPE;
        }
    }

    if (SUCCEEDED(hr) && _spCurrentType != nullptr)
    {
        GUID guiNewSubtype;
        if (FAILED(pMediaType->GetGUID(MF_MT_SUBTYPE, &guiNewSubtype)) || 
            guiNewSubtype != _guiCurrentSubtype)
        {
            hr = MF_E_INVALIDTYPE;
        }
    }

    // We don't return any "close match" types.
    if (ppMediaType)
    {
        *ppMediaType = nullptr;
    }

    TRACEHR_RET(hr);
}


// Return the number of preferred media types.
IFACEMETHODIMP CStreamSink::GetMediaTypeCount(DWORD *pdwTypeCount)
{
    if (pdwTypeCount == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        // We've got only one media type
        *pdwTypeCount = 1;
    }

    TRACEHR_RET(hr);
}


// Return a preferred media type by index.
IFACEMETHODIMP CStreamSink::GetMediaTypeByIndex(
    /* [in] */ DWORD dwIndex,
    /* [out] */ IMFMediaType **ppType)
{
    if (ppType == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if ( dwIndex > 0 )
    {
        hr = MF_E_NO_MORE_TYPES;
    }
    else
    {
        *ppType = _spCurrentType.Get();
        if (*ppType != nullptr)
        {
            (*ppType)->AddRef();
        }
    }

    TRACEHR_RET(hr);
}


// Set the current media type.
IFACEMETHODIMP CStreamSink::SetCurrentMediaType(IMFMediaType *pMediaType)
{
    HRESULT hr = S_OK;
    try
    {
        if (pMediaType == nullptr)
        {
            Throw(E_INVALIDARG);
        }
        AutoLock lock(_critSec);

        ThrowIfError(CheckShutdown());

        // We don't allow format changes after streaming starts.
        ThrowIfError(ValidateOperation(OpSetMediaType));

        // We set media type already
        if (_state >= State_Ready)
        {
            ThrowIfError(IsMediaTypeSupported(pMediaType, nullptr));
        }

        GUID guiMajorType;
        pMediaType->GetMajorType(&guiMajorType);
        _fIsVideo = (guiMajorType == MFMediaType_Video);

		// initialize for Video only!
		if (_fIsVideo )// && (m_locator == nullptr))
		{
			m_spCurrentCoordinateSystem = _pParent->GetSpatialCoordinateSystem();
			/*
			// HERE WE SHOULD GET THE UNITY STUFF
				// SEE https://github.com/microsoft/MixedRealityToolkit-Unity/blob/9ab0e9b69afe45f7856b5731436ae427c3fe7cc4/Assets/MixedRealityToolkit.Providers/WindowsMixedReality/WindowsMixedRealityUtilities.cs
				// AND https://docs.unity3d.com/ScriptReference/XR.WSA.WorldManager.GetNativeISpatialCoordinateSystemPtr.html
				//public static SpatialCoordinateSystem SpatialCoordinateSystem = > 
				//	spatialCoordinateSystem ? ? (spatialCoordinateSystem = Marshal.GetObjectForIUnknown(WorldManager.GetNativeISpatialCoordinateSystemPtr()) as SpatialCoordinateSystem);
			_originFrameOfReference2 = Windows::Perception::Spatial::SpatialStageFrameOfReference::Current;
			if (_originFrameOfReference2 == nullptr)
			{
				std::string bla("CoreBridgeSink: Creating new CreateStationaryFrameOfReferenceAtCurrentLocation!");
				VizarioCore_passLogFromNativeLib(convertUTFFormat8toV(bla).c_str());
				m_locator = Windows::Perception::Spatial::SpatialLocator::GetDefault();
				if (m_locator != nullptr)
				{
					_originFrameOfReference = m_locator->CreateStationaryFrameOfReferenceAtCurrentLocation();
					m_spCurrentCoordinateSystem = _originFrameOfReference->CoordinateSystem;
					
					//_locatabilityChangedToken = ...
					//	m_locator->LocatabilityChanged +=
					//	ref new Windows::Foundation::TypedEventHandler<Windows::Perception::Spatial::SpatialLocator^, Platform::Object^>(this, &CStreamSink::OnLocatabilityChanged);
					
				}
			}
			else
			{
				std::string bla("CoreBridgeSink: Using SpatialStageFrameReference!");
				VizarioCore_passLogFromNativeLib(convertUTFFormat8toV(bla).c_str());
				m_spCurrentCoordinateSystem = _originFrameOfReference2->CoordinateSystem;
			}
			*/
		}


        ThrowIfError(MFCreateMediaType(_spCurrentType.ReleaseAndGetAddressOf()));
        ThrowIfError(pMediaType->CopyAllItems(_spCurrentType.Get()));
        ThrowIfError(_spCurrentType->GetGUID(MF_MT_SUBTYPE, &_guiCurrentSubtype));
        if (_state < State_Ready)
        {
            _state = State_Ready;
        }
        else if (_state > State_Ready)
        {
            ComPtr<IMFMediaType> spType;
            ThrowIfError(MFCreateMediaType(&spType));
            ThrowIfError(pMediaType->CopyAllItems(spType.Get()));
            ProcessFormatChange(spType.Get());
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }
    TRACEHR_RET(hr);
}

// Return the current media type, if any.
IFACEMETHODIMP CStreamSink::GetCurrentMediaType(IMFMediaType **ppMediaType)
{
    if (ppMediaType == nullptr)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        if (_spCurrentType == nullptr)
        {
            hr = MF_E_NOT_INITIALIZED;
        }
    }

    if (SUCCEEDED(hr))
    {
        *ppMediaType = _spCurrentType.Get();
        (*ppMediaType)->AddRef();
    }

    TRACEHR_RET(hr);
}


// Return the major type GUID.
IFACEMETHODIMP CStreamSink::GetMajorType(GUID *pguidMajorType)
{
    if (pguidMajorType == nullptr)
    {
        return E_INVALIDARG;
    }

    if (!_spCurrentType)
    {
        return MF_E_NOT_INITIALIZED;
    }
    
    *pguidMajorType = (_fIsVideo) ? MFMediaType_Video : MFMediaType_Audio;

    return S_OK;
}


// private methods
HRESULT CStreamSink::Initialize(CMediaSink *pParent)//, INetworkChannel ^networkSender)
{
    assert(pParent != nullptr);

    HRESULT hr = S_OK;

    // Create the event queue helper.
    hr = MFCreateEventQueue(&_spEventQueue);

    // Allocate a new work queue for async operations.
    if (SUCCEEDED(hr))
    {
        hr = MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_STANDARD, &_WorkQueueId);
    }

    if (SUCCEEDED(hr))
    {
        _spSink = pParent;
        _pParent = pParent;
    }

    TRACEHR_RET(hr);
}


// Called when the presentation clock starts.
HRESULT CStreamSink::Start(MFTIME start)
{
    AutoLock lock(_critSec);

    HRESULT hr = S_OK;

    hr = ValidateOperation(OpStart);

    if (SUCCEEDED(hr))
    {
        if (start != PRESENTATION_CURRENT_POSITION)
        {
            _StartTime = start;        // Cache the start time.
            _fGetStartTimeFromSample = false;
        }
        else
        {
            _fGetStartTimeFromSample = true;
        }
        _state = State_Started;
//        _fWaitingForFirstSample = _fIsVideo;
        hr = QueueAsyncOperation(OpStart);
    }

    TRACEHR_RET(hr);
}

// Called when the presentation clock stops.
HRESULT CStreamSink::Stop()
{
    AutoLock lock(_critSec);

    HRESULT hr = S_OK;

    hr = ValidateOperation(OpStop);

    if (SUCCEEDED(hr))
    {
        _state = State_Stopped;
        hr = QueueAsyncOperation(OpStop);
    }

    TRACEHR_RET(hr);
}

// Called when the presentation clock pauses.
HRESULT CStreamSink::Pause()
{
    AutoLock lock(_critSec);

    HRESULT hr = S_OK;

    hr = ValidateOperation(OpPause);

    if (SUCCEEDED(hr))
    {
        _state = State_Paused;
        hr = QueueAsyncOperation(OpPause);
    }

    TRACEHR_RET(hr);
}

// Called when the presentation clock restarts.
HRESULT CStreamSink::Restart()
{
    AutoLock lock(_critSec);

    HRESULT hr = S_OK;

    hr = ValidateOperation(OpRestart);

    if (SUCCEEDED(hr))
    {
        _state = State_Started;
        hr = QueueAsyncOperation(OpRestart);
    }

    TRACEHR_RET(hr);
}

// Class-static matrix of operations vs states.
// If an entry is TRUE, the operation is valid from that state.
BOOL CStreamSink::ValidStateMatrix[CStreamSink::State_Count][CStreamSink::Op_Count] =
{
// States:    Operations:
//            SetType   Start     Restart   Pause     Stop      Sample    Marker   
/* NotSet */  TRUE,     FALSE,    FALSE,    FALSE,    FALSE,    FALSE,    FALSE,   

/* Ready */   TRUE,     TRUE,     FALSE,    TRUE,     TRUE,     FALSE,    TRUE,    

/* Start */   TRUE,     TRUE,     FALSE,    TRUE,     TRUE,     TRUE,     TRUE,    

/* Pause */   TRUE,     TRUE,     TRUE,     TRUE,     TRUE,     TRUE,     TRUE,    

/* Stop */    TRUE,     TRUE,     FALSE,    FALSE,    TRUE,     FALSE,    TRUE,    

};

// Checks if an operation is valid in the current state.
HRESULT CStreamSink::ValidateOperation(StreamOperation op)
{
    assert(!_IsShutdown);

    HRESULT hr = S_OK;

    if (ValidStateMatrix[_state][op])
    {
        return S_OK;
    }
    else if (_state == State_TypeNotSet)
    {
        return MF_E_NOT_INITIALIZED;
    }
    else
    {
        return MF_E_INVALIDREQUEST;
    }
}

// Shuts down the stream sink.
HRESULT CStreamSink::Shutdown()
{
    AutoLock lock(_critSec);

    if (!_IsShutdown)
    {
        if (_spEventQueue)
        {
            _spEventQueue->Shutdown();
        }

        MFUnlockWorkQueue(_WorkQueueId);

        _SampleQueue.Clear();

        _spSink.Reset();
        _spEventQueue.Reset();
        _spByteStream.Reset();
        _spCurrentType.Reset();

        _IsShutdown = true;
    }

    return S_OK;
}


// Puts an async operation on the work queue.
HRESULT CStreamSink::QueueAsyncOperation(StreamOperation op)
{
    HRESULT hr = S_OK;
    ComPtr<CAsyncOperation> spOp;
    spOp.Attach(new CAsyncOperation(op)); // Created with ref count = 1
    if (!spOp)
    {
        hr = E_OUTOFMEMORY;
    }

    if (SUCCEEDED(hr))
    {
        hr = MFPutWorkItem2(_WorkQueueId, 0, &_WorkQueueCB, spOp.Get());
    }

    TRACEHR_RET(hr);
}

HRESULT CStreamSink::OnDispatchWorkItem(IMFAsyncResult *pAsyncResult)
{
    // Called by work queue thread. Need to hold the critical section.
    AutoLock lock(_critSec);

    try
    {
        ComPtr<IUnknown> spState;

        ThrowIfError(pAsyncResult->GetState(&spState));

        // The state object is a CAsncOperation object.
        CAsyncOperation *pOp = static_cast<CAsyncOperation *>(spState.Get());
        StreamOperation op = pOp->m_op;

        switch (op)
        {
        case OpStart:
        case OpRestart:
            // Send MEStreamSinkStarted.
            ThrowIfError(QueueEvent(MEStreamSinkStarted, GUID_NULL, S_OK, nullptr));

            // There might be samples queue from earlier (ie, while paused).
            bool fRequestMoreSamples;
            /*
			if (!_Connected)
            {
                // Just drop samples if we are not connected
                fRequestMoreSamples = DropSamplesFromQueue();
            }
            else
            {*/
                fRequestMoreSamples = SendSampleFromQueue();
            //}
            if (fRequestMoreSamples)
            {
                // If false there is no samples in the queue now so request one
                ThrowIfError(QueueEvent(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr));
            }
            break;

        case OpStop:
            // Drop samples from queue.
            DropSamplesFromQueue();

            // Send the event even if the previous call failed.
            ThrowIfError(QueueEvent(MEStreamSinkStopped, GUID_NULL, S_OK, nullptr));
            break;

        case OpPause:
            ThrowIfError(QueueEvent(MEStreamSinkPaused, GUID_NULL, S_OK, nullptr));
            break;

        case OpProcessSample:
        case OpPlaceMarker:
        case OpSetMediaType:
            DispatchProcessSample(pOp);
            break;
        }
    }
    catch(Exception ^exc)
    {
        HandleError(exc->HResult);
    }
    return S_OK;
}

// Complete a ProcessSample or PlaceMarker request.
void CStreamSink::DispatchProcessSample(CAsyncOperation *pOp)
{
    assert(pOp != nullptr);
    bool fRequestMoreSamples = false;
    /*if (!_Connected)
    {
        fRequestMoreSamples = DropSamplesFromQueue();
    }
    else
    {*/
        fRequestMoreSamples = SendSampleFromQueue();
    //}

    // Ask for another sample
    if (fRequestMoreSamples)
    {
        if (pOp->m_op == OpProcessSample)
        {
            ThrowIfError(QueueEvent(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr));
        }
    }
}

// Drop samples in the queue
bool CStreamSink::DropSamplesFromQueue()
{
    ProcessSamplesFromQueue(true);

    return true;
}

// Send sample from the queue
bool CStreamSink::SendSampleFromQueue()
{
    return ProcessSamplesFromQueue(false);
}

bool CStreamSink::ProcessSamplesFromQueue(bool fFlush)
{

	bool fNeedMoreSamples = false;

    ComPtr<IUnknown> spunkSample;

    bool fSendSamples = true;
    bool fSendEOS = false;
    
    if (FAILED(_SampleQueue.RemoveFront(&spunkSample)))
    {
        fNeedMoreSamples = true;
        fSendSamples = false;
    }

    while (fSendSamples)
    {
        ComPtr<IMFSample> spSample;
//        ComPtr<IBufferPacket> spPacket;
        bool fProcessingSample = false;
        assert(spunkSample);

        // Figure out if this is a marker or a sample.
        // If this is a sample, write it to the file.
        // Now handle the sample/marker appropriately.
        if (SUCCEEDED(spunkSample.As(&spSample)))
        {
            assert(spSample);    // Not a marker, must be a sample

            if (!fFlush)
            {
                // Prepare sample for sending
 //               spPacket = PrepareSample(spSample.Get(), false);
				TryStoreRawSample(spSample.Get());
                fProcessingSample = true;
            }
        }
        else
        {
            ComPtr<IMarker> spMarker;
            // Check if it is a marker
            if (SUCCEEDED(spunkSample.As(&spMarker)))
            {
                MFSTREAMSINK_MARKER_TYPE markerType;
                PROPVARIANT var;
                PropVariantInit(&var);
                ThrowIfError(spMarker->GetMarkerType(&markerType));
                // Get the context data.
                ThrowIfError(spMarker->GetContext(&var));

                HRESULT hr = QueueEvent(MEStreamSinkMarker, GUID_NULL, S_OK, &var);

                PropVariantClear(&var);

                ThrowIfError(hr);

                if (markerType == MFSTREAMSINK_MARKER_ENDOFSEGMENT)
                {
                    fSendEOS = true;
                }
            }
            else
            {
                ComPtr<IMFMediaType> spType;
                ThrowIfError(spunkSample.As(&spType));
                if (!fFlush)
                {
//                    spPacket = PrepareFormatChange(spType.Get());
                }
            }
        }
/*
        if (spPacket)
        {
            ComPtr<CStreamSink> spThis = this;
            // Send the sample
            concurrency::create_task(_networkSender->SendAsync(spPacket.Get())).then([this, spThis, fProcessingSample](concurrency::task<void>& sendTask)
            {
                AutoLock lock(_critSec);
                try
                {
                    sendTask.get();
                    ThrowIfError(CheckShutdown());
                    if (_state == State_Started && fProcessingSample)
                    {
                        // If we are still in started state request another sample
                        ThrowIfError(QueueEvent(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr));
                    }
                }
                catch(Exception ^exc)
                {
                    HandleError(exc->HResult);
                }
            });
            // We stop if we processed a sample otherwise keep looking
            fSendSamples = !fProcessingSample;
        }
*/
        if (fSendSamples)
        {
            if (FAILED(_SampleQueue.RemoveFront(spunkSample.ReleaseAndGetAddressOf())))
            {
                fNeedMoreSamples = true;
                fSendSamples = false;
            }
        }

    }     

    if (fSendEOS)
    {
        ComPtr<CMediaSink> spParent = _pParent;
        concurrency::create_task([spParent](){
            spParent->ReportEndOfStream();
        });
    }

    return fNeedMoreSamples;
}

// Processing format change
void CStreamSink::ProcessFormatChange(IMFMediaType *pMediaType)
{
    assert(pMediaType != nullptr);

    // Add the media type to the sample queue.
    ThrowIfError(_SampleQueue.InsertBack(pMediaType));

    // Unless we are paused, start an async operation to dispatch the next sample.
    // Queue the operation.
    ThrowIfError(QueueAsyncOperation(OpSetMediaType));
}
/*
std::string serializeToJSON(
	ABI::Windows::Foundation::Numerics::Matrix4x4 viewMatrix,
	ABI::Windows::Foundation::Numerics::Matrix4x4 projectionMatrix,
	Windows::Foundation::Numerics::float4x4 world2cam,
	int frameID)
{
	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
	writer.StartObject();
	writer.Key("frameid");
	writer.Uint(frameID);
	writer.Key("viewMatrix");
	writer.StartArray();
	writer.Double(viewMatrix.M11); writer.Double(viewMatrix.M12); writer.Double(viewMatrix.M13); writer.Double(viewMatrix.M14);
	writer.Double(viewMatrix.M21); writer.Double(viewMatrix.M22); writer.Double(viewMatrix.M23); writer.Double(viewMatrix.M24);
	writer.Double(viewMatrix.M31); writer.Double(viewMatrix.M32); writer.Double(viewMatrix.M33); writer.Double(viewMatrix.M34);
	writer.Double(viewMatrix.M41); writer.Double(viewMatrix.M42); writer.Double(viewMatrix.M43); writer.Double(viewMatrix.M44);
	writer.EndArray();
	writer.Key("projectionMatrix");
	writer.StartArray();
	writer.Double(projectionMatrix.M11); writer.Double(projectionMatrix.M12); writer.Double(projectionMatrix.M13); writer.Double(projectionMatrix.M14);
	writer.Double(projectionMatrix.M21); writer.Double(projectionMatrix.M22); writer.Double(projectionMatrix.M23); writer.Double(projectionMatrix.M24);
	writer.Double(projectionMatrix.M31); writer.Double(projectionMatrix.M32); writer.Double(projectionMatrix.M33); writer.Double(projectionMatrix.M34);
	writer.Double(projectionMatrix.M41); writer.Double(projectionMatrix.M42); writer.Double(projectionMatrix.M43); writer.Double(projectionMatrix.M44);
	writer.EndArray();
	writer.Key("world2cam");
	writer.StartArray();
	writer.Double(world2cam.m11); writer.Double(world2cam.m12); writer.Double(world2cam.m13); writer.Double(world2cam.m14);
	writer.Double(world2cam.m21); writer.Double(world2cam.m22); writer.Double(world2cam.m23); writer.Double(world2cam.m24);
	writer.Double(world2cam.m31); writer.Double(world2cam.m32); writer.Double(world2cam.m33); writer.Double(world2cam.m34);
	writer.Double(world2cam.m41); writer.Double(world2cam.m42); writer.Double(world2cam.m43); writer.Double(world2cam.m44);
	writer.EndArray();
	writer.EndObject();
	return sb.GetString();
}
*/
EXTERN_GUID(MFSampleExtension_Spatial_CameraViewTransform, 0x4e251fa4, 0x830f, 0x4770, 0x85, 0x9a, 0x4b, 0x8d, 0x99, 0xaa, 0x80, 0x9b);
EXTERN_GUID(MFSampleExtension_Spatial_CameraCoordinateSystem, 0x9d13c82f, 0x2199, 0x4e67, 0x91, 0xcd, 0xd1, 0xa4, 0x18, 0x1f, 0x25, 0x34);
EXTERN_GUID(MFSampleExtension_Spatial_CameraProjectionTransform, 0x47f9fcb5, 0x2a02, 0x4f26, 0xa4, 0x77, 0x79, 0x2f, 0xdf, 0x95, 0x88, 0x6a);

void CStreamSink::TryStoreRawSample(IMFSample *pSample)
{
	assert(pSample);
	DWORD cbTotalSampleLength = 0;
	pSample->GetTotalLength(&cbTotalSampleLength);

	DWORD cBuffers = 0;
	HRESULT hr = pSample->GetBufferCount(&cBuffers);

	if (SUCCEEDED(hr))
	{
		for (DWORD nIndex = 0; nIndex < cBuffers; ++nIndex)
		{
			ComPtr<IMFMediaBuffer> spMediaBuffer;

			// Get buffer from the sample
			hr = pSample->GetBufferByIndex(nIndex, &spMediaBuffer);
			if (FAILED(hr))
			{
				break;
			}
			BYTE* dataPtr = 0;
			DWORD maxLen = 0;
			DWORD curLen = 0;
			hr = spMediaBuffer->Lock(
				&dataPtr,
				&maxLen,
				&curLen
			);
			if (FAILED(hr))
			{
				break;
			}
			if (IsVideo())
			{
				VZSource_pushVideoPacket(dataPtr, curLen);
			}
			else
			{
				VZSource_pushAudioPacket(dataPtr, curLen);
			}
			

			/*
			ComPtr<IUnknown> spUnknown;
			Windows::Perception::Spatial::SpatialCoordinateSystem^  frameCoordinateSystem;
			ABI::Windows::Foundation::Numerics::Matrix4x4 viewMatrix;
			ABI::Windows::Foundation::Numerics::Matrix4x4 projectionMatrix;
			Windows::Foundation::Numerics::float4x4 world2cam;
			bool gotPose = false;
//				if (m_locator != nullptr)
//				{
				UINT32 cbBlobSize = 0;
				hr = pSample->GetUnknown(MFSampleExtension_Spatial_CameraCoordinateSystem, IID_PPV_ARGS(&spUnknown));
				if (SUCCEEDED(hr))
				{
					// CAREFUL HERE!!!
					frameCoordinateSystem = safe_cast<Windows::Perception::Spatial::SpatialCoordinateSystem^>(reinterpret_cast<Platform::Object^>(spUnknown.Get()));
					Platform::IBox<Windows::Foundation::Numerics::float4x4>^ frameToOriginReference = 
						frameCoordinateSystem->TryGetTransformTo(m_spCurrentCoordinateSystem);
					if (frameToOriginReference != nullptr) {
						world2cam = frameToOriginReference->Value;
						hr = pSample->GetBlob(MFSampleExtension_Spatial_CameraViewTransform,
							(UINT8*)(&viewMatrix),
							sizeof(viewMatrix),
							&cbBlobSize);
						if (SUCCEEDED(hr) && cbBlobSize == sizeof(ABI::Windows::Foundation::Numerics::Matrix4x4))
						{
							hr = pSample->GetBlob(MFSampleExtension_Spatial_CameraProjectionTransform,
								(UINT8*)(&projectionMatrix),
								sizeof(projectionMatrix),
								&cbBlobSize);
							if (SUCCEEDED(hr) && cbBlobSize == sizeof(ABI::Windows::Foundation::Numerics::Matrix4x4))
							{
								gotPose = true;
							}
						}
					}
				}
//				}

			//
			// TODO: Here we should get the pose from the Core above!!!
			std::wstring_convert<std::codecvt_utf8_utf16<char>, char> convert; 
			std::string bla = "{}";
			if (gotPose)
				bla = serializeToJSON(viewMatrix, projectionMatrix, world2cam, _frameCnt);
			// copy only Y plane!
			int numBytes2Copy = (MFVideoFormat_YV12 == _guiCurrentSubtype) ? (curLen << 1)/3 : curLen;
			bool ok = VizarioCore_pushImageData(dataPtr, numBytes2Copy, convertUTFFormat8toV(bla).c_str(), _frameCnt++);
			*/
			spMediaBuffer->Unlock();
		}
	}
}


/*
// Set the information if we are connected to a client or not
HRESULT CStreamSink::SetConnected(bool fConnected, LONGLONG llCurrentTime)
{
    AutoLock lock(_critSec);
    HRESULT hr = S_OK;
    try
    {
        if (_spCurrentType == nullptr)
        {
            Throw(MF_E_NOT_INITIALIZED);
        }

        _StartTime = llCurrentTime;
        _Connected = fConnected;
        if (fConnected)
        {
            if (_spFirstVideoSample)
            {
                ComPtr<IBufferPacket> spPacket = PrepareSample(_spFirstVideoSample.Get(), true);
                _spFirstVideoSample.Reset();

                if (spPacket != nullptr)
                {
                    ComPtr<CStreamSink> spThis = this;
                    // Send the sample
                    concurrency::create_task(_networkSender->SendAsync(spPacket.Get())).then([this, spThis](concurrency::task<void>& sendTask)
                    {
                        AutoLock lock(_critSec);
                        try
                        {
                            sendTask.get();
                            ThrowIfError(CheckShutdown());
                        }
                        catch(Exception ^exc)
                        {
                            HandleError(exc->HResult);
                        }
                    });
                }
				
            }


            TRACE(TRACE_LEVEL_LOW, L"SetConnected start=%I64d\n", _StartTime);

            _fFirstSampleAfterConnect = true;
        }
    }
    catch(Exception ^exc)
    {
        hr = exc->HResult;
    }

    TRACEHR_RET(hr);
}

// Prepare sample for sending over the network by serializing it to a network packet
ComPtr<Network::IBufferPacket> CStreamSink::PrepareSample(IMFSample *pSample, bool fForce)
{
    assert(pSample);

    ComPtr<IMediaBufferWrapper> spBuffer;
    ComPtr<IBufferPacket> spPacket;
    const size_t c_cbHeaderSize = sizeof(StspOperationHeader) + sizeof(StspSampleHeader);
    DWORD cbTotalSampleLength = 0;
    LONGLONG llSampleTime;

    ThrowIfError(pSample->GetSampleTime(&llSampleTime));
    llSampleTime -= _StartTime;    

    if (llSampleTime < 0 && !fForce)
    {
        // Return nullptr;
        return spPacket;
    }

    if (llSampleTime < 0)
    {
        llSampleTime = 0;
    }

    // Create packet and initialize it with current sample
    ThrowIfError(CreateBufferPacketFromMFSample(pSample, &spPacket));
    ThrowIfError(pSample->GetTotalLength(&cbTotalSampleLength));
    ThrowIfError(CreateMediaBufferWrapper(c_cbHeaderSize, &spBuffer));

    // Prepare the headers
    BYTE *pBuf = spBuffer->GetBuffer();
    // Operation header
    StspOperationHeader *pOpHeader = reinterpret_cast<StspOperationHeader *>(pBuf);
    pOpHeader->eOperation = StspOperation_ServerSample;
    pOpHeader->cbDataSize = sizeof(StspSampleHeader) + cbTotalSampleLength;

    // Sample header
    StspSampleHeader *pSampleHeader = reinterpret_cast<StspSampleHeader *>(pBuf + sizeof(StspOperationHeader));
    ZeroMemory(pSampleHeader, sizeof(*pSampleHeader));
    GetIdentifier(&pSampleHeader->dwStreamId);
    if (_fGetStartTimeFromSample)
    {
        pSample->GetSampleTime(&_StartTime);
        _fGetStartTimeFromSample = false;
        llSampleTime = 0;
    }
    pSampleHeader->ullTimestamp = llSampleTime;
    if (FAILED(pSample->GetSampleDuration(&pSampleHeader->ullDuration)))
    {
        pSampleHeader->ullDuration = 0;
    }

    // Set up video samples
    if (IsVideo())
    {
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, BottomFieldFirst);
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, CleanPoint);
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, DerivedFromTopField);
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, Discontinuity);
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, Interlaced);
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, RepeatFirstField);
        SET_SAMPLE_FLAG(pSampleHeader->dwFlags, pSampleHeader->dwFlagMasks, pSample, SingleField);
    }

    ThrowIfError(spBuffer->SetCurrentLength(c_cbHeaderSize));

    // Put headers before sample
    ThrowIfError(spPacket->InsertBuffer(0, spBuffer.Get()));

    _fFirstSampleAfterConnect = false;

    return spPacket;
}

// Fill stream description and prepare attributes blob.
ComPtr<Network::IMediaBufferWrapper> CStreamSink::FillStreamDescription(IMFMediaType *pMediaType, StspStreamDescription *pStreamDescription)
{
    assert(pStreamDescription != nullptr);

    ComPtr<IMFMediaType> spMediaTypeToSend;

    // Clear the stream descriptor
    ZeroMemory(pStreamDescription, sizeof(*pStreamDescription));

    // Get major type (Audio, Video and so on)
    ThrowIfError(GetMajorType(&pStreamDescription->guiMajorType));

    // Get subtype (format of the stream)
    ThrowIfError(pMediaType->GetGUID(MF_MT_SUBTYPE, &pStreamDescription->guiSubType));

    ThrowIfError(MFCreateMediaType(&spMediaTypeToSend));

    FilterOutputMediaType(pMediaType, spMediaTypeToSend.Get());

    pStreamDescription->dwStreamId = _dwIdentifier;

    ComPtr<IMediaBufferWrapper> spAttributes;
    // Get size of attributes blob
    ThrowIfError(MFGetAttributesAsBlobSize(spMediaTypeToSend.Get(), &pStreamDescription->cbAttributesSize));

    // Prepare a buffer for attribute blob
    ThrowIfError(CreateMediaBufferWrapper(pStreamDescription->cbAttributesSize, &spAttributes));

    // Set length of the buffer
    ThrowIfError(spAttributes->SetCurrentLength(pStreamDescription->cbAttributesSize));

    // Copy attributes to the buffer
    ThrowIfError(MFGetAttributesAsBlob(spMediaTypeToSend.Get(), spAttributes->GetBuffer(), pStreamDescription->cbAttributesSize));

    return spAttributes;
}


// Prepare packet with format change information to be sent over the wire
ComPtr<Network::IBufferPacket> CStreamSink::PrepareFormatChange(IMFMediaType *pMediaType)
{
    const DWORD c_cbPacketSize = sizeof(StspOperationHeader) + sizeof(StspStreamDescription);

    ComPtr<IMediaBufferWrapper> spBuffer;
    ThrowIfError(CreateMediaBufferWrapper(c_cbPacketSize, &spBuffer));

    // Prepare operation header
    BYTE *pBuf = spBuffer->GetBuffer();
    StspOperationHeader *pOpHeader = reinterpret_cast<StspOperationHeader *>(pBuf);
    pOpHeader->cbDataSize = sizeof(StspStreamDescription);
    pOpHeader->eOperation = StspOperation_ServerFormatChange;

    // Prepare description
    StspStreamDescription *pStreamDescription = reinterpret_cast<StspStreamDescription *>(pBuf + sizeof(StspOperationHeader));
    ComPtr<IMediaBufferWrapper> spAttr = FillStreamDescription(pMediaType, pStreamDescription);

    // Add size of variable size attribute blob to size of the package.
    pOpHeader->cbDataSize += pStreamDescription->cbAttributesSize;

    // Set length of the packet
    ThrowIfError(spBuffer->SetCurrentLength(c_cbPacketSize));

    // Prepare packet to send
    ComPtr<IBufferPacket> spPacket;

    ThrowIfError(CreateBufferPacket(&spPacket));
    // Add fixed size header and description to the packet
    ThrowIfError(spPacket->AddBuffer(spBuffer.Get()));
    // Add attributes
    ThrowIfError(spPacket->AddBuffer(spAttr.Get()));

    return spPacket;
}
*/
CStreamSink::CAsyncOperation::CAsyncOperation(StreamOperation op)
    : _cRef(1)
    , m_op(op)
{
}

CStreamSink::CAsyncOperation::~CAsyncOperation()
{
    assert(_cRef == 0);
}

ULONG CStreamSink::CAsyncOperation::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

ULONG CStreamSink::CAsyncOperation::Release()
{
    ULONG cRef = InterlockedDecrement(&_cRef);
    if (cRef == 0)
    {
        delete this;
    }

    return cRef; 
}

HRESULT CStreamSink::CAsyncOperation::QueryInterface(REFIID iid, void **ppv)
{
    if (!ppv)
    {
        return E_POINTER;
    }
    if (iid == IID_IUnknown)
    {
        *ppv = static_cast<IUnknown*>(this);
    }
    else
    {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
/*
HRESULT CStreamSink::AppendParameterSets(IMFSample *pCopyFrom, IBufferPacket *pPacket)
{
    static const BYTE rgbStartCode[] = {0x00, 0x00, 0x00, 0x01};
    static const size_t cbStartCode = _countof(rgbStartCode);

    ComPtr<IMediaBufferWrapper> spStartCodeBuffer;
    ComPtr<IMFMediaBuffer> spMediaBuffer;
    ComPtr<IMediaBufferWrapper> spParameterSetsBuffer;
    DWORD dwMediaBufferLen = 0;

    HRESULT hr = CreateMediaBufferWrapper(cbStartCode, &spStartCodeBuffer);

    if (SUCCEEDED(hr))
    {
        CopyMemory(spStartCodeBuffer->GetBuffer(), rgbStartCode, cbStartCode);
        hr = pPacket->InsertBuffer(0, spStartCodeBuffer.Get());
    }

    if (SUCCEEDED(hr))
    {
        hr = pCopyFrom->ConvertToContiguousBuffer(&spMediaBuffer);
        if (SUCCEEDED(hr))
        {
            spMediaBuffer->GetCurrentLength(&dwMediaBufferLen);
            if (dwMediaBufferLen < sizeof(DWORD))
            {
                hr = E_UNEXPECTED;
            }
        }
        
    }

    if (SUCCEEDED(hr))
    {
        hr = CreateMediaBufferWrapper(spMediaBuffer.Get(), &spParameterSetsBuffer);
    }

    if (SUCCEEDED(hr))
    {
        DWORD dwParameterSetLen;
        CopyMemory(&dwParameterSetLen, spParameterSetsBuffer->GetBuffer(), sizeof(DWORD));

        if (dwMediaBufferLen < dwParameterSetLen)
        {
            hr = E_UNEXPECTED;
        }

        if (SUCCEEDED(hr))
        {
            hr = spParameterSetsBuffer->SetCurrentLength(dwParameterSetLen);
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = pPacket->InsertBuffer(0, spParameterSetsBuffer.Get());
    }

    return hr;
}
*/
void CStreamSink::HandleError(HRESULT hr)
{
    if (!_IsShutdown)
    {
        QueueEvent(MEError, GUID_NULL, hr, nullptr);
    }
}
