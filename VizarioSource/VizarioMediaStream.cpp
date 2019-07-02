//// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
//// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
//// PARTICULAR PURPOSE.
////
//// Copyright (c) Microsoft Corporation. All rights reserved

#include "pch.h"
#include "VizarioMediaStream.h"
#include "VideoBufferLock.h"
#include <initguid.h>
#include <math.h>
#include <wrl\module.h>

// TESTING ONLY!!!
#include <deque>
class DataChunk
{
public:
	DataChunk() : m_data(0), m_length(0) {}
	DataChunk(const uint8_t *md, const int len)
	{
		m_data = new uint8_t[len];
		memcpy(m_data, md, len);
		m_length = len;
	}
	~DataChunk()
	{
		if (m_data)
			delete[] m_data;
	}

	uint8_t* getData() const
	{
		return m_data;
	}
	int getLength() const
	{
		return m_length;
	}
private:
	uint8_t* m_data;
	int m_length;
};
std::deque<DataChunk*> m_videochunks;
std::mutex m_videochunkmutex;
std::deque<DataChunk*> m_audiochunks;
std::mutex m_audiochunkmutex;

extern "C" {
	__declspec(dllexport) bool __cdecl VZSource_pushVideoPacket(const uint8_t* frameData, const int frameDataSize);
	__declspec(dllexport) bool __cdecl VZSource_pushAudioPacket(const uint8_t* frameData, const int frameDataSize);
}

bool __cdecl VZSource_pushVideoPacket(const uint8_t* frameData, const int frameDataSize)
{
	m_videochunkmutex.lock();
	m_videochunks.push_back(new DataChunk(frameData, frameDataSize));
	m_videochunkmutex.unlock();
	return true;
}

bool __cdecl VZSource_pushAudioPacket(const uint8_t* frameData, const int frameDataSize)
{
	m_audiochunkmutex.lock();
	m_audiochunks.push_back(new DataChunk(frameData, frameDataSize));
	m_audiochunkmutex.unlock();
	return true;
}

// TESTING ONLY


namespace
{
	// TODO! GET THIS FROM THE SINK!
    const DWORD c_dwOutputImageHeight = 480;
    const DWORD c_dwOutputImageWidth = 640;
    const DWORD c_cbOutputSamplenNumPixels = c_dwOutputImageWidth * c_dwOutputImageHeight;
    const DWORD c_cbOutputSampleSize = c_cbOutputSamplenNumPixels * 4;
    const DWORD c_dwOutputFrameRateNumerator = 30000;
    const DWORD c_dwOutputFrameRateDenominator = 1000;
    const LONGLONG c_llOutputFrameDuration = 333333ll; // in 100s of nanosecs
}

class CVizarioMediaStream::CSourceLock
{
public:
    _Acquires_lock_(_spSource)
        CSourceLock(CVizarioMediaSource *pSource)
        : _spSource(pSource)
    {
        if (_spSource)
        {
            _spSource->Lock();
        }
    }

