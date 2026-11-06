/*
SoLoud audio engine
Copyright (c) 2013-2018 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef SOLOUD_WAVSTREAM_H
#define SOLOUD_WAVSTREAM_H

#include <stdio.h>
#include <atomic>
#include <chrono>
#include "soloud.h"

// ###### flutter_soloud local patch (bounded wavstream seek) ######
// Wall-clock cap, in microseconds, on the pending seek work one
// WavStreamInstance::getAudio() call does, on top of its frame budget (see
// the bounded-seek notes in soloud_wavstream.cpp). getAudio() runs once per
// SAMPLE_GRANULARITY (512) source frames of playback, so this bounds the CPU
// one seeking voice takes to about cap / (512 frames' duration): 0.5 ms per
// 11.6 ms at 44.1 kHz, ~4% of real time, however slow the CPU. Not applied
// on the web, where only the frame budget is (the clock may not be usable on
// the AudioWorklet thread).
#define WAVSTREAM_SEEK_CALL_CAP_US 500

namespace SoLoud { class MBOggDecoder; }
#ifndef dr_flac_h
struct drflac;
#endif
#ifndef dr_mp3_h
struct drmp3;
// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
struct drmp3_seek_point;
#endif
#ifndef dr_wav_h
struct drwav;
#endif

namespace SoLoud
{
	class WavStream;
	class File;

	// ###### flutter_soloud local patch (bounded wavstream seek) ######
	// Test/tuning knob: the cap in use, WAVSTREAM_SEEK_CALL_CAP_US unless
	// changed; 0 leaves only the frame budget. Read by the audio thread at
	// each getAudio() with seek work pending.
	extern std::atomic<unsigned int> gWavStreamSeekCallCapUs;

	class WavStreamInstance : public AudioSourceInstance
	{
		WavStream *mParent;
		unsigned int mOffset;
		File *mFile;
		union codec
		{
			MBOggDecoder *mOgg;
			drflac *mFlac;
			drmp3 *mMp3;
			drwav *mWav;
		} mCodec;
		bool mStreamEnded; // Set to true when decoder reaches end of stream

		// ###### flutter_soloud local patch (bounded wavstream seek) ######
		// Seek work left for getAudio(), which does at most a fixed budget of
		// it per call, and stops earlier once WAVSTREAM_SEEK_CALL_CAP_US have
		// passed (see WavStreamInstance::seek in soloud_wavstream.cpp).
		enum SEEK_WORK
		{
			SEEK_WORK_NONE = 0,
			// MP3 through the seek table: decode on to the byte offset of
			// table point mSeekMp3Point, then count frames from there.
			SEEK_WORK_MP3_SYNC,
			// Decode and drop mSeekSkipFrames frames.
			SEEK_WORK_SKIP
		};
		unsigned int mSeekWork;
		unsigned long long mSeekSkipFrames;
		unsigned int mSeekMp3Point;
		// SEEK_WORK_MP3_SYNC: stream frame (encoder delay included) to land on.
		unsigned long long mSeekMp3Frame;
		// Output frame the pending seek lands on, and its time, which the voice
		// reports until the audio from there starts.
		unsigned int mSeekTargetFrame;
		double mSeekTargetPosition;
		// A seek at or past the end, or one that ran into the end while
		// skipping: nothing left to play.
		bool mSeekAtEnd;
		// When the last seek() started, and whether the next getAudio() is the
		// first since: its time cap counts from there, so it covers the work
		// seek() itself did.
		std::chrono::steady_clock::time_point mSeekClockStart;
		bool mSeekClockFromSeek;
		// Does up to one call's budget of the pending seek work. True when it
		// is all done (or ran into the end).
		bool advanceSeek_internal();
		unsigned int seekSkipBudget_internal() const;
		// Decodes and drops up to aFrames frames; returns how many it read.
		unsigned long long skipFrames_internal(unsigned long long aFrames);
		// MP3 seek-table seek, first step: positions the decoder a little
		// before table point aPoint's frame. False if the table cannot serve
		// aFrame.
		bool startMp3TableSeek_internal(unsigned long long aFrame);
		// Starts decoding from the start of the stream to output frame aPos.
		void seekFromStart_internal(unsigned int aPos);

	public:
		WavStreamInstance(WavStream *aParent);
		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual result seek(double aSeconds, float* mScratch, unsigned int mScratchSize);
		virtual result rewind();
		virtual bool hasEnded();
		virtual ~WavStreamInstance();
	};

	enum WAVSTREAM_FILETYPE
	{
		WAVSTREAM_WAV = 0,
		WAVSTREAM_OGG = 1,
		WAVSTREAM_FLAC = 2,
		WAVSTREAM_MP3 = 3
	};

	class WavStream : public AudioSource
	{
		result loadwav(File *fp);
		result loadogg(File *fp);
		result loadflac(File *fp);
		result loadmp3(File *fp);
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		void freeMp3SeekTable();
	public:
		int mFiletype;
		char *mFilename;
		File *mMemFile;
		File *mStreamFile;
		unsigned int mSampleCount;
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		// MP3 seek table built once by loadmp3() and bound by every instance.
		// Instances bind it when they are created; with none (building it
		// failed), seeks decode from the start, in bounded steps.
		drmp3_seek_point *mMp3SeekPoints;
		unsigned int mMp3SeekPointCount;

		WavStream();
		virtual ~WavStream();
		result load(const char *aFilename);
		result loadMem(const unsigned char *aData, unsigned int aDataLen, bool aCopy = false, bool aTakeOwnership = true);
		result loadToMem(const char *aFilename);
		result loadFile(File *aFile);
		result loadFileToMem(File *aFile);		
		virtual AudioSourceInstance *createInstance();
		time getLength();

	public:
		result parse(File *aFile);
	};
};

#endif