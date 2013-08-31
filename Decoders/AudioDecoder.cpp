/*
 *  Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013 Stephen F. Booth <me@sbooth.org>
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

#include <AudioToolbox/AudioFormat.h>
#include <CoreFoundation/CoreFoundation.h>

#include "HTTPInputSource.h"
#include "AudioDecoder.h"
#include "Logger.h"
#include "CFErrorUtilities.h"
#include "CreateStringForOSType.h"
#include "LoopableRegionDecoder.h"

// ========================================
// Error Codes
// ========================================
const CFStringRef	AudioDecoderErrorDomain					= CFSTR("org.sbooth.AudioEngine.ErrorDomain.AudioDecoder");

#pragma mark Static Methods

bool AudioDecoder::sAutomaticallyOpenDecoders = false;
std::vector<AudioDecoder::SubclassInfo> AudioDecoder::sRegisteredSubclasses;

CFArrayRef AudioDecoder::CreateSupportedFileExtensions()
{
	CFMutableArrayRef supportedFileExtensions = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

	for(auto subclassInfo : sRegisteredSubclasses) {
		CFArrayRef decoderFileExtensions = subclassInfo.mCreateSupportedFileExtensions();
		CFArrayAppendArray(supportedFileExtensions, decoderFileExtensions, CFRangeMake(0, CFArrayGetCount(decoderFileExtensions)));
		CFRelease(decoderFileExtensions);
	}

	return supportedFileExtensions;
}

CFArrayRef AudioDecoder::CreateSupportedMIMETypes()
{
	CFMutableArrayRef supportedMIMETypes = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

	for(auto subclassInfo : sRegisteredSubclasses) {
		CFArrayRef decoderMIMETypes = subclassInfo.mCreateSupportedMIMETypes();
		CFArrayAppendArray(supportedMIMETypes, decoderMIMETypes, CFRangeMake(0, CFArrayGetCount(decoderMIMETypes)));
		CFRelease(decoderMIMETypes);
	}

	return supportedMIMETypes;
}

bool AudioDecoder::HandlesFilesWithExtension(CFStringRef extension)
{
	if(nullptr == extension)
		return false;

	for(auto subclassInfo : sRegisteredSubclasses) {
		if(subclassInfo.mHandlesFilesWithExtension(extension))
			return true;
	}

	return false;
}

bool AudioDecoder::HandlesMIMEType(CFStringRef mimeType)
{
	if(nullptr == mimeType)
		return false;

	for(auto subclassInfo : sRegisteredSubclasses) {
		if(subclassInfo.mHandlesMIMEType(mimeType))
			return true;
	}

	return false;
}

AudioDecoder * AudioDecoder::CreateDecoderForURL(CFURLRef url, CFErrorRef *error)
{
	return CreateDecoderForURL(url, nullptr, error);
}

AudioDecoder * AudioDecoder::CreateDecoderForURL(CFURLRef url, CFStringRef mimeType, CFErrorRef *error)
{
	if(nullptr == url)
		return nullptr;

	// Create the input source which will feed the decoder
	InputSource *inputSource = InputSource::CreateInputSourceForURL(url, 0, error);
	
	if(nullptr == inputSource)
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSource(inputSource, mimeType, error);
	
	if(nullptr == decoder)
		delete inputSource, inputSource = nullptr;
	
	return decoder;
}

// If this returns nullptr, the caller is responsible for deleting inputSource
// If this returns an AudioDecoder instance, the instance takes ownership of inputSource
AudioDecoder * AudioDecoder::CreateDecoderForInputSource(InputSource *inputSource, CFErrorRef *error)
{
	return CreateDecoderForInputSource(inputSource, nullptr, error);
}

AudioDecoder * AudioDecoder::CreateDecoderForInputSource(InputSource *inputSource, CFStringRef mimeType, CFErrorRef *error)
{
	if(nullptr == inputSource)
		return nullptr;

	AudioDecoder *decoder = nullptr;

	// Open the input source if it isn't already
	if(AutomaticallyOpenDecoders() && !inputSource->IsOpen() && !inputSource->Open(error))
		return nullptr;

#if 0
	// If the input is an instance of HTTPInputSource, use the MIME type from the server
	// This code is disabled because most HTTP servers don't send the correct MIME types
	HTTPInputSource *httpInputSource = dynamic_cast<HTTPInputSource *>(inputSource);
	bool releaseMIMEType = false;
	if(!mimeType && httpInputSource && httpInputSource->IsOpen()) {
		mimeType = httpInputSource->CopyContentMIMEType();
		if(mimeType)
			releaseMIMEType = true;
	}
#endif

	// The MIME type takes precedence over the file extension
	if(mimeType) {
		for(auto subclassInfo : sRegisteredSubclasses) {
			if(subclassInfo.mHandlesMIMEType(mimeType)) {
				decoder = subclassInfo.mCreateDecoder(inputSource);
				if(AutomaticallyOpenDecoders() && !decoder->Open(error)) {
					decoder->mInputSource = nullptr;
					delete decoder, decoder = nullptr;
				}
			}

			if(decoder)
				break;
		}

#if 0
		if(releaseMIMEType)
			CFRelease(mimeType), mimeType = nullptr;
#endif

		if(decoder)
			return decoder;
	}

	// If no MIME type was specified, use the extension-based resolvers

	CFURLRef inputURL = inputSource->GetURL();
	if(!inputURL)
		return nullptr;

	CFStringRef pathExtension = CFURLCopyPathExtension(inputURL);
	if(!pathExtension) {
		if(error) {
			CFStringRef description = CFCopyLocalizedString(CFSTR("The type of the file “%@” could not be determined."), "");
			CFStringRef failureReason = CFCopyLocalizedString(CFSTR("Unknown file type"), "");
			CFStringRef recoverySuggestion = CFCopyLocalizedString(CFSTR("The file's extension may be missing or may not match the file's type."), "");
			
			*error = CreateErrorForURL(InputSourceErrorDomain, InputSourceFileNotFoundError, description, inputURL, failureReason, recoverySuggestion);
			
			CFRelease(description), description = nullptr;
			CFRelease(failureReason), failureReason = nullptr;
			CFRelease(recoverySuggestion), recoverySuggestion = nullptr;
		}

		return nullptr;
	}

	// TODO: Some extensions (.oga for example) support multiple audio codecs (Vorbis, FLAC, Speex)
	// and if openDecoder is false the wrong decoder type may be returned, since the file isn't analyzed
	// until Open() is called

	for(auto subclassInfo : sRegisteredSubclasses) {
		if(subclassInfo.mHandlesFilesWithExtension(pathExtension)) {
			decoder = subclassInfo.mCreateDecoder(inputSource);
			if(AutomaticallyOpenDecoders() && !decoder->Open(error)) {
				decoder->mInputSource = nullptr;
				delete decoder, decoder = nullptr;
			}
		}

		if(decoder)
			break;
	}

	CFRelease(pathExtension), pathExtension = nullptr;

	return decoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForURLRegion(CFURLRef url, SInt64 startingFrame, CFErrorRef *error)
{
	if(nullptr == url)
		return nullptr;

	InputSource *inputSource = InputSource::CreateInputSourceForURL(url, 0, error);

	if(nullptr == inputSource)
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSourceRegion(inputSource, startingFrame, error);

	if(nullptr == decoder)
		delete inputSource, inputSource = nullptr;

	return decoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForURLRegion(CFURLRef url, SInt64 startingFrame, UInt32 frameCount, CFErrorRef *error)
{
	if(nullptr == url)
		return nullptr;

	InputSource *inputSource = InputSource::CreateInputSourceForURL(url, 0, error);

	if(nullptr == inputSource)
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSourceRegion(inputSource, startingFrame, frameCount, error);

	if(nullptr == decoder)
		delete inputSource, inputSource = nullptr;

	return decoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForURLRegion(CFURLRef url, SInt64 startingFrame, UInt32 frameCount, UInt32 repeatCount, CFErrorRef *error)
{
	if(nullptr == url)
		return nullptr;

	InputSource *inputSource = InputSource::CreateInputSourceForURL(url, 0, error);

	if(nullptr == inputSource)
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSourceRegion(inputSource, startingFrame, frameCount, repeatCount, error);

	if(nullptr == decoder)
		delete inputSource, inputSource = nullptr;

	return decoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForInputSourceRegion(InputSource *inputSource, SInt64 startingFrame, CFErrorRef *error)
{
	if(nullptr == inputSource)
		return nullptr;

	if(!inputSource->SupportsSeeking())
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSource(inputSource, error);

	if(nullptr == decoder)
		return nullptr;

	if(!decoder->SupportsSeeking()) {
		delete decoder, decoder = nullptr;
		return nullptr;
	}

	AudioDecoder *regionDecoder = CreateDecoderForDecoderRegion(decoder, startingFrame, error);

	if(nullptr == regionDecoder) {
		delete decoder, decoder = nullptr;
		return nullptr;
	}

	return regionDecoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForInputSourceRegion(InputSource *inputSource, SInt64 startingFrame, UInt32 frameCount, CFErrorRef *error)
{
	if(nullptr == inputSource)
		return nullptr;

	if(!inputSource->SupportsSeeking())
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSource(inputSource, error);

	if(nullptr == decoder)
		return nullptr;

	if(!decoder->SupportsSeeking()) {
		delete decoder, decoder = nullptr;
		return nullptr;
	}

	AudioDecoder *regionDecoder = CreateDecoderForDecoderRegion(decoder, startingFrame, frameCount, error);

	if(nullptr == regionDecoder) {
		delete decoder, decoder = nullptr;
		return nullptr;
	}

	return regionDecoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForInputSourceRegion(InputSource *inputSource, SInt64 startingFrame, UInt32 frameCount, UInt32 repeatCount, CFErrorRef *error)
{
	if(nullptr == inputSource)
		return nullptr;

	if(!inputSource->SupportsSeeking())
		return nullptr;

	AudioDecoder *decoder = CreateDecoderForInputSource(inputSource, error);

	if(nullptr == decoder)
		return nullptr;

	if(!decoder->SupportsSeeking()) {
		delete decoder, decoder = nullptr;
		return nullptr;
	}

	AudioDecoder *regionDecoder = CreateDecoderForDecoderRegion(decoder, startingFrame, frameCount, repeatCount, error);

	if(nullptr == regionDecoder) {
		delete decoder, decoder = nullptr;
		return nullptr;
	}

	return regionDecoder;
}

AudioDecoder * AudioDecoder::CreateDecoderForDecoderRegion(AudioDecoder *decoder, SInt64 startingFrame, CFErrorRef */*error*/)
{
	if(nullptr == decoder)
		return nullptr;
	
	if(!decoder->SupportsSeeking())
		return nullptr;
	
	return new LoopableRegionDecoder(decoder, startingFrame);
}