    _Releases_lock_(_spSource)
        ~CSourceLock()
    {
        if (_spSource)
        {
            _spSource->Unlock();
        }
    }

private:
    ComPtr<CVizarioMediaSource> _spSource;
};
/*
BYTE Clip(int i)
{
    return (i > 255 ? 255 : (i < 0 ? 0 : i));
}

DWORD YUVToRGB( BYTE Y, BYTE U, BYTE V)
{
    int C = INT(Y) - 16;
    int D = INT(U) - 128;
    int E = INT(V) - 128;

    INT R = ( 298 * C           + 409 * E + 128) >> 8;
    INT G = ( 298 * C - 100 * D - 208 * E + 128) >> 8;
    INT B = ( 298 * C + 516 * D           + 128) >> 8;

    DWORD ret = 0xff000000;
    BYTE *bCols = reinterpret_cast<BYTE*>(&ret);

    bCols[2] = Clip(R);
    bCols[1] = Clip(G);
    bCols[0] = Clip(B);

    return ret;
}

ref class CFrameGenerator abstract
{
internal:
    CFrameGenerator() { }

    void PrepareFrame(BYTE *pBuf, LONGLONG llTimestamp, LONG lPitch)
    {
        ZeroMemory(pBuf, lPitch * c_dwOutputImageHeight);

        sin((llTimestamp/100000.0)*3.1415);
        DrawFrame(pBuf, YUVToRGB(128, 128 + BYTE(127 * sin(llTimestamp/10000000.0)), 128 + BYTE(127 * cos(llTimestamp/3300000.0))), lPitch);
    }

protected private:
    virtual void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) = 0;

    void SetColor(_In_reads_bytes_(cElements) DWORD *pBuf, DWORD dwColor, DWORD cElements)
    {
        for(DWORD nIndex = 0; nIndex < cElements; ++nIndex)
        {
            pBuf[nIndex] = dwColor;
        }
    }
};

ref class CSquareDrawer sealed: public CFrameGenerator
{
protected private:
    void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) override
    {
        const DWORD dwDimension = min(c_dwOutputImageWidth, c_dwOutputImageHeight);
        const int nFirstLine = (c_dwOutputImageHeight-dwDimension)/2;    
        const int nStartPos = (c_dwOutputImageWidth-dwDimension)/2;

        for (int nLine = 0; nLine < dwDimension; ++nLine, pBuf += lPitch)
        {
            DWORD *pLine = reinterpret_cast<DWORD *>(pBuf) + nStartPos;
            SetColor(pLine, dwColor, dwDimension);
        }
    }
};

ref class CCircleDrawer sealed: public CFrameGenerator
{
protected private:
    void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) override
    {
        const int dwDimension = min(c_dwOutputImageWidth, c_dwOutputImageHeight);
        const int dwRadius = dwDimension/2;
        const int nFirstLine = (c_dwOutputImageHeight-dwDimension)/2;    
        const int nStartPos = (c_dwOutputImageWidth-dwDimension)/2;

        for (int nLine = -dwRadius; nLine < dwRadius; ++nLine, pBuf += lPitch)
        {
            const int nXPos = (int)sqrt(dwRadius*(double)dwRadius - nLine*(double)nLine);
            const int nStartPos = (c_dwOutputImageWidth / 2) - nXPos;
            const int cPixels = nXPos * 2;
            DWORD *pLine = reinterpret_cast<DWORD *>(pBuf) + nStartPos;

            SetColor(pLine, dwColor, cPixels);
        }
    }
};

ref class CTriangleDrawer sealed: public CFrameGenerator
{
protected private:
    void DrawFrame(BYTE *pBuf, DWORD dwColor, LONG lPitch) override
    {
        const DWORD dwDimension = min(c_dwOutputImageWidth, c_dwOutputImageHeight);

        const int nFirstLine = (c_dwOutputImageHeight-dwDimension)/2;    
        const int nStartPos = (c_dwOutputImageWidth-dwDimension)/2;

        int nLeft = c_dwOutputImageWidth / 2;
        int nRight = nLeft + 1;
        const int cLinesPerPixel = 2;

        for (int nLine = 0, nLinesToGrow = 1; nLine < dwDimension; ++nLine, pBuf += lPitch, --nLinesToGrow)
        {
            if (nLinesToGrow == 0)
            {
                nLinesToGrow = cLinesPerPixel;
                --nLeft;
                ++nRight;
            }

            DWORD *pLine = reinterpret_cast<DWORD *>(pBuf) + nLeft;
            SetColor(pLine, dwColor, nRight - nLeft);
        }
    }
};

CFrameGenerator ^CreateFrameGenerator(GeometricShape eShape)
{
    switch(eShape)
    {
    case GeometricShape_Square:
        {
            return ref new CSquareDrawer();
        }
    case GeometricShape_Circle:
        {
            return ref new CCircleDrawer();
        }
    case GeometricShape_Triangle:
        {
            return ref new CTriangleDrawer();
        }
    default:
        {
            throw ref new InvalidArgumentException();
        }
    }
}
*/
ComPtr<CVizarioMediaStream> CVizarioMediaStream::CreateInstance(CVizarioMediaSource *pSource) //, GeometricShape eShape)
{
    if (pSource == nullptr)
    {
        throw ref new InvalidArgumentException();
    }

    ComPtr<CVizarioMediaStream> spStream;
	spStream.Attach(new(std::nothrow) CVizarioMediaStream(pSource));// , eShape));
    if (spStream == nullptr)
    {
        throw ref new OutOfMemoryException();
    }

    spStream->Initialize();

    return spStream;
}

