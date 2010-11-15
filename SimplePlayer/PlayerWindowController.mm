/*
 *  Copyright (C) 2009, 2010 Stephen F. Booth <me@sbooth.org>
 *  All Rights Reserved
 */

#import "PlayerWindowController.h"

#include <libkern/OSAtomic.h>

#define DSP_ENABLED 0

#if DSP_ENABLED
#  include <SFBAudioEngine/DSPAudioPlayer.h>
#else
#  include <SFBAudioEngine/AudioPlayer.h>
#endif /* DSP_ENABLED */
#include <SFBAudioEngine/AudioDecoder.h>

#if DSP_ENABLED
#  define PLAYER (static_cast<DSPAudioPlayer *>(_player))
#else
#  define PLAYER (static_cast<AudioPlayer *>(_player))
#endif /* DSP_ENABLED */

// ========================================
// Player flags
// ========================================
enum {
	ePlayerFlagRenderingStarted			= 1 << 0,
	ePlayerFlagRenderingFinished		= 1 << 1
};

volatile static uint32_t sPlayerFlags = 0;

@interface PlayerWindowController (Callbacks)
- (void) uiTimerFired:(NSTimer *)timer;
@end

@interface PlayerWindowController (Private)
- (void) updateWindowUI;
@end

// This is called from the realtime rendering thread and as such MUST NOT BLOCK!!
static void renderingStarted(void *context, const AudioDecoder *decoder)
{
#pragma unused(context)
#pragma unused(decoder)
	OSAtomicTestAndSetBarrier(7 /* ePlayerFlagRenderingStarted */, &sPlayerFlags);
}

// This is called from the realtime rendering thread and as such MUST NOT BLOCK!!
static void renderingFinished(void *context, const AudioDecoder *decoder)
{
#pragma unused(context)
#pragma unused(decoder)
	OSAtomicTestAndSetBarrier(6 /* ePlayerFlagRenderingFinished */, &sPlayerFlags);
}

@implementation PlayerWindowController

@synthesize slider = _slider;
@synthesize elapsed = _elapsed;
@synthesize remaining = _remaining;
@synthesize playButton = _playButton;
@synthesize forwardButton = _forwardButton;
@synthesize backwardButton = _backwardButton;

- (id) init
{
	if(nil == (self = [super initWithWindowNibName:@"PlayerWindow"])) {
		[self release];
		return nil;
	}
	
#if DSP_ENABLED
	_player = new DSPAudioPlayer();
#else	
	_player = new AudioPlayer();
#endif

	// Update the UI 5 times per second in all run loop modes (so menus, etc. don't stop updates)
	_uiTimer = [NSTimer timerWithTimeInterval:(1.0 / 5) target:self selector:@selector(uiTimerFired:) userInfo:nil repeats:YES];

	// addTimer:forMode: will retain _uiTimer
	[[NSRunLoop mainRunLoop] addTimer:_uiTimer forMode:NSRunLoopCommonModes];
	
	return self;
}

- (void) dealloc
{
	[_uiTimer invalidate], _uiTimer = nil;
	
	delete PLAYER, _player = NULL;

	[_slider release], _slider = nil;
	[_elapsed release], _elapsed = nil;
	[_remaining release], _remaining = nil;
	[_playButton release], _playButton = nil;
	[_forwardButton release], _forwardButton = nil;
	[_backwardButton release], _backwardButton = nil;
	
	[super dealloc];
}

- (void) awakeFromNib
{
	// Disable the UI since no file is loaded
	[self updateWindowUI];
}

- (NSString *) windowFrameAutosaveName
{
	return @"Player Window";
}

- (IBAction) playPause:(id)sender
{
#pragma unused(sender)
	PLAYER->PlayPause();
}

- (IBAction) seekForward:(id)sender
{
#pragma unused(sender)
	PLAYER->SeekForward();
}

- (IBAction) seekBackward:(id)sender
{
#pragma unused(sender)
	PLAYER->SeekBackward();
}

- (IBAction) seek:(id)sender
{
	NSParameterAssert(nil != sender);
	
	SInt64 desiredFrame = static_cast<SInt64>([sender doubleValue] * PLAYER->GetTotalFrames());
	
	PLAYER->SeekToFrame(desiredFrame);
}

- (BOOL) playFile:(NSString *)file
{
	NSParameterAssert(nil != file);

	NSURL *url = [NSURL fileURLWithPath:file];
	
	AudioDecoder *decoder = AudioDecoder::CreateDecoderForURL(reinterpret_cast<CFURLRef>(url));
	if(NULL == decoder)
		return NO;

	PLAYER->Stop();

	// Register for rendering started/finished notifications so the UI can be updated properly
	decoder->SetRenderingStartedCallback(renderingStarted, self);
	decoder->SetRenderingFinishedCallback(renderingFinished, self);
	
	if(true == PLAYER->Enqueue(decoder)) {
		PLAYER->Play();
		[[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url];
	}
	else {
		delete decoder;
		return NO;
	}
	
	return YES;
}

@end

@implementation PlayerWindowController (Callbacks)

- (void) uiTimerFired:(NSTimer *)timer
{
#pragma unused(timer)
	// To avoid blocking the realtime rendering thread, flags are set in the callbacks and subsequently handled here
	if(ePlayerFlagRenderingStarted & sPlayerFlags) {
		OSAtomicTestAndClearBarrier(7 /* ePlayerFlagRenderingStarted */, &sPlayerFlags);
		
		[self updateWindowUI];
		
		return;
	}
	else if(ePlayerFlagRenderingFinished & sPlayerFlags) {
		OSAtomicTestAndClearBarrier(6 /* ePlayerFlagRenderingFinished */, &sPlayerFlags);
		
		[self updateWindowUI];
		
		return;
	}

	if(!PLAYER->IsPlaying())
		[_playButton setTitle:@"Resume"];
	else
		[_playButton setTitle:@"Pause"];
	
	double fractionComplete = static_cast<double>(PLAYER->GetCurrentFrame()) / static_cast<double>(PLAYER->GetTotalFrames());
	
	[_slider setDoubleValue:fractionComplete];
	[_elapsed setDoubleValue:PLAYER->GetCurrentTime()];
	[_remaining setDoubleValue:(-1 * PLAYER->GetRemainingTime())];
}

@end

@implementation PlayerWindowController (Private)

- (void) updateWindowUI
{
	NSURL *url = reinterpret_cast<const NSURL *>(PLAYER->GetPlayingURL());
	
	// Nothing happening, reset the window
	if(NULL == url) {
		[[self window] setRepresentedURL:nil];
		[[self window] setTitle:@""];
		
		[_slider setEnabled:NO];
		[_playButton setState:NSOffState];
		[_playButton setEnabled:NO];
		[_backwardButton setEnabled:NO];
		[_forwardButton setEnabled:NO];
		
		[_elapsed setHidden:YES];
		[_remaining setHidden:YES];
		
		return;
	}
	
	bool seekable = PLAYER->SupportsSeeking();
	
	// Update the window's title and represented file
	[[self window] setRepresentedURL:url];
	[[self window] setTitle:[[NSFileManager defaultManager] displayNameAtPath:[url path]]];
	
	[[NSDocumentController sharedDocumentController] noteNewRecentDocumentURL:url];
	
	// Update the UI
	[_slider setEnabled:seekable];
	[_playButton setEnabled:YES];
	[_backwardButton setEnabled:seekable];
	[_forwardButton setEnabled:seekable];
	
	// Show the times
	[_elapsed setHidden:NO];
	[_remaining setHidden:NO];	
}

@end
