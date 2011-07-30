/*
 *  Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011 Stephen F. Booth <me@sbooth.org>
 *  All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    - Neither the name of Stephen F. Booth nor the names of its 
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <libkern/OSAtomic.h>
#include <pthread.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_error.h>
#include <mach/task.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#include <Accelerate/Accelerate.h>
#include <stdexcept>
#include <new>
#include <algorithm>
#include <iomanip>

#include <log4cxx/logger.h>
#include <log4cxx/logmanager.h>
#include <log4cxx/basicconfigurator.h>
#include <log4cxx/ndc.h>

#include "AudioPlayer.h"
#include "DecoderStateData.h"
#include "AllocateABL.h"
#include "DeallocateABL.h"
#include "ChannelLayoutsAreEqual.h"
#include "CFOperatorOverloads.h"
#include "CreateChannelLayout.h"

#include "CARingBuffer.h"

// ========================================
// Macros
// ========================================
#define RING_BUFFER_CAPACITY_FRAMES				16384
#define RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES		2048
#define DECODER_THREAD_IMPORTANCE				6
#define SLEEP_TIME_USEC							1000
#define DECLARE_LOGGER							log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"))

static void InitializeLoggingSubsystem() __attribute__ ((constructor));
static void InitializeLoggingSubsystem()
{
	// Turn off logging by default
	if(!log4cxx::LogManager::getLoggerRepository()->isConfigured())
		log4cxx::Logger::getRootLogger()->setLevel(log4cxx::Level::getOff());
}

// ========================================
// Set the calling thread's timesharing and importance
// ========================================
static bool
setThreadPolicy(integer_t importance)
{
	// Turn off timesharing
	thread_extended_policy_data_t extendedPolicy = { 0 };
	kern_return_t error = thread_policy_set(mach_thread_self(),
											THREAD_EXTENDED_POLICY,
											(thread_policy_t)&extendedPolicy, 
											THREAD_EXTENDED_POLICY_COUNT);
	
	if(KERN_SUCCESS != error) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_WARN(logger, "Couldn't set thread's extended policy: " << mach_error_string(error));
		return false;
	}
	
	// Give the thread the specified importance
	thread_precedence_policy_data_t precedencePolicy = { importance };
	error = thread_policy_set(mach_thread_self(), 
							  THREAD_PRECEDENCE_POLICY, 
							  (thread_policy_t)&precedencePolicy, 
							  THREAD_PRECEDENCE_POLICY_COUNT);
	
	if (error != KERN_SUCCESS) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_WARN(logger, "Couldn't set thread's precedence policy: " << mach_error_string(error));
		return false;
	}
	
	return true;
}

// ========================================
// The AUGraph input callback
// ========================================
static OSStatus
myAURenderCallback(void *							inRefCon,
				   AudioUnitRenderActionFlags *		ioActionFlags,
				   const AudioTimeStamp *			inTimeStamp,
				   UInt32							inBusNumber,
				   UInt32							inNumberFrames,
				   AudioBufferList *				ioData)
{
	assert(NULL != inRefCon);

	AudioPlayer *player = static_cast<AudioPlayer *>(inRefCon);
	return player->Render(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
}

static OSStatus
auGraphDidRender(void *							inRefCon,
				 AudioUnitRenderActionFlags *	ioActionFlags,
				 const AudioTimeStamp *			inTimeStamp,
				 UInt32							inBusNumber,
				 UInt32							inNumberFrames,
				 AudioBufferList *				ioData)
{
	assert(NULL != inRefCon);

	AudioPlayer *player = static_cast<AudioPlayer *>(inRefCon);
	return player->DidRender(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
}

//#pragma mark -Audio Session Interruption Listener
//
//void rioInterruptionListener(void *inClientData, UInt32 inInterruption)
//{
//    printf("Session interrupted! --- %s ---", inInterruption == kAudioSessionBeginInterruption ? "Begin Interruption" : "End Interruption");
//    
//    aurioTouchAppDelegate *THIS = (aurioTouchAppDelegate*)inClientData;
//    
//    if (inInterruption == kAudioSessionEndInterruption) {
//        // make sure we are again the active session
//        AudioSessionSetActive(true);
//        AudioOutputUnitStart(THIS->rioUnit);
//    }
//    
//    if (inInterruption == kAudioSessionBeginInterruption) {
//        AudioOutputUnitStop(THIS->rioUnit);
//    }
//}
//
//#pragma mark -Audio Session Property Listener
//
//void propListener(  void *                  inClientData,
//				  AudioSessionPropertyID  inID,
//				  UInt32                  inDataSize,
//				  const void *            inData)
//{
//    aurioTouchAppDelegate *THIS = (aurioTouchAppDelegate*)inClientData;
//    if (inID == kAudioSessionProperty_AudioRouteChange)
//    {
//        try {
//            // if there was a route change, we need to dispose the current rio unit and create a new one
//            XThrowIfError(AudioComponentInstanceDispose(THIS->rioUnit), "couldn't dispose remote i/o unit");        
//			
//            SetupRemoteIO(THIS->rioUnit, THIS->inputProc, THIS->thruFormat);
//            
//            UInt32 size = sizeof(THIS->hwSampleRate);
//            XThrowIfError(AudioSessionGetProperty(kAudioSessionProperty_CurrentHardwareSampleRate, &size, &THIS->hwSampleRate), "couldn't get new sample rate");
//			
//            XThrowIfError(AudioOutputUnitStart(THIS->rioUnit), "couldn't start unit");
//			
//            // we need to rescale the sonogram view's color thresholds for different input
//            CFStringRef newRoute;
//            size = sizeof(CFStringRef);
//            XThrowIfError(AudioSessionGetProperty(kAudioSessionProperty_AudioRoute, &size, &newRoute), "couldn't get new audio route");
//            if (newRoute)
//            {   
//                CFShow(newRoute);
//                if (CFStringCompare(newRoute, CFSTR("Headset"), NULL) == kCFCompareEqualTo) // headset plugged in
//                {
//                    colorLevels[0] = .3;                
//                    colorLevels[5] = .5;
//                }
//                else if (CFStringCompare(newRoute, CFSTR("Receiver"), NULL) == kCFCompareEqualTo) // headset plugged in
//                {
//                    colorLevels[0] = 0;
//                    colorLevels[5] = .333;
//                    colorLevels[10] = .667;
//                    colorLevels[15] = 1.0;
//                    
//                }           
//                else
//                {
//                    colorLevels[0] = 0;
//                    colorLevels[5] = .333;
//                    colorLevels[10] = .667;
//                    colorLevels[15] = 1.0;
//                    
//                }
//            }
//        } catch (CAXException e) {
//            char buf[256];
//            fprintf(stderr, "Error: %s (%s)\n", e.mOperation, e.FormatError(buf));
//        }
//        
//    }
//}
//
//#pragma mark -RIO Render Callback
//
//static OSStatus PerformThru(
//                            void                        *inRefCon, 
//                            AudioUnitRenderActionFlags  *ioActionFlags, 
//                            const AudioTimeStamp        *inTimeStamp, 
//                            UInt32                      inBusNumber, 
//                            UInt32                      inNumberFrames, 
//                            AudioBufferList             *ioData)
//{
//    aurioTouchAppDelegate *THIS = (aurioTouchAppDelegate *)inRefCon;
//    OSStatus err = AudioUnitRender(THIS->rioUnit, ioActionFlags, inTimeStamp, 1, inNumberFrames, ioData);
//    if (err) { printf("PerformThru: error %d\n", (int)err); return err; }
//    
//    // Remove DC component
//    for(UInt32 i = 0; i < ioData->mNumberBuffers; ++i)
//        THIS->dcFilter[i].InplaceFilter((SInt32*)(ioData->mBuffers[i].mData), inNumberFrames, 1);
//    
//    if (THIS->displayMode == aurioTouchDisplayModeOscilloscopeWaveform)
//    {
//        // The draw buffer is used to hold a copy of the most recent PCM data to be drawn on the oscilloscope
//        if (drawBufferLen != drawBufferLen_alloced)
//        {
//            int drawBuffer_i;
//            
//            // Allocate our draw buffer if needed
//            if (drawBufferLen_alloced == 0)
//                for (drawBuffer_i=0; drawBuffer_i<kNumDrawBuffers; drawBuffer_i++)
//                    drawBuffers[drawBuffer_i] = NULL;
//            
//            // Fill the first element in the draw buffer with PCM data
//            for (drawBuffer_i=0; drawBuffer_i<kNumDrawBuffers; drawBuffer_i++)
//            {
//                drawBuffers[drawBuffer_i] = (SInt8 *)realloc(drawBuffers[drawBuffer_i], drawBufferLen);
//                bzero(drawBuffers[drawBuffer_i], drawBufferLen);
//            }
//            
//            drawBufferLen_alloced = drawBufferLen;
//        }
//        
//        int i;
//        
//        SInt8 *data_ptr = (SInt8 *)(ioData->mBuffers[0].mData);
//        for (i=0; i<inNumberFrames; i++)
//        {
//            if ((i+drawBufferIdx) >= drawBufferLen)
//            {
//                cycleOscilloscopeLines();
//                drawBufferIdx = -i;
//            }
//            drawBuffers[0][i + drawBufferIdx] = data_ptr[2];
//            data_ptr += 4;
//        }
//        drawBufferIdx += inNumberFrames;
//    }
//    
//    else if ((THIS->displayMode == aurioTouchDisplayModeSpectrum) || (THIS->displayMode == aurioTouchDisplayModeOscilloscopeFFT))
//    {
//        if (THIS->fftBufferManager == NULL) return noErr;
//        
//        if (THIS->fftBufferManager->NeedsNewAudioData())
//        {
//            THIS->fftBufferManager->GrabAudioData(ioData); 
//        }
//        
//    }
//    if (THIS->mute == YES) { SilenceData(ioData); }
//    
//    return err;
//}


// ========================================
// The decoder thread's entry point
// ========================================
static void *
decoderEntry(void *arg)
{
	assert(NULL != arg);
	
	AudioPlayer *player = static_cast<AudioPlayer *>(arg);
	return player->DecoderThreadEntry();
}

// ========================================
// The collector thread's entry point
// ========================================
static void *
collectorEntry(void *arg)
{
	assert(NULL != arg);
	
	AudioPlayer *player = static_cast<AudioPlayer *>(arg);
	return player->CollectorThreadEntry();
}

// ========================================
// AudioConverter input callback
// ========================================
static OSStatus
myAudioConverterComplexInputDataProc(AudioConverterRef				inAudioConverter,
									 UInt32							*ioNumberDataPackets,
									 AudioBufferList				*ioData,
									 AudioStreamPacketDescription	**outDataPacketDescription,
									 void							*inUserData)
{

#pragma unused(inAudioConverter)
#pragma unused(outDataPacketDescription)

	assert(NULL != inUserData);
	assert(NULL != ioNumberDataPackets);

	DecoderStateData *decoderStateData = static_cast<DecoderStateData *>(inUserData);

	decoderStateData->ResetBufferList();

	UInt32 framesRead = decoderStateData->mDecoder->ReadAudio(decoderStateData->mBufferList, *ioNumberDataPackets);

	// Point ioData at our decoded audio
	ioData->mNumberBuffers = decoderStateData->mBufferList->mNumberBuffers;
	for(UInt32 bufferIndex = 0; bufferIndex < decoderStateData->mBufferList->mNumberBuffers; ++bufferIndex)
		ioData->mBuffers[bufferIndex] = decoderStateData->mBufferList->mBuffers[bufferIndex];

	*ioNumberDataPackets = framesRead;

	return noErr;
}

#pragma mark Creation/Destruction


AudioPlayer::AudioPlayer()
	: mAUGraph(NULL), mOutputNode(NULL), mMixerNode(NULL), mFlags(0), mDecoderQueue(NULL), mRingBuffer(NULL), mRingBufferChannelLayout(NULL), mRingBufferCapacity(RING_BUFFER_CAPACITY_FRAMES), mRingBufferWriteChunkSize(RING_BUFFER_WRITE_CHUNK_SIZE_FRAMES), mFramesDecoded(0), mFramesRendered(0), mGuard(), mDecoderSemaphore(), mCollectorSemaphore(), mFramesRenderedLastPass(0)
{
	mDecoderQueue = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);
	
	if(NULL == mDecoderQueue)
		throw std::bad_alloc();

	mRingBuffer = new CARingBuffer();

	// ========================================
	// Initialize the decoder array
	for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex)
		mActiveDecoders[bufferIndex] = NULL;

	// ========================================
	// Launch the decoding thread
	mKeepDecoding = true;
	int creationResult = pthread_create(&mDecoderThread, NULL, decoderEntry, this);
	if(0 != creationResult) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_FATAL(logger, "pthread_create failed: " << strerror(creationResult));
		
		CFRelease(mDecoderQueue), mDecoderQueue = NULL;
		delete mRingBuffer, mRingBuffer = NULL;

		throw std::runtime_error("pthread_create failed");
	}
	
	// ========================================
	// Launch the collector thread
	mKeepCollecting = true;
	creationResult = pthread_create(&mCollectorThread, NULL, collectorEntry, this);
	if(0 != creationResult) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_FATAL(logger, "pthread_create failed: " << strerror(creationResult));
		
		mKeepDecoding = false;
		mDecoderSemaphore.Signal();
		
		int joinResult = pthread_join(mDecoderThread, NULL);
		if(0 != joinResult)
			LOG4CXX_WARN(logger, "pthread_join failed: " << strerror(joinResult));
		
		mDecoderThread = static_cast<pthread_t>(0);
		
		CFRelease(mDecoderQueue), mDecoderQueue = NULL;
		delete mRingBuffer, mRingBuffer = NULL;

		throw std::runtime_error("pthread_create failed");
	}
	
	// ========================================
	// The AUGraph will always receive audio in the canonical Core Audio format
	mRingBufferFormat.mFormatID				= kAudioFormatLinearPCM;
	mRingBufferFormat.mFormatFlags			= kAudioFormatFlagsAudioUnitCanonical;

	mRingBufferFormat.mSampleRate			= 0;
	mRingBufferFormat.mChannelsPerFrame		= 0;
	mRingBufferFormat.mBitsPerChannel		= 8 * sizeof(AudioUnitSampleType);
	
	mRingBufferFormat.mBytesPerPacket		= (mRingBufferFormat.mBitsPerChannel / 8);
	mRingBufferFormat.mFramesPerPacket		= 1;
	mRingBufferFormat.mBytesPerFrame		= mRingBufferFormat.mBytesPerPacket * mRingBufferFormat.mFramesPerPacket;
	
	mRingBufferFormat.mReserved				= 0;

	// ========================================
	// Set up output
	if(!OpenOutput()) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_FATAL(logger, "OpenOutput() failed");
		throw std::runtime_error("OpenOutput() failed");
	}
}

AudioPlayer::~AudioPlayer()
{
	Stop();

	// Stop the processing graph and reclaim its resources
	if(!CloseOutput()) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_ERROR(logger, "CloseOutput() failed");
	}

	// End the decoding thread
	mKeepDecoding = false;
	mDecoderSemaphore.Signal();

	int joinResult = pthread_join(mDecoderThread, NULL);
	if(0 != joinResult) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_ERROR(logger, "pthread_join failed: " << strerror(joinResult));
	}
	
	mDecoderThread = static_cast<pthread_t>(0);

	// End the collector thread
	mKeepCollecting = false;
	mCollectorSemaphore.Signal();
	
	joinResult = pthread_join(mCollectorThread, NULL);
	if(0 != joinResult) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_ERROR(logger, "pthread_join failed: " << strerror(joinResult));
	}
	
	mCollectorThread = static_cast<pthread_t>(0);

	// Force any decoders left hanging by the collector to end
	for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
		if(NULL != mActiveDecoders[bufferIndex])
			delete mActiveDecoders[bufferIndex], mActiveDecoders[bufferIndex] = NULL;
	}
	
	// Clean up any queued decoders
	while(0 < CFArrayGetCount(mDecoderQueue)) {
		AudioDecoder *decoder = static_cast<AudioDecoder *>(const_cast<void *>(CFArrayGetValueAtIndex(mDecoderQueue, 0)));
		CFArrayRemoveValueAtIndex(mDecoderQueue, 0);
		delete decoder;
	}
	
	CFRelease(mDecoderQueue), mDecoderQueue = NULL;

	// Clean up the ring buffer and associated resources
	if(mRingBuffer)
		delete mRingBuffer, mRingBuffer = NULL;

	if(mRingBufferChannelLayout)
		free(mRingBufferChannelLayout), mRingBufferChannelLayout = NULL;
}

#pragma mark Playback Control

bool AudioPlayer::Play()
{
	if(!OutputIsRunning())
		return StartOutput();

	return true;
}

bool AudioPlayer::Pause()
{
	if(OutputIsRunning())
		StopOutput();

	return true;
}

bool AudioPlayer::Stop()
{
	Guard::Locker lock(mGuard);

	if(OutputIsRunning())
		StopOutput();

	StopActiveDecoders();
	
	ResetOutput();

	mFramesDecoded = 0;
	mFramesRendered = 0;

	return true;
}

AudioPlayer::PlayerState AudioPlayer::GetPlayerState() const
{
	if(OutputIsRunning())
		return AudioPlayer::ePlaying;

	DecoderStateData *currentDecoderState = GetCurrentDecoderState();

	if(NULL == currentDecoderState)
		return AudioPlayer::eStopped;

	if(eDecoderStateDataFlagRenderingStarted & currentDecoderState->mFlags)
		return AudioPlayer::ePaused;

	if(eDecoderStateDataFlagDecodingStarted & currentDecoderState->mFlags)
		return AudioPlayer::ePending;

	return AudioPlayer::eStopped;
}

CFURLRef AudioPlayer::GetPlayingURL() const
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return NULL;
	
	return currentDecoderState->mDecoder->GetURL();
}

#pragma mark Playback Properties

bool AudioPlayer::GetCurrentFrame(SInt64& currentFrame) const
{
	SInt64 totalFrames;
	return GetPlaybackPosition(currentFrame, totalFrames);
}

bool AudioPlayer::GetTotalFrames(SInt64& totalFrames) const
{
	SInt64 currentFrame;
	return GetPlaybackPosition(currentFrame, totalFrames);
}

bool AudioPlayer::GetPlaybackPosition(SInt64& currentFrame, SInt64& totalFrames) const
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();

	if(NULL == currentDecoderState)
		return false;

	currentFrame	= (-1 == currentDecoderState->mFrameToSeek ? currentDecoderState->mFramesRendered : currentDecoderState->mFrameToSeek);
	totalFrames		= currentDecoderState->mTotalFrames;

	return true;
}

bool AudioPlayer::GetCurrentTime(CFTimeInterval& currentTime) const
{
	CFTimeInterval totalTime;
	return GetPlaybackTime(currentTime, totalTime);
}

bool AudioPlayer::GetTotalTime(CFTimeInterval& totalTime) const
{
	CFTimeInterval currentTime;
	return GetPlaybackTime(currentTime, totalTime);
}

bool AudioPlayer::GetPlaybackTime(CFTimeInterval& currentTime, CFTimeInterval& totalTime) const
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();

	if(NULL == currentDecoderState)
		return false;

	SInt64 currentFrame		= (-1 == currentDecoderState->mFrameToSeek ? currentDecoderState->mFramesRendered : currentDecoderState->mFrameToSeek);
	SInt64 totalFrames		= currentDecoderState->mTotalFrames;
	Float64 sampleRate		= currentDecoderState->mDecoder->GetFormat().mSampleRate;
	currentTime				= currentFrame / sampleRate;
	totalTime				= totalFrames / sampleRate;

	return true;
}

bool AudioPlayer::GetPlaybackPositionAndTime(SInt64& currentFrame, SInt64& totalFrames, CFTimeInterval& currentTime, CFTimeInterval& totalTime) const
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();

	if(NULL == currentDecoderState)
		return false;

	currentFrame		= (-1 == currentDecoderState->mFrameToSeek ? currentDecoderState->mFramesRendered : currentDecoderState->mFrameToSeek);
	totalFrames			= currentDecoderState->mTotalFrames;
	Float64 sampleRate	= currentDecoderState->mDecoder->GetFormat().mSampleRate;
	currentTime			= currentFrame / sampleRate;
	totalTime			= totalFrames / sampleRate;

	return true;	
}

#pragma mark Seeking

bool AudioPlayer::SeekForward(CFTimeInterval secondsToSkip)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;

	SInt64 frameCount		= static_cast<SInt64>(secondsToSkip * currentDecoderState->mDecoder->GetFormat().mSampleRate);
	SInt64 currentFrame		= (-1 == currentDecoderState->mFrameToSeek ? currentDecoderState->mFramesRendered : currentDecoderState->mFrameToSeek);
	SInt64 desiredFrame		= currentFrame + frameCount;
	SInt64 totalFrames		= currentDecoderState->mTotalFrames;
	
	return SeekToFrame(std::min(desiredFrame, totalFrames - 1));
}

bool AudioPlayer::SeekBackward(CFTimeInterval secondsToSkip)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;

	SInt64 frameCount		= static_cast<SInt64>(secondsToSkip * currentDecoderState->mDecoder->GetFormat().mSampleRate);	
	SInt64 currentFrame		= (-1 == currentDecoderState->mFrameToSeek ? currentDecoderState->mFramesRendered : currentDecoderState->mFrameToSeek);
	SInt64 desiredFrame		= currentFrame - frameCount;
	
	return SeekToFrame(std::max(0LL, desiredFrame));
}

bool AudioPlayer::SeekToTime(CFTimeInterval timeInSeconds)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;
	
	SInt64 desiredFrame		= static_cast<SInt64>(timeInSeconds * currentDecoderState->mDecoder->GetFormat().mSampleRate);	
	SInt64 totalFrames		= currentDecoderState->mTotalFrames;
	
	return SeekToFrame(std::max(0LL, std::min(desiredFrame, totalFrames - 1)));
}

bool AudioPlayer::SeekToFrame(SInt64 frame)
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;
	
	if(!currentDecoderState->mDecoder->SupportsSeeking())
		return false;
	
	if(0 > frame || frame >= currentDecoderState->mTotalFrames)
		return false;

	if(!OSAtomicCompareAndSwap64Barrier(currentDecoderState->mFrameToSeek, frame, &currentDecoderState->mFrameToSeek))
		return false;

	mDecoderSemaphore.Signal();

	return true;	
}

bool AudioPlayer::SupportsSeeking() const
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();
	
	if(NULL == currentDecoderState)
		return false;
	
	return currentDecoderState->mDecoder->SupportsSeeking();
}

#pragma mark Player Parameters

bool AudioPlayer::GetVolume(Float32& volume) const
{
	return GetVolumeForChannel(0, volume);
}

bool AudioPlayer::SetVolume(Float32 volume)
{
	return SetVolumeForChannel(0, volume);
}

bool AudioPlayer::GetVolumeForChannel(UInt32 channel, Float32& volume) const
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mOutputNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_WARN(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}

	result = AudioUnitGetParameter(au, kHALOutputParam_Volume, kAudioUnitScope_Global, channel, &volume);
	if(noErr != result) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_WARN(logger, "AudioUnitGetParameter (kHALOutputParam_Volume, kAudioUnitScope_Global, " << channel << ") failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::SetVolumeForChannel(UInt32 channel, Float32 volume)
{
	if(0 > volume || 1 < volume)
		return false;

	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mOutputNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_WARN(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}

	result = AudioUnitSetParameter(au, kHALOutputParam_Volume, kAudioUnitScope_Global, channel, volume, 0);
	if(noErr != result) {
		log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");
		LOG4CXX_WARN(logger, "AudioUnitSetParameter (kHALOutputParam_Volume, kAudioUnitScope_Global, " << channel << ") failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::GetPreGain(Float32& preGain) const
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mMixerNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}
	
	result = AudioUnitGetParameter(au, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, &preGain);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioUnitGetParameter (kMultiChannelMixerParam_Volume, kAudioUnitScope_Input) failed: " << result);
		return false;
	}
	
	return true;
}

bool AudioPlayer::SetPreGain(Float32 preGain)
{
	if(0 > preGain || 1 < preGain)
		return false;

	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mMixerNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}

	result = AudioUnitSetParameter(au, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, preGain, 0);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioUnitSetParameter (kMultiChannelMixerParam_Volume, kAudioUnitScope_Input) failed: " << result);
		return false;
	}

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Digital pregain set to " << preGain);

	return true;
}

#if !TARGET_OS_IPHONE

#pragma mark Hog Mode

bool AudioPlayer::OutputDeviceIsHogged() const
{
	// Is it hogged by us?
	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyHogMode, 
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster 
	};

	pid_t hogPID = static_cast<pid_t>(-1);
	UInt32 dataSize = sizeof(hogPID);

	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &hogPID);
	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_WARN(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyHogMode) failed: " << result);
		return false;
	}

	return (hogPID == getpid() ? true : false);
}

bool AudioPlayer::StartHoggingOutputDevice()
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Taking hog mode for device 0x" << std::hex << deviceID);

	// Is it hogged already?
	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyHogMode, 
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster 
	};

	pid_t hogPID = static_cast<pid_t>(-1);
	UInt32 dataSize = sizeof(hogPID);

	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &hogPID);
	if(kAudioHardwareNoError != result) {
		LOG4CXX_WARN(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyHogMode) failed: " << result);
		return false;
	}

	// The device is already hogged
	if(hogPID != static_cast<pid_t>(-1)) {
		LOG4CXX_DEBUG(logger, "Device is already hogged by pid: " << hogPID);
		return false;
	}

	bool restartIO = OutputIsRunning();
	if(restartIO)
		StopOutput();

	hogPID = getpid();

	result = AudioObjectSetPropertyData(deviceID, &propertyAddress, 0, NULL, sizeof(hogPID), &hogPID);
	if(kAudioHardwareNoError != result) {
		LOG4CXX_WARN(logger, "AudioObjectSetPropertyData (kAudioDevicePropertyHogMode) failed: " << result);
		return false;
	}

	// If IO was enabled before, re-enable it
	if(restartIO && !OutputIsRunning())
		StartOutput();

	return true;
}

bool AudioPlayer::StopHoggingOutputDevice()
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Releasing hog mode for device 0x" << std::hex << deviceID);

	// Is it hogged by us?
	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyHogMode, 
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster 
	};

	pid_t hogPID = static_cast<pid_t>(-1);
	UInt32 dataSize = sizeof(hogPID);

	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &hogPID);
	if(kAudioHardwareNoError != result) {
		LOG4CXX_WARN(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyHogMode) failed: " << result);
		return false;
	}

	// If we don't own hog mode we can't release it
	if(hogPID != getpid())
		return false;

	bool restartIO = OutputIsRunning();
	if(restartIO)
		StopOutput();

	// Release hog mode.
	hogPID = static_cast<pid_t>(-1);

	result = AudioObjectSetPropertyData(deviceID, &propertyAddress, 0, NULL, sizeof(hogPID), &hogPID);
	if(kAudioHardwareNoError != result) {
		LOG4CXX_WARN(logger, "AudioObjectSetPropertyData (kAudioDevicePropertyHogMode) failed: " << result);
		return false;
	}

	if(restartIO && !OutputIsRunning())
		StartOutput();

	return true;
}

#pragma mark Device Parameters

bool AudioPlayer::GetDeviceMasterVolume(Float32& volume) const
{
	return GetDeviceVolumeForChannel(kAudioObjectPropertyElementMaster, volume);
}

bool AudioPlayer::SetDeviceMasterVolume(Float32 volume)
{
	return SetDeviceVolumeForChannel(kAudioObjectPropertyElementMaster, volume);
}

bool AudioPlayer::GetDeviceVolumeForChannel(UInt32 channel, Float32& volume) const
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyVolumeScalar, 
		kAudioDevicePropertyScopeOutput,
		channel 
	};

	if(!AudioObjectHasProperty(deviceID, &propertyAddress)) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_WARN(logger, "AudioObjectHasProperty (kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, " << channel << ") is false");
		return false;
	}

	UInt32 dataSize = sizeof(volume);
	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &volume);

	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_WARN(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, " << channel << ") failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::SetDeviceVolumeForChannel(UInt32 channel, Float32 volume)
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Setting output device 0x" << std::hex << deviceID << " channel " << channel << " volume to " << volume);

	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyVolumeScalar, 
		kAudioDevicePropertyScopeOutput,
		channel 
	};

	if(!AudioObjectHasProperty(deviceID, &propertyAddress)) {
		LOG4CXX_WARN(logger, "AudioObjectHasProperty (kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, " << channel << ") is false");
		return false;
	}

	OSStatus result = AudioObjectSetPropertyData(deviceID, &propertyAddress, 0, NULL, sizeof(volume), &volume);

	if(kAudioHardwareNoError != result) {
		LOG4CXX_WARN(logger, "AudioObjectSetPropertyData (kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, " << channel << ") failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::GetDevicePreferredStereoChannels(std::pair<UInt32, UInt32>& preferredStereoChannels) const
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyPreferredChannelsForStereo, 
		kAudioDevicePropertyScopeOutput,
		kAudioObjectPropertyElementMaster 
	};

	if(!AudioObjectHasProperty(deviceID, &propertyAddress)) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_WARN(logger, "AudioObjectHasProperty (kAudioDevicePropertyPreferredChannelsForStereo, kAudioDevicePropertyScopeOutput) failed is false");
		return false;
	}

	UInt32 preferredChannels [2];
	UInt32 dataSize = sizeof(preferredChannels);
	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &preferredChannels);

	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_WARN(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyPreferredChannelsForStereo, kAudioDevicePropertyScopeOutput) failed: " << result);
		return false;
	}

	preferredStereoChannels.first = preferredChannels[0];
	preferredStereoChannels.second = preferredChannels[1];

	return true;
}

#pragma mark DSP Effects

bool AudioPlayer::AddEffect(OSType subType, OSType manufacturer, UInt32 flags, UInt32 mask, AudioUnit *effectUnit1)
{
	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Adding DSP effect: " << subType << " " << manufacturer);

	// Get the source node for the graph's output node
	UInt32 numInteractions = 0;
	OSStatus result = AUGraphCountNodeInteractions(mAUGraph, mOutputNode, &numInteractions);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphCountNodeInteractions failed: " << result);
		return false;
	}

	AUNodeInteraction interactions [numInteractions];

	result = AUGraphGetNodeInteractions(mAUGraph, mOutputNode, &numInteractions, interactions);	
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphGetNodeInteractions failed: " << result);
		return false;
	}

	AUNode sourceNode = -1;
	for(UInt32 interactionIndex = 0; interactionIndex < numInteractions; ++interactionIndex) {
		AUNodeInteraction interaction = interactions[interactionIndex];

		if(kAUNodeInteraction_Connection == interaction.nodeInteractionType && mOutputNode == interaction.nodeInteraction.connection.destNode) {
			sourceNode = interaction.nodeInteraction.connection.sourceNode;
			break;
		}
	}						

	// Unable to determine the preceding node, so bail
	if(-1 == sourceNode) {
		LOG4CXX_ERROR(logger, "Unable to determine input node");
		return false;
	}

	// Create the effect node and set its format
	AudioComponentDescription desc = { kAudioUnitType_Effect, subType, manufacturer, flags, mask };

	AUNode effectNode = -1;
	result = AUGraphAddNode(mAUGraph, &desc, &effectNode);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphAddNode failed: " << result);
		return false;
	}

	AudioUnit effectUnit = NULL;
	result = AUGraphNodeInfo(mAUGraph, effectNode, NULL, &effectUnit);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);

		result = AUGraphRemoveNode(mAUGraph, effectNode);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "AUGraphRemoveNode failed: " << result);

		return false;
	}

//	result = AudioUnitSetProperty(effectUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &mRingBufferFormat, sizeof(mRingBufferFormat));
//	if(noErr != result) {
////		ERR("AudioUnitSetProperty(kAudioUnitProperty_StreamFormat) failed: %i", result);
//
//		// If the property couldn't be set (the AU may not support this format), remove the new node
//		result = AUGraphRemoveNode(mAUGraph, effectNode);
//		if(noErr != result)
//			;//			ERR("AUGraphRemoveNode failed: %i", result);
//
//		return false;
//	}
//
//	result = AudioUnitSetProperty(effectUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &mRingBufferFormat, sizeof(mRingBufferFormat));
//	if(noErr != result) {
////		ERR("AudioUnitSetProperty(kAudioUnitProperty_StreamFormat) failed: %i", result);
//
//		// If the property couldn't be set (the AU may not support this format), remove the new node
//		result = AUGraphRemoveNode(mAUGraph, effectNode);
//		if(noErr != result)
//			;			//ERR("AUGraphRemoveNode failed: %i", result);
//
//		return false;
//	}

	// Insert the effect at the end of the graph, before the output node
	result = AUGraphDisconnectNodeInput(mAUGraph, mOutputNode, 0);

	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphDisconnectNodeInput failed: " << result);

		result = AUGraphRemoveNode(mAUGraph, effectNode);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "AUGraphRemoveNode failed: " << result);

		return false;
	}

	// Reconnect the nodes
	result = AUGraphConnectNodeInput(mAUGraph, sourceNode, 0, effectNode, 0);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphConnectNodeInput failed: " << result);
		return false;
	}

	result = AUGraphConnectNodeInput(mAUGraph, effectNode, 0, mOutputNode, 0);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphConnectNodeInput failed: " << result);
		return false;
	}

	result = AUGraphUpdate(mAUGraph, NULL);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphUpdate failed: " << result);

		// If the update failed, restore the previous node state
		result = AUGraphConnectNodeInput(mAUGraph, sourceNode, 0, mOutputNode, 0);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphConnectNodeInput failed: " << result);
			return false;
		}
	}

	if(NULL != effectUnit1)
		*effectUnit1 = effectUnit;

	return true;
}

bool AudioPlayer::RemoveEffect(AudioUnit effectUnit)
{
	if(NULL == effectUnit)
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Removing DSP effect: " << effectUnit);

	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphGetNodeCount failed: " << result);
		return false;
	}

	AUNode effectNode = -1;
	for(UInt32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
		AUNode node = -1;
		result = AUGraphGetIndNode(mAUGraph, nodeIndex, &node);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphGetIndNode failed: " << result);
			return false;
		}

		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
			return false;
		}

		// This is the unit to remove
		if(effectUnit == au) {
			effectNode = node;
			break;
		}
	}

	if(-1 == effectNode) {
		LOG4CXX_ERROR(logger, "Unable to find the AUNode for the specified AudioUnit");
		return false;
	}

	// Get the current input and output nodes for the node to delete
	UInt32 numInteractions = 0;
	result = AUGraphCountNodeInteractions(mAUGraph, effectNode, &numInteractions);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphCountNodeInteractions failed: " << result);
		return false;
	}

	AUNodeInteraction *interactions = static_cast<AUNodeInteraction *>(calloc(numInteractions, sizeof(AUNodeInteraction)));
	if(NULL == interactions) {
		LOG4CXX_ERROR(logger, "Unable to allocate memory");
		return false;
	}

	result = AUGraphGetNodeInteractions(mAUGraph, effectNode, &numInteractions, interactions);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphGetNodeInteractions failed: " << result);

		free(interactions), interactions = NULL;

		return false;
	}

	AUNode sourceNode = -1, destNode = -1;
	for(UInt32 interactionIndex = 0; interactionIndex < numInteractions; ++interactionIndex) {
		AUNodeInteraction interaction = interactions[interactionIndex];

		if(kAUNodeInteraction_Connection == interaction.nodeInteractionType) {
			if(effectNode == interaction.nodeInteraction.connection.destNode)
				sourceNode = interaction.nodeInteraction.connection.sourceNode;
			else if(effectNode == interaction.nodeInteraction.connection.sourceNode)
				destNode = interaction.nodeInteraction.connection.destNode;
		}
	}						

	free(interactions), interactions = NULL;

	if(-1 == sourceNode || -1 == destNode) {
		LOG4CXX_ERROR(logger, "Unable to find the source or destination nodes");
		return false;
	}

	result = AUGraphDisconnectNodeInput(mAUGraph, effectNode, 0);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphDisconnectNodeInput failed: " << result);
		return false;
	}

	result = AUGraphDisconnectNodeInput(mAUGraph, destNode, 0);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphDisconnectNodeInput failed: " << result);
		return false;
	}

	result = AUGraphRemoveNode(mAUGraph, effectNode);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphRemoveNode failed: " << result);
		return false;
	}

	// Reconnect the nodes
	result = AUGraphConnectNodeInput(mAUGraph, sourceNode, 0, destNode, 0);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphConnectNodeInput failed: " << result);
		return false;
	}

	result = AUGraphUpdate(mAUGraph, NULL);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphUpdate failed: " << result);
		return false;
	}

	return true;
}

#pragma mark Device Management

bool AudioPlayer::CreateOutputDeviceUID(CFStringRef& deviceUID) const
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyDeviceUID, 
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster 
	};

	UInt32 dataSize = sizeof(deviceUID);
	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &deviceUID);
	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyDeviceUID) failed: " << result);
		return NULL;
	}

	return true;
}

bool AudioPlayer::SetOutputDeviceUID(CFStringRef deviceUID)
{
	AudioDeviceID deviceID = kAudioDeviceUnknown;

	// If NULL was passed as the device UID, use the default output device
	if(NULL == deviceUID) {
		AudioObjectPropertyAddress propertyAddress = { 
			kAudioHardwarePropertyDefaultOutputDevice, 
			kAudioObjectPropertyScopeGlobal, 
			kAudioObjectPropertyElementMaster 
		};

		UInt32 specifierSize = sizeof(deviceID);

		OSStatus result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &specifierSize, &deviceID);
		if(kAudioHardwareNoError != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AudioObjectGetPropertyData (kAudioHardwarePropertyDefaultOutputDevice) failed: " << result);
			return false;
		}
	}
	else {
		AudioObjectPropertyAddress propertyAddress = { 
			kAudioHardwarePropertyDeviceForUID, 
			kAudioObjectPropertyScopeGlobal, 
			kAudioObjectPropertyElementMaster 
		};

		AudioValueTranslation translation = {
			&deviceUID, sizeof(deviceUID),
			&deviceID, sizeof(deviceID)
		};

		UInt32 specifierSize = sizeof(translation);

		OSStatus result = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &specifierSize, &translation);
		if(kAudioHardwareNoError != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AudioObjectGetPropertyData (kAudioHardwarePropertyDeviceForUID) failed: " << result);
			return false;
		}
	}

	// The device isn't connected or doesn't exist
	if(kAudioDeviceUnknown == deviceID)
		return false;

	return SetOutputDeviceID(deviceID);
}

bool AudioPlayer::GetOutputDeviceID(AudioDeviceID& deviceID) const
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mOutputNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}

	UInt32 dataSize = sizeof(deviceID);

	result = AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceID, &dataSize);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioOutputUnitProperty_CurrentDevice) failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::SetOutputDeviceID(AudioDeviceID deviceID)
{
	if(kAudioDeviceUnknown == deviceID)
		return false;

	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mOutputNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}

	// Update our output AU to use the specified device
	result = AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceID, sizeof(deviceID));
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioUnitSetProperty (kAudioOutputUnitProperty_CurrentDevice) failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::GetOutputDeviceSampleRate(Float64& sampleRate) const
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyNominalSampleRate, 
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster 
	};

	UInt32 dataSize = sizeof(sampleRate);
	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &sampleRate);
	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyNominalSampleRate) failed: " << result);
		return false;
	}

	return true;
}

bool AudioPlayer::SetOutputDeviceSampleRate(Float64 sampleRate)
{
	AudioDeviceID deviceID;
	if(!GetOutputDeviceID(deviceID))
		return false;

	// Determine if this will actually be a change
	AudioObjectPropertyAddress propertyAddress = { 
		kAudioDevicePropertyNominalSampleRate, 
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster 
	};

	Float64 currentSampleRate;
	UInt32 dataSize = sizeof(currentSampleRate);
	
	OSStatus result = AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, NULL, &dataSize, &currentSampleRate);
	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioObjectGetPropertyData (kAudioDevicePropertyNominalSampleRate) failed: " << result);
		return false;
	}

	// Nothing to do
	if(currentSampleRate == sampleRate)
		return true;

	// Set the sample rate
	dataSize = sizeof(sampleRate);
	result = AudioObjectSetPropertyData(deviceID, &propertyAddress, 0, NULL, sizeof(sampleRate), &sampleRate);
	if(kAudioHardwareNoError != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioObjectSetPropertyData (kAudioDevicePropertyNominalSampleRate) failed: " << result);
		return false;
	}

	return true;
}

#endif

#pragma mark Playlist Management

bool AudioPlayer::Enqueue(CFURLRef url)
{
	if(NULL == url)
		return false;
	
	AudioDecoder *decoder = AudioDecoder::CreateDecoderForURL(url);
	
	if(NULL == decoder)
		return false;
	
	bool success = Enqueue(decoder);
	
	if(!success)
		delete decoder;
	
	return success;
}

bool AudioPlayer::Enqueue(AudioDecoder *decoder)
{
	if(NULL == decoder)
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Enqueuing \"" << decoder->GetURL() << "\"");

	// The lock is held for the entire method, because enqueuing a track is an inherently
	// sequential operation.  Without the lock, if Enqueue() is called from multiple
	// threads a crash can occur in mRingBuffer->Allocate() under a sitation similar to the following:
	//  1. Thread A calls Enqueue() for decoder A
	//  2. Thread B calls Enqueue() for decoder B
	//  3. Both threads enter the if(NULL == GetCurrentDecoderState() && queueEmpty) block
	//  4. Thread A is suspended
	//  5. Thread B finishes the ring buffer setup, and signals the decoding thread
	//  6. The decoding thread starts decoding
	//  7. Thread A is awakened, and immediately allocates a new ring buffer
	//  8. The decoding or rendering threads crash, because the memory they are using was freed out
	//     from underneath them
	// In practce, the only time I've seen this happen is when using GuardMalloc, presumably because the 
	// normal execution time of Enqueue() isn't sufficient to lead to this condition.
	Mutex::Locker lock(mGuard);

	bool queueEmpty = (0 == CFArrayGetCount(mDecoderQueue));		

	// If there are no decoders in the queue, set up for playback
	if(NULL == GetCurrentDecoderState() && queueEmpty) {
		if(mRingBufferChannelLayout)
			free(mRingBufferChannelLayout), mRingBufferChannelLayout = NULL;

		// Open the decoder if necessary
		CFErrorRef error = NULL;
		if(!decoder->IsOpen() && !decoder->Open(&error)) {
			if(error) {
				LOG4CXX_ERROR(logger, "Error opening decoder: " << error);
				CFRelease(error), error = NULL;
			}

			return false;
		}

		AudioStreamBasicDescription format = decoder->GetFormat();
		if(!SetAUGraphSampleRateAndChannelsPerFrame(format.mSampleRate, format.mChannelsPerFrame))
			return false;

		AudioChannelLayout *channelLayout = decoder->GetChannelLayout();

		// Assign a default channel layout if the decoder has an unknown layout
		bool allocatedChannelLayout = false;
		if(NULL == channelLayout)
			channelLayout = CreateDefaultAudioChannelLayout(mRingBufferFormat.mChannelsPerFrame);
		
		bool success = SetAUGraphChannelLayout(channelLayout);
		
		if(allocatedChannelLayout)
			free(channelLayout), channelLayout = NULL;

		if(!success)
			return false;

		// Allocate enough space in the ring buffer for the new format
		mRingBuffer->Allocate(mRingBufferFormat.mChannelsPerFrame, mRingBufferFormat.mBytesPerFrame, mRingBufferCapacity);
	}
	// Otherwise, enqueue this decoder if the format matches
	else if(decoder->IsOpen()) {
		AudioStreamBasicDescription		nextFormat			= decoder->GetFormat();
		AudioChannelLayout				*nextChannelLayout	= decoder->GetChannelLayout();
		
		// The two files can be joined seamlessly only if they have the same sample rates and channel counts
		if(nextFormat.mSampleRate != mRingBufferFormat.mSampleRate) {
			LOG4CXX_WARN(logger, "Enqueue failed: Ring buffer sample rate (" << mRingBufferFormat.mSampleRate << " Hz) and decoder sample rate (" << nextFormat.mSampleRate << " Hz) don't match");
			return false;
		}
		else if(nextFormat.mChannelsPerFrame != mRingBufferFormat.mChannelsPerFrame) {
			LOG4CXX_WARN(logger, "Enqueue failed: Ring buffer channel count (" << mRingBufferFormat.mChannelsPerFrame << ") and decoder channel count (" << nextFormat.mChannelsPerFrame << ") don't match");
			return false;
		}

		// If the decoder has an explicit channel layout, enqueue it if it matches the ring buffer's channel layout
		if(NULL != nextChannelLayout && !ChannelLayoutsAreEqual(nextChannelLayout, mRingBufferChannelLayout)) {
			LOG4CXX_WARN(logger, "Enqueue failed: Ring buffer channel layout (" << mRingBufferChannelLayout << ") and decoder channel layout (" << nextChannelLayout << ") don't match");
			return false;
		}
		// If the decoder doesn't have an explicit channel layout, enqueue it if the default layout matches
		else if(NULL == nextChannelLayout) {
			AudioChannelLayout *defaultLayout = CreateDefaultAudioChannelLayout(nextFormat.mChannelsPerFrame);
			bool layoutsMatch = ChannelLayoutsAreEqual(defaultLayout, mRingBufferChannelLayout);
			free(defaultLayout), defaultLayout = NULL;

			if(!layoutsMatch) {
				LOG4CXX_WARN(logger, "Enqueue failed: Decoder has no channel layout and ring buffer channel layout (" << mRingBufferChannelLayout << ") isn't the default for " << nextFormat.mChannelsPerFrame << " channels");
				return false;
			}
		}
	}
	// If the decoder isn't open the format isn't yet known.  Enqueue it and hope things work out for the best
	
	// Add the decoder to the queue
	CFArrayAppendValue(mDecoderQueue, decoder);

	mDecoderSemaphore.Signal();
	
	return true;
}

bool AudioPlayer::SkipToNextTrack()
{
	DecoderStateData *currentDecoderState = GetCurrentDecoderState();

	if(NULL == currentDecoderState)
		return false;

	OSAtomicTestAndSetBarrier(6 /* eAudioPlayerFlagMuteOutput */, &mFlags);

	OSAtomicTestAndSetBarrier(3 /* eDecoderStateDataFlagStopDecoding */, &currentDecoderState->mFlags);

	// Signal the decoding thread that decoding is finished (inner loop)
	mDecoderSemaphore.Signal();

	// Wait for decoding to finish or a SIGSEGV could occur if the collector collects an active decoder
	while(!(eDecoderStateDataFlagDecodingFinished & currentDecoderState->mFlags)) {
		int result = usleep(SLEEP_TIME_USEC);
		if(0 != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_WARN(logger, "Couldn't wait for decoding to finish: " << strerror(errno));
		}
	}

	OSAtomicTestAndSetBarrier(4 /* eDecoderStateDataFlagRenderingFinished */, &currentDecoderState->mFlags);

	// Effect a flush of the ring buffer
	mFramesDecoded = 0;
	mFramesRendered = 0;
	
	// Signal the decoding thread to start the next decoder (outer loop)
	mDecoderSemaphore.Signal();

	OSAtomicTestAndClearBarrier(6 /* eAudioPlayerFlagMuteOutput */, &mFlags);

	return true;
}

