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
			df->open(aParent->mFilename);
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

	// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
	// Seeks to PCM frame aFrame (counted from the start of the stream, encoder
	// delay included) through the bound seek table, sample exact. dr_mp3's own
	// table seek is not: it restarts the decoder at a seek point and drops,
	// without counting them, the first MP3 frames whose bit reservoir is gone,
	// so it lands a frame or two late and the voice never reaches its end.
	// Instead jump to a seek point well before the target, decode on to the
	// byte offset of the seek point serving the target, whose frame index the
	// table gives, set the frame count there and decode on to the target. A
	// target at or past the end of the stream leaves the decoder at the end.
	// Returns false if the target is too close to the start for this or the
	// stream does not line up with the table.
	static bool mp3SeekThroughTable(drmp3 *aMp3, drmp3_uint64 aFrame)
	{
		const drmp3_uint64 lead = 16 * 1152; // dropped frames and decoder warm-up
		drmp3_seek_point *points = aMp3->pSeekPoints;
		drmp3_uint32 i = 0;
		while (i + 1 < aMp3->seekPointCount && points[i + 1].pcmFrameIndex <= aFrame)
			i++;
		// The MP3 frame at points[i].seekPosInBytes is the first of the
		// mp3FramesToDiscard frames before the one holding
		// points[i].pcmFrameIndex; the last of them starts at PCM frame next.
		// That count is DRMP3_SEEK_LEADING_MP3_FRAMES (2 by default) as the
		// table was built with; the macro itself is only defined where dr_mp3's
		// implementation is compiled, so read it from the table.
		drmp3_uint64 next = points[i].pcmFrameIndex - points[i].pcmFramesToDiscard;
		if (aFrame < points[0].pcmFrameIndex || next < points[0].pcmFrameIndex + lead)
			return false;
		if (!drmp3_seek_to_pcm_frame(aMp3, next - lead))
			return false;
		for (;;)
		{
			drmp3_uint64 left = aMp3->pcmFramesRemainingInMP3Frame;
			if (mp3Skip(aMp3, left) != left)
				return false;
			drmp3_uint64 at = aMp3->streamCursor - aMp3->dataSize;
			if (at == points[i].seekPosInBytes)
				break;
			if (at > points[i].seekPosInBytes || mp3Skip(aMp3, 1) != 1)
				return false;
		}
		// Decode the leading frames up to the one starting at next.
		for (drmp3_uint32 k = 1; k < points[i].mp3FramesToDiscard; k++)
		{
			drmp3_uint64 left = aMp3->pcmFramesRemainingInMP3Frame;
			if (mp3Skip(aMp3, left) != left || mp3Skip(aMp3, 1) != 1)
				return false;
		}
		aMp3->currentPCMFrame = next - aMp3->pcmFramesRemainingInMP3Frame;
		// The frame count is in sync now, so this comes up short only when the
		// decoder runs out of frames: the target is at or past the end, and the
		// decoder is at the end, as decoding from the start would leave it.
		mp3Skip(aMp3, aFrame - aMp3->currentPCMFrame);
		return true;
	}

	result WavStreamInstance::seek(double aSeconds, float *mScratch, unsigned int mScratchSize)
	{
		if (mCodec.mOgg)
		{
			int pos = (int)floor(mBaseSamplerate * aSeconds);
			double newPosition;

			switch (mParent->mFiletype)
			{
#if !defined(NO_XIPH_LIBS)
			case WAVSTREAM_OGG:
				mCodec.mOgg->seek(pos);
				mOffset = pos;
				newPosition = float(pos / mBaseSamplerate);
				mStreamPosition = newPosition;
				mStreamEnded = false;
				return 0;
#endif
			case WAVSTREAM_FLAC:
				drflac_seek_to_pcm_frame(mCodec.mFlac, pos);
				mOffset = pos;
				mStreamPosition = float(pos / mBaseSamplerate);
				return 0;
			case WAVSTREAM_MP3:
				// ###### flutter_soloud local patch (wavstream mp3 seek table) ######
				// Without a seek table dr_mp3 seeks backward by decoding from the
				// start of the stream, here under the audio mutex, so on a long
				// file every voice stalls. With the source's table bound, decode
				// from a seek point near the target instead. pos counts output
				// frames, the table counts from the start of the stream with the
				// encoder delay included.
				if (!mCodec.mMp3->pSeekPoints)
				{
					drmp3_seek_to_pcm_frame(mCodec.mMp3, pos);
				}
				else if (!mp3SeekThroughTable(mCodec.mMp3, (drmp3_uint64)pos + mCodec.mMp3->delayInPCMFrames))
				{
					// Near the start, or the stream did not line up with the
					// table: decode from the start of the stream.
					drmp3_seek_to_pcm_frame(mCodec.mMp3, 0);
					mp3Skip(mCodec.mMp3, pos);
				}
				mOffset = pos;
				mStreamPosition = float(pos / mBaseSamplerate);
				return 0;
			case WAVSTREAM_WAV:
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
