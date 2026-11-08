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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "soloud.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "soloud_wavstream.h"
#include "soloud_file.h"
#if !defined(NO_XIPH_LIBS)
#include "mb_ogg.h"
#endif

namespace SoLoud
{
	size_t drflac_read_func(void* pUserData, void* pBufferOut, size_t bytesToRead)
	{
		File *fp = (File*)pUserData;
		return fp->read((unsigned char*)pBufferOut, (unsigned int)bytesToRead);
	}

	size_t drmp3_read_func(void* pUserData, void* pBufferOut, size_t bytesToRead)
	{
		File *fp = (File*)pUserData;
		return fp->read((unsigned char*)pBufferOut, (unsigned int)bytesToRead);
	}

	size_t drwav_read_func(void* pUserData, void* pBufferOut, size_t bytesToRead)
	{
		File *fp = (File*)pUserData;
		return fp->read((unsigned char*)pBufferOut, (unsigned int)bytesToRead);
	}

	drflac_bool32 drflac_seek_func(void* pUserData, int offset, drflac_seek_origin origin)
	{
		File *fp = (File*)pUserData;
		if (origin != DRFLAC_SEEK_SET)
			offset += fp->pos();
		fp->seek(offset);
		return 1;
	}

	drmp3_bool32 drmp3_seek_func(void* pUserData, int offset, drmp3_seek_origin origin)
	{
		File *fp = (File*)pUserData;
		if (origin != DRMP3_SEEK_SET)
			offset += fp->pos();
		fp->seek(offset);
		return 1;
	}

	drmp3_bool32 drwav_seek_func(void* pUserData, int offset, drwav_seek_origin origin)
	{
		File *fp = (File*)pUserData;
		if (origin != DRWAV_SEEK_SET)
			offset += fp->pos();
		fp->seek(offset);
		return 1;
	}