bool AudioPlayer::ClearQueuedDecoders()
{
	Mutex::Tryer lock(mGuard);
	if(!lock)
		return false;

	while(0 < CFArrayGetCount(mDecoderQueue)) {
		AudioDecoder *decoder = static_cast<AudioDecoder *>(const_cast<void *>(CFArrayGetValueAtIndex(mDecoderQueue, 0)));
		CFArrayRemoveValueAtIndex(mDecoderQueue, 0);
		delete decoder;
	}

	return true;	
}

#pragma mark Ring Buffer Parameters

bool AudioPlayer::SetRingBufferCapacity(uint32_t bufferCapacity)
{
	if(0 == bufferCapacity || mRingBufferWriteChunkSize > bufferCapacity)
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Setting ring buffer capacity to " << bufferCapacity);

	return OSAtomicCompareAndSwap32Barrier(mRingBufferCapacity, bufferCapacity, reinterpret_cast<int32_t *>(&mRingBufferCapacity));
}

bool AudioPlayer::SetRingBufferWriteChunkSize(uint32_t chunkSize)
{
	if(0 == chunkSize || mRingBufferCapacity < chunkSize)
		return false;

	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_DEBUG(logger, "Setting ring buffer write chunk size to " << chunkSize);

	return OSAtomicCompareAndSwap32Barrier(mRingBufferWriteChunkSize, chunkSize, reinterpret_cast<int32_t *>(&mRingBufferWriteChunkSize));
}

