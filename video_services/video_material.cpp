//===========================================================================//
//
// Purpose: Webm capable video_services replacement
// Written by: Noodles
// Utilising Błażej Szczygieł's libsimplewebm
//
//===========================================================================//

#include "video_material.h"
#include "video_services.h"
#include "tier0/platform.h"
#include "tier1/KeyValues.h"
#include "tier3/tier3.h"
#include "materialsystem/imaterial.h"
#include "filesystem.h"
#include "tier1/utlbuffer.h"

#ifdef _LINUX
#include "SDL2/SDL_audio.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define BUFFER_SIZE 81920 * 4

//=============================================================================
// 
// Video texture regenerator
// 
//=============================================================================
void CYUVTextureRegenerator::RegenerateTextureBits( ITexture *pTexture, IVTFTexture *pVTFTexture, Rect_t *pSubRect )
{
	unsigned char *imageData = pVTFTexture->ImageData();
	int rowSize = pVTFTexture->RowSizeInBytes( 0 );
	// Bik shader only supports YUV420
	// TODO - support more video colour formats but I will need shaders
	if ( m_decodedImage && m_decodedImage->chromaShiftW == 1 && m_decodedImage->chromaShiftH == 1 )
	{
		unsigned char *pixels = m_decodedImage->planes[m_channel];
		int lineSize = m_decodedImage->linesize[m_channel];
		for ( int y = 0; y < m_videoHeight; ++y )
		{
			memcpy( imageData, pixels, m_videoWidth );
			imageData += rowSize;
			pixels += lineSize;
		}
	}
}

void CYUVTextureRegenerator::Release()
{
}

//=============================================================================
// 
// Video material
// 
//=============================================================================

//-----------------------------------------------------------------------------
// Purpose: a paranoid amount of initialisation
//-----------------------------------------------------------------------------
CVideoMaterial::CVideoMaterial()
{
	m_mkvReader = nullptr;
	m_demuxer = nullptr;
	m_videoDecoder = nullptr;
	m_audioDecoder = nullptr;
	m_audioFrame = new WebMFrame();
	m_image = new VPXDecoder::Image();
	m_pcm = nullptr;

	m_videoWidth = 0;
	m_videoHeight = 0;
	m_textureWidth = 0;
	m_textureHeight = 0;

	m_videoReady = false;
	m_videoPlaying = false;
	m_videoStarted = false;
	m_videoLooping = false;
	m_videoStopped = false;
	m_videoEnded = false;

	m_soundKilled = false;

	m_videoPath[0] = '\0';
	//m_pcmOverflowSize = 0;
	//m_pcmOverflowOffset = 0;
	m_volume = 0.0f;
	m_videoTime = 0.0;
	m_curTime = 0.0;
	m_prevTicks = 0;

	m_currentFrame = 0;

	m_yTextureRegen = nullptr;
	m_crTextureRegen = nullptr;
	m_cbTextureRegen = nullptr;

	m_pAudioDevice = nullptr;

	m_nAudioBufferWriteOffset = 0;
	m_nAudioBufferReadOffset = 0;

	m_nAudioBufferSize = 0;
	m_nBytesPerSample = 0;
	m_nBytesPerSecond = 0;
	m_nAudioBufferWritten = 0;

#ifdef _WIN32
	m_beginningEventHandle = NULL;
	m_halfwayEventHandle = NULL;
	m_bufferDestroyedEventHandle = NULL;

	ZeroMemory( &m_waveFormat, sizeof( WAVEFORMATEX ) );
	m_directSoundBuffer = nullptr;
	
	m_directSoundNotify = nullptr;

	m_hTest = NULL;
#endif
}

