/*************************************************************************/
/*  audio_driver_osx.cpp                                                 */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                    http://www.godotengine.org                         */
/*************************************************************************/
/* Copyright (c) 2007-2017 Juan Linietsky, Ariel Manzur.                 */
/* Copyright (c) 2014-2017 Godot Engine contributors (cf. AUTHORS.md)    */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/
#ifdef OSX_ENABLED

#include "audio_driver_osx.h"

static OSStatus outputDeviceAddressCB(AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses, void *__nullable inClientData) {
	AudioDriverOSX *driver = (AudioDriverOSX *)inClientData;

	driver->reopen();

	return noErr;
}

Error AudioDriverOSX::initDevice() {
	AudioStreamBasicDescription strdesc;
	strdesc.mFormatID = kAudioFormatLinearPCM;
	strdesc.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	strdesc.mChannelsPerFrame = channels;
	strdesc.mSampleRate = 44100;
	strdesc.mFramesPerPacket = 1;
	strdesc.mBitsPerChannel = 16;
	strdesc.mBytesPerFrame = strdesc.mBitsPerChannel * strdesc.mChannelsPerFrame / 8;
	strdesc.mBytesPerPacket = strdesc.mBytesPerFrame * strdesc.mFramesPerPacket;

	OSStatus result;
	AURenderCallbackStruct callback;
	AudioComponentDescription desc;
	AudioComponent comp = NULL;
	const AudioUnitElement output_bus = 0;
	const AudioUnitElement bus = output_bus;
	const AudioUnitScope scope = kAudioUnitScope_Input;

	zeromem(&desc, sizeof(desc));
	desc.componentType = kAudioUnitType_Output;
	desc.componentSubType = 0; /* !!! FIXME: ? */
	comp = AudioComponentFindNext(NULL, &desc);
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;

	result = AudioComponentInstanceNew(comp, &audio_unit);
	ERR_FAIL_COND_V(result != noErr, FAILED);
	ERR_FAIL_COND_V(comp == NULL, FAILED);

	result = AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat, scope, bus, &strdesc, sizeof(strdesc));
	ERR_FAIL_COND_V(result != noErr, FAILED);

	zeromem(&callback, sizeof(AURenderCallbackStruct));
	callback.inputProc = &AudioDriverOSX::output_callback;
	callback.inputProcRefCon = this;
	result = AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback, scope, bus, &callback, sizeof(callback));
	ERR_FAIL_COND_V(result != noErr, FAILED);

	result = AudioUnitInitialize(audio_unit);
	ERR_FAIL_COND_V(result != noErr, FAILED);

	return OK;
}

Error AudioDriverOSX::finishDevice() {
	OSStatus result;

	if (active) {
		result = AudioOutputUnitStop(audio_unit);
		ERR_FAIL_COND_V(result != noErr, FAILED);

		active = false;
	}

	result = AudioUnitUninitialize(audio_unit);
	ERR_FAIL_COND_V(result != noErr, FAILED);

	return OK;
}

Error AudioDriverOSX::init() {
	OSStatus result;

	active = false;
	channels = 2;

	outputDeviceAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice;
	outputDeviceAddress.mScope = kAudioObjectPropertyScopeGlobal;
	outputDeviceAddress.mElement = kAudioObjectPropertyElementMaster;

	result = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &outputDeviceAddress, &outputDeviceAddressCB, this);
	ERR_FAIL_COND_V(result != noErr, FAILED);

	const int samples = 1024;
	samples_in = memnew_arr(int32_t, samples); // whatever
	buffer_frames = samples / channels;

	return initDevice();
};

Error AudioDriverOSX::reopen() {
	Error err;
	bool restart = false;

	lock();

	if (active) {
		restart = true;
	}

	err = finishDevice();
	if (err != OK) {
		ERR_PRINT("finishDevice failed");
		unlock();
		return err;
	}

	err = initDevice();
	if (err != OK) {
		ERR_PRINT("initDevice failed");
		unlock();
		return err;
	}

	if (restart) {
		start();
	}

	unlock();

	return OK;
}

OSStatus AudioDriverOSX::output_callback(void *inRefCon,
		AudioUnitRenderActionFlags *ioActionFlags,
		const AudioTimeStamp *inTimeStamp,
		UInt32 inBusNumber, UInt32 inNumberFrames,
		AudioBufferList *ioData) {

	AudioBuffer *abuf;
	AudioDriverOSX *ad = (AudioDriverOSX *)inRefCon;

	bool mix = true;

	if (!ad->active)
		mix = false;
	else if (ad->mutex) {
		mix = ad->mutex->try_lock() == OK;
	};

	if (!mix) {
		for (unsigned int i = 0; i < ioData->mNumberBuffers; i++) {
			abuf = &ioData->mBuffers[i];
			zeromem(abuf->mData, abuf->mDataByteSize);
		};
		return 0;
	};

	int frames_left;

	for (unsigned int i = 0; i < ioData->mNumberBuffers; i++) {

		abuf = &ioData->mBuffers[i];
		frames_left = inNumberFrames;
		int16_t *out = (int16_t *)abuf->mData;

		while (frames_left) {

			int frames = MIN(frames_left, ad->buffer_frames);
			//ad->lock();
			ad->audio_server_process(frames, ad->samples_in);
			//ad->unlock();

			for (int i = 0; i < frames * ad->channels; i++) {

				out[i] = ad->samples_in[i] >> 16;
			}

			frames_left -= frames;
			out += frames * ad->channels;
		};
	};

	if (ad->mutex)
		ad->mutex->unlock();

	return 0;
};

void AudioDriverOSX::start() {
	if (!active) {
		OSStatus result = AudioOutputUnitStart(audio_unit);
		if (result != noErr) {
			ERR_PRINT("AudioOutputUnitStart failed");
		} else {
			active = true;
		}
	}
};

int AudioDriverOSX::get_mix_rate() const {
	return 44100;
};

AudioDriver::SpeakerMode AudioDriverOSX::get_speaker_mode() const {
	return SPEAKER_MODE_STEREO;
};

void AudioDriverOSX::lock() {
	if (mutex)
		mutex->lock();
};
void AudioDriverOSX::unlock() {
	if (mutex)
		mutex->unlock();
};

void AudioDriverOSX::finish() {
	OSStatus result;

	finishDevice();

	result = AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &outputDeviceAddress, &outputDeviceAddressCB, this);
	if (result != noErr) {
		ERR_PRINT("AudioObjectRemovePropertyListener failed");
	}

	memdelete_arr(samples_in);
};

AudioDriverOSX::AudioDriverOSX() {

	mutex = Mutex::create(); //NULL;
};

AudioDriverOSX::~AudioDriverOSX(){

};

#endif