AudioDecoder * AudioDecoder::CreateDecoderForDecoderRegion(AudioDecoder *decoder, SInt64 startingFrame, UInt32 frameCount, CFErrorRef */*error*/)
{
	if(nullptr == decoder)
		return nullptr;
	
	if(!decoder->SupportsSeeking())
		return nullptr;
	
	return new LoopableRegionDecoder(decoder, startingFrame, frameCount);
}

AudioDecoder * AudioDecoder::CreateDecoderForDecoderRegion(AudioDecoder *decoder, SInt64 startingFrame, UInt32 frameCount, UInt32 repeatCount, CFErrorRef *)
{
	if(nullptr == decoder)
		return nullptr;
	
	if(!decoder->SupportsSeeking())
		return nullptr;
	
	return new LoopableRegionDecoder(decoder, startingFrame, frameCount, repeatCount);
}

#pragma mark Creation and Destruction

AudioDecoder::AudioDecoder()
	: mInputSource(nullptr), mChannelLayout(nullptr), mIsOpen(false), mRepresentedObject(nullptr)
{
	memset(&mSourceFormat, 0, sizeof(mSourceFormat));
}

AudioDecoder::AudioDecoder(InputSource *inputSource)
	: mInputSource(inputSource), mChannelLayout(nullptr), mIsOpen(false), mRepresentedObject(nullptr)
{
	assert(nullptr != inputSource);

	memset(&mFormat, 0, sizeof(mSourceFormat));
	memset(&mSourceFormat, 0, sizeof(mSourceFormat));
}