CVizarioMediaStream::CVizarioMediaStream(CVizarioMediaSource *pSource)//, GeometricShape eShape)
    : _cRef(1)
    , _spSource(pSource)
    , _eSourceState(SourceState_Invalid)
//    , _eShape(eShape)
    , _flRate(1.0f)
{
    auto module = ::Microsoft::WRL::GetModuleBase();
    if (module != nullptr)
    {
        module->IncrementObjectCount();
    }
}


CVizarioMediaStream::~CVizarioMediaStream(void)
{
    auto module = ::Microsoft::WRL::GetModuleBase();
    if (module != nullptr)
    {
        module->DecrementObjectCount();
    }
}

// IUnknown methods

IFACEMETHODIMP CVizarioMediaStream::QueryInterface(REFIID riid, void **ppv)
{
    if (ppv == nullptr)
    {
        return E_POINTER;
    }
    HRESULT hr = E_NOINTERFACE;
    (*ppv) = nullptr;
    if (riid == IID_IUnknown || 
        riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaStream)
    {
        (*ppv) = static_cast<IMFMediaStream *>(this);
        AddRef();
        hr = S_OK;
    }

    return hr;
}

IFACEMETHODIMP_(ULONG) CVizarioMediaStream::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

IFACEMETHODIMP_(ULONG) CVizarioMediaStream::Release()
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

IFACEMETHODIMP CVizarioMediaStream::BeginGetEvent(IMFAsyncCallback *pCallback, IUnknown *punkState)
{
    HRESULT hr = S_OK;

    CSourceLock lock(_spSource.Get());

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = _spEventQueue->BeginGetEvent(pCallback, punkState);
    }

    return hr;
}

IFACEMETHODIMP CVizarioMediaStream::EndGetEvent(IMFAsyncResult *pResult, IMFMediaEvent **ppEvent)
{
    HRESULT hr = S_OK;

    CSourceLock lock(_spSource.Get());

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = _spEventQueue->EndGetEvent(pResult, ppEvent);
    }

    return hr;
}

IFACEMETHODIMP CVizarioMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent)
{
    // NOTE:
    // GetEvent can block indefinitely, so we don't hold the lock.
    // This requires some juggling with the event queue pointer.

    HRESULT hr = S_OK;

    ComPtr<IMFMediaEventQueue> spQueue;

    {
        CSourceLock lock(_spSource.Get());

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

    return hr;
}

IFACEMETHODIMP CVizarioMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, PROPVARIANT const *pvValue)
{
    HRESULT hr = S_OK;

    CSourceLock lock(_spSource.Get());

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        hr = _spEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
    }

    return hr;
}

// IMFMediaStream methods

IFACEMETHODIMP CVizarioMediaStream::GetMediaSource(IMFMediaSource **ppMediaSource)
{
    if (ppMediaSource == nullptr)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;

    CSourceLock lock(_spSource.Get());

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        *ppMediaSource = _spSource.Get();
        (*ppMediaSource)->AddRef();
    }

    return hr;
}

IFACEMETHODIMP CVizarioMediaStream::GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor)
{
    if (ppStreamDescriptor == nullptr)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = S_OK;

    CSourceLock lock(_spSource.Get());

    hr = CheckShutdown();

    if (SUCCEEDED(hr))
    {
        *ppStreamDescriptor = _spStreamDescriptor.Get();
        (*ppStreamDescriptor)->AddRef();
    }

    return hr;
}