CVideoMaterial::~CVideoMaterial()
{
	m_videoEnded = true;
	m_videoStopped = true;
	m_videoFrames.Purge();

	DestroySoundBuffer();

	// Nooodles; often the same video material is used over and over, so unless you completely rid of it
	// issues arise. I don't know how much of this is necessary anymore now that it actually dies, but better safe than sorry

	if ( m_crTexture.IsValid() )
	{
		m_crTexture->SetTextureRegenerator( nullptr );
		m_crTexture.Shutdown( true );
	}
	if ( m_cbTexture.IsValid() )
	{
		m_cbTexture->SetTextureRegenerator( nullptr );
		m_cbTexture.Shutdown( true );
	}
	if ( m_yTexture.IsValid() )
	{
		m_yTexture->SetTextureRegenerator( nullptr );
		m_yTexture.Shutdown( true );
	}

	delete m_yTextureRegen;
	delete m_crTextureRegen;
	delete m_cbTextureRegen;

	IMaterial *material = m_videoMaterial;
	m_videoMaterial.Shutdown();
	// Removes any material that might reference the video texture
	materials->UncacheUnusedMaterials();

	// kill it if it remains
	if ( material )
		material->DeleteIfUnreferenced();

#ifdef _WIN32
	if ( m_beginningEventHandle )
		CloseHandle( m_beginningEventHandle );
	if ( m_halfwayEventHandle )
		CloseHandle( m_halfwayEventHandle );
	if ( m_bufferDestroyedEventHandle )
		CloseHandle( m_bufferDestroyedEventHandle );
#endif

	delete m_pcm;
	delete m_image;
	delete m_audioDecoder;
	delete m_videoDecoder;
	delete m_demuxer;
	delete m_mkvReader;

	delete m_audioFrame;

	delete m_pAudioBuffer;

}

bool CVideoMaterial::LoadVideo( const char *pMaterialName, const char *pVideoFileName, void *pSoundDevice )
{
	Q_strncpy( m_videoPath, pVideoFileName, sizeof( m_videoPath ) );
	m_mkvReader = new MkvReader( m_videoPath );
	if ( !m_mkvReader )
		return false;

	m_demuxer = new WebMDemuxer( m_mkvReader );
	if ( !m_demuxer || !m_demuxer->isOpen() )
	{
		if ( m_demuxer )
		{
			delete m_mkvReader;
			delete m_demuxer;
			m_mkvReader = nullptr;
			m_demuxer = nullptr;
		}
		return false;
	}

#ifdef WIN32
	SYSTEM_INFO info;
	GetSystemInfo( &info );
	int numthreads = clamp( info.dwNumberOfProcessors - 2, 1, 8);
	m_videoDecoder = new VPXDecoder( *m_demuxer, numthreads );
#else
	// TODO - Get linux core count
	m_videoDecoder = new VPXDecoder( *m_demuxer, 4 );
#endif
	m_audioDecoder = new OpusVorbisDecoder( *m_demuxer );
	m_pcm = m_audioDecoder->isOpen() ? new short[m_audioDecoder->getBufferSamples() * m_demuxer->getChannels()] : NULL;
	m_videoWidth = m_demuxer->getWidth();
	m_videoHeight = m_demuxer->getHeight();
	m_frameRate.SetFPS( m_demuxer->getFrameRate() ); // This is a guessed framerate from the first 50 frames
	CreateSoundBuffer(pSoundDevice);

	// ---------------------------
	// texture
	char ytexture[MAX_PATH];
	Q_snprintf( ytexture, MAX_PATH, "%s_y", pMaterialName );
	char crtexture[MAX_PATH];
	Q_snprintf( crtexture, MAX_PATH, "%s_cr", pMaterialName );
	char cbtexture[MAX_PATH];
	Q_snprintf( cbtexture, MAX_PATH, "%s_cb", pMaterialName );

	int tex_flags = TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_PROCEDURAL |
		TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY;

	// need a power of 2 or weird things happen
	m_textureWidth = SmallestPowerOfTwoGreaterOrEqual( m_videoWidth );
	m_textureHeight = SmallestPowerOfTwoGreaterOrEqual( m_videoHeight );

	// create the textures
	m_yTexture.InitProceduralTexture( ytexture, "VideoCacheTextures", m_textureWidth, m_textureHeight, IMAGE_FORMAT_I8, tex_flags );
	// CB and CR are half the size of the Y (the brightness)
	m_cbTexture.InitProceduralTexture( cbtexture, "VideoCacheTextures", m_textureWidth >> 1, m_textureHeight >> 1, IMAGE_FORMAT_I8, tex_flags );
	m_crTexture.InitProceduralTexture( crtexture, "VideoCacheTextures", m_textureWidth >> 1, m_textureHeight >> 1, IMAGE_FORMAT_I8, tex_flags );

	m_yTextureRegen = new CYUVTextureRegenerator( CHANNEL_Y, m_videoWidth, m_videoHeight );
	m_cbTextureRegen = new CYUVTextureRegenerator( CHANNEL_CB, m_videoWidth / 2, m_videoHeight / 2 );
	m_crTextureRegen = new CYUVTextureRegenerator( CHANNEL_CR, m_videoWidth / 2, m_videoHeight / 2 );
	m_yTexture->SetTextureRegenerator( m_yTextureRegen );
	m_crTexture->SetTextureRegenerator( m_crTextureRegen );
	m_cbTexture->SetTextureRegenerator( m_cbTextureRegen );

	// ---------------------------
	// material
	// 
	// Use the Bik shader as it deals with YUV420
	KeyValues *pVMTKeyValues = new KeyValues( "Bik" );
	pVMTKeyValues->SetString( "$ytexture", ytexture );
	pVMTKeyValues->SetString( "$cbtexture", cbtexture );
	pVMTKeyValues->SetString( "$crtexture", crtexture );
	pVMTKeyValues->SetInt( "$nofog", 1 );
	pVMTKeyValues->SetInt( "$spriteorientation", 3 );
	pVMTKeyValues->SetInt( "$translucent", 1 );
	pVMTKeyValues->SetInt( "$nolod", 1 );
	pVMTKeyValues->SetInt( "$vertexcolor", 1 );
	pVMTKeyValues->SetInt( "$vertexalpha", 1 );
	pVMTKeyValues->SetInt( "$nomip", 1 );
	m_videoMaterial.Init( pMaterialName, pVMTKeyValues );

	// Refresh the material vars because apparently init doesn't do this
	// and retains the previous video's frame
	m_videoMaterial->Refresh();

	m_videoReady = true;
	m_videoStarted = false;

	// update the procedural texture with the first frame of the video
	WebMFrame video_frame;
	VPXDecoder::Image image;
	while ( m_demuxer->readFrame( &video_frame, nullptr ) )
	{
		if ( !m_videoDecoder->decode( video_frame ) )
			continue;

		VPXDecoder::IMAGE_ERROR err;
		if ( ( err = m_videoDecoder->getImage( image ) ) == VPXDecoder::NO_FRAME )
			continue;

		if ( err == VPXDecoder::IMAGE_ERROR::NO_IMAGE_ERROR )
		{
			m_yTextureRegen->m_decodedImage = &image;
			m_crTextureRegen->m_decodedImage = &image;
			m_cbTextureRegen->m_decodedImage = &image;

			m_yTexture->Download();
			m_crTexture->Download();
			m_cbTexture->Download();

			break;
		}
	}

	m_demuxer->resetVideo();
	return true;
}

