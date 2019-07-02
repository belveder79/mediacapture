//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#include "pch.h"
#include <InitGuid.h>
#include "VizarioMediaSink.h"
#include "VizarioStreamSink.h"
#include "VizarioMediaSinkProxy.h"

using namespace Vizario::MediaSink;

namespace
{
    class ShutdownFunc
    {
    public:
        HRESULT operator()(IMFStreamSink *pStream) const
        {
            static_cast<CStreamSink *>(pStream)->Shutdown();
            return S_OK;
        }
    };

    class StartFunc
    {
    public:
        StartFunc(LONGLONG llStartTime)
            : _llStartTime(llStartTime)
        {
        }

        HRESULT operator()(IMFStreamSink *pStream) const
        {
            return static_cast<CStreamSink *>(pStream)->Start(_llStartTime);
        }

        LONGLONG _llStartTime;
    };

    class StopFunc
    {
    public:
        HRESULT operator()(IMFStreamSink *pStream) const
        {
            return static_cast<CStreamSink *>(pStream)->Stop();
        }
    };

    template <class T, class TFunc>
    HRESULT ForEach(ComPtrList<T> &col, TFunc fn)
    {
        ComPtrList<T>::POSITION pos = col.FrontPosition();
        ComPtrList<T>::POSITION endPos = col.EndPosition();
        HRESULT hr = S_OK;

        for (;pos != endPos; pos = col.Next(pos))
        {
            ComPtr<T> spStream;

            hr = col.GetItemPos(pos, &spStream);
            if (FAILED(hr))
            {
                break;
            }

            hr = fn(spStream.Get());
        }

        return hr;
    }

    static void AddAttribute(_In_ GUID guidKey, _In_ IPropertyValue ^value, _In_ IMFAttributes *pAttr)
    {
        PropertyType type = value->Type;
        switch (type)
        {
            case PropertyType::UInt8Array:
            {
                Array<BYTE>^ arr;
                value->GetUInt8Array(&arr);
                
                ThrowIfError(pAttr->SetBlob(guidKey, arr->Data, arr->Length));
            }
            break;

            case PropertyType::Double:
            {
                ThrowIfError(pAttr->SetDouble(guidKey, value->GetDouble()));
            }
            break;

            case PropertyType::Guid:
            {
                ThrowIfError(pAttr->SetGUID(guidKey, value->GetGuid()));
            }
            break;

            case PropertyType::String:
            {
                ThrowIfError(pAttr->SetString(guidKey, value->GetString()->Data()));
            }
            break;

            case PropertyType::UInt32:
            {
                ThrowIfError(pAttr->SetUINT32(guidKey, value->GetUInt32()));
            }
            break;

            case PropertyType::UInt64:
            {
                ThrowIfError(pAttr->SetUINT64(guidKey, value->GetUInt64()));
            }
            break;

            // ignore unknown values
        }
    }

    void ConvertPropertiesToMediaType(_In_ IMediaEncodingProperties ^mep, _Outptr_ IMFMediaType **ppMT)
    {
        if (mep == nullptr || ppMT == nullptr)
        {
            throw ref new InvalidArgumentException();
        }
        ComPtr<IMFMediaType> spMT;
        *ppMT = nullptr;
        ThrowIfError(MFCreateMediaType(&spMT));

        auto it = mep->Properties->First();            

        while (it->HasCurrent)
        {
            auto currentValue = it->Current;
            AddAttribute(currentValue->Key, safe_cast<IPropertyValue^>(currentValue->Value), spMT.Get());
            it->MoveNext();
        }

        GUID guiMajorType = safe_cast<IPropertyValue^>(mep->Properties->Lookup(MF_MT_MAJOR_TYPE))->GetGuid();

        if (guiMajorType != MFMediaType_Video && guiMajorType != MFMediaType_Audio)
        {
            Throw(E_UNEXPECTED);
        }

        *ppMT = spMT.Detach();
    }