IFACEMETHODIMP CVizarioMediaStream::RequestSample(IUnknown *pToken)
{
    HRESULT hr = S_OK;

    CSourceLock lock(_spSource.Get());

    try
    {
        ThrowIfError(CheckShutdown());

        if (_eSourceState != SourceState_Started)
        {
            // We cannot be asked for a sample unless we are in started state
            ThrowException(MF_E_INVALIDREQUEST);
        }

        // Trigger sample delivery
        DeliverSample(pToken);
    }
    catch (Exception ^exc)
    {
        hr = HandleError(exc->HResult);
    }

    return hr;
}

HRESULT CVizarioMediaStream::Start()
{
    HRESULT hr = S_OK;

    try
    {
        ThrowIfError(CheckShutdown());

        if (_eSourceState == SourceState_Stopped ||
            _eSourceState == SourceState_Started)
        {
            if (_eSourceState == SourceState_Stopped)
            {
                _llCurrentTimestamp = 0;
            }
            _eSourceState = SourceState_Started;

            // Inform the client that we've started
            ThrowIfError(QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr));
        }
        else
        {
            ThrowException(MF_E_INVALID_STATE_TRANSITION);
        }
    }
    catch (Exception ^exc)
    {
        hr = HandleError(exc->HResult);
    }

    return hr;
}

HRESULT CVizarioMediaStream::Stop()
{
    HRESULT hr = S_OK;

    try
    {
        ThrowIfError(CheckShutdown());

        if (_eSourceState == SourceState_Started)
        {
            _eSourceState = SourceState_Stopped;
            // Inform the client that we've stopped.
            ThrowIfError(QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr));
        }
        else
        {
            hr = MF_E_INVALID_STATE_TRANSITION;    
        }
    }
    catch (Exception ^exc)
    {
        hr = HandleError(exc->HResult);
    }

    return hr;
}

HRESULT CVizarioMediaStream::SetRate(float flRate)
{
    HRESULT hr = S_OK;

    try
    {
        ThrowIfError(CheckShutdown());

        _flRate = flRate;
    }
    catch (Exception ^exc)
    {
        hr = HandleError(exc->HResult);
    }

    return hr;
}

void CVizarioMediaStream::Shutdown()
{
    try
    {
        ThrowIfError(CheckShutdown());

        if (_spEventQueue)
        {
            _spEventQueue->Shutdown();
            _spEventQueue.Reset();
        }

        if (_spAllocEx != nullptr)
        {
            _spAllocEx->UninitializeSampleAllocator();
        }

        _spStreamDescriptor.Reset();
        _spDeviceManager.Reset();
        _eSourceState = SourceState_Shutdown;

//        _frameGenerator = nullptr;
    }
    catch (Exception ^exc)
    {

    }
}

void CVizarioMediaStream::SetDXGIDeviceManager(IMFDXGIDeviceManager *pManager)
{
    _spDeviceManager = pManager;

    if (_spDeviceManager)
    {
        CreateVideoSampleAllocator();
    }
}

void CVizarioMediaStream::Initialize()
{
    // Create the media event queue.
    ThrowIfError(MFCreateEventQueue(&_spEventQueue));
    ComPtr<IMFStreamDescriptor> spSD;
    ComPtr<IMFMediaTypeHandler> spMediaTypeHandler;

    // Create a media type object.
    _spMediaType = CreateMediaType();

    // Now we can create MF stream descriptor.
    ThrowIfError(MFCreateStreamDescriptor(c_dwGeometricStreamId, 1, _spMediaType.GetAddressOf(), &spSD));
    ThrowIfError(spSD->GetMediaTypeHandler(&spMediaTypeHandler));
    // Set current media type
    ThrowIfError(spMediaTypeHandler->SetCurrentMediaType(_spMediaType.Get()));
    _spStreamDescriptor = spSD;
    // State of the stream is started.
    _eSourceState = SourceState_Stopped;

//    _frameGenerator = CreateFrameGenerator(_eShape);
}