const char *CVideoMaterial::GetVideoFileName()
{
	return m_videoPath;
}

VideoResult_t CVideoMaterial::GetLastResult()
{
	return VideoResult_t::SYSTEM_NOT_AVAILABLE;
}

VideoFrameRate_t &CVideoMaterial::GetVideoFrameRate()
{
	return m_frameRate;
}

// Video playback state functions
bool CVideoMaterial::IsVideoReadyToPlay()
{
	return m_videoReady;
}

bool CVideoMaterial::IsVideoPlaying()
{
	return m_videoPlaying;
}

bool CVideoMaterial::IsNewFrameReady()
{
	return false;
}

bool CVideoMaterial::IsFinishedPlaying()
{
	return m_videoEnded;
}

#ifdef _WIN32
//-----------------------------------------------------------------------------
// Purpose: Thread function that deals with pausing the sound buffer if
//	the other half of the sound buffer hasn't recently been filled
//-----------------------------------------------------------------------------
unsigned CVideoMaterial::_HandleBufferEvents( void *params )
{
	CVideoMaterial *m = (CVideoMaterial *)params;m_pAudioBuffer1
		case WAIT_OBJECT_0:
			if ( !m->WasAudioBufferCopied(false) )
			{
				m->m_mutex.Lock();
				IDirectSoundBuffer_Stop( m->m_directSoundBuffer );
				m->m_mutex.Unlock();
			}
			break;

		case WAIT_OBJECT_0 + 1:
			if ( !m->WasAudioBufferCopied( true ) )
			{
				m->m_mutex.Lock();
				IDirectSoundBuffer_Stop( m->m_directSoundBuffer );
				m->m_mutex.Unlock();
			}
			break;

		case WAIT_OBJECT_0 + 2:
			return 1;
			break;

		case WAIT_FAILED:
			break;

		default:
			// Timeout occurred or abandoned
			break;
		}
	}
	return 1;
}
#endif