#pragma mark Callbacks

OSStatus AudioPlayer::Render(AudioUnitRenderActionFlags		*ioActionFlags,
								const AudioTimeStamp			*inTimeStamp,
								UInt32							inBusNumber,
								UInt32							inNumberFrames,
								AudioBufferList					*ioData)
{

#pragma unused(inTimeStamp)
#pragma unused(inBusNumber)

	assert(NULL != ioActionFlags);
	assert(NULL != ioData);

	// Mute functionality
	if(eAudioPlayerFlagMuteOutput & mFlags)
		return noErr;

	// If the ring buffer doesn't contain any valid audio, skip some work
	UInt32 framesAvailableToRead = static_cast<UInt32>(mFramesDecoded - mFramesRendered);
	if(0 == framesAvailableToRead) {
		*ioActionFlags |= kAudioUnitRenderAction_OutputIsSilence;
		
		size_t byteCountToZero = inNumberFrames * sizeof(float);
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex) {
			memset(ioData->mBuffers[bufferIndex].mData, 0, byteCountToZero);
			ioData->mBuffers[bufferIndex].mDataByteSize = static_cast<UInt32>(byteCountToZero);
		}
		
		return noErr;
	}

	// Restrict reads to valid decoded audio
	UInt32 framesToRead = std::min(framesAvailableToRead, inNumberFrames);
	CARingBufferError result = mRingBuffer->Fetch(ioData, framesToRead, mFramesRendered);
	if(kCARingBufferError_OK != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "CARingBuffer::Fetch failed: " << result << ", requested " << framesToRead << " frames from " << mFramesRendered);
		return 1;
	}

	mFramesRenderedLastPass = framesToRead;
	OSAtomicAdd64Barrier(framesToRead, &mFramesRendered);

	// If the ring buffer didn't contain as many frames as were requested, fill the remainder with silence
	if(framesToRead != inNumberFrames) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_WARN(logger, "Insufficient audio in ring buffer: " << framesToRead << " frames available, " << inNumberFrames << " requested");
		
		UInt32 framesOfSilence = inNumberFrames - framesToRead;
		size_t byteCountToZero = framesOfSilence * sizeof(float);
		for(UInt32 bufferIndex = 0; bufferIndex < ioData->mNumberBuffers; ++bufferIndex) {
			float *bufferAlias = static_cast<float *>(ioData->mBuffers[bufferIndex].mData);
			memset(bufferAlias + framesToRead, 0, byteCountToZero);
			ioData->mBuffers[bufferIndex].mDataByteSize += static_cast<UInt32>(byteCountToZero);
		}
	}

	// If there is adequate space in the ring buffer for another chunk, signal the reader thread
	UInt32 framesAvailableToWrite = static_cast<UInt32>(mRingBuffer->GetCapacityFrames() - (mFramesDecoded - mFramesRendered));
	
	if(mRingBufferWriteChunkSize <= framesAvailableToWrite)
		mDecoderSemaphore.Signal();

	return noErr;
}