AudioDecoder::~AudioDecoder()
{
	if(mInputSource)
		delete mInputSource, mInputSource = nullptr;

	if(mChannelLayout)
		free(mChannelLayout), mChannelLayout = nullptr;
}

#pragma mark Base Functionality

CFStringRef AudioDecoder::CreateSourceFormatDescription() const
{
	if(!IsOpen())
		return nullptr;

	CFStringRef		sourceFormatDescription		= nullptr;
	UInt32			sourceFormatNameSize		= sizeof(sourceFormatDescription);
	OSStatus		result						= AudioFormatGetProperty(kAudioFormatProperty_FormatName, 
																		 sizeof(mSourceFormat), 
																		 &mSourceFormat, 
																		 &sourceFormatNameSize, 
																		 &sourceFormatDescription);

	if(noErr != result) {
		CFStringRef osType = CreateStringForOSType((OSType)result);
		LOGGER_WARNING("org.sbooth.AudioEngine.AudioDecoder", "AudioFormatGetProperty (kAudioFormatProperty_FormatName) failed: " << result << osType);
		CFRelease(osType), osType = nullptr;
	}
	
	return sourceFormatDescription;
}

CFStringRef AudioDecoder::CreateFormatDescription() const
{
	if(!IsOpen())
		return nullptr;

	CFStringRef		sourceFormatDescription		= nullptr;
	UInt32			specifierSize				= sizeof(sourceFormatDescription);
	OSStatus		result						= AudioFormatGetProperty(kAudioFormatProperty_FormatName, 
																		 sizeof(mFormat), 
																		 &mFormat, 
																		 &specifierSize, 
																		 &sourceFormatDescription);

	if(noErr != result) {
		CFStringRef osType = CreateStringForOSType((OSType)result);
		LOGGER_WARNING("org.sbooth.AudioEngine.AudioDecoder", "AudioFormatGetProperty (kAudioFormatProperty_FormatName) failed: " << result << osType);
		CFRelease(osType), osType = nullptr;
	}
	
	return sourceFormatDescription;
}

CFStringRef AudioDecoder::CreateChannelLayoutDescription() const
{
	if(!IsOpen())
		return nullptr;

	CFStringRef		channelLayoutDescription	= nullptr;
	UInt32			specifierSize				= sizeof(channelLayoutDescription);
	OSStatus		result						= AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutName, 
																		 sizeof(mChannelLayout), 
																		 mChannelLayout, 
																		 &specifierSize, 
																		 &channelLayoutDescription);

	if(noErr != result) {
		CFStringRef osType = CreateStringForOSType((OSType)result);
		LOGGER_WARNING("org.sbooth.AudioEngine.AudioDecoder", "AudioFormatGetProperty (kAudioFormatProperty_ChannelLayoutName) failed: " << result << osType);
		CFRelease(osType), osType = nullptr;
	}
	
	return channelLayoutDescription;
}