	WavStreamInstance::WavStreamInstance(WavStream *aParent)
	{
		mParent = aParent;
		mOffset = 0;
		mCodec.mOgg = 0;
		mCodec.mFlac = 0;
		mFile = 0;
		mStreamEnded = false;
		// ###### flutter_soloud local patch (bounded wavstream seek) ######
		mSeekWork = SEEK_WORK_NONE;
		mSeekSkipFrames = 0;
		mSeekMp3Point = 0;
		mSeekMp3Frame = 0;
		mSeekTargetFrame = 0;
		mSeekTargetPosition = 0;
		mSeekAtEnd = false;
		mSeekClockFromSeek = false;
		if (aParent->mMemFile)
		{
			MemoryFile *mf = new MemoryFile();
			mFile = mf;
			mf->openMem(aParent->mMemFile->getMemPtr(), aParent->mMemFile->length(), false, false);
		}
		else
		if (aParent->mFilename)
		{
			DiskFile *df = new DiskFile;
			mFile = df;
			// ###### flutter_soloud local patch (wavstream missing file) ######
			// The file may have been deleted or moved since the source was
			// loaded. Without it the voice has nothing to play and ends (see
			// hasEnded()); the decoders must not read a file that is not open.
			if (df->open(aParent->mFilename) != SO_NO_ERROR)
			{
				delete df;
				mFile = 0;
			}
		}
		else
		if (aParent->mStreamFile)
		{
			mFile = aParent->mStreamFile;
			mFile->seek(0);
		}
		else
		{
			return;
		}
		
		if (mFile)
		{
			if (mParent->mFiletype == WAVSTREAM_WAV)
			{
				mCodec.mWav = new drwav;
				if (!drwav_init(mCodec.mWav, drwav_read_func, drwav_seek_func, NULL, (void*)mFile, NULL))
				{
					delete mCodec.mWav;
					mCodec.mWav = 0;
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
			}
			else
#if !defined(NO_XIPH_LIBS)
			if (mParent->mFiletype == WAVSTREAM_OGG)
			{
				mCodec.mOgg = new MBOggDecoder();
				if (!mCodec.mOgg->open(mFile))
				{
					delete mCodec.mOgg;
					mCodec.mOgg = 0;
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
			}
			else
#endif
			if (mParent->mFiletype == WAVSTREAM_FLAC)
			{
				mCodec.mFlac = drflac_open(drflac_read_func, drflac_seek_func, NULL, (void*)mFile, NULL);
				if (!mCodec.mFlac)
				{
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
			}
			else
			if (mParent->mFiletype == WAVSTREAM_MP3)
			{
				mCodec.mMp3 = new drmp3;
				if (!drmp3_init(mCodec.mMp3, drmp3_read_func, drmp3_seek_func, NULL, NULL, (void*)mFile, NULL))
				{
					delete mCodec.mMp3;
					mCodec.mMp3 = 0;
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
				// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
				// Seek through the source's table (see WavStream::loadmp3).
				else if (mParent->mMp3SeekPoints)
				{
					drmp3_bind_seek_table(mCodec.mMp3, mParent->mMp3SeekPointCount, mParent->mMp3SeekPoints);
				}
			}
			else
			{
				if (mFile != mParent->mStreamFile)
					delete mFile;
				mFile = NULL;
				return;
			}
		}
	}

	WavStreamInstance::~WavStreamInstance()
	{
		switch (mParent->mFiletype)
		{
#if !defined(NO_XIPH_LIBS)
		case WAVSTREAM_OGG:
			if (mCodec.mOgg)
			{
				delete mCodec.mOgg;
			}
			break;
#endif
		case WAVSTREAM_FLAC:
			if (mCodec.mFlac)
			{
				drflac_close(mCodec.mFlac);
			}
			break;
		case WAVSTREAM_MP3:
			if (mCodec.mMp3)
			{
				drmp3_uninit(mCodec.mMp3);
				delete mCodec.mMp3;
				mCodec.mMp3 = 0;
			}
			break;
		case WAVSTREAM_WAV:
			if (mCodec.mWav)
			{
				drwav_uninit(mCodec.mWav);
				delete mCodec.mWav;
				mCodec.mWav = 0;
			}
			break;
		}
		if (mFile != mParent->mStreamFile)
		{
			delete mFile;
		}
	}

	unsigned int WavStreamInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{			
		unsigned int offset = 0;
		float tmp[512 * MAX_CHANNELS];
		if (mFile == NULL)
			return 0;
		// ###### flutter_soloud local patch (bounded wavstream seek) ######
		// Do (up to a budget of) the work a seek left (see seek()). Until it
		// is done, play silence and stay at the target: the call reports the
		// frames it was asked for, so the mixer neither stops nor loops the
		// voice, but they are not source frames, so mSourceSamplePosition
		// (which the mixer advances by what this returns, for loop end
		// points) and the reported position do not move. The call that
		// finishes the work plays from the target exactly.
		if (mSeekWork != SEEK_WORK_NONE && !advanceSeek_internal())
		{
			unsigned int k;
			for (k = 0; k < mChannels; k++)
				memset(aBuffer + k * aBufferSize, 0, sizeof(float) * aSamplesToRead);
			mSourceSamplePosition -= aSamplesToRead;
			mStreamPosition = mSeekTargetPosition;
			return aSamplesToRead;
		}
		if (mSeekAtEnd)
			return 0; // see hasEnded()
		switch (mParent->mFiletype)
		{
		case WAVSTREAM_FLAC:
			{
				unsigned int i, j, k;

				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					offset += (unsigned int)drflac_read_pcm_frames_f32(mCodec.mFlac, blockSize, tmp);

					for (j = 0; j < blockSize; j++)
					{
						for (k = 0; k < mChannels; k++)
						{
							aBuffer[k * aBufferSize + i + j] = tmp[j * mCodec.mFlac->channels + k];
						}
					}
				}
				mOffset += offset;
				return offset;
			}
			break;
		case WAVSTREAM_MP3:
			{
				unsigned int i, j, k;

				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					offset += (unsigned int)drmp3_read_pcm_frames_f32(mCodec.mMp3, blockSize, tmp);

					for (j = 0; j < blockSize; j++)
					{
						for (k = 0; k < mChannels; k++)
						{
							aBuffer[k * aBufferSize + i + j] = tmp[j * mCodec.mMp3->channels + k];
						}
					}
				}
				mOffset += offset;
				return offset;
			}
		break;
#if !defined(NO_XIPH_LIBS)
		case WAVSTREAM_OGG:
			{
				unsigned int i;
				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					unsigned int got = mCodec.mOgg->read(aBuffer + i, blockSize, aBufferSize);
					offset += got;
					if (got == 0)
					{
						mStreamEnded = true;
						return offset;
					}
				}
				mOffset += offset;
				return offset;
			}
			break;
#endif
		case WAVSTREAM_WAV:
			{
				unsigned int i, j, k;

				for (i = 0; i < aSamplesToRead; i += 512)
				{
					unsigned int blockSize = (aSamplesToRead - i) > 512 ? 512 : aSamplesToRead - i;
					offset += (unsigned int)drwav_read_pcm_frames_f32(mCodec.mWav, blockSize, tmp);

					for (j = 0; j < blockSize; j++)
					{
						for (k = 0; k < mChannels; k++)
						{
							aBuffer[k * aBufferSize + i + j] = tmp[j * mCodec.mWav->channels + k];
						}
					}
				}
				mOffset += offset;
				return offset;
			}
			break;
		}
		return aSamplesToRead;
	}

	// ###### flutter_soloud local patch (bounded wavstream seek) ######
	//
	// The mixer runs a voice's seek() inside a mix block, under the audio
	// mutex: a queued seek at the start of a block, and a loop restart in the
	// middle of one. Every voice waits for it, so seek() must be cheap. Some
	// seeks decode their way to the target, and that work grows with the
	// target or the file:
	// - MP3 without a seek table (dr_mp3 decodes from the start of the
	//   stream), and MP3 whose table seek does not line up with the stream;
	// - FLAC without a usable seek table (dr_flac, built without CRC support
	//   and so without its binary search, decodes from the start or from
	//   where it is), and FLAC with one (it decodes on from the seek point
	//   before the target, up to the table's spacing);
	// - Opus in Ogg (MBOggDecoder::seekOpus decodes from the start);
	// - MS and IMA ADPCM WAV (dr_wav decodes from the start or from where it
	//   is).
	// For these, seek() only positions the decoder with a bounded amount of
	// work (a file seek, and at most a few codec frames of decoding), at or
	// before the target, and records what is left: decoding and dropping
	// frames up to the target, and for MP3 through the table first decoding
	// on to the table point the target is counted from. getAudio() does at
	// most a bounded amount of that per call, and plays silence until it is
	// done; the call that finishes it plays from the target, sample exact. A
	// seek whose work fits one call is done by the first getAudio() after it
	// and plays no silence at all.
	//
	// What one call does is the smaller of a frame budget
	// (seekSkipBudget_internal()) and about 0.5 ms of wall time
	// (WAVSTREAM_SEEK_CALL_CAP_US, counted for the first call from the start
	// of seek(), so it covers seek()'s own work): the time cap is checked
	// after every MP3 frame while syncing on a seek-table point, and after
	// every chunk of at most kSeekChunkFrames while skipping, never before the
	// first, so every call makes progress and may overrun the cap by one
	// chunk. (Opus is the exception: MBOggDecoder decodes a whole Ogg page
	// when it needs more, typically a second of audio, ~1.2 ms on a fast
	// desktop CPU, so a call can overrun by a page; plain Opus playback has
	// the same cost once per page.) On a fast desktop CPU the two are close
	// (the budget is ~0.3-1 ms of decoding there, so either can bind); on a
	// slow one (an ARM TV box decodes 10-15 times slower) the time cap binds,
	// so a seeking voice never takes more than ~0.5 ms per 512 frames of
	// playback (~4% of real time at 44.1 kHz), and its silence before the
	// target lasts longer instead. On the web only the frame budget applies.
	//
	// Meanwhile the voice reports the target as its position, as for any
	// seek: seek() sets it, and each silent getAudio() puts it back (the
	// mixer advances it every block); once the audio from the target starts
	// it advances from there (within a mix block of the audio, as positions
	// do). A seek at or past the end leaves nothing to play: getAudio()
	// returns nothing and hasEnded() is true, so the mixer stops the voice,
	// or loops it. A loop restart is a seek too; a new seek replaces the
	// work an earlier one left. Vorbis and FLAC in Ogg seek by bisection
	// (bounded), and PCM/float WAV by a file seek: unchanged.

	// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
	// Decodes and drops aFrames PCM frames. Returns how many it decoded.
	static drmp3_uint64 mp3Skip(drmp3 *aMp3, drmp3_uint64 aFrames)
	{
		float tmp[512 * 2]; // MP3 has at most two channels
		drmp3_uint64 done = 0;
		while (done < aFrames)
		{
			drmp3_uint64 n = aFrames - done > 512 ? 512 : aFrames - done;
			drmp3_uint64 got = drmp3_read_pcm_frames_f32(aMp3, n, tmp);
			done += got;
			if (got < n)
				break;
		}
		return done;
	}

	// ###### flutter_soloud local patch (bounded wavstream seek) ######
	// How far before the MP3 frame it syncs on a seek through the table starts
	// decoding: the first frames after a decoder restart are dropped while the
	// bit reservoir refills, and the decoder needs a few frames to warm up.
	static const drmp3_uint64 kMp3SyncLead = 16 * 1152;
	// PCM frames per MP3 frame (MPEG-1 Layer III), to charge decoding one
	// against the budget.
	static const long long kMp3FrameCost = 1152;
	// Frames skipped between two checks of the time cap: ~25 us of MP3 on a
	// fast desktop CPU, ~0.3 ms on a slow ARM one, against ~25 ns a clock read.
	static const unsigned long long kSeekChunkFrames = 2048;

	std::atomic<unsigned int> gWavStreamSeekCallCapUs(WAVSTREAM_SEEK_CALL_CAP_US);

	// True once the time cap of this call (aDeadline; NULL: none) has passed.
	// Never on the web: performance.now(), which the clock reads there, may
	// not exist in the AudioWorklet scope.
	static bool seekDeadlinePassed(const std::chrono::steady_clock::time_point *aDeadline)
	{
#if defined(__EMSCRIPTEN__)
		(void)aDeadline;
		return false;
#else
		return aDeadline != NULL && std::chrono::steady_clock::now() >= *aDeadline;
#endif
	}

	// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
	// One step of an MP3 seek through the table, sample exact (see
	// startMp3TableSeek_internal()). dr_mp3's own table seek is not: it
	// restarts the decoder at a seek point and drops, without counting them,
	// the first MP3 frames whose bit reservoir is gone, so it lands a frame or
	// two late and the voice never reaches its end. Instead decode, from a
	// seek point well before, on to the byte offset of aPoint, whose frame
	// index the table gives, and set the frame count there. Decodes MP3
	// frames while aBudget lasts, charging each, and zeroes it once
	// aDeadline has passed (checked after each frame). Returns 1 when in
	// sync, 0 when the budget ran out first, -1 when the stream does not line
	// up with the table.
	static int mp3SyncToSeekPoint(drmp3 *aMp3, const drmp3_seek_point &aPoint, long long &aBudget,
		const std::chrono::steady_clock::time_point *aDeadline)
	{
		// The MP3 frame at aPoint.seekPosInBytes is the first of the
		// mp3FramesToDiscard frames before the one holding
		// aPoint.pcmFrameIndex; the last of them starts at PCM frame next.
		// That count is DRMP3_SEEK_LEADING_MP3_FRAMES (2 by default) as the
		// table was built with; the macro itself is only defined where dr_mp3's
		// implementation is compiled, so read it from the table.
		const drmp3_uint64 next = aPoint.pcmFrameIndex - aPoint.pcmFramesToDiscard;
		while (aBudget > 0)
		{
			drmp3_uint64 left = aMp3->pcmFramesRemainingInMP3Frame;
			if (mp3Skip(aMp3, left) != left)
				return -1;
			const drmp3_uint64 at = aMp3->streamCursor - aMp3->dataSize;
			if (at == aPoint.seekPosInBytes)
			{
				// Decode the leading frames up to the one starting at next.
				for (drmp3_uint32 k = 1; k < aPoint.mp3FramesToDiscard; k++)
				{
					left = aMp3->pcmFramesRemainingInMP3Frame;
					if (mp3Skip(aMp3, left) != left || mp3Skip(aMp3, 1) != 1)
						return -1;
					aBudget -= kMp3FrameCost;
				}
				aMp3->currentPCMFrame = next - aMp3->pcmFramesRemainingInMP3Frame;
				return 1;
			}
			if (at > aPoint.seekPosInBytes || mp3Skip(aMp3, 1) != 1)
				return -1;
			aBudget -= kMp3FrameCost;
			if (aBudget > 0 && seekDeadlinePassed(aDeadline))
				aBudget = 0;
		}
		return 0;
	}

	// ###### flutter_soloud local patch (bounded wavstream seek) ######
	// Seeks dr_flac to aFrame, the frame right after a seek point, through its
	// seek table: that decodes only the FLAC frame at the seek point (dr_flac
	// picks the last seek point before the target, and the target is in its
	// frame). If the table cannot serve it, dr_flac would fall back to a
	// brute-force seek, decoding from the start of the stream or from where it
	// is; not here, where the caller then decodes in bounded steps instead.
	static bool flacSeekThroughTable(drflac *aFlac, drflac_uint64 aFrame)
	{
		const drflac_bool32 noBruteForce = aFlac->_noBruteForceSeek;
		aFlac->_noBruteForceSeek = DRFLAC_TRUE;
		const drflac_bool32 ok = drflac_seek_to_pcm_frame(aFlac, aFrame);
		aFlac->_noBruteForceSeek = noBruteForce;
		return ok && aFlac->currentPCMFrame == aFrame;
	}

	// ###### flutter_soloud local patch (bounded wavstream seek) ######
	// Frames of pending seek work getAudio() decodes and drops per call, at
	// most (the time cap may stop it earlier): about 0.5-1 ms of decoding on a
	// fast desktop CPU (measured through WavStream on an i9-13900K, in frames
	// per ms: MP3 ~88,000, FLAC ~110,000-127,000, Opus ~41,000, IMA ADPCM
	// ~66,000, MS ADPCM ~31,000), so ~1-2 ms on a slower desktop or in
	// WebAssembly (where it is the only bound): well inside a mix block, with
	// room for the other voices. A call comes every SAMPLE_GRANULARITY (512)
	// source frames of playback, so on a fast CPU the work goes on at 64-128
	// times real time; a seek needing more than one call plays that much
	// silence first (e.g. ~0.3 s for an MP3 seek 30 s past where decoding
	// starts; where the time cap binds, proportionally longer: ~3-4 s on a
	// CPU 10-15 times slower). With
	// a seek table (MP3, FLAC) the work is at most a table spacing plus a few
	// frames, and usually fits one call.
	unsigned int WavStreamInstance::seekSkipBudget_internal() const
	{
		switch (mParent->mFiletype)
		{
		case WAVSTREAM_OGG: // Opus
		case WAVSTREAM_WAV: // ADPCM
			return 32768;
		default: // MP3, FLAC
			return 65536;
		}
	}

	unsigned long long WavStreamInstance::skipFrames_internal(unsigned long long aFrames)
	{
		if (mParent->mFiletype == WAVSTREAM_MP3)
			return mp3Skip(mCodec.mMp3, aFrames);
		// Small, as this runs inside getAudio() on the audio thread.
		float tmp[1024];
		unsigned long long done = 0;
		while (done < aFrames)
		{
			unsigned int chunk = 0;
			unsigned long long got = 0;
			switch (mParent->mFiletype)
			{
#if !defined(NO_XIPH_LIBS)
			case WAVSTREAM_OGG:
				// Channel-planar, at most MAX_CHANNELS channels.
				chunk = 1024 / MAX_CHANNELS;
				if (aFrames - done < chunk)
					chunk = (unsigned int)(aFrames - done);
				got = mCodec.mOgg->read(tmp, chunk, chunk);
				break;
#endif
			case WAVSTREAM_FLAC:
				chunk = 1024 / (mCodec.mFlac->channels ? mCodec.mFlac->channels : 1);
				if (aFrames - done < chunk)
					chunk = (unsigned int)(aFrames - done);
				got = drflac_read_pcm_frames_f32(mCodec.mFlac, chunk, tmp);
				break;
			case WAVSTREAM_WAV:
				chunk = 1024 / (mCodec.mWav->channels ? mCodec.mWav->channels : 1);
				if (chunk == 0)
					return done; // more channels than the buffer holds: cannot skip
				if (aFrames - done < chunk)
					chunk = (unsigned int)(aFrames - done);
				got = drwav_read_pcm_frames_f32(mCodec.mWav, chunk, tmp);
				break;
			default:
				return done;
			}
			done += got;
			if (got < chunk)
				break;
		}
		return done;
	}

	bool WavStreamInstance::startMp3TableSeek_internal(unsigned long long aFrame)
	{
		drmp3 *mp3 = mCodec.mMp3;
		const drmp3_seek_point *points = mp3->pSeekPoints;
		const drmp3_uint32 count = mp3->seekPointCount;
		if (points == NULL || count == 0 || aFrame < points[0].pcmFrameIndex)
			return false;
		// The table point serving the target.
		drmp3_uint32 i = 0;
		while (i + 1 < count && points[i + 1].pcmFrameIndex <= aFrame)
			i++;
		const drmp3_uint64 next = points[i].pcmFrameIndex - points[i].pcmFramesToDiscard;
		if (next < points[0].pcmFrameIndex + kMp3SyncLead)
			return false; // too close to the start
		// Start decoding at the last table point at least kMp3SyncLead frames
		// before: dr_mp3 seeks to a table point's own frame with a few MP3
		// frames of decoding, where a seek to any other frame decodes on from
		// the point before it (up to a table spacing, here in seek()).
		drmp3_uint32 j = i;
		while (j > 0 && points[j].pcmFrameIndex > next - kMp3SyncLead)
			j--;
		if (!drmp3_seek_to_pcm_frame(mp3, points[j].pcmFrameIndex))
			return false;
		mSeekMp3Point = i;
		mSeekMp3Frame = aFrame;
		mSeekWork = SEEK_WORK_MP3_SYNC;
		return true;
	}

	void WavStreamInstance::seekFromStart_internal(unsigned int aPos)
	{
		// Each is a file seek and a decoder reset, no decoding.
		switch (mParent->mFiletype)
		{
#if !defined(NO_XIPH_LIBS)
		case WAVSTREAM_OGG:
			mCodec.mOgg->rewind();
			break;
#endif
		case WAVSTREAM_FLAC:
			drflac_seek_to_pcm_frame(mCodec.mFlac, 0);
			break;
		case WAVSTREAM_MP3:
			// Reading on skips the encoder delay by itself.
			drmp3_seek_to_pcm_frame(mCodec.mMp3, 0);
			break;
		case WAVSTREAM_WAV:
			drwav_seek_to_pcm_frame(mCodec.mWav, 0);
			break;
		}
		mSeekSkipFrames = aPos;
		mSeekWork = aPos > 0 ? SEEK_WORK_SKIP : SEEK_WORK_NONE;
	}

	bool WavStreamInstance::advanceSeek_internal()
	{
		long long budget = (long long)seekSkipBudget_internal();
		// This call's time cap: from now, or for the first call after a
		// seek() from the start of that seek().
		std::chrono::steady_clock::time_point deadline;
		const std::chrono::steady_clock::time_point *deadlinePtr = NULL;
#if !defined(__EMSCRIPTEN__)
		const unsigned int capUs = gWavStreamSeekCallCapUs.load(std::memory_order_relaxed);
		if (capUs > 0)
		{
			deadline = (mSeekClockFromSeek ? mSeekClockStart : std::chrono::steady_clock::now()) +
				std::chrono::microseconds(capUs);
			deadlinePtr = &deadline;
		}
#endif
		mSeekClockFromSeek = false;
		if (mSeekWork == SEEK_WORK_MP3_SYNC)
		{
			drmp3 *mp3 = mCodec.mMp3;
			const int synced = mp3SyncToSeekPoint(mp3, mp3->pSeekPoints[mSeekMp3Point], budget, deadlinePtr);
			if (synced > 0 && mp3->currentPCMFrame <= mSeekMp3Frame)
			{
				mSeekSkipFrames = mSeekMp3Frame - mp3->currentPCMFrame;
				mSeekWork = mSeekSkipFrames > 0 ? SEEK_WORK_SKIP : SEEK_WORK_NONE;
			}
			else if (synced != 0)
			{
				// The stream does not line up with the table here: decode
				// from the start of the stream instead.
				seekFromStart_internal(mSeekTargetFrame);
			}
		}
		// Skip in chunks, up to the budget, until the time cap has passed.
		// The MP3 sync above zeroes the budget when it hit the cap.
		unsigned long long left = budget > 0 ? (unsigned long long)budget : 0;
		while (mSeekWork == SEEK_WORK_SKIP && left > 0)
		{
			unsigned long long n = mSeekSkipFrames < left ? mSeekSkipFrames : left;
			if (n > kSeekChunkFrames)
				n = kSeekChunkFrames;
			const unsigned long long got = skipFrames_internal(n);
			mSeekSkipFrames -= got;
			left -= got;
			if (got < n)
			{
				// The stream ended before the target, though the length it
				// declared reaches past it: nothing left to play.
				mSeekWork = SEEK_WORK_NONE;
				mSeekAtEnd = true;
			}
			else if (mSeekSkipFrames == 0)
			{
				mSeekWork = SEEK_WORK_NONE;
			}
			else if (seekDeadlinePassed(deadlinePtr))
			{
				break;
			}
		}
		return mSeekWork == SEEK_WORK_NONE;
	}

	result WavStreamInstance::seek(double aSeconds, float *mScratch, unsigned int mScratchSize)
	{
		if (mCodec.mOgg)
		{
			int pos = (int)floor(mBaseSamplerate * aSeconds);
			double newPosition;

			// ###### flutter_soloud local patch (bounded wavstream seek) ######
			// A new seek replaces whatever work an earlier one left. The
			// bounded paths below set the target and their work (see above).
			// The next getAudio() counts its time cap from here.
#if !defined(__EMSCRIPTEN__)
			mSeekClockStart = std::chrono::steady_clock::now();
			mSeekClockFromSeek = true;
#endif
			mSeekWork = SEEK_WORK_NONE;
			mSeekSkipFrames = 0;
			mSeekAtEnd = false;
			const unsigned int target = pos > 0 ? (unsigned int)pos : 0;
			mSeekTargetFrame = target;
			mSeekTargetPosition = float(target / mBaseSamplerate);

			switch (mParent->mFiletype)
			{
#if !defined(NO_XIPH_LIBS)
			case WAVSTREAM_OGG:
				// ###### flutter_soloud local patch (bounded wavstream seek) ######
				// Opus seeks by decoding from the start (MBOggDecoder::seekOpus):
				// rewind, and decode on in bounded steps.
				if (mCodec.mOgg->getCodecType() == MBOggDecoder::CODEC_OPUS)
				{
					mOffset = target;
					mStreamPosition = mSeekTargetPosition;
					mStreamEnded = false;
					if (mParent->mSampleCount > 0 && target >= mParent->mSampleCount)
						mSeekAtEnd = true;
					else
						seekFromStart_internal(target);
					return 0;
				}
				mCodec.mOgg->seek(pos);
				mOffset = pos;
				newPosition = float(pos / mBaseSamplerate);
				mStreamPosition = newPosition;
				mStreamEnded = false;
				return 0;
#endif
			case WAVSTREAM_FLAC:
			{
				// ###### flutter_soloud local patch (bounded wavstream seek) ######
				drflac *flac = mCodec.mFlac;
				mOffset = target;
				mStreamPosition = mSeekTargetPosition;
				if (flac->totalPCMFrameCount > 0 && target >= flac->totalPCMFrameCount)
				{
					mSeekAtEnd = true;
					return 0;
				}
				// The last usable seek point before the target, as dr_flac's
				// seek-table seek would pick it.
				const drflac_seekpoint *point = NULL;
				drflac_uint32 k;
				for (k = 0; flac->pSeekpoints != NULL && k < flac->seekpointCount; k++)
				{
					const drflac_seekpoint &p = flac->pSeekpoints[k];
					if (p.firstPCMFrame >= target)
						break;
					if (p.pcmFrameCount != 0 && p.pcmFrameCount <= flac->maxBlockSizeInPCMFrames &&
						(flac->totalPCMFrameCount == 0 || p.firstPCMFrame <= flac->totalPCMFrameCount))
						point = &p;
				}
				const drflac_uint64 cur = flac->currentPCMFrame;
				if (target >= cur && (point == NULL || point->firstPCMFrame + 1 <= cur))
				{
					// Ahead, with no seek point in between: decode on from here.
					mSeekSkipFrames = target - cur;
					mSeekWork = mSeekSkipFrames > 0 ? SEEK_WORK_SKIP : SEEK_WORK_NONE;
				}
				else if (point != NULL && flacSeekThroughTable(flac, point->firstPCMFrame + 1))
				{
					mSeekSkipFrames = target - (point->firstPCMFrame + 1);
					mSeekWork = mSeekSkipFrames > 0 ? SEEK_WORK_SKIP : SEEK_WORK_NONE;
				}
				else
				{
					seekFromStart_internal(target);
				}
				return 0;
			}
			case WAVSTREAM_MP3:
			{
				// ###### flutter_soloud local patch (bounded wavstream seek) ######
				// With the source's seek table bound (WavStream::loadmp3),
				// decode from a table point near the target; without one,
				// from the start of the stream, or on from where the decoder
				// is if the target is ahead. pos counts output frames, the
				// table and the decoder count from the start of the stream
				// with the encoder delay included.
				drmp3 *mp3 = mCodec.mMp3;
				mOffset = target;
				mStreamPosition = mSeekTargetPosition;
				if (target >= mParent->mSampleCount)
				{
					// Leave the decoder where it is, in sync for the next seek.
					mSeekAtEnd = true;
					return 0;
				}
				const drmp3_uint64 frame = (drmp3_uint64)target + mp3->delayInPCMFrames;
				// Reading from before the delay skips it first.
				const drmp3_uint64 cur = mp3->currentPCMFrame > mp3->delayInPCMFrames ?
					mp3->currentPCMFrame : mp3->delayInPCMFrames;
				if (mp3->pSeekPoints == NULL && frame >= cur)
				{
					mSeekSkipFrames = frame - cur;
					mSeekWork = mSeekSkipFrames > 0 ? SEEK_WORK_SKIP : SEEK_WORK_NONE;
				}
				else if (!startMp3TableSeek_internal(frame))
				{
					// Near the start, or no table: decode from the start.
					seekFromStart_internal(target);
				}
				return 0;
			}
			case WAVSTREAM_WAV:
				// ###### flutter_soloud local patch (bounded wavstream seek) ######
				// ADPCM seeks by decoding, from the start or from where it is.
				if (mCodec.mWav->translatedFormatTag == DR_WAVE_FORMAT_ADPCM ||
					mCodec.mWav->translatedFormatTag == DR_WAVE_FORMAT_DVI_ADPCM)
				{
					drwav *wav = mCodec.mWav;
					mOffset = target;
					mStreamPosition = mSeekTargetPosition;
					if (target >= wav->totalPCMFrameCount)
					{
						mSeekAtEnd = true;
						return 0;
					}
					if (target >= wav->readCursorInPCMFrames)
					{
						mSeekSkipFrames = target - wav->readCursorInPCMFrames;
						mSeekWork = mSeekSkipFrames > 0 ? SEEK_WORK_SKIP : SEEK_WORK_NONE;
					}
					else
					{
						seekFromStart_internal(target);
					}
					return 0;
				}
				drwav_seek_to_pcm_frame(mCodec.mWav, pos);
				mOffset = pos;
				mStreamPosition = float(pos / mBaseSamplerate);
				return 0;
			default:
				break;
			}
		}
		else
		{
			return AudioSourceInstance::seek(aSeconds, mScratch, mScratchSize);
		}

		return NOT_IMPLEMENTED;
	}


	result WavStreamInstance::rewind()
	{
		// ###### flutter_soloud local patch (bounded wavstream seek) ######
		// Each rewind below is a file seek and a decoder reset; it replaces
		// whatever work a seek left.
		mSeekWork = SEEK_WORK_NONE;
		mSeekSkipFrames = 0;
		mSeekAtEnd = false;
		mSeekClockFromSeek = false;
		switch (mParent->mFiletype)
		{
#if !defined(NO_XIPH_LIBS)
		case WAVSTREAM_OGG:
			if (mCodec.mOgg)
			{
				mCodec.mOgg->rewind();
			}
			break;
#endif
		case WAVSTREAM_FLAC:
			if (mCodec.mFlac)
			{
				drflac_seek_to_pcm_frame(mCodec.mFlac, 0);
			}
			break;
		case WAVSTREAM_MP3:
			if (mCodec.mMp3)
			{
				drmp3_seek_to_pcm_frame(mCodec.mMp3, 0);
			}
			break;
		case WAVSTREAM_WAV:
			if (mCodec.mWav)
			{
				drwav_seek_to_pcm_frame(mCodec.mWav, 0);
			}
			break;
		}
		mOffset = 0;
		mStreamPosition = 0.0f;
		mStreamEnded = false;
		return 0;
	}

	bool WavStreamInstance::hasEnded()
	{
		// ###### flutter_soloud local patch (wavstream missing file) ######
		// No file: it could not be opened, or no decoder accepted it.
		// getAudio() plays nothing, so the voice ends instead of staying
		// silent forever.
		if (mFile == NULL)
		{
			return 1;
		}
		// ###### flutter_soloud local patch (bounded wavstream seek) ######
		// A seek at or past the end, or one that ran into the end.
		if (mSeekAtEnd)
		{
			return 1;
		}
		// For OGG streams, use the stream ended flag since mSampleCount may not
		// match actual decoded samples (empty final page in OGG), and for OGG/FLAC
		// the STREAMINFO total_samples can be 0 so we must not check mOffset >= mSampleCount.
		if (mParent->mFiletype == WAVSTREAM_OGG)
		{
			return mStreamEnded;
		}
		if (mOffset >= mParent->mSampleCount)
		{
			return 1;
		}
		return 0;
	}

	WavStream::WavStream()
	{
		mFilename = 0;
		mSampleCount = 0;
		mFiletype = WAVSTREAM_WAV;
		mMemFile = 0;
		mStreamFile = 0;
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		mMp3SeekPoints = 0;
		mMp3SeekPointCount = 0;
	}
	
	WavStream::~WavStream()
	{
		stop();
		delete[] mFilename;
		delete mMemFile;
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		delete[] mMp3SeekPoints;
	}
	
	// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
	// Instances bind the seek table, so stop them before freeing it (as the
	// destructor does) instead of leaving them pointing at freed memory.
	void WavStream::freeMp3SeekTable()
	{
		if (mMp3SeekPoints)
		{
			stop();
			delete[] mMp3SeekPoints;
			mMp3SeekPoints = 0;
			mMp3SeekPointCount = 0;
		}
	}

#define MAKEDWORD(a,b,c,d) (((d) << 24) | ((c) << 16) | ((b) << 8) | (a))

	result WavStream::loadwav(File * fp)
	{
		fp->seek(0);
		drwav decoder;

		if (!drwav_init(&decoder, drwav_read_func, drwav_seek_func, NULL, (void*)fp, NULL))
			return FILE_LOAD_FAILED;

		mChannels = decoder.channels;
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}

		mBaseSamplerate = (float)decoder.sampleRate;
		mSampleCount = (unsigned int)decoder.totalPCMFrameCount;
		mFiletype = WAVSTREAM_WAV;
		drwav_uninit(&decoder);

		return SO_NO_ERROR;
	}