bool CVideoMaterial::CreateSoundBuffer(void *pSoundDevice)
{
	if ( !m_audioDecoder->isOpen() )
		return false;

	if( !pSoundDevice )
	{
		ConMsg("No sound device!\n");
		return false;
	}

#ifdef _LINUX
	// todo; Error checking

	// this is a copy recieved from services so we don't need to allocate it
	m_pAudioDevice = (SDL_AudioSpec*)pSoundDevice;
	m_nBytesPerSample = m_pAudioDevice->channels * ( SDL_AUDIO_BITSIZE( m_pAudioDevice->format ) / 8 );
	m_nBytesPerSecond = m_pAudioDevice->freq * m_nBytesPerSample;

	//Calculate buffer size
	m_nAudioBufferSize = BUFFER_SIZE;

	//Allocate and initialize byte buffer
	m_pAudioBuffer = new Uint8[ m_nAudioBufferSize ];
	memset( m_pAudioBuffer, 0, m_nAudioBufferSize );

	return true;

#elif _WIN32
	m_pSoundDevice = (IDirectSound8 *)pSoundDevice;

	if ( !m_pSoundDevice )
		return false;


	if ( m_directSoundBuffer )
		return true;

	ZeroMemory( &m_waveFormat, sizeof( WAVEFORMATEX ) );
	m_waveFormat.wFormatTag = WAVE_FORMAT_PCM;
	m_waveFormat.nChannels = m_demuxer->getChannels();
	m_waveFormat.nSamplesPerSec = m_demuxer->getSampleRate();
	m_waveFormat.wBitsPerSample = 16; // S16
	m_waveFormat.nBlockAlign = ( m_waveFormat.nChannels * m_waveFormat.wBitsPerSample ) / 8;
	m_waveFormat.nAvgBytesPerSec = m_waveFormat.nSamplesPerSec * m_waveFormat.nBlockAlign;
	m_waveFormat.cbSize = 0;


	DSBUFFERDESC dsbd;
	ZeroMemory( &dsbd, sizeof( DSBUFFERDESC ) );
	dsbd.dwSize = sizeof( DSBUFFERDESC );
	dsbd.dwBufferBytes = BUFFER_SIZE * m_waveFormat.nBlockAlign;
	dsbd.lpwfxFormat = &m_waveFormat;
	// if we have the losefocus cvar determine if we want to remove the global focus flag
	dsbd.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GLOBALFOCUS;
	ConVar *snd_mute_losefocus = g_pCVar->FindVar( "snd_mute_losefocus" );
	if ( snd_mute_losefocus )
	{
		if ( snd_mute_losefocus->GetBool() )
			dsbd.dwFlags = dsbd.dwFlags & ~( DSBCAPS_GLOBALFOCUS );
	}

	if ( FAILED( m_pSoundDevice->CreateSoundBuffer( &dsbd, &m_directSoundBuffer, NULL ) ) )
		return false;

	// Set notification positions
	DSBPOSITIONNOTIFY posNotify[2];
	ZeroMemory( posNotify, sizeof( posNotify ) );

	// create nofitication
	m_beginningEventHandle = CreateEvent( NULL, FALSE, FALSE, NULL );
	m_halfwayEventHandle = CreateEvent( NULL, FALSE, FALSE, NULL );
	m_bufferDestroyedEventHandle = CreateEvent( NULL, FALSE, FALSE, NULL );

	// notifcations at end and halfway mark
	posNotify[0].dwOffset = ( BUFFER_SIZE * m_waveFormat.nBlockAlign ) - 1;
	posNotify[0].hEventNotify = m_beginningEventHandle;
	posNotify[1].dwOffset = ( BUFFER_HALF_SIZE * m_waveFormat.nBlockAlign ) - 1;
	posNotify[1].hEventNotify = m_halfwayEventHandle;

	if ( FAILED( m_directSoundBuffer->QueryInterface( IID_IDirectSoundNotify, (LPVOID *)&m_directSoundNotify ) ) )
		return false;

	m_directSoundNotify->SetNotificationPositions( 2, posNotify );

	IDirectSoundBuffer_Play( m_directSoundBuffer, 0, 0, DSBPLAY_LOOPING );
	m_hTest = CreateSimpleThread( _HandleBufferEvents, this );
	return true;
#else
	return false;
#endif
}