OSStatus AudioPlayer::DidRender(AudioUnitRenderActionFlags		*ioActionFlags,
								   const AudioTimeStamp				*inTimeStamp,
								   UInt32							inBusNumber,
								   UInt32							inNumberFrames,
								   AudioBufferList					*ioData)
{
	
#pragma unused(inTimeStamp)
#pragma unused(inBusNumber)
#pragma unused(inNumberFrames)
#pragma unused(ioData)
	
	if(kAudioUnitRenderAction_PostRender & (*ioActionFlags)) {

		// There is nothing more to do if no frames were rendered
		if(0 == mFramesRenderedLastPass)
			return noErr;

		// mFramesRenderedLastPass contains the number of valid frames that were rendered
		// However, these could have come from any number of decoders depending on the buffer sizes
		// So it is necessary to split them up here

		SInt64 framesRemainingToDistribute = mFramesRenderedLastPass;
		DecoderStateData *decoderState = GetCurrentDecoderState();

		// mActiveDecoders is not an ordered array, so to ensure that callbacks are performed
		// in the proper order multiple passes are made here
		while(NULL != decoderState) {
			SInt64 timeStamp = decoderState->mTimeStamp;

			SInt64 decoderFramesRemaining = (-1 == decoderState->mTotalFrames ? mFramesRenderedLastPass : decoderState->mTotalFrames - decoderState->mFramesRendered);
			SInt64 framesFromThisDecoder = std::min(decoderFramesRemaining, static_cast<SInt64>(mFramesRenderedLastPass));

			if(0 == decoderState->mFramesRendered && !(eDecoderStateDataFlagRenderingStarted & decoderState->mFlags)) {
				decoderState->mDecoder->PerformRenderingStartedCallback();
				OSAtomicTestAndSetBarrier(5 /* eDecoderStateDataFlagRenderingStarted */, &decoderState->mFlags);
			}

			OSAtomicAdd64Barrier(framesFromThisDecoder, &decoderState->mFramesRendered);

			if((eDecoderStateDataFlagDecodingFinished & decoderState->mFlags) && decoderState->mFramesRendered == decoderState->mTotalFrames/* && !(eDecoderStateDataFlagRenderingFinished & decoderState->mFlags)*/) {
				decoderState->mDecoder->PerformRenderingFinishedCallback();			

				OSAtomicTestAndSetBarrier(4 /* eDecoderStateDataFlagRenderingFinished */, &decoderState->mFlags);
				decoderState = NULL;

				// Since rendering is finished, signal the collector to clean up this decoder
				mCollectorSemaphore.Signal();
			}

			framesRemainingToDistribute -= framesFromThisDecoder;

			if(0 == framesRemainingToDistribute)
				break;

			decoderState = GetDecoderStateStartingAfterTimeStamp(timeStamp);
		}
	}

	return noErr;
}