	result WavStream::loadogg(File * fp)
	{
#if defined(NO_XIPH_LIBS)
        printf("[WavStream::loadogg] NO_XIPH_LIBS defined, returning FILE_LOAD_FAILED\n");
		return FILE_LOAD_FAILED;
#else
		fp->seek(0);
		MBOggDecoder decoder;
		if (!decoder.open(fp))
		{
            printf("[WavStream::loadogg] decoder.open failed\n");
			return FILE_LOAD_FAILED;
		}
		mChannels = decoder.getChannels();
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}
		mBaseSamplerate = (float)decoder.getSampleRate();
		int samples = (int)decoder.getLengthInSamples();
		mFiletype = WAVSTREAM_OGG;
		mSampleCount = samples;
		return 0;
#endif
	}

	result WavStream::loadflac(File * fp)
	{
		fp->seek(0);
		drflac* decoder = drflac_open(drflac_read_func, drflac_seek_func, NULL, (void*)fp, NULL);

		if (decoder == NULL)
			return FILE_LOAD_FAILED;
		
		mChannels = decoder->channels;
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}

		mBaseSamplerate = (float)decoder->sampleRate;
		mSampleCount = (unsigned int)decoder->totalPCMFrameCount;
		mFiletype = WAVSTREAM_FLAC;
		drflac_close(decoder);

		return SO_NO_ERROR;
	}

	result WavStream::loadmp3(File * fp)
	{
		fp->seek(0);
		drmp3 decoder;
		if (!drmp3_init(&decoder, drmp3_read_func, drmp3_seek_func, NULL, NULL, (void*)fp, NULL))
			return FILE_LOAD_FAILED;


		mChannels = decoder.channels;
		if (mChannels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}

		drmp3_uint64 samples = drmp3_get_pcm_frame_count(&decoder);

		mBaseSamplerate = (float)decoder.sampleRate;
		mSampleCount = (unsigned int)samples;
		mFiletype = WAVSTREAM_MP3;

		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		// Build the seek table once per source, here on the loading thread
		// rather than under the audio mutex, for every instance to bind (see
		// mp3SeekThroughTable). It costs two header-only passes over the file
		// and at most 4096 points (96 KiB). Points hold byte offsets and frame
		// indices from the start of the file, so they suit any decoder reading
		// the same bytes. With no table, seeks work as before.
		freeMp3SeekTable();
		drmp3_uint32 count = 4096;
		drmp3_seek_point *points = new drmp3_seek_point[count];
		if (drmp3_calculate_seek_points(&decoder, &count, points) && count > 0)
		{
			mMp3SeekPoints = new drmp3_seek_point[count];
			memcpy(mMp3SeekPoints, points, count * sizeof(drmp3_seek_point));
			mMp3SeekPointCount = count;
		}
		delete[] points;

		drmp3_uninit(&decoder);

		return SO_NO_ERROR;
	}

	result WavStream::load(const char *aFilename)
	{
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		freeMp3SeekTable();
		delete[] mFilename;
		delete mMemFile;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;
		DiskFile fp;
		int res = fp.open(aFilename);
		if (res != SO_NO_ERROR)
			return res;
		
		int len = (int)strlen(aFilename);
		mFilename = new char[len+1];		
		memcpy(mFilename, aFilename, len);
		mFilename[len] = 0;
		
		res = parse(&fp);

		if (res != SO_NO_ERROR)
		{
			delete[] mFilename;
			mFilename = 0;
			return res;
		}

		return 0;
	}

	result WavStream::loadMem(const unsigned char *aData, unsigned int aDataLen, bool aCopy, bool aTakeOwnership)
	{
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		freeMp3SeekTable();
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		if (aData == NULL || aDataLen == 0)
			return INVALID_PARAMETER;

		MemoryFile *mf = new MemoryFile();
		int res = mf->openMem(aData, aDataLen, aCopy, aTakeOwnership);
		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		res = parse(mf);

		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		mMemFile = mf;

		return 0;
	}

	result WavStream::loadToMem(const char *aFilename)
	{
		DiskFile df;
		int res = df.open(aFilename);
		if (res == SO_NO_ERROR)
		{
			res = loadFileToMem(&df);
		}
		return res;
	}

	result WavStream::loadFile(File *aFile)
	{
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		freeMp3SeekTable();
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		int res = parse(aFile);

		if (res != SO_NO_ERROR)
		{
			return res;
		}

		mStreamFile = aFile;

		return 0;
	}

	result WavStream::loadFileToMem(File *aFile)
	{
		// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
		freeMp3SeekTable();
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		MemoryFile *mf = new MemoryFile();
		int res = mf->openFileToMem(aFile);
		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		res = parse(mf);

		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		mMemFile = mf;

		return res;
	}


	result WavStream::parse(File *aFile)
	{
		int tag = aFile->read32();
		int res = SO_NO_ERROR;
		if (tag == MAKEDWORD('O', 'g', 'g', 'S'))
		{
            printf("[WavStream::parse] -> loadogg\n");
			res = loadogg(aFile);
		}
		else
		if (tag == MAKEDWORD('R', 'I', 'F', 'F'))
		{
            printf("[WavStream::parse] -> loadwav\n");
			res = loadwav(aFile);
		}
		else
		if (tag == MAKEDWORD('f', 'L', 'a', 'C'))
		{
            printf("[WavStream::parse] -> loadflac\n");
			res = loadflac(aFile);
		}
		else
		if (loadmp3(aFile) == SO_NO_ERROR)
		{
            printf("[WavStream::parse] -> loadmp3\n");
			res = SO_NO_ERROR;
		}
		else
		{
            printf("[WavStream::parse] -> FILE_LOAD_FAILED\n");
			res = FILE_LOAD_FAILED;
		}
		return res;
	}

	AudioSourceInstance *WavStream::createInstance()
	{
		return new WavStreamInstance(this);
	}

	double WavStream::getLength()
	{
		if (mBaseSamplerate == 0)
			return 0;
		return mSampleCount / mBaseSamplerate;
	}
};