void CVideoMaterial::DestroySoundBuffer()
{
#ifdef _WIN32
	if ( m_directSoundBuffer )
	{
		if ( m_hTest )
		{
			SetEvent( m_bufferDestroyedEventHandle );
			ThreadJoin( m_hTest );
			ReleaseThreadHandle( m_hTest );
		}

		if ( !m_soundKilled )
		{
			m_directSoundNotify->Release();
			m_directSoundBuffer->Stop();
			m_directSoundBuffer->Release();
		}
	}
	m_directSoundNotify = nullptr;
	m_directSoundBuffer = nullptr;
#endif
}

bool CVideoMaterial::StartVideo()
{
	if ( m_videoStarted )
		return true;

	m_videoStarted = true;
	m_videoPlaying = true;
	m_videoStopped = false;

	m_currentFrame = 0;
	m_videoTime = 0.0;
	m_curTime = 0.0;

	m_prevTicks = Plat_MSTime();

	return true;
}

bool CVideoMaterial::StopVideo()
{
	if ( !m_videoStarted )
		return true;

	m_videoStopped = true;
	m_videoStarted = false;
	DestroySoundBuffer();

	return true;
}

void CVideoMaterial::SetLooping( bool bLoopVideo )
{
	m_videoLooping = bLoopVideo;
}

bool CVideoMaterial::IsLooping()
{
	return m_videoLooping;
}

void CVideoMaterial::SetPaused( bool bPauseState )
{
	if ( m_videoStarted )
	{
		// Unpause
		if ( !m_videoPlaying && !bPauseState )
		{
#ifdef _WIN32
			if ( m_directSoundBuffer )
				IDirectSoundBuffer_Play( m_directSoundBuffer, 0, 0, DSBPLAY_LOOPING );
#endif
			m_prevTicks = Plat_MSTime();
		}
		// Pause
		else if ( m_videoPlaying && bPauseState )
		{
#ifdef _WIN32
			if ( m_directSoundBuffer )
				IDirectSoundBuffer_Stop( m_directSoundBuffer );
#endif
		}
	}

	m_videoPlaying = !bPauseState;
}

bool CVideoMaterial::IsPaused()
{
	return !m_videoPlaying;
}

float CVideoMaterial::GetVideoDuration()
{
	if ( m_demuxer )
		return m_demuxer->getLength();
	return 0.0f;
}

int CVideoMaterial::GetFrameCount()
{
	return 0;
}

bool CVideoMaterial::SetFrame( int FrameNum )
{
	return false;
}

int	CVideoMaterial::GetCurrentFrame()
{
	return m_currentFrame;
}

bool CVideoMaterial::SetTime( float flTime )
{
	return false;
}

float CVideoMaterial::GetCurrentVideoTime()
{
	return 0.0f;
}

bool CVideoMaterial::NeedNewFrame( double curtime )
{
	if ( m_videoFrames.Count() == 0 )
		return true;

	if ( m_videoFrames.Tail()->time <= curtime )
		return true;

	return false;
}

void CVideoMaterial::RestartVideo()
{
	m_currentFrame = 0;
	m_demuxer->resetVideo();
	m_curTime = m_videoTime = 0.0;
	m_prevTicks = Plat_MSTime();
#ifdef _WIN32
	if ( m_directSoundBuffer && !m_soundKilled )
		m_directSoundBuffer->SetCurrentPosition( 0 );
#endif
}