#pragma mark Thread Entry Points

void * AudioPlayer::DecoderThreadEntry()
{
	log4cxx::NDC::push("Decoding Thread");
	log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");

	// ========================================
	// Make ourselves a high priority thread
	if(!setThreadPolicy(DECODER_THREAD_IMPORTANCE))
		LOG4CXX_WARN(logger, "Couldn't set decoder thread importance");
	
	// Two seconds and zero nanoseconds
	mach_timespec_t timeout = { 2, 0 };

	while(mKeepDecoding) {

		// ========================================
		// Try to lock the queue and remove the head element, which contains the next decoder to use
		DecoderStateData *decoderState = NULL;
		{
			Mutex::Tryer lock(mGuard);

			if(lock && 0 < CFArrayGetCount(mDecoderQueue)) {
				AudioDecoder *decoder = (AudioDecoder *)CFArrayGetValueAtIndex(mDecoderQueue, 0);

				// Create the decoder state
				decoderState = new DecoderStateData(decoder);
				decoderState->mTimeStamp = mFramesDecoded;

				CFArrayRemoveValueAtIndex(mDecoderQueue, 0);
			}
		}

		// ========================================
		// Open the decoder if necessary
		if(decoderState) {
			CFErrorRef error = NULL;
			if(!decoderState->mDecoder->IsOpen() && !decoderState->mDecoder->Open(&error))  {
				if(error) {
					LOG4CXX_ERROR(logger, "Error opening decoder: " << error);
					CFRelease(error), error = NULL;
				}

				// TODO: Perform CouldNotOpenDecoder() callback ??

				delete decoderState, decoderState = NULL;
			}
		}

		// ========================================
		// Ensure the decoder's format is compatible with the ring buffer
		if(decoderState) {
			AudioStreamBasicDescription		nextFormat			= decoderState->mDecoder->GetFormat();
			AudioChannelLayout				*nextChannelLayout	= decoderState->mDecoder->GetChannelLayout();

			// The two files can be joined seamlessly only if they have the same sample rates and channel counts
			bool formatsMatch = true;

			if(nextFormat.mSampleRate != mRingBufferFormat.mSampleRate) {
				LOG4CXX_WARN(logger, "Gapless join failed: Ring buffer sample rate (" << mRingBufferFormat.mSampleRate << " Hz) and decoder sample rate (" << nextFormat.mSampleRate << " Hz) don't match");
				formatsMatch = false;
			}
			else if(nextFormat.mChannelsPerFrame != mRingBufferFormat.mChannelsPerFrame) {
				LOG4CXX_WARN(logger, "Gapless join failed: Ring buffer channel count (" << mRingBufferFormat.mChannelsPerFrame << ") and decoder channel count (" << nextFormat.mChannelsPerFrame << ") don't match");
				formatsMatch = false;
			}

			// If the decoder has an explicit channel layout, enqueue it if it matches the ring buffer's channel layout
			if(nextChannelLayout && !ChannelLayoutsAreEqual(nextChannelLayout, mRingBufferChannelLayout)) {
				LOG4CXX_WARN(logger, "Gapless join failed: Ring buffer channel layout (" << mRingBufferChannelLayout << ") and decoder channel layout (" << nextChannelLayout << ") don't match");
				formatsMatch = false;
			}
			// If the decoder doesn't have an explicit channel layout, enqueue it if the default layout matches
			else if(NULL == nextChannelLayout) {
				AudioChannelLayout *defaultLayout = CreateDefaultAudioChannelLayout(nextFormat.mChannelsPerFrame);
				bool layoutsMatch = ChannelLayoutsAreEqual(defaultLayout, mRingBufferChannelLayout);
				free(defaultLayout), defaultLayout = NULL;

				if(!layoutsMatch) {
					LOG4CXX_WARN(logger, "Gapless join failed: Decoder has no channel layout and ring buffer channel layout (" << mRingBufferChannelLayout << ") isn't the default for " << nextFormat.mChannelsPerFrame << " channels");
					formatsMatch = false;
				}
			}

			// If the formats don't match, the decoder can't be used with the current ring buffer format
			if(!formatsMatch)
				delete decoderState, decoderState = NULL;
		}

		// ========================================
		// Append the decoder state to the list of active decoders
		if(decoderState) {
			for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
				if(NULL != mActiveDecoders[bufferIndex])
					continue;
				
				if(OSAtomicCompareAndSwapPtrBarrier(NULL, decoderState, reinterpret_cast<void **>(&mActiveDecoders[bufferIndex])))
					break;
				else
					LOG4CXX_WARN(logger, "OSAtomicCompareAndSwapPtrBarrier() failed");
			}
		}
		
		// ========================================
		// If a decoder was found at the head of the queue, process it
		if(decoderState) {
			AudioDecoder *decoder = decoderState->mDecoder;

			LOG4CXX_DEBUG(logger, "Decoding starting for \"" << decoder->GetURL() << "\"");
			LOG4CXX_DEBUG(logger, "Decoder format: " << decoder->GetFormat());
			LOG4CXX_DEBUG(logger, "Decoder channel layout: " << decoder->GetChannelLayout());
			
			SInt64 startTime = decoderState->mTimeStamp;

			AudioStreamBasicDescription decoderFormat = decoder->GetFormat();

			// ========================================
			// Create the AudioConverter which will convert from the decoder's format to the graph's format
			AudioConverterRef audioConverter = NULL;
			OSStatus result = AudioConverterNew(&decoderFormat, &mRingBufferFormat, &audioConverter);
			if(noErr != result) {
				LOG4CXX_ERROR(logger, "AudioConverterNew failed: " << result);

				// If this happens, output will be impossible
				OSAtomicTestAndSetBarrier(7 /* eDecoderStateDataFlagDecodingFinished */, &decoderState->mFlags);
			}

			// ========================================
			// Allocate the buffer lists which will serve as the transport between the decoder and the ring buffer
			UInt32 inputBufferSize = mRingBufferWriteChunkSize * mRingBufferFormat.mBytesPerFrame;
			UInt32 dataSize = sizeof(inputBufferSize);
			result = AudioConverterGetProperty(audioConverter, kAudioConverterPropertyCalculateInputBufferSize, &dataSize, &inputBufferSize);
			if(noErr != result)
				LOG4CXX_ERROR(logger, "AudioConverterGetProperty (kAudioConverterPropertyCalculateInputBufferSize) failed: " << result);
			
			// ========================================
			// Allocate the buffer lists which will serve as the transport between the decoder and the ring buffer			
			decoderState->AllocateBufferList(inputBufferSize / decoderFormat.mBytesPerFrame);

			AudioBufferList *bufferList = AllocateABL(mRingBufferFormat, mRingBufferWriteChunkSize);

			// ========================================
			// Decode the audio file in the ring buffer until finished or cancelled
			while(mKeepDecoding && decoderState && !(eDecoderStateDataFlagStopDecoding & decoderState->mFlags)) {

				// Fill the ring buffer with as much data as possible
				for(;;) {
					// Determine how many frames are available in the ring buffer
					UInt32 framesAvailableToWrite = static_cast<UInt32>(mRingBuffer->GetCapacityFrames() - (mFramesDecoded - mFramesRendered));

					// Force writes to the ring buffer to be at least mRingBufferWriteChunkSize
					if(mRingBufferWriteChunkSize <= framesAvailableToWrite) {

						// Seek to the specified frame
						if(-1 != decoderState->mFrameToSeek) {
							LOG4CXX_TRACE(logger, "Seeking to frame " << decoderState->mFrameToSeek);

							OSAtomicTestAndSetBarrier(6 /* eAudioPlayerFlagMuteOutput */, &mFlags);
							
							SInt64 currentFrameBeforeSeeking = decoder->GetCurrentFrame();
							
							SInt64 newFrame = decoder->SeekToFrame(decoderState->mFrameToSeek);
							
							if(newFrame != decoderState->mFrameToSeek)
								LOG4CXX_ERROR(logger, "Error seeking to frame  " << decoderState->mFrameToSeek);
							
							// Update the seek request
							if(!OSAtomicCompareAndSwap64Barrier(decoderState->mFrameToSeek, -1, &decoderState->mFrameToSeek))
								LOG4CXX_ERROR(logger, "OSAtomicCompareAndSwap64Barrier() failed ");
							
							// If the seek failed do not update the counters
							if(-1 != newFrame) {
								SInt64 framesSkipped = newFrame - currentFrameBeforeSeeking;
								
								// Treat the skipped frames as if they were rendered, and update the counters accordingly
								if(!OSAtomicCompareAndSwap64Barrier(decoderState->mFramesRendered, newFrame, &decoderState->mFramesRendered))
									LOG4CXX_ERROR(logger, "OSAtomicCompareAndSwap64Barrier() failed ");
								
								OSAtomicAdd64Barrier(framesSkipped, &mFramesDecoded);
								if(!OSAtomicCompareAndSwap64Barrier(mFramesRendered, mFramesDecoded, &mFramesRendered))
									LOG4CXX_ERROR(logger, "OSAtomicCompareAndSwap64Barrier() failed ");

								// Reset the converter and output to flush any buffers
								result = AudioConverterReset(audioConverter);
								if(noErr != result)
									LOG4CXX_ERROR(logger, "AudioConverterReset failed: " << result);

								// If sample rate conversion is being performed, ResetOutput() needs to be called to flush any
								// state the AudioConverter may have.  In the future, if ResetOutput() does anything other than
								// reset the AudioConverter state the if(mSampleRateConverter) will need to be removed
//								if(mSampleRateConverter) {
//									// ResetOutput() is not safe to call when the device is running, because the player
//									// could be in the middle of a render callback
//									if(OutputIsRunning())
//										OSAtomicTestAndSetBarrier(2 /* eAudioPlayerFlagResetNeeded */, &mFlags);
//									// Even if the device isn't running, AudioConverters are not thread-safe
//									else {
//										Mutex::Locker lock(mGuard);
//										ResetOutput();
//									}
//								}
							}

							OSAtomicTestAndClearBarrier(6 /* eAudioPlayerFlagMuteOutput */, &mFlags);
						}

						SInt64 startingFrameNumber = decoder->GetCurrentFrame();

						if(-1 == startingFrameNumber) {
							LOG4CXX_ERROR(logger, "Unable to determine starting frame number ");
							break;
						}

						// If this is the first frame, decoding is just starting
						if(0 == startingFrameNumber && !(eDecoderStateDataFlagDecodingStarted & decoderState->mFlags)) {
							decoder->PerformDecodingStartedCallback();
							OSAtomicTestAndSetBarrier(7 /* eDecoderStateDataFlagDecodingStarted */, &decoderState->mFlags);
						}

						// Read the input chunk, converting from the decoder's format to the AUGraph's format
						UInt32 framesDecoded = mRingBufferWriteChunkSize;
						
						result = AudioConverterFillComplexBuffer(audioConverter, myAudioConverterComplexInputDataProc, decoderState, &framesDecoded, bufferList, NULL);
						if(noErr != result)
							LOG4CXX_ERROR(logger, "AudioConverterFillComplexBuffer failed: " << result);

						// Store the decoded audio
						if(0 != framesDecoded) {

							CARingBufferError result = mRingBuffer->Store(bufferList, framesDecoded, startingFrameNumber + startTime);
							if(kCARingBufferError_OK != result)
								LOG4CXX_ERROR(logger, "CARingBuffer::Store failed: " << result);

							OSAtomicAdd64Barrier(framesDecoded, &mFramesDecoded);
						}
						
						// If no frames were returned, this is the end of stream
						if(0 == framesDecoded/* && !(eDecoderStateDataFlagDecodingFinished & decoderState->mFlags)*/) {
							LOG4CXX_DEBUG(logger, "Decoding finished for \"" << decoder->GetURL() << "\"");

							// Some formats (MP3) may not know the exact number of frames in advance
							// without processing the entire file, which is a potentially slow operation
							// Rather than require preprocessing to ensure an accurate frame count, update 
							// it here so EOS is correctly detected in DidRender()
							decoderState->mTotalFrames = startingFrameNumber;

							decoder->PerformDecodingFinishedCallback();
							
							// Decoding is complete
							OSAtomicTestAndSetBarrier(6 /* eDecoderStateDataFlagDecodingFinished */, &decoderState->mFlags);
							decoderState = NULL;

							break;
						}
					}
					// Not enough space remains in the ring buffer to write an entire decoded chunk
					else
						break;
				}

				// Wait for the audio rendering thread to signal us that it could use more data, or for the timeout to happen
				mDecoderSemaphore.TimedWait(timeout);
			}
			
			// ========================================
			// Clean up
			// Set the appropriate flags for collection if decoding was stopped early
			if(decoderState) {
				OSAtomicTestAndSetBarrier(6 /* eDecoderStateDataFlagDecodingFinished */, &decoderState->mFlags);
				decoderState = NULL;
			}

			if(bufferList)
				bufferList = DeallocateABL(bufferList);
			
			if(audioConverter) {
				result = AudioConverterDispose(audioConverter);
				if(noErr != result)
					LOG4CXX_ERROR(logger, "AudioConverterDispose failed: " << result);
				audioConverter = NULL;
			}
		}

		// Wait for another thread to wake us, or for the timeout to happen
		mDecoderSemaphore.TimedWait(timeout);
	}

	LOG4CXX_DEBUG(logger, "Decoding thread terminating");

	log4cxx::NDC::pop();

	return NULL;
}