    DWORD GetStreamId(Windows::Media::Capture::MediaStreamType mediaStreamType)
    {
        switch(mediaStreamType)
        {
        case Windows::Media::Capture::MediaStreamType::VideoRecord:
            return 0;
        case Windows::Media::Capture::MediaStreamType::Audio:
            return 1;
        }

        throw ref new InvalidArgumentException();
    }
}

CMediaSink::CMediaSink() 
: _cRef(1)
, _IsShutdown(false)
, _llStartTime(0)
, _cStreamsEnded(0)
{
}

CMediaSink::~CMediaSink()
{
    assert(_IsShutdown);
}

HRESULT CMediaSink::RuntimeClassInitialize(
    ISinkCallback ^callback,
    Windows::Media::MediaProperties::IMediaEncodingProperties ^audioEncodingProperties,
    Windows::Media::MediaProperties::IMediaEncodingProperties ^videoEncodingProperties
    )
{
    try
    {
        _callback = callback;

        // Set up media streams
        SetMediaStreamProperties(MediaStreamType::Audio, audioEncodingProperties);
        SetMediaStreamProperties(MediaStreamType::VideoRecord, videoEncodingProperties);
    }
    catch (Exception ^exc)
    {
        _callback = nullptr;
        return exc->HResult;
    }

    return S_OK;
}

Windows::Perception::Spatial::SpatialCoordinateSystem^ CMediaSink::GetSpatialCoordinateSystem()
{
	return _callback->GetSpatialCoordinateSystem();
}

void CMediaSink::FireRawImageCaptured(RawImageCapturedArgs^ args)
{
	try {
		_callback->FireRawImageCaptured(args);
	}
	catch (Exception^ e)
	{
		e->ToString();
	}
}

void CMediaSink::SetMediaStreamProperties(
    Windows::Media::Capture::MediaStreamType MediaStreamType,
    _In_opt_ Windows::Media::MediaProperties::IMediaEncodingProperties ^mediaEncodingProperties)
{
    if (MediaStreamType != MediaStreamType::VideoRecord && MediaStreamType != MediaStreamType::Audio)
    {
        throw ref new InvalidArgumentException();
    }

    RemoveStreamSink(GetStreamId(MediaStreamType));
    const unsigned int streamId = GetStreamId(MediaStreamType);

    if (mediaEncodingProperties != nullptr)
    {
        ComPtr<IMFStreamSink> spStreamSink;
        ComPtr<IMFMediaType> spMediaType;
        ConvertPropertiesToMediaType(mediaEncodingProperties, &spMediaType);
        ThrowIfError(AddStreamSink(streamId, spMediaType.Get(), spStreamSink.GetAddressOf()));
    }
}