bool CVideoMaterial::Update()
{
	if ( !StartVideo() )
	{
		return false;
	}

	// the video has stopped, there is nothing more to do
	if ( m_videoStopped )
		return false;

	// we're not stopped, but we're paused
	if ( !m_videoPlaying )
	{
		return true;
	}

	// Update time
	unsigned int curTicks = Plat_MSTime();
	m_curTime += ( curTicks - m_prevTicks ) / 1000.0;
	m_prevTicks = curTicks;

	if ( m_curTime < m_videoTime && m_nAudioBufferWritten > m_pAudioDevice->size )
	{
		return true;
	}

	// Has the stream ended?
	if ( m_demuxer->isEOS() )
	{
		// Noodles; this might be stupid
		if ( m_videoFrames.Count() > 0 )
		{
#ifdef _WIN32
			IDirectSoundBuffer_Stop( m_directSoundBuffer );
#endif
		}
		else
		{
			if ( m_videoLooping )
			{
				RestartVideo();
			}
			else
			{
				m_videoEnded = true;
				StopVideo();
				return false;
			}
		}
	}

	// if we have audio check if we should update the buffer and make sure we're playing
	bool bUpdateBuffer = false;

	// Read until we've filled the buffer or got enough frames
	while ( NeedNewFrame( m_curTime ) || bUpdateBuffer || m_nAudioBufferWritten <= m_pAudioDevice->size )
	{
		WebMFrame *video_frame = new WebMFrame();
		// did we reach the EOS
		if ( !m_demuxer->readFrame( video_frame, m_audioFrame ) )
		{
			bUpdateBuffer = false;
			delete video_frame;
			break;
		}
		else if ( !video_frame->isValid() )
		{
			delete video_frame;
		}
		else
			m_vecVideoFrames.Insert( video_frame );


		int nPCMOverflowSize = 0;
		int nPCMOverflowOffset = 0;

		if ( m_audioFrame->isValid() )
		{
			bUpdateBuffer = false;

			int nBytesRead = 0;
			int numOutSamples = 0;
			m_audioDecoder->getPCMS16( *m_audioFrame, m_pcm, numOutSamples );
			nBytesRead = numOutSamples * m_nBytesPerSample;
			m_nAudioBufferWritten += nBytesRead;

			// if the current number of samples is larger than what we can fit save it for the next half
			if ( (m_nAudioBufferWriteOffset + nBytesRead) >= m_nAudioBufferSize )
			{
				// save amount gone over
				nPCMOverflowSize = (m_nAudioBufferWriteOffset + nBytesRead) - m_nAudioBufferSize;
				nPCMOverflowOffset = nBytesRead - nPCMOverflowSize;
				nBytesRead -= nPCMOverflowSize;
				bUpdateBuffer = true;
			}

			memcpy( m_pAudioBuffer + m_nAudioBufferWriteOffset, m_pcm, nBytesRead );
			m_nAudioBufferWriteOffset += nBytesRead;

			if( m_nAudioBufferWriteOffset == m_nAudioBufferSize )
			{
				m_nAudioBufferWriteOffset = 0;
				memcpy( &m_pAudioBuffer[0], (char *)(m_pcm) + nPCMOverflowOffset, nPCMOverflowSize );
				m_nAudioBufferWriteOffset += nPCMOverflowSize;
			}

			// if our timer is waaaaayyy ahead of the audio time set it back
			if ( m_curTime > m_audioFrame->time )
			{
				m_curTime = m_videoTime;
			}
		}
	}

	// roll back for videos with no audio
	if ( !m_demuxer->isEOS() && !m_audioDecoder->isOpen() )
	{
		// if our current time is out, roll it back
		// Noodles; I feel this will cause issues, but it seems fine right now
		double frameDur = 1.0 / m_frameRate.GetFPS();
		if ( m_videoFrames.Count() > 0 && ( m_curTime - m_videoFrames.Head()->time ) > ( frameDur * 6.0 ) )
		{
			m_curTime = m_videoTime - frameDur;
		}
	}

	while ( m_videoFrames.Count() > 0 && m_curTime >= m_videoTime )
	{
		if ( m_videoFrames.Head()->isValid() )
		{
			// TODO figure out how to skip frames
			m_videoDecoder->decode( *m_videoFrames.Head() );

			VPXDecoder::IMAGE_ERROR err;
			if ( ( err = m_videoDecoder->getImage( *m_image ) ) != VPXDecoder::NO_FRAME )
			{
				if ( err == VPXDecoder::IMAGE_ERROR::NO_IMAGE_ERROR )
				{
					m_yTextureRegen->m_decodedImage = m_image;
					m_crTextureRegen->m_decodedImage = m_image;
					m_cbTextureRegen->m_decodedImage = m_image;

					m_yTexture->Download();
					m_crTexture->Download();
					m_cbTexture->Download();
				}
			}
			m_videoTime = m_videoFrames.Head()->time;
			m_currentFrame++;
		}

		WebMFrame *frame = m_videoFrames.RemoveAtHead();
		if( frame )
			delete frame;
	}
	
	return true;
}