void * AudioPlayer::CollectorThreadEntry()
{
	log4cxx::NDC::push("Collecting Thread");
	log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer");

	// The collector should be signaled when there is cleanup to be done, so there is no need for a short timeout
	mach_timespec_t timeout = { 30, 0 };

	while(mKeepCollecting) {
		
		for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
			DecoderStateData *decoderState = mActiveDecoders[bufferIndex];
			
			if(NULL == decoderState)
				continue;

			if(!(eDecoderStateDataFlagDecodingFinished & decoderState->mFlags) || !(eDecoderStateDataFlagRenderingFinished & decoderState->mFlags))
				continue;

			bool swapSucceeded = OSAtomicCompareAndSwapPtrBarrier(decoderState, NULL, reinterpret_cast<void **>(&mActiveDecoders[bufferIndex]));

			if(swapSucceeded)
				delete decoderState, decoderState = NULL;
		}
		
		// Wait for any thread to signal us to try and collect finished decoders
		mCollectorSemaphore.TimedWait(timeout);
	}
	
	LOG4CXX_DEBUG(logger, "Collecting thread terminating");
	
	log4cxx::NDC::pop();

	return NULL;
}

#pragma mark AudioHardware Utilities

bool AudioPlayer::OpenOutput()
{
	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_TRACE(logger, "OpenOutput");

	OSStatus result = NewAUGraph(&mAUGraph);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "NewAUGraph failed: " << result);
		return false;
	}

	// The graph will look like:
	// MultiChannelMixer -> Output
	AudioComponentDescription desc;

	// Set up the mixer node
	desc.componentType			= kAudioUnitType_Mixer;
	desc.componentSubType		= kAudioUnitSubType_MultiChannelMixer;
	desc.componentManufacturer	= kAudioUnitManufacturer_Apple;
	desc.componentFlags			= 0;
	desc.componentFlagsMask		= 0;

	result = AUGraphAddNode(mAUGraph, &desc, &mMixerNode);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphAddNode failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}
	
	// Set up the output node
	desc.componentType			= kAudioUnitType_Output;