// TODO: Get This information from the SINK!
ComPtr<IMFMediaType> CVizarioMediaStream::CreateMediaType()
{
    ComPtr<IMFMediaType> spOutputType;
    ThrowIfError(MFCreateMediaType(&spOutputType));

    ThrowIfError(spOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    ThrowIfError(spOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32));
    ThrowIfError(spOutputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
    ThrowIfError(spOutputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    ThrowIfError(spOutputType->SetUINT32(MF_MT_SAMPLE_SIZE, c_cbOutputSampleSize));
    ThrowIfError(MFSetAttributeSize(spOutputType.Get(), MF_MT_FRAME_SIZE, c_dwOutputImageWidth, c_dwOutputImageHeight));
    ThrowIfError(MFSetAttributeRatio(spOutputType.Get(), MF_MT_FRAME_RATE, c_dwOutputFrameRateNumerator, c_dwOutputFrameRateDenominator));
    ThrowIfError(spOutputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    ThrowIfError(MFSetAttributeRatio(spOutputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    return spOutputType;
}

void CVizarioMediaStream::DeliverSample(IUnknown *pToken)
{
	ComPtr<IMFSample> spSample = CreateImage();// _eShape);
    ThrowIfError(spSample->SetSampleTime(_llCurrentTimestamp));
    _llCurrentTimestamp += c_llOutputFrameDuration;
    ThrowIfError(spSample->SetSampleDuration(c_llOutputFrameDuration));
    // If token was not null set the sample attribute.
    ThrowIfError(spSample->SetUnknown(MFSampleExtension_Token, pToken));
    // Send a sample event.
    ThrowIfError(_spEventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, spSample.Get()));
}



// ComPtr<IMFSample> CVizarioMediaStream::CreateImage(GeometricShape eShape)
ComPtr<IMFSample> CVizarioMediaStream::CreateImage()
{
    ComPtr<IMFMediaBuffer> spOutputBuffer;
    ComPtr<IMFSample> spSample;
    const LONG pitch = c_dwOutputImageWidth * sizeof(DWORD);
/*
    if (_frameGenerator == nullptr)
    {
        ThrowException(E_UNEXPECTED);
    }
*/
    if (_spDeviceManager != nullptr)
    {
        ThrowIfError(_spAllocEx->AllocateSample(&spSample));
        ThrowIfError(spSample->GetBufferByIndex(0, &spOutputBuffer));
    }
    else
    {
        ThrowIfError(MFCreateMemoryBuffer(c_cbOutputSampleSize, &spOutputBuffer));
        ThrowIfError(MFCreateSample(&spSample));
        ThrowIfError(spSample->AddBuffer(spOutputBuffer.Get()));
    }
    
    VideoBufferLock lock(spOutputBuffer.Get(), MF2DBuffer_LockFlags_Write, c_dwOutputImageHeight, pitch);

	
	m_videochunkmutex.lock();
	if (m_videochunks.size() > 0)
	{
		DataChunk* x = m_videochunks.front();
		m_videochunks.pop_front();
		memcpy(lock.GetData(), x->getData(), x->getLength());
		delete x;
	}
	m_videochunkmutex.unlock();
    //_frameGenerator->PrepareFrame(lock.GetData(), _llCurrentTimestamp, lock.GetStride());

    ThrowIfError(spOutputBuffer->SetCurrentLength(c_cbOutputSampleSize));

    return spSample;
}

HRESULT CVizarioMediaStream::HandleError(HRESULT hErrorCode)
{
    if (hErrorCode != MF_E_SHUTDOWN)
    {
        // Send MEError to the client
        hErrorCode = QueueEvent(MEError, GUID_NULL, hErrorCode, nullptr);
    }

    return hErrorCode;
}

void CVizarioMediaStream::CreateVideoSampleAllocator()
{
    ComPtr<IMFAttributes> spAttributes;

    ThrowIfError(MFCreateAttributes(&spAttributes, 1));

    ThrowIfError(spAttributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE));
    ThrowIfError(MFCreateVideoSampleAllocatorEx(IID_IMFVideoSampleAllocatorEx, (void**)&_spAllocEx));
    ThrowIfError(_spAllocEx->SetDirectXManager(_spDeviceManager.Get()));
    ThrowIfError(_spAllocEx->InitializeSampleAllocatorEx(1, 4, spAttributes.Get(), _spMediaType.Get()));
}