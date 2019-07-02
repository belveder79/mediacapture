using System;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Windows.Devices.Enumeration;
using Windows.Media;
using Windows.Media.Capture;
using Windows.Media.Core;
using Windows.Media.MediaProperties;
using Windows.Media.Playback;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

// The Blank Page item template is documented at https://go.microsoft.com/fwlink/?LinkId=402352&clcid=0x409

namespace TestAppUWP
{
    /// <summary>
    /// An empty page that can be used on its own or navigated to within a Frame.
    /// </summary>
    public sealed partial class MainPage : Page
    {
        public MainPage()
        {
            this.InitializeComponent();
            m_isPlaying = false;
            // TODO: Move this to the Initialization Routine once a connection is established
            // aka move it to "StartCamera" in here... (now it is started and sets up everything at the beginning with fixed parameters
            try
            {
                Uri pathUri = new Uri("vizario://square");
                MediaExtensionManager manager = new MediaExtensionManager();
                manager.RegisterSchemeHandler("Vizario.MediaSource.VizarioSchemeHandler", "vizario:");
                mediaSimple.Source = MediaSource.CreateFromUri(pathUri);
                mediaSimple.MediaPlayer.Play();
            }
            catch (Exception ex)
            {
                if (ex is FormatException)
                {
                    // handle exception.
                    // For example: Log error or notify user problem with file
                }
            }
        }

        private MediaCapture m_mediaCapture;
        private MediaEncodingProfile m_encodingProfile;
        private Vizario.MediaSink.VizarioMediaSinkProxy m_mediaSink;
        private IMediaExtension m_mfExtension;
        private bool m_isInitialized = false;
        private bool m_isPlaying = false;
        private static readonly SemaphoreSlim m_mediaCaptureLifeLock = new SemaphoreSlim(1);

        private async void Button_Click(object sender, RoutedEventArgs e)
        {
            // Using Windows.Media.Capture.MediaCapture APIs 
            // to stream from webcam
            if (!m_isPlaying)
            {
                await InitializeCameraAsync();
                // TODO: Move player init/start code here!
            }
            else
                await CleanupCameraAsync();
        }



#if NETFX_CORE
        //==================================================================================================
        // THIS WAS TAKEN FROM BARCODELIB
        #region Event handlers

        private async void MediaCapture_Failed(MediaCapture sender, MediaCaptureFailedEventArgs errorEventArgs)
        {
            Debug.WriteLine("MediaCapture_Failed: (0x{0:X}) {1}", errorEventArgs.Code, errorEventArgs.Message);
            await CleanupCameraAsync();
        }

        #endregion Event handlers

        #region MediaCapture methods

        /// <summary>
        /// Initializes the MediaCapture, registers events, gets camera device information for mirroring and rotating, and starts preview
        /// </summary>
        /// <returns></returns>
        private async Task InitializeCameraAsync()
        {
            Debug.WriteLine("InitializeCameraAsync");

            await m_mediaCaptureLifeLock.WaitAsync();

            if (m_mediaCapture == null)
            {
                // Attempt to get the back camera if one is available, but use any camera device if not
                var cameraDevice = await FindCameraDeviceByPanelAsync(Windows.Devices.Enumeration.Panel.Front);

                if (cameraDevice == null)
                {
                    //CLE Debug.WriteLine("No camera device found!");
                    m_mediaCaptureLifeLock.Release();
                    return;
                }

                // Create MediaCapture and its settings
                m_mediaCapture = new MediaCapture();

                // Register for a notification when something goes wrong
                m_mediaCapture.Failed += MediaCapture_Failed;

                var settings = new MediaCaptureInitializationSettings
                {
                    VideoDeviceId = cameraDevice.Id,
                    SharingMode = MediaCaptureSharingMode.ExclusiveControl,
                    StreamingCaptureMode = StreamingCaptureMode.AudioAndVideo
                    // MemoryPreference = MediaCaptureMemoryPreference.Auto,
                };

                // Initialize MediaCapture
                try
                {
                    m_mediaSink = new Vizario.MediaSink.VizarioMediaSinkProxy();
                    m_mediaSink.RawImageCapturedEvent += RawImageCaptured;

                    //m_encodingProfile = MediaEncodingProfile.CreateAvi(VideoEncodingQuality.Vga);

                    m_encodingProfile = new MediaEncodingProfile();
                    // see https://docs.microsoft.com/en-us/uwp/api/windows.media.mediaproperties.videoencodingproperties.subtype#Windows_Media_MediaProperties_VideoEncodingProperties_Subtype
                    m_encodingProfile.Video = VideoEncodingProperties.CreateUncompressed("ARGB32", 640, 480);
                    m_encodingProfile.Video.FrameRate.Numerator = 30000;
                    m_encodingProfile.Video.FrameRate.Denominator = 1000;
                    m_encodingProfile.Audio = AudioEncodingProperties.CreatePcm(44100, 1, 32);

                    m_mfExtension = await m_mediaSink.InitializeAsync(m_encodingProfile.Audio, m_encodingProfile.Video);
                    await m_mediaCapture.InitializeAsync(settings);

                    m_mediaCapture.CaptureDeviceExclusiveControlStatusChanged += MediaCapture_CaptureDeviceExclusiveControlStatusChanged;
                    m_mediaCapture.CameraStreamStateChanged += MediaCapture_CameraStreamStateChanged;

                    m_isInitialized = true;
                }
                catch (UnauthorizedAccessException)
                {
                    Debug.WriteLine("The app was denied access to the camera");
                }
                catch (Exception e)
                {
                    Debug.WriteLine("Exception caught! " + e.ToString());
                }
                finally
                {
                    m_mediaCaptureLifeLock.Release();
                }

                // If initialization succeeded, start the preview
                if (m_isInitialized)
                {
                    await StartPreviewAsync();
                }
            }
            else
            {
                m_mediaCaptureLifeLock.Release();
            }
        }