#if TARGET_OS_IPHONE
	desc.componentSubType		= kAudioUnitSubType_RemoteIO;
#else
	desc.componentSubType		= kAudioUnitSubType_HALOutput;
#endif
	desc.componentManufacturer	= kAudioUnitManufacturer_Apple;
	desc.componentFlags			= 0;
	desc.componentFlagsMask		= 0;

	result = AUGraphAddNode(mAUGraph, &desc, &mOutputNode);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphAddNode failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}

	result = AUGraphConnectNodeInput(mAUGraph, mMixerNode, 0, mOutputNode, 0);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphConnectNodeInput failed: " << result);
		
		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);
		
		mAUGraph = NULL;
		return false;
	}
	
	// Install the input callback
	AURenderCallbackStruct cbs = { myAURenderCallback, this };
	result = AUGraphSetNodeInputCallback(mAUGraph, mMixerNode, 0, &cbs);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphSetNodeInputCallback failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}
	
	// Open the graph
	result = AUGraphOpen(mAUGraph);	
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphOpen failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}
	
	// Initialize the graph
	result = AUGraphInitialize(mAUGraph);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphInitialize failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}
	
	// Set the mixer's volume on the input and output
	AudioUnit au = NULL;
	result = AUGraphNodeInfo(mAUGraph, mMixerNode, NULL, &au);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}
	
	result = AudioUnitSetParameter(au, kMultiChannelMixerParam_Volume, kAudioUnitScope_Input, 0, 1.f, 0);
	if(noErr != result)
		LOG4CXX_ERROR(logger, "AudioUnitSetParameter (kMultiChannelMixerParam_Volume, kAudioUnitScope_Input) failed: " << result);
	
	result = AudioUnitSetParameter(au, kMultiChannelMixerParam_Volume, kAudioUnitScope_Output, 0, 1.f, 0);
	if(noErr != result)
		LOG4CXX_ERROR(logger, "AudioUnitSetParameter (kMultiChannelMixerParam_Volume, kAudioUnitScope_Output) failed: " << result);
	
	// Install the render notification
	result = AUGraphAddRenderNotify(mAUGraph, auGraphDidRender, this);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphAddRenderNotify failed: " << result);

		result = DisposeAUGraph(mAUGraph);
		if(noErr != result)
			LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);

		mAUGraph = NULL;
		return false;
	}

	return true;
}

bool AudioPlayer::CloseOutput()
{
	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_TRACE(logger, "CloseOutput");

	Boolean graphIsRunning = false;
	OSStatus result = AUGraphIsRunning(mAUGraph, &graphIsRunning);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphIsRunning failed: " << result);
		return false;
	}

	if(graphIsRunning) {
		result = AUGraphStop(mAUGraph);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphStop failed: " << result);
			return false;
		}
	}

	Boolean graphIsInitialized = false;
	result = AUGraphIsInitialized(mAUGraph, &graphIsInitialized);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphIsInitialized failed: " << result);
		return false;
	}

	if(graphIsInitialized) {
		result = AUGraphUninitialize(mAUGraph);		
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphUninitialize failed: " << result);
			return false;
		}
	}

	result = AUGraphClose(mAUGraph);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphClose failed: " << result);
		return false;
	}

	result = DisposeAUGraph(mAUGraph);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "DisposeAUGraph failed: " << result);
		return false;
	}

	mAUGraph = NULL;
	mMixerNode = NULL;
	mOutputNode = NULL;

	return true;
}

bool AudioPlayer::StartOutput()
{
	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_TRACE(logger, "StartOutput");

	// We don't want to start output in the middle of a buffer modification
	Mutex::Locker lock(mGuard);

	OSStatus result = AUGraphStart(mAUGraph);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphStart failed: " << result);
		return false;
	}
	
	return true;
}

bool AudioPlayer::StopOutput()
{
	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_TRACE(logger, "StopOutput");

	OSStatus result = AUGraphStop(mAUGraph);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphStop failed: " << result);
		return false;
	}
	
	return true;
}

bool AudioPlayer::OutputIsRunning() const
{
	Boolean isRunning = false;
	OSStatus result = AUGraphIsRunning(mAUGraph, &isRunning);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphIsRunning failed: " << result);
		return false;
	}

	return isRunning;
}

bool AudioPlayer::ResetOutput()
{
	log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
	LOG4CXX_TRACE(logger, "Resetting output");

	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	if(noErr != result) {
		LOG4CXX_ERROR(logger, "AUGraphIsRunning failed: " << result);
		return false;
	}

	for(UInt32 i = 0; i < nodeCount; ++i) {
		AUNode node = 0;
		result = AUGraphGetIndNode(mAUGraph, i, &node);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphGetIndNode failed: " << result);
			return false;
		}

		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
			return false;
		}

		result = AudioUnitReset(au, kAudioUnitScope_Global, 0);
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AudioUnitReset failed: " << result);
			return false;
		}
	}

	return true;
}

#pragma mark AUGraph Utilities

bool AudioPlayer::GetAUGraphLatency(Float64& latency) const
{
	latency = 0;

	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphGetNodeCount failed: " << result);
		return false;
	}

	for(UInt32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
		AUNode node = 0;
		result = AUGraphGetIndNode(mAUGraph, nodeIndex, &node);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphGetIndNode failed: " << result);
			return false;
		}

		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
			return false;
		}

		Float64 auLatency = 0;
		UInt32 dataSize = sizeof(auLatency);
		result = AudioUnitGetProperty(au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &auLatency, &dataSize);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_Latency, kAudioUnitScope_Global) failed: " << result);
			return false;
		}

		latency += auLatency;
	}

	return true;
}

bool AudioPlayer::GetAUGraphTailTime(Float64& tailTime) const
{
	tailTime = 0;

	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphGetNodeCount failed: " << result);
		return false;
	}

	for(UInt32 nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
		AUNode node = 0;
		result = AUGraphGetIndNode(mAUGraph, nodeIndex, &node);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphGetIndNode failed: " << result);
			return false;
		}

		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
			return false;
		}

		Float64 auTailTime = 0;
		UInt32 dataSize = sizeof(auTailTime);
		result = AudioUnitGetProperty(au, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &auTailTime, &dataSize);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_TailTime, kAudioUnitScope_Global) failed: " << result);
			return false;
		}

		tailTime += auTailTime;
	}

	return true;
}

bool AudioPlayer::SetPropertyOnAUGraphNodes(AudioUnitPropertyID propertyID, const void *propertyData, UInt32 propertyDataSize)
{
	if(NULL == propertyData || 0 >= propertyDataSize)
		return  false;

	UInt32 nodeCount = 0;
	OSStatus result = AUGraphGetNodeCount(mAUGraph, &nodeCount);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphGetNodeCount failed: " << result);
		return false;
	}

	// Iterate through the nodes and attempt to set the property
	for(UInt32 i = 0; i < nodeCount; ++i) {
		AUNode node;
		result = AUGraphGetIndNode(mAUGraph, i, &node);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphGetIndNode failed: " << result);
			return false;
		}

		AudioUnit au = NULL;
		result = AUGraphNodeInfo(mAUGraph, node, NULL, &au);

		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphGetNodeCount failed: " << result);
			return false;
		}

		if(mOutputNode == node) {
			// For AUHAL as the output node, you can't set the device side, so just set the client side
			result = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Input, 0, propertyData, propertyDataSize);
			if(noErr != result) {
				log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
				LOG4CXX_ERROR(logger, "AudioUnitSetProperty (" << propertyID << ", kAudioUnitScope_Input) failed: " << result);
				return false;
			}
		}
		else {
			UInt32 elementCount = 0;
			UInt32 dataSize = sizeof(elementCount);
			result = AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &elementCount, &dataSize);
			if(noErr != result) {
				log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
				LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_ElementCount, kAudioUnitScope_Input) failed: " << result);
				return false;
			}

			for(UInt32 j = 0; j < elementCount; ++j) {
/*				Boolean writable;
				 err = AudioUnitGetPropertyInfo(au, propertyID, kAudioUnitScope_Input, j, &dataSize, &writable);

				 if(noErr != err && kAudioUnitErr_InvalidProperty != err)
				 return err;

				 if(kAudioUnitErr_InvalidProperty == err || !writable)
				 continue;*/

				result = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Input, j, propertyData, propertyDataSize);				
				if(noErr != result) {
					log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
					LOG4CXX_ERROR(logger, "AudioUnitSetProperty (" << propertyID << ", kAudioUnitScope_Input) failed: " << result);
					return false;
				}
			}

			elementCount = 0;
			dataSize = sizeof(elementCount);
			result = AudioUnitGetProperty(au, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &elementCount, &dataSize);			
			if(noErr != result) {
				log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
				LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_ElementCount, kAudioUnitScope_Output) failed: " << result);
				return false;
			}

			for(UInt32 j = 0; j < elementCount; ++j) {
/*				Boolean writable;
				 err = AudioUnitGetPropertyInfo(au, propertyID, kAudioUnitScope_Output, j, &dataSize, &writable);

				 if(noErr != err && kAudioUnitErr_InvalidProperty != err)
				 return err;

				 if(kAudioUnitErr_InvalidProperty == err || !writable)
				 continue;*/

				result = AudioUnitSetProperty(au, propertyID, kAudioUnitScope_Output, j, propertyData, propertyDataSize);				
				if(noErr != result) {
					log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
					LOG4CXX_ERROR(logger, "AudioUnitSetProperty (" << propertyID << ", kAudioUnitScope_Output) failed: " << result);
					return false;
				}
			}
		}
	}

	return true;
}