// Material / Texture Info functions
IMaterial *CVideoMaterial::GetMaterial()
{
	return m_videoMaterial;
}

// Where the video is actually is within the texture
void CVideoMaterial::GetVideoTexCoordRange( float *pMaxU, float *pMaxV )
{
	*pMaxU = m_videoWidth / (float)m_textureWidth;
	*pMaxV = m_videoHeight / (float)m_textureHeight;
}

void CVideoMaterial::GetVideoImageSize( int *pWidth, int *pHeight )
{
	*pWidth = m_videoWidth;
	*pHeight = m_videoHeight;
}

// Audio Functions
bool CVideoMaterial::HasAudio()
{
	return m_audioDecoder && m_audioDecoder->isOpen();
}

bool CVideoMaterial::SetVolume( float fVolume )
{
#ifdef _WIN32
	if ( !m_directSoundBuffer )
		return false;

	float vol = min( 1.0f, max( 0.0f, fVolume ) );
	m_volume = vol;
	// TODO figure out what fucking value I'm supposed to use
	float log_volume = pow( vol, 0.2 );
	m_directSoundBuffer->SetVolume( (LONG)( -10000 * ( 1.0f - log_volume ) ) );
	return true;
#else
	return false;
#endif
}

float CVideoMaterial::GetVolume()
{
	return m_volume;
}

void CVideoMaterial::SetMuted( bool bMuteState )
{
}

bool CVideoMaterial::IsMuted()
{
	return false;
}

VideoResult_t CVideoMaterial::SoundDeviceCommand( VideoSoundDeviceOperation_t operation, void *pDevice, void *pData )
{
#ifdef _WIN32
	if ( operation == VideoSoundDeviceOperation_t::SET_DIRECT_SOUND_DEVICE && pDevice )
	{
		// if we had a sound buffer before, kill it
		if ( m_pSoundDevice )
		{
			m_soundKilled = true;
			DestroySoundBuffer();
			m_soundKilled = false;
		}

		m_pSoundDevice = (IDirectSound8 *)pDevice;
		CreateSoundBuffer();
		return VideoResult_t::SUCCESS;
	}
#elif _LINUX
	// Maybe called when changing audio device?
	if( operation == VideoSoundDeviceOperation_t::SET_SDL_SOUND_DEVICE )
	{
	}
	// Called on start up and sound restart
	else if( operation == VideoSoundDeviceOperation_t::SET_SDL_PARAMS )
	{
		// todo
	}
	else if( operation == VideoSoundDeviceOperation_t::SDLMIXER_CALLBACK )
	{
		if ( !m_audioDecoder->isOpen() )
			return VideoResult_t::SUCCESS;

		if( !m_videoStarted )
			return VideoResult_t::SUCCESS;

		int length = *(int *)pData;
		Uint8 *stream = (Uint8 *)pDevice;
		SDL_MixAudioFormat( stream, &m_pAudioBuffer[m_nAudioBufferReadOffset], m_pAudioDevice->format, length, 64 );
		m_nAudioBufferReadOffset += length;
		m_nAudioBufferWritten -= length;

		if( m_nAudioBufferReadOffset == m_nAudioBufferSize )
		{
			m_nAudioBufferReadOffset = 0;
		}

		return VideoResult_t::SUCCESS;
	}
#endif
	return VideoResult_t::SYSTEM_NOT_AVAILABLE;
}