        private void MediaCapture_CameraStreamStateChanged(MediaCapture sender, object args)
        {
            Debug.WriteLine("Stream State Changed!");
        }

        private void MediaCapture_CaptureDeviceExclusiveControlStatusChanged(MediaCapture sender, MediaCaptureDeviceExclusiveControlStatusChangedEventArgs args)
        {
            Debug.WriteLine("Device Status Changed!");
        }

        private void RawImageCaptured(object sender, object args)
        {
            Vizario.MediaSink.RawImageCapturedArgs info = (Vizario.MediaSink.RawImageCapturedArgs)args;
        }

        /// <summary>
        /// Starts the preview and adjusts it for for rotation and mirroring after making a request to keep the screen on and unlocks the UI
        /// </summary>
        /// <returns></returns>
        private async Task StartPreviewAsync()
        {
            Debug.WriteLine("StartPreviewAsync");

            // Start the preview
            try
            {
                await m_mediaCapture.StartPreviewToCustomSinkAsync(m_encodingProfile, m_mfExtension);
            }
            catch (Exception e)
            {
                Debug.WriteLine("Exception caught: " + e.ToString());
            }
            finally
            {
                m_isPlaying = true;
            }
        }

        /// <summary>
        /// Stops the preview and deactivates a display request, to allow the screen to go into power saving modes, and locks the UI
        /// </summary>
        /// <returns></returns>
        private async Task StopPreviewAsync()
        {
            m_isPlaying = false;
            await m_mediaCapture.StopPreviewAsync();
        }

        /// <summary>
        /// Cleans up the camera resources (after stopping the preview if necessary) and unregisters from MediaCapture events
        /// </summary>
        /// <returns></returns>
        private async Task CleanupCameraAsync()
        {
            await m_mediaCaptureLifeLock.WaitAsync();

            try
            {
                if (m_isInitialized)
                {
                    if (m_isPlaying)
                    {
                        // The call to stop the preview is included here for completeness, but can be
                        // safely removed if a call to MediaCapture.Dispose() is being made later,
                        // as the preview will be automatically stopped at that point
                        await StopPreviewAsync();
                    }

                    m_isInitialized = false;
                }

                if (m_mediaCapture != null)
                {
                    m_mediaCapture.Failed -= MediaCapture_Failed;
                    m_mediaCapture.CaptureDeviceExclusiveControlStatusChanged -= MediaCapture_CaptureDeviceExclusiveControlStatusChanged;
                    m_mediaCapture.CameraStreamStateChanged -= MediaCapture_CameraStreamStateChanged;
                    m_mediaCapture.Dispose();
                    m_mediaCapture = null;
                }

                if (m_mediaSink != null)
                {
                    m_mediaSink.RawImageCapturedEvent -= RawImageCaptured;
                    m_mediaSink.Dispose();
                    m_mediaSink = null;
                }

                if (m_mfExtension != null)
                {
                    m_mfExtension = null;
                }

            }
            finally
            {
                m_mediaCaptureLifeLock.Release();
            }
        }

        #endregion MediaCapture methods

        #region Helper functions

        /// <summary>
        /// Queries the available video capture devices to try and find one mounted on the desired panel
        /// </summary>
        /// <param name="desiredPanel">The panel on the device that the desired camera is mounted on</param>
        /// <returns>A DeviceInformation instance with a reference to the camera mounted on the desired panel if available,
        ///          any other camera if not, or null if no camera is available.</returns>
        private static async Task<DeviceInformation> FindCameraDeviceByPanelAsync(Windows.Devices.Enumeration.Panel desiredPanel)
        {
            // Get available devices for capturing pictures
            var allVideoDevices = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture);

            // Get the desired camera by panel
            DeviceInformation desiredDevice = allVideoDevices.FirstOrDefault(x => x.EnclosureLocation != null && x.EnclosureLocation.Panel == desiredPanel);

            // If there is no device mounted on the desired panel, return the first device found
            return desiredDevice ?? allVideoDevices.FirstOrDefault();
        }
        #endregion Helper functions
#endif
    }
}