bool AudioPlayer::SetAUGraphSampleRateAndChannelsPerFrame(Float64 sampleRate, UInt32 channelsPerFrame)
{
	// ========================================
	// If the graph is running, stop it
	Boolean graphIsRunning = FALSE;
	OSStatus result = AUGraphIsRunning(mAUGraph, &graphIsRunning);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphIsRunning failed: " << result);
		return false;
	}
	
	if(graphIsRunning) {
		result = AUGraphStop(mAUGraph);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphStop failed: " << result);
			return false;
		}
	}
	
	// ========================================
	// If the graph is initialized, uninitialize it
	Boolean graphIsInitialized = FALSE;
	result = AUGraphIsInitialized(mAUGraph, &graphIsInitialized);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphIsInitialized failed: " << result);
		return false;
	}
	
	if(graphIsInitialized) {
		result = AUGraphUninitialize(mAUGraph);		
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphUninitialize failed: " << result);
			return false;
		}
	}

	// ========================================
	// Save the interaction information and then clear all the connections
	UInt32 interactionCount = 0;
	result = AUGraphGetNumberOfInteractions(mAUGraph, &interactionCount);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphGetNumberOfInteractions failed: " << result);
		return false;
	}

	AUNodeInteraction interactions [interactionCount];

	for(UInt32 i = 0; i < interactionCount; ++i) {
		result = AUGraphGetInteractionInfo(mAUGraph, i, &interactions[i]);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphGetInteractionInfo failed: " << result);
			return false;
		}
	}

	result = AUGraphClearConnections(mAUGraph);	
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphClearConnections failed: " << result);
		return false;
	}
	
	AudioStreamBasicDescription format = mRingBufferFormat;
	
	format.mChannelsPerFrame	= channelsPerFrame;
	format.mSampleRate			= sampleRate;

	// ========================================
	// Attempt to set the new stream format
	if(!SetPropertyOnAUGraphNodes(kAudioUnitProperty_StreamFormat, &format, sizeof(format))) {
		// If the new format could not be set, restore the old format to ensure a working graph
		if(!SetPropertyOnAUGraphNodes(kAudioUnitProperty_StreamFormat, &mRingBufferFormat, sizeof(mRingBufferFormat))) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "Unable to restore AUGraph format: " << result);
		}

		// Do not free connections here, so graph can be rebuilt
	}
	else
		mRingBufferFormat = format;

	// ========================================
	// Restore the graph's connections and input callbacks
	for(UInt32 i = 0; i < interactionCount; ++i) {
		switch(interactions[i].nodeInteractionType) {

				// Reestablish the connection
			case kAUNodeInteraction_Connection:
			{
				result = AUGraphConnectNodeInput(mAUGraph, 
												 interactions[i].nodeInteraction.connection.sourceNode, 
												 interactions[i].nodeInteraction.connection.sourceOutputNumber,
												 interactions[i].nodeInteraction.connection.destNode, 
												 interactions[i].nodeInteraction.connection.destInputNumber);
				
				if(noErr != result) {
					log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
					LOG4CXX_ERROR(logger, "AUGraphConnectNodeInput failed: " << result);
					return false;
				}

				break;
			}

				// Reestablish the input callback
			case kAUNodeInteraction_InputCallback:
			{
				result = AUGraphSetNodeInputCallback(mAUGraph, 
													 interactions[i].nodeInteraction.inputCallback.destNode, 
													 interactions[i].nodeInteraction.inputCallback.destInputNumber,
													 &interactions[i].nodeInteraction.inputCallback.cback);

				if(noErr != result) {
					log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
					LOG4CXX_ERROR(logger, "AUGraphSetNodeInputCallback failed: " << result);
					return false;
				}

				break;
			}				
		}
	}
	
	// ========================================
	// Output units perform sample rate conversion if the input sample rate is not equal to
	// the output sample rate. For high sample rates, the sample rate conversion can require 
	// more rendered frames than are available by default in kAudioUnitProperty_MaximumFramesPerSlice (512)
	// For example, 192 KHz audio converted to 44.1 HHz requires approximately (192 / 44.1) * 512 = 2229 frames
	// So if the input and output sample rates on the output device don't match, adjust 
	// kAudioUnitProperty_MaximumFramesPerSlice to ensure enough audio data is passed per render cycle
	AudioUnit au = NULL;
	result = AUGraphNodeInfo(mAUGraph, mOutputNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}

	Float64 inputSampleRate = 0;
	UInt32 dataSize = sizeof(inputSampleRate);
	result = AudioUnitGetProperty(au, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &inputSampleRate, &dataSize);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_SampleRate, kAudioUnitScope_Global) failed: " << result);
		return false;
	}

	Float64 outputSampleRate = 0;
	dataSize = sizeof(outputSampleRate);
	result = AudioUnitGetProperty(au, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &outputSampleRate, &dataSize);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_SampleRate, kAudioUnitScope_Global) failed: " << result);
		return false;
	}

	// Apparently all AudioUnits on iOS except RemoteIO require kAudioUnitProperty_MaximumFramesPerSlice to be 4096
#if !TARGET_OS_IPHONE
	if(inputSampleRate != outputSampleRate) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_DEBUG(logger, "Input sample rate (" << inputSampleRate << ") and output sample rate (" << outputSampleRate << ") don't match");

		UInt32 currentMaxFrames = 0;
		dataSize = sizeof(currentMaxFrames);
		result = AudioUnitGetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &currentMaxFrames, &dataSize);		
		if(noErr != result) {
			LOG4CXX_ERROR(logger, "AudioUnitGetProperty (kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global) failed: " << result);
			return false;
		}

		Float64 ratio = inputSampleRate / outputSampleRate;
		Float64 multiplier = std::max(1.0, ceil(ratio));

		// Round up to the nearest power of 16
		UInt32 newMaxFrames = static_cast<UInt32>(currentMaxFrames * multiplier);
		newMaxFrames += 16;
		newMaxFrames &= 0xFFFFFFF0;

		if(newMaxFrames > currentMaxFrames) {
			LOG4CXX_DEBUG(logger, "Adjusting kAudioUnitProperty_MaximumFramesPerSlice to " << newMaxFrames);

			if(!SetPropertyOnAUGraphNodes(kAudioUnitProperty_MaximumFramesPerSlice, &newMaxFrames, sizeof(newMaxFrames))) {
				LOG4CXX_ERROR(logger, "SetPropertyOnAUGraphNodes (kAudioUnitProperty_MaximumFramesPerSlice) failed");
				return false;
			}
		}
	}
#endif

	// If the graph was initialized, reinitialize it
	if(graphIsInitialized) {
		result = AUGraphInitialize(mAUGraph);		
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphInitialize failed: " << result);
			return false;
		}
	}

	// If the graph was running, restart it
	if(graphIsRunning) {
		result = AUGraphStart(mAUGraph);
		if(noErr != result) {
			log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
			LOG4CXX_ERROR(logger, "AUGraphStart failed: " << result);
			return false;
		}
	}

	return true;
}

bool AudioPlayer::SetAUGraphChannelLayout(AudioChannelLayout *channelLayout)
{
	AudioUnit au = NULL;
	OSStatus result = AUGraphNodeInfo(mAUGraph, mOutputNode, NULL, &au);
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "AUGraphNodeInfo failed: " << result);
		return false;
	}
	
	// Attempt to set the new channel layout
	result = SetPropertyOnAUGraphNodes(kAudioUnitProperty_AudioChannelLayout, channelLayout, sizeof(channelLayout));
	if(noErr != result) {
		log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("org.sbooth.AudioEngine.AudioPlayer"));
		LOG4CXX_ERROR(logger, "SetPropertyOnAUGraphNodes (kAudioUnitProperty_AudioChannelLayout) failed: " << result);
		return false;
	}
	else {
		if(mRingBufferChannelLayout)
			free(mRingBufferChannelLayout), mRingBufferChannelLayout = NULL;
		mRingBufferChannelLayout = CopyChannelLayout(channelLayout);
	}

	return true;
}

#pragma mark Other Utilities

DecoderStateData * AudioPlayer::GetCurrentDecoderState() const
{
	DecoderStateData *result = NULL;
	for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
		DecoderStateData *decoderState = mActiveDecoders[bufferIndex];
		
		if(NULL == decoderState)
			continue;
		
		if(eDecoderStateDataFlagRenderingFinished & decoderState->mFlags)
			continue;

		if(NULL == result)
			result = decoderState;
		else if(decoderState->mTimeStamp < result->mTimeStamp)
			result = decoderState;
	}
	
	return result;
}

DecoderStateData * AudioPlayer::GetDecoderStateStartingAfterTimeStamp(SInt64 timeStamp) const
{
	DecoderStateData *result = NULL;
	for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
		DecoderStateData *decoderState = mActiveDecoders[bufferIndex];
		
		if(NULL == decoderState)
			continue;
		
		if(eDecoderStateDataFlagRenderingFinished & decoderState->mFlags)
			continue;

		if(NULL == result && decoderState->mTimeStamp > timeStamp)
			result = decoderState;
		else if(decoderState->mTimeStamp > timeStamp && decoderState->mTimeStamp < result->mTimeStamp)
			result = decoderState;
	}
	
	return result;
}

void AudioPlayer::StopActiveDecoders()
{
	// The player must be stopped or a SIGSEGV could occur in this method
	// This must be ensured by the caller!

	// Request that any decoders still actively decoding stop
	for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
		DecoderStateData *decoderState = mActiveDecoders[bufferIndex];
		
		if(NULL == decoderState)
			continue;
		
		OSAtomicTestAndSetBarrier(3 /* eDecoderStateDataFlagStopDecoding */, &decoderState->mFlags);
	}

	mDecoderSemaphore.Signal();

	for(UInt32 bufferIndex = 0; bufferIndex < kActiveDecoderArraySize; ++bufferIndex) {
		DecoderStateData *decoderState = mActiveDecoders[bufferIndex];
		
		if(NULL == decoderState)
			continue;
		
		OSAtomicTestAndSetBarrier(4 /* eDecoderStateDataFlagRenderingFinished */, &decoderState->mFlags);
	}

	mCollectorSemaphore.Signal();
}
