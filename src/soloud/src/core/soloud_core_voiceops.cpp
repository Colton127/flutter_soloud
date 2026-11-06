/*
SoLoud audio engine
Copyright (c) 2013-2015 Jari Komppa

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

#include "soloud.h"
#include "soloud_internal.h"
#include <math.h> // floor (voice control)
#include <chrono> // steady_clock (voice control)
#include <thread> // yield (voice control)

#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#endif

// Direct voice operations (no mutexes - called from other functions)

namespace SoLoud
{
	result Soloud::setVoiceRelativePlaySpeed_internal(unsigned int aVoice, float aSpeed)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		if (aSpeed <= 0.0f)
		{
			return INVALID_PARAMETER;
		}

		if (mVoice[aVoice])
		{
			mVoice[aVoice]->mSetRelativePlaySpeed = aSpeed;
			updateVoiceRelativePlaySpeed_internal(aVoice);
		}

		return 0;
	}

	void Soloud::setVoicePause_internal(unsigned int aVoice, int aPause)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		mActiveVoiceDirty = true;
		if (mVoice[aVoice])
		{
			mVoice[aVoice]->mPauseScheduler.mActive = 0;
			const bool wasPaused =
				(mVoice[aVoice]->mFlags & AudioSourceInstance::PAUSED) != 0;

			if (aPause)
			{
				mVoice[aVoice]->mFlags |= AudioSourceInstance::PAUSED;
				if (!wasPaused)
					mVoiceInactiveCallbackPending = true;
			}
			else
			{
				mVoice[aVoice]->mFlags &= ~AudioSourceInstance::PAUSED;
			}
		}
	}

	void Soloud::setVoicePan_internal(unsigned int aVoice, float aPan)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		if (mVoice[aVoice])
		{
			mVoice[aVoice]->mPan = aPan;
			float l = (float)cos((aPan + 1) * M_PI / 4);
			float r = (float)sin((aPan + 1) * M_PI / 4);
			mVoice[aVoice]->mChannelVolume[0] = l;
			mVoice[aVoice]->mChannelVolume[1] = r;
			if (mVoice[aVoice]->mChannels == 4)
			{
				mVoice[aVoice]->mChannelVolume[2] = l;
				mVoice[aVoice]->mChannelVolume[3] = r;
			}
			if (mVoice[aVoice]->mChannels == 6)
			{
				mVoice[aVoice]->mChannelVolume[2] = 1.0f / (float)sqrt(2.0f);
				mVoice[aVoice]->mChannelVolume[3] = 1;
				mVoice[aVoice]->mChannelVolume[4] = l;
				mVoice[aVoice]->mChannelVolume[5] = r;
			}
			if (mVoice[aVoice]->mChannels == 8)
			{
				mVoice[aVoice]->mChannelVolume[2] = 1.0f / (float)sqrt(2.0f);
				mVoice[aVoice]->mChannelVolume[3] = 1;
				mVoice[aVoice]->mChannelVolume[4] = l;
				mVoice[aVoice]->mChannelVolume[5] = r;
				mVoice[aVoice]->mChannelVolume[6] = l;
				mVoice[aVoice]->mChannelVolume[7] = r;
			}
		}
	}

	void Soloud::setVoiceVolume_internal(unsigned int aVoice, float aVolume)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		mActiveVoiceDirty = true;
		if (mVoice[aVoice])
		{
			mVoice[aVoice]->mSetVolume = aVolume;
			updateVoiceVolume_internal(aVoice);
		}
	}

	void Soloud::stopVoice_internal(unsigned int aVoice)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		mActiveVoiceDirty = true;
		if (mVoice[aVoice])
		{
			mVoiceInactiveCallbackPending = true;
			// ###### flutter_soloud local patch (retroactive re-mix) ######
			// Journal the death so a later rollback does not resurrect this
			// voice in the rewritten window (no-op when the ring is disabled
			// or a re-mix is in progress -- the re-mix reproduces journaled
			// deaths, it does not create new ones).
			journalDeath_internal(aVoice, mVoice[aVoice]->mPlayIndex);
			// Delete via temporary variable to avoid recursion
			AudioSourceInstance * v = mVoice[aVoice];
			const unsigned int playIndex = v->mPlayIndex;
			mVoice[aVoice] = 0;
			// ###### flutter_soloud local patch (non-blocking voice control) ######
			// Invalidate the handle for the lock-free getters right away: the
			// slot may be past mHighestVoice by the time the mutex is
			// released, and unlockAudioMutex_internal() would not revisit it.
			publishVoiceState_internal(aVoice);

			// Clear any resample data channel assigned to this voice
			unsigned int i;
			for (i = 0; i < mMaxActiveVoices; i++)
			{
				if (mResampleDataOwner[i] == v)
				{
					mResampleDataOwner[i] = NULL;
				}
			}

			// Queue rather than call: the audio mutex is held here (see
			// the assert above) and the embedder callback must not run
			// under it. unlockAudioMutex_internal() dispatches these.
			// mEndedVoiceQueue holds VOICE_COUNT entries and a voice can
			// only be queued once per stop, so it cannot overflow.
			if (mEndedVoiceCount < VOICE_COUNT)
			{
				handle endedHandle = (aVoice + 1) | (playIndex << 12);
				mEndedVoiceQueue[mEndedVoiceCount++] = endedHandle;
			}

#ifdef __EMSCRIPTEN__
			// Freeing the instance takes the heap lock, and a contended lock
			// on the AudioWorklet rendering thread lowers to a futex wait,
			// which Emscripten aborts on (futex waits are illegal there).
			// Defer the deletion to the main browser thread; it is drained by
			// unlockAudioMutex_internal() (see mPendingVoiceFree).
			if (!emscripten_is_main_browser_thread() &&
				mPendingVoiceFreeCount < VOICE_COUNT)
			{
				mPendingVoiceFree[mPendingVoiceFreeCount++] = v;
			}
			else
#endif
			delete v;
		}
	}

	void Soloud::updateVoiceRelativePlaySpeed_internal(unsigned int aVoice)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		mVoice[aVoice]->mOverallRelativePlaySpeed = m3dData[aVoice].mDopplerValue * mVoice[aVoice]->mSetRelativePlaySpeed;
		mVoice[aVoice]->mSamplerate = mVoice[aVoice]->mBaseSamplerate * mVoice[aVoice]->mOverallRelativePlaySpeed;
	}

	void Soloud::updateVoiceVolume_internal(unsigned int aVoice)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		mVoice[aVoice]->mOverallVolume = mVoice[aVoice]->mSetVolume * m3dData[aVoice].m3dVolume;
		if (mVoice[aVoice]->mFlags & AudioSourceInstance::PAUSED)
		{
			int i;
			for (i = 0; i < MAX_CHANNELS; i++)
			{
				mVoice[aVoice]->mCurrentChannelVolume[i] = mVoice[aVoice]->mChannelVolume[i] * mVoice[aVoice]->mOverallVolume;
			}
		}
	}
}

// ###### flutter_soloud local patch (non-blocking voice control) ######
//
// Voice control without the audio mutex. The mixer holds that mutex for a
// whole mix block, which runs every voice's getAudio() -- file reads and
// decoding for streamed sources -- and a source seek ran under it on the
// caller's thread (an MP3 stream seeks by decoding). Control calls made from
// the UI thread waited for all of it. See the overview above
// PublishedVoiceState in soloud.h; this section holds the machinery:
//
// - queueVoiceCommand_internal(): producer side, under mVoiceCommandMutex.
// - applyVoiceCommands_internal(): consumer side for volume/pan/pause, run
//   by lockAudioMutex_internal() under the audio mutex.
// - applyQueuedSeeks_internal(): consumer side for seeks, run by the mixer
//   at a block start and by a control call on the idle-mixer path; every
//   other lock holder applies only seeks whose source opts in as cheap.
// - publishVoiceState(s)_internal(): run by stopVoice_internal(),
//   insertVoice_internal() and unlockAudioMutex_internal().
// - readVoiceState_internal(): lock-free reader for the getters.
//
// The audio thread only ever takes the audio mutex. It never takes
// mVoiceCommandMutex or mVoiceGroupMutex, and the seqlock writers it runs
// never wait on a reader, so nothing here can stall a mix.

namespace SoLoud
{
	namespace
	{
		// Slot of a (non-group) voice handle, or VOICE_COUNT or more when the
		// handle cannot name a voice (0, or a slot past the voice table).
		inline unsigned int voiceSlotOf(handle aVoiceHandle)
		{
			return (aVoiceHandle & 0xfff) - 1;
		}

		// Seqlock writers never wait, so a reader only spins while a writer is
		// between its two sequence stores; yield in case it was preempted
		// there.
		inline void seqlockBackoff(unsigned int &aSpins)
		{
			if (++aSpins > 64)
				std::this_thread::yield();
		}
	}

	void Soloud::publishVoiceState_internal(unsigned int aVoice)
	{
		SOLOUD_ASSERT(aVoice < VOICE_COUNT);
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		PublishedVoiceState &p = mPublishedVoice[aVoice];
		AudioSourceInstance *v = mVoice[aVoice];
		if (v == NULL && p.mHandle.load(std::memory_order_relaxed) == 0)
			return;

		const unsigned int seq = p.mSeq.load(std::memory_order_relaxed);
		p.mSeq.store(seq + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		if (v != NULL)
		{
			p.mHandle.store(getHandleFromVoice_internal(aVoice), std::memory_order_relaxed);
			p.mVolume.store(v->mSetVolume, std::memory_order_relaxed);
			p.mPan.store(v->mPan, std::memory_order_relaxed);
			p.mPaused.store((v->mFlags & AudioSourceInstance::PAUSED) ? 1 : 0,
				std::memory_order_relaxed);
			p.mPosition.store(v->mStreamPosition, std::memory_order_relaxed);
			p.mSeekTicket.store(mVoiceSeekAppliedTicket[aVoice], std::memory_order_relaxed);
		}
		else
		{
			p.mHandle.store(0, std::memory_order_relaxed);
		}
		p.mSeq.store(seq + 2, std::memory_order_release);
	}

	void Soloud::publishVoiceStates_internal()
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		// Slots at or past mHighestVoice are empty; stopVoice_internal()
		// cleared their published handle when it emptied them.
		unsigned int i;
		for (i = 0; i < mHighestVoice && i < VOICE_COUNT; i++)
			publishVoiceState_internal(i);
	}

	bool Soloud::readVoiceState_internal(handle aVoiceHandle, VoiceStateSnapshot &aState)
	{
		handle h = aVoiceHandle;
		if ((h & 0xfffff000) == 0xfffff000)
		{
			// Like getVoiceFromHandle_internal(): a voice group answers for
			// its first voice.
			std::lock_guard<std::mutex> groupLock(mVoiceGroupMutex);
			const handle *group = voiceGroupHandleToArray_internal(h);
			if (group == NULL)
				return false;
			h = *group;
		}
		const unsigned int slot = voiceSlotOf(h);
		if (h == 0 || slot >= VOICE_COUNT)
			return false;

		// The ticket first: the state a command produced is published before
		// the ticket that covers it (see unlockAudioMutex_internal()).
		const unsigned long long applied =
			mVoiceCommandAppliedTicket.load(std::memory_order_acquire);

		const PublishedVoiceState &p = mPublishedVoice[slot];
		unsigned int publishedHandle = 0;
		unsigned int paused = 0;
		unsigned long long publishedSeekTicket = 0;
		unsigned int spins = 0;
		for (;;)
		{
			const unsigned int seq = p.mSeq.load(std::memory_order_acquire);
			if ((seq & 1) == 0)
			{
				publishedHandle = p.mHandle.load(std::memory_order_relaxed);
				aState.mVolume = p.mVolume.load(std::memory_order_relaxed);
				aState.mPan = p.mPan.load(std::memory_order_relaxed);
				paused = p.mPaused.load(std::memory_order_relaxed);
				aState.mPosition = p.mPosition.load(std::memory_order_relaxed);
				publishedSeekTicket = p.mSeekTicket.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				if (p.mSeq.load(std::memory_order_relaxed) == seq)
					break;
			}
			seqlockBackoff(spins);
		}
		if (publishedHandle != h)
			return false;
		aState.mPaused = paused != 0;

		// Overlay the fields set by commands the mixer has not applied yet.
		const PendingVoiceControl &q = mPendingVoice[slot];
		unsigned int pendingHandle = 0;
		unsigned long long volumeTicket = 0, panTicket = 0, pauseTicket = 0, seekTicket = 0;
		float volume = 0, pan = 0;
		unsigned int pause = 0;
		double seek = 0;
		spins = 0;
		for (;;)
		{
			const unsigned int seq = q.mSeq.load(std::memory_order_acquire);
			if ((seq & 1) == 0)
			{
				pendingHandle = q.mHandle.load(std::memory_order_relaxed);
				volumeTicket = q.mVolumeTicket.load(std::memory_order_relaxed);
				panTicket = q.mPanTicket.load(std::memory_order_relaxed);
				pauseTicket = q.mPauseTicket.load(std::memory_order_relaxed);
				seekTicket = q.mSeekTicket.load(std::memory_order_relaxed);
				volume = q.mVolume.load(std::memory_order_relaxed);
				pan = q.mPan.load(std::memory_order_relaxed);
				pause = q.mPause.load(std::memory_order_relaxed);
				seek = q.mSeek.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				if (q.mSeq.load(std::memory_order_relaxed) == seq)
					break;
			}
			seqlockBackoff(spins);
		}
		if (pendingHandle == h)
		{
			if (volumeTicket > applied)
				aState.mVolume = volume;
			if (panTicket > applied)
				aState.mPan = pan;
			if (pauseTicket > applied)
				aState.mPaused = pause != 0;
			// Seeks have their own queue: compare with the newest seek applied
			// to this slot, published with the position it produced.
			if (seekTicket > publishedSeekTicket)
				aState.mPosition = seek;
		}
		return true;
	}

	bool Soloud::isMixerIdle_internal() const
	{
		const long long start = mMixBlockStartNs.load(std::memory_order_relaxed);
		if (start == 0)
			return true;
		const long long now = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		long long idle = 4 * mMixBlockLengthNs.load(std::memory_order_relaxed);
		if (idle < (long long)VOICE_COMMAND_MIXER_IDLE_MS * 1000000)
			idle = (long long)VOICE_COMMAND_MIXER_IDLE_MS * 1000000;
		return now - start > idle;
	}

	bool Soloud::queueVoiceCommand_internal(unsigned int aType, handle aVoiceHandle, double aValue)
	{
		const bool isSeek = aType == VoiceCommand::SEEK;
		for (;;)
		{
			// With no mixer running (device stopped, or nobody calling mix()), a
			// queued command would wait for the next mix block: apply it now
			// instead. The mutex is free then, unless a long block is in progress
			// or another call holds it; the try-lock then fails and the command is
			// queued as usual. So a seek on a paused voice with the device stopped
			// still reaches the source right away (e.g. to start prefetching the
			// target). The seeks queued before (while a mixer still ran) go first:
			// with no mixer, this is the only place they can run, and a later seek
			// of the same voice must land after them.
			if (isMixerIdle_internal() && tryLockAudioMutex_internal())
			{
				applyQueuedSeeks_internal();
				return false;
			}
			// With the render-ahead ring, a volume, pan or pause change normally
			// lands retroactively at the playhead, inside audio already mixed
			// ahead (retroactiveParam_internal), which a later mix block cannot
			// do as well. Keep that when the mutex is free and no other volume,
			// pan or pause change is queued (applying those here could re-mix the
			// ring for each, on this caller); a hold in progress (a mix block) or
			// queued changes send it to the queue, where the mixer still places
			// it at the playhead of its next block. Queued seeks stay queued
			// either way (see applyQueuedSeeks_internal). A seek does not rewrite
			// the ring, so while the mixer runs it is always queued: a source seek
			// would otherwise run on the caller.
			if (!isSeek && isRenderAheadEnabled() && mRenderRing.isInited() &&
				tryLockAudioMutex_internal(true))
			{
				return false;
			}

			{
				// Lock order: mVoiceGroupMutex, then mVoiceCommandMutex. Neither
				// is ever held while waiting for the audio mutex.
				std::lock_guard<std::mutex> groupLock(mVoiceGroupMutex);
				std::lock_guard<std::mutex> commandLock(mVoiceCommandMutex);

				handle single[2] = { aVoiceHandle, 0 };
				const handle *voices = voiceGroupHandleToArray_internal(aVoiceHandle);
				if (voices == NULL)
					voices = single;
				unsigned int count = 0;
				const handle *h;
				for (h = voices; *h; h++)
					count++;

				// Seeks go to their own ring (see applyQueuedSeeks_internal).
				VoiceCommand *ring = isSeek ? mSeekCommand : mVoiceCommand;
				std::atomic<unsigned int> &ringHead = isSeek ? mSeekCommandHead : mVoiceCommandHead;
				std::atomic<unsigned int> &ringTail = isSeek ? mSeekCommandTail : mVoiceCommandTail;
				const unsigned int head = ringHead.load(std::memory_order_acquire);
				unsigned int tail = ringTail.load(std::memory_order_relaxed);
				// All of a voice group's commands or none: a group call is never
				// half queued and half applied.
				if (count <= VOICE_COMMAND_QUEUE_SIZE - (tail - head))
				{
					for (h = voices; *h; h++)
					{
						const handle voiceHandle = *h;
						const unsigned int slot = voiceSlotOf(voiceHandle);
						// Like FOR_ALL_VOICES_PRE, skip handles that do not name a
						// live voice (a voice group handle never does).
						if ((voiceHandle & 0xfffff000) == 0xfffff000 || slot >= VOICE_COUNT ||
							mPublishedVoice[slot].mHandle.load(std::memory_order_acquire) != voiceHandle)
						{
							continue;
						}

						const unsigned long long ticket = ++mVoiceCommandTicket;

						// Pending value first, so a get right after this set returns
						// it; the mixer may apply the command as soon as it is queued.
						PendingVoiceControl &q = mPendingVoice[slot];
						const unsigned int seq = q.mSeq.load(std::memory_order_relaxed);
						q.mSeq.store(seq + 1, std::memory_order_relaxed);
						std::atomic_thread_fence(std::memory_order_release);
						if (q.mHandle.load(std::memory_order_relaxed) != voiceHandle)
						{
							// The slot holds another voice now: its values are not
							// this voice's.
							q.mHandle.store(voiceHandle, std::memory_order_relaxed);
							q.mVolumeTicket.store(0, std::memory_order_relaxed);
							q.mPanTicket.store(0, std::memory_order_relaxed);
							q.mPauseTicket.store(0, std::memory_order_relaxed);
							q.mSeekTicket.store(0, std::memory_order_relaxed);
						}
						switch (aType)
						{
						case VoiceCommand::VOLUME:
							q.mVolume.store((float)aValue, std::memory_order_relaxed);
							q.mVolumeTicket.store(ticket, std::memory_order_relaxed);
							break;
						case VoiceCommand::PAN:
							q.mPan.store((float)aValue, std::memory_order_relaxed);
							q.mPanTicket.store(ticket, std::memory_order_relaxed);
							break;
						case VoiceCommand::PAUSE:
							q.mPause.store(aValue != 0 ? 1 : 0, std::memory_order_relaxed);
							q.mPauseTicket.store(ticket, std::memory_order_relaxed);
							break;
						case VoiceCommand::SEEK:
							q.mSeek.store(aValue, std::memory_order_relaxed);
							q.mSeekTicket.store(ticket, std::memory_order_relaxed);
							break;
						}
						q.mSeq.store(seq + 2, std::memory_order_release);

						VoiceCommand &command = ring[tail % VOICE_COMMAND_QUEUE_SIZE];
						command.mTicket = ticket;
						command.mHandle = voiceHandle;
						command.mType = aType;
						command.mValue = aValue;
						tail++;
					}
					ringTail.store(tail, std::memory_order_release);
					return true;
				}
			}

			if (!isSeek)
			{
				// The queue is full (nothing has mixed for a long while, so
				// nothing drained it): take the mutex like the original call did.
				// Locking applies every volume/pan/pause change queued so far
				// first, so the order holds.
				lockAudioMutex_internal();
				return false;
			}
			// VOICE_COMMAND_QUEUE_SIZE seeks queued since the last block start.
			// Applying them here would run their source seeks on this caller, so
			// wait for the mixer's next block to drain the queue instead; if the
			// mixer has stopped meanwhile, the idle path above takes over. A UI
			// cannot get here (it seeks a few times per block); a tight loop of
			// seeks waits at most about a block per VOICE_COMMAND_QUEUE_SIZE.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	result Soloud::applyVoiceCommand_internal(unsigned int aType, unsigned int aVoice, double aValue)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		if (aVoice >= VOICE_COUNT || mVoice[aVoice] == NULL)
			return INVALID_PARAMETER;
		const float value = (float)aValue;
		switch (aType)
		{
		case VoiceCommand::VOLUME:
			// ###### flutter_soloud local patch (retroactive re-mix) ######
			if (!retroactiveParam_internal(aVoice, RetroJournalEntry::PARAM_VOLUME, value, 0, playheadTimeLocked_internal()) &&
				mVoice[aVoice] != NULL)
			{
				mVoice[aVoice]->mVolumeFader.mActive = 0;
				setVoiceVolume_internal(aVoice, value);
			}
			break;
		case VoiceCommand::PAN:
			// ###### flutter_soloud local patch (retroactive re-mix) ######
			if (!retroactiveParam_internal(aVoice, RetroJournalEntry::PARAM_PAN, value, 0, playheadTimeLocked_internal()) &&
				mVoice[aVoice] != NULL)
			{
				mVoice[aVoice]->mPanFader.mActive = 0;
				setVoicePan_internal(aVoice, value);
			}
			break;
		case VoiceCommand::PAUSE:
			// ###### flutter_soloud local patch (retroactive re-mix) ######
			// Retroactive only for pausing; unpausing keeps legacy placement.
			if (value == 0.0f ||
				!retroactiveParam_internal(aVoice, RetroJournalEntry::PARAM_PAUSE, 1.0f, 0, playheadTimeLocked_internal()))
			{
				setVoicePause_internal(aVoice, value != 0.0f);
			}
			break;
		case VoiceCommand::SEEK:
		{
			const result res = mVoice[aVoice]->seek(aValue, mScratch.mData, mScratchSize);
			if (res == SO_NO_ERROR)
			{
				mVoice[aVoice]->mSourceSamplePosition = (uint64_t)floor(
					aValue * mVoice[aVoice]->mBaseSamplerate);
			}
			return res;
		}
		default:
			return INVALID_PARAMETER;
		}
		return SO_NO_ERROR;
	}

	void Soloud::applyVoiceCommands_internal()
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		unsigned int head = mVoiceCommandHead.load(std::memory_order_relaxed);
		const unsigned int tail = mVoiceCommandTail.load(std::memory_order_acquire);
		while (head != tail)
		{
			const VoiceCommand command = mVoiceCommand[head % VOICE_COMMAND_QUEUE_SIZE];
			head++;
			// Hand the slot back before applying: applying can take a while
			// (a retroactive re-mix), and producers may refill it meanwhile.
			mVoiceCommandHead.store(head, std::memory_order_release);
			// The voice may have ended, or its slot been reused, since the
			// command was queued: the handle check drops the command then.
			const int ch = getVoiceFromHandle_internal(command.mHandle);
			if (ch >= 0)
				applyVoiceCommand_internal(command.mType, (unsigned int)ch, command.mValue);
			mVoiceCommandDrainedTicket = command.mTicket;
		}
	}

	// Queued seeks run here, and (but for cheap ones, below) only from two
	// places: the mixer at the start of a block (lockAudioMutex_internal(true)
	// in mix_internal()), and a control call on the idle-mixer path
	// (queueVoiceCommand_internal()), where no mixer would run them. Every
	// other call that takes the audio mutex applies the queued
	// volume/pan/pause changes but leaves those seeks queued, so a source's seek work (decoding, file reads) never lands on a
	// thread that merely locked after it -- play() on the UI thread, or
	// getActiveVoiceCount() under the plugin's device-lifecycle mutex.
	//
	// Leaving them queued is order-safe because a seek writes only its own
	// voice's source state (decoder, file position) and play position
	// (mStreamPosition, mSourceSamplePosition), and reads only the source
	// state and mBaseSamplerate; a later locking call that runs before it
	// cannot observe the difference:
	// - stop(), stopAll(), stopAudioSource(), stopBus, deinit() and the
	//   voice-ended path remove the voice; its queued seek then fails the
	//   handle check below and is dropped (the queue holds handles, never
	//   instance or source pointers), so once a stop returns nothing touches
	//   that voice's instance or its AudioSource again.
	// - play*() inserts a voice with a new handle; a seek queued for an
	//   earlier voice in the same slot fails the handle check.
	// - Volume/pan/pause (queued or direct), faders, schedulePause/Stop,
	//   setRelativePlaySpeed, setProtectVoice, setInaudibleBehavior, looping
	//   and loop points, filters, 3D, buses, voice groups, getActiveVoiceCount
	//   and the other counters touch none of the state a seek writes or
	//   reads. Loop points are read by the mixer, after the seek, and by
	//   NativeStream's seek itself, which then sees the newer settings.
	// - getStreamPosition() is lock-free and reports the queued target
	//   (PendingVoiceControl); getStreamTime() and getLoopCount() count play
	//   time, which a seek does not change.
	// - With the render-ahead ring, a retroactive volume/pan/pause change
	//   rolls voices back to a checkpoint and re-mixes; run first, it can no
	//   longer roll back (undo) a seek issued before it, as it could when the
	//   seek ran on the caller.
	// - setSamplerate() changes mBaseSamplerate, which a seek reads: a
	//   queued seek then lands on the target at the new rate. The plugin
	//   never calls it on a voice.
	//
	// Only the newest seek queued for a voice runs: one with a newer seek
	// queued after it (its slot's pending seek ticket is newer) is skipped.
	// Nothing plays between two seeks applied at the same block start, so the
	// older one could only cost time inside the block. The newer one is
	// already queued, or about to be (its producer publishes the ticket
	// before the command); in that case it runs at the next block.
	//
	// The one exception to "mixer only": a voice whose instance reports
	// hasCheapSeek() (NativeStream: its seek only posts a request to its
	// decoder thread). Every other lock holder applies those seeks too
	// (aOnlyCheap), so the decoder starts on the target as soon as anything
	// takes the mutex. They are consumed in place and the rest stay queued
	// in order; all seeks of one voice share its instance, so a voice's seeks
	// are either all cheap or all left for the mixer, and their order holds.
	// Seek-cost-wise this is safe for any caller; order-wise the reasoning
	// above holds either way, since a seek applied earlier is just what a
	// locking call used to see before seeks were queued.
	void Soloud::applyQueuedSeeks_internal(bool aOnlyCheap)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		unsigned int head = mSeekCommandHead.load(std::memory_order_relaxed);
		const unsigned int tail = mSeekCommandTail.load(std::memory_order_acquire);
		// Entries in [head, tail) belong to the consumer (the audio mutex
		// holder): producers only write at and past tail. Cheap seeks applied
		// in place below are marked consumed with handle 0, which no voice has.
		bool leading = true;
		unsigned int i;
		for (i = head; i != tail; i++)
		{
			VoiceCommand &entry = mSeekCommand[i % VOICE_COMMAND_QUEUE_SIZE];
			const VoiceCommand command = entry;
			bool consumed = true;
			const unsigned int slot = voiceSlotOf(command.mHandle);
			// A single atomic load, never the seqlock: the mixer must not spin
			// on a producer. A newer ticket was written for this slot after
			// this command's either for this voice (superseded) or for a voice
			// that replaced it (this one is gone); skip it either way.
			if (slot < VOICE_COUNT &&
				mPendingVoice[slot].mSeekTicket.load(std::memory_order_acquire) <= command.mTicket)
			{
				const int ch = getVoiceFromHandle_internal(command.mHandle);
				if (ch >= 0)
				{
					if (aOnlyCheap && !mVoice[ch]->hasCheapSeek())
					{
						consumed = false; // left for the mixer
					}
					else
					{
						applyVoiceCommand_internal(VoiceCommand::SEEK, (unsigned int)ch, command.mValue);
						mVoiceSeekAppliedTicket[ch] = command.mTicket;
					}
				}
			}
			if (!consumed)
			{
				leading = false;
				continue;
			}
			if (leading)
			{
				// Hand the slot back to the producers.
				mSeekCommandHead.store(i + 1, std::memory_order_release);
			}
			else
			{
				entry.mHandle = 0;
			}
		}
	}
}