///  IMFMediaSink
IFACEMETHODIMP CMediaSink::GetCharacteristics(DWORD *pdwCharacteristics)
{
    if (pdwCharacteristics == NULL)
    {
        return E_INVALIDARG;
    }
    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        // Rateless sink.
        *pdwCharacteristics = MEDIASINK_RATELESS;
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::AddStreamSink(
    DWORD dwStreamSinkIdentifier,
    IMFMediaType *pMediaType,
    IMFStreamSink **ppStreamSink)
{
    CStreamSink *pStream = nullptr;
    ComPtr<IMFStreamSink> spMFStream;
    AutoLock lock(_critSec);
    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = GetStreamSinkById(dwStreamSinkIdentifier, &spMFStream);
    }

    if (SUCCEEDED(hr))
    {
        hr = MF_E_STREAMSINK_EXISTS;
    }
    else
    {
        hr = S_OK;
    }

    if (SUCCEEDED(hr))
    {
        pStream = new CStreamSink(dwStreamSinkIdentifier);
        if (pStream == nullptr)
        {
            hr = E_OUTOFMEMORY;
        }
        spMFStream.Attach(pStream);
    }

    // Initialize the stream.
    if (SUCCEEDED(hr))
    {
		hr = pStream->Initialize(this);// , _networkSender);
    }

    if (SUCCEEDED(hr) && pMediaType != nullptr)
    {
        hr = pStream->SetCurrentMediaType(pMediaType);
    }

    if (SUCCEEDED(hr))
    {
        StreamContainer::POSITION pos = _streams.FrontPosition();
        StreamContainer::POSITION posEnd = _streams.EndPosition();

        // Insert in proper position
        for (; pos != posEnd; pos = _streams.Next(pos))
        {
            DWORD dwCurrId;
            ComPtr<IMFStreamSink> spCurr;
            hr = _streams.GetItemPos(pos, &spCurr);
            if (FAILED(hr))
            {
                break;
            }
            hr = spCurr->GetIdentifier(&dwCurrId);
            if (FAILED(hr))
            {
                break;
            }

            if (dwCurrId > dwStreamSinkIdentifier)
            {
                break;
            }
        }

        if (SUCCEEDED(hr))
        {
            hr = _streams.InsertPos(pos, pStream);
        }
    }

    if (SUCCEEDED(hr))
    {
        *ppStreamSink = spMFStream.Detach();
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::RemoveStreamSink(DWORD dwStreamSinkIdentifier)
{
    AutoLock lock(_critSec);
    HRESULT hr = CheckShutdown();
    StreamContainer::POSITION pos = _streams.FrontPosition();
    StreamContainer::POSITION endPos = _streams.EndPosition();
    ComPtr<IMFStreamSink> spStream;

    if (SUCCEEDED(hr))
    {
        for (;pos != endPos; pos = _streams.Next(pos))
        {
            hr = _streams.GetItemPos(pos, &spStream);
            DWORD dwId;

            if (FAILED(hr))
            {
                break;
            }

            hr = spStream->GetIdentifier(&dwId);
            if (FAILED(hr) || dwId == dwStreamSinkIdentifier)
            {
                break;
            }
        }

        if (pos == endPos)
        {
            hr = MF_E_INVALIDSTREAMNUMBER;
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = _streams.Remove(pos, nullptr);
        static_cast<CStreamSink *>(spStream.Get())->Shutdown();
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::GetStreamSinkCount(_Out_ DWORD *pcStreamSinkCount)
{
    if (pcStreamSinkCount == NULL)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        *pcStreamSinkCount = _streams.GetCount();
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::GetStreamSinkByIndex(
DWORD dwIndex,
_Outptr_ IMFStreamSink **ppStreamSink)
{
    if (ppStreamSink == NULL)
    {
        return E_INVALIDARG;
    }

    ComPtr<IMFStreamSink> spStream;
    AutoLock lock(_critSec);
    DWORD cStreams = _streams.GetCount();

    if (dwIndex >= cStreams)
    {
        return MF_E_INVALIDINDEX;
    }

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        StreamContainer::POSITION pos = _streams.FrontPosition();
        StreamContainer::POSITION endPos = _streams.EndPosition();
        DWORD dwCurrent = 0;

        for (;pos != endPos && dwCurrent < dwIndex; pos = _streams.Next(pos), ++dwCurrent)
        {
            // Just move to proper position
        }

        if (pos == endPos)
        {
            hr = MF_E_UNEXPECTED;
        }
        else
        {
            hr = _streams.GetItemPos(pos, &spStream);
        }
    }

    if (SUCCEEDED(hr))
    {
        *ppStreamSink = spStream.Detach();
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::GetStreamSinkById(
DWORD dwStreamSinkIdentifier,
IMFStreamSink **ppStreamSink)
{
    if (ppStreamSink == NULL)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);
    HRESULT hr = CheckShutdown();
    ComPtr<IMFStreamSink> spResult;

    if (SUCCEEDED(hr))
    {
        StreamContainer::POSITION pos = _streams.FrontPosition();
        StreamContainer::POSITION endPos = _streams.EndPosition();

        for (;pos != endPos; pos = _streams.Next(pos))
        {
            ComPtr<IMFStreamSink> spStream;
            hr = _streams.GetItemPos(pos, &spStream);
            DWORD dwId;

            if (FAILED(hr))
            {
                break;
            }

            hr = spStream->GetIdentifier(&dwId);
            if (FAILED(hr))
            {
                break;
            }
            else if (dwId == dwStreamSinkIdentifier)
            {
                spResult = spStream;
                break;
            }
        }

        if (pos == endPos)
        {
            hr = MF_E_INVALIDSTREAMNUMBER;
        }
    }

    if (SUCCEEDED(hr))
    {
        assert(spResult);
        *ppStreamSink = spResult.Detach();
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::SetPresentationClock(IMFPresentationClock *pPresentationClock)
{
    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    // If we already have a clock, remove ourselves from that clock's
    // state notifications.
    if (SUCCEEDED(hr))
    {
        if (_spClock)
        {
            hr = _spClock->RemoveClockStateSink(this);
        }
    }

    // Register ourselves to get state notifications from the new clock.
    if (SUCCEEDED(hr))
    {
        if (pPresentationClock)
        {
            hr = pPresentationClock->AddClockStateSink(this);
        }
    }

    if (SUCCEEDED(hr))
    {
        // Release the pointer to the old clock.
        // Store the pointer to the new clock.
        _spClock = pPresentationClock;
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::GetPresentationClock(IMFPresentationClock **ppPresentationClock)
{
    if (ppPresentationClock == NULL)
    {
        return E_INVALIDARG;
    }

    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        if (_spClock == NULL)
        {
            hr = MF_E_NO_CLOCK; // There is no presentation clock.
        }
        else
        {
            // Return the pointer to the caller.
            *ppPresentationClock = _spClock.Get();
            (*ppPresentationClock)->AddRef();
        }
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::Shutdown()
{
    ISinkCallback ^callback;
    {
        AutoLock lock(_critSec);

        HRESULT hr = CheckShutdown();

        if (SUCCEEDED(hr))
        {
            ForEach(_streams, ShutdownFunc());
            _streams.Clear();

            _spClock.Reset();

            _IsShutdown = true;
            callback = _callback;
        }
    }

    if (callback != nullptr)
    {
        callback->OnShutdown();
    }

    return S_OK;
}

// IMFClockStateSink
IFACEMETHODIMP CMediaSink::OnClockStart(
                                        MFTIME hnsSystemTime,
                                        LONGLONG llClockStartOffset)
{
    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        TRACE(TRACE_LEVEL_LOW, L"OnClockStart ts=%I64d\n", llClockStartOffset);
        // Start each stream.
        _llStartTime = llClockStartOffset;
        hr = ForEach(_streams, StartFunc(llClockStartOffset));
    }

    TRACEHR_RET(hr);
}

IFACEMETHODIMP CMediaSink::OnClockStop(
                                       MFTIME hnsSystemTime)
{
    AutoLock lock(_critSec);

    HRESULT hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        // Stop each stream
        hr = ForEach(_streams, StopFunc());
    }

    TRACEHR_RET(hr);
}


IFACEMETHODIMP CMediaSink::OnClockPause(
                                        MFTIME hnsSystemTime)
{
    return MF_E_INVALID_STATE_TRANSITION;
}

IFACEMETHODIMP CMediaSink::OnClockRestart(
MFTIME hnsSystemTime)
{
    return MF_E_INVALID_STATE_TRANSITION;
}

IFACEMETHODIMP CMediaSink::OnClockSetRate(
/* [in] */ MFTIME hnsSystemTime,
/* [in] */ float flRate)
{
    return S_OK;
}

void CMediaSink::ReportEndOfStream()
{
    AutoLock lock(_critSec);
    ++_cStreamsEnded;
}
