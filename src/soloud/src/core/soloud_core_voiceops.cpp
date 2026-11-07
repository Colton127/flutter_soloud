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
#include "soloud_thread.h" // mVoiceCommandMutex (voice control)
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
//   by lockAudioMutex_internal() under the audio mutex; it copies the queue
//   out under mVoiceCommandMutex and applies the copy
//   (applyVoiceCommandBatch_internal()) after releasing it.
// - applyQueuedSeeks_internal(): consumer side for seeks, run by the mixer
//   at a block start and by a control call on the idle-mixer path; every
//   other lock holder applies only seeks whose source opts in as cheap.
// - publishVoiceState(s)_internal(): run by stopVoice_internal(),
//   insertVoice_internal() and unlockAudioMutex_internal().
// - readVoiceState_internal(): lock-free reader for the getters. Not for
//   the audio thread, nor for a source's getAudio() or seek() it runs: a
//   reader spins, then yields, while a control thread is between its two
//   sequence stores, and a voice group's read takes mVoiceGroupMutex. The
//   mix reads mVoice directly.
//
// The audio thread blocks on nothing but the audio mutex. It never takes
// mVoiceGroupMutex and only try-locks mVoiceCommandMutex (a control call
// holds it for a few stores; the queue then waits for the next block), and
// the seqlock writers it runs never wait on a reader, so nothing here can
// stall a mix.

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

		// Scoped hold of mVoiceCommandMutex (a Thread mutex). With aTryOnly
		// (the mixer), it may not be taken: check locked().
		class VoiceCommandLock
		{
		public:
			VoiceCommandLock(void *aMutex, bool aTryOnly) : mMutex(aMutex), mLocked(true)
			{
				if (aTryOnly)
					mLocked = Thread::tryLockMutex(mMutex) == 0;
				else
					Thread::lockMutex(mMutex);
			}
			~VoiceCommandLock()
			{
				if (mLocked)
					Thread::unlockMutex(mMutex);
			}
			bool locked() const { return mLocked; }

		private:
			VoiceCommandLock(const VoiceCommandLock &);
			VoiceCommandLock &operator=(const VoiceCommandLock &);
			void *mMutex;
			bool mLocked;
		};

		// Ticket and value of one field of a slot's pending changes. Called
		// with mVoiceCommandMutex held, so no producer is writing them.
		inline unsigned long long pendingTicket(const PendingVoiceControl &aPending, unsigned int aType)
		{
			switch (aType)
			{
			case VoiceCommand::VOLUME: return aPending.mVolumeTicket.load(std::memory_order_relaxed);
			case VoiceCommand::PAN: return aPending.mPanTicket.load(std::memory_order_relaxed);
			case VoiceCommand::PAUSE: return aPending.mPauseTicket.load(std::memory_order_relaxed);
			default: return aPending.mSeekTicket.load(std::memory_order_relaxed);
			}
		}

		inline double pendingValue(const PendingVoiceControl &aPending, unsigned int aType)
		{
			switch (aType)
			{
			case VoiceCommand::VOLUME: return aPending.mVolume.load(std::memory_order_relaxed);
			case VoiceCommand::PAN: return aPending.mPan.load(std::memory_order_relaxed);
			case VoiceCommand::PAUSE: return aPending.mPause.load(std::memory_order_relaxed) ? 1.0 : 0.0;
			default: return aPending.mSeek.load(std::memory_order_relaxed);
			}
		}

		// States of a volume/pan/pause change while a batch is applied.
		enum
		{
			BATCH_DROPPED = 0, // its voice is gone
			BATCH_IMMEDIATE,   // applied to the live voice state
			BATCH_RETROACTIVE  // placed at the playhead inside the re-mix
		};
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

	// The queue holds at most one entry per voice slot and type (volume,
	// pan, pause, seek): which slot has a change of which type waiting. The
	// value is the slot's PendingVoiceControl field, which every call
	// overwrites with the newest value and ticket. A call whose voice already
	// has an entry of its type only updates that field, so the newest value
	// wins and the queue never holds more than VOICE_COUNT entries per type:
	// a call never waits for room (there is no full-queue path), and a drain
	// applies at most that many changes.
	//
	// Why coalescing keeps the call order:
	// - A consumer copies the whole queue out at once (under
	//   mVoiceCommandMutex, which every producer holds for its whole call)
	//   and applies the copy under the audio mutex, before whatever runs
	//   under that hold: a mix block, or a locking API call. Nothing runs in
	//   between two changes of one drain, so only the state after all of them
	//   can be observed, and that state does not depend on where in the queue
	//   a change sits: changes to different voices, or to different fields
	//   (volume, pan, pause, seek) of one voice, write disjoint state; for
	//   one field of one voice, the newest value is the one it ends up with
	//   either way.
	// - A locking call (fadeVolume, stop, play, getActiveVoiceCount, ...)
	//   drains the queue before it runs, and a change made after it returns
	//   queues anew. Coalescing only merges changes queued before the same
	//   drain, so no change moves across a locking call: setVolume(0) then
	//   fadeVolume() fades from 0, and fadeVolume() then setVolume() stops
	//   the fade, as before.
	// - With the render-ahead ring, the changes of one drain are all placed
	//   at the same playhead time (applyVoiceCommandBatch_internal()), so
	//   their order among themselves does not matter there either.
	// - The voice group of a call is expanded under both mutexes, and a
	//   consumer takes mVoiceCommandMutex to copy the queue out, so a group
	//   call is applied to all of its voices in one drain or not yet at all.
	// - Handles: an entry is keyed on the slot, but the field carries the
	//   handle of the voice it was set for. A call for a new voice in the
	//   slot resets the slot's fields; an entry of the old voice whose field
	//   was reset (ticket 0) is dropped, and the new voice's own change
	//   takes over an entry still queued for its type. A consumer applies an
	//   entry only to the voice whose handle the field carries, if it still
	//   plays.
	//
	// What coalescing gives up is an intermediate value that never reached
	// a mix block anyway: setPause(true) then setPause(false) before the next
	// block leaves the voice playing, without the pause in between (with the
	// render-ahead ring that pause used to be placed at the playhead and the
	// resume at the end of the audio already mixed ahead, muting the voice in
	// between; now it just keeps playing). Of several seeks of a voice, only
	// the newest runs.
	bool Soloud::queueVoiceCommand_internal(unsigned int aType, handle aVoiceHandle, double aValue)
	{
		const bool isSeek = aType == VoiceCommand::SEEK;
		// With no mixer running (device stopped, or nobody calling mix()), a
		// queued seek would wait for the next mix block: apply it now
		// instead. The mutex is free then, unless a long block is in progress
		// or another call holds it; the try-lock then fails and the seek is
		// queued as usual. So a seek on a paused voice with the device stopped
		// still reaches the source right away (e.g. to start prefetching the
		// target). The seeks queued before (while a mixer still ran) go first:
		// with no mixer, this is the only place they can run, and a later seek
		// of the same voice must land after them.
		if (isSeek && isMixerIdle_internal() && tryLockAudioMutex_internal())
		{
			applyQueuedSeeks_internal();
			return false;
		}

		unsigned int queued = 0;
		{
			// Lock order: mVoiceGroupMutex, then mVoiceCommandMutex. Neither
			// is ever held while waiting for the audio mutex.
			std::lock_guard<std::mutex> groupLock(mVoiceGroupMutex);
			VoiceCommandLock commandLock(mVoiceCommandMutex, false);

			handle single[2] = { aVoiceHandle, 0 };
			const handle *voices = voiceGroupHandleToArray_internal(aVoiceHandle);
			if (voices == NULL)
				voices = single;
			const unsigned char bit = (unsigned char)(1u << aType);
			const handle *h;
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

				// The value, for the consumer and for a get right after this
				// set.
				PendingVoiceControl &q = mPendingVoice[slot];
				const unsigned int seq = q.mSeq.load(std::memory_order_relaxed);
				q.mSeq.store(seq + 1, std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_release);
				if (q.mHandle.load(std::memory_order_relaxed) != voiceHandle)
				{
					// The slot holds another voice now: its values are not
					// this voice's, and its entries still queued are dropped.
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

				// The entry, unless the slot already has one of this type: that
				// one applies the value just stored.
				if (!(mVoiceCommandQueued[slot] & bit))
				{
					mVoiceCommandQueued[slot] |= bit;
					if (isSeek)
					{
						SOLOUD_ASSERT(mSeekCommandListCount < VOICE_COUNT);
						mSeekCommandList[mSeekCommandListCount++] = (unsigned short)slot;
					}
					else
					{
						SOLOUD_ASSERT(mVoiceCommandListCount < VOICE_COUNT * 3);
						mVoiceCommandList[mVoiceCommandListCount++] = (unsigned short)(slot * 4 + aType);
					}
				}
				queued++;
			}
			mVoiceCommandPending.store(mVoiceCommandListCount, std::memory_order_release);
			mSeekCommandPending.store(mSeekCommandListCount, std::memory_order_release);
		}
		if (queued == 0 || isSeek)
			return true;

		// Volume, pan and pause: apply at once when the mutex is free and
		// either no mixer runs (nothing would apply them soon) or the
		// render-ahead ring is on (a change normally lands retroactively at
		// the playhead, inside audio already mixed ahead, which a later mix
		// block cannot do as well). Taking the mutex applies everything
		// queued, this change included, with one re-mix of the ring for all
		// of it. A hold in progress (a mix block) leaves it to the next one,
		// which places it at the playhead of its block. Queued seeks stay
		// queued while a mixer runs (see applyQueuedSeeks_internal).
		const bool idle = isMixerIdle_internal();
		if ((idle || (isRenderAheadEnabled() && mRenderRing.isInited())) && tryLockAudioMutex_internal())
		{
			if (idle)
				applyQueuedSeeks_internal();
			unlockAudioMutex_internal();
		}
		return true;
	}

	result Soloud::applySeek_internal(unsigned int aVoice, double aSeconds)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		if (aVoice >= VOICE_COUNT || mVoice[aVoice] == NULL)
			return INVALID_PARAMETER;
		const result res = mVoice[aVoice]->seek(aSeconds, mScratch.mData, mScratchSize);
		if (res == SO_NO_ERROR)
		{
			mVoice[aVoice]->mSourceSamplePosition = (uint64_t)floor(
				aSeconds * mVoice[aVoice]->mBaseSamplerate);
		}
		return res;
	}

	void Soloud::applyVoiceCommands_internal(bool aTryOnly)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		// A producer publishes the count before it releases the mutex, so a
		// change made before this call on this thread is seen here.
		if (mVoiceCommandPending.load(std::memory_order_acquire) == 0)
			return;
		unsigned int count = 0;
		unsigned long long drained = 0;
		{
			VoiceCommandLock lock(mVoiceCommandMutex, aTryOnly);
			if (!lock.locked())
				return; // a control call holds it: the next block applies them
			unsigned int i;
			for (i = 0; i < mVoiceCommandListCount; i++)
			{
				const unsigned int slot = mVoiceCommandList[i] >> 2;
				const unsigned int type = mVoiceCommandList[i] & 3;
				mVoiceCommandQueued[slot] &= (unsigned char)~(1u << type);
				const PendingVoiceControl &q = mPendingVoice[slot];
				VoiceCommand &c = mVoiceCommandBatch[count];
				c.mTicket = pendingTicket(q, type);
				if (c.mTicket == 0)
					continue; // reset for a newer voice in the slot
				c.mHandle = q.mHandle.load(std::memory_order_relaxed);
				c.mType = (unsigned short)type;
				c.mValue = pendingValue(q, type);
				count++;
			}
			mVoiceCommandListCount = 0;
			mVoiceCommandPending.store(0, std::memory_order_relaxed);
			// Every volume/pan/pause change with a ticket up to here is in the
			// copy, or was replaced by a newer value that is.
			drained = mVoiceCommandTicket;
		}
		applyVoiceCommandBatch_internal(mVoiceCommandBatch, count);
		mVoiceCommandDrainedTicket = drained;
	}

	// One drain applies at most one change per voice and type. With the
	// render-ahead ring, every change that can be placed retroactively at the
	// playhead is placed in a single rollback: restore the checkpoint
	// (retroactiveBegin_internal), schedule each change at the playhead
	// (retroactiveParamInWindow_internal), re-mix the ring once
	// (retroactiveEnd_internal). One by one, as a single call does it
	// (retroactiveParam_internal), each change re-mixed the ring again, and a
	// drain of a few hundred changes took as many re-mixes, on whichever
	// thread drained: the mixer, or an unrelated locking call such as
	// getActiveVoiceCount(). The result is the same either way: all of them
	// land at the same playhead time, and each re-mix replays the journal of
	// the ones placed before it. The changes that cannot be placed so (a
	// paused voice, a resume, or the rollback declined) are applied to the
	// live voice state afterwards, as the legacy setters do, so the restore
	// cannot undo them.
	void Soloud::applyVoiceCommandBatch_internal(VoiceCommand *aBatch, unsigned int aCount)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		unsigned int retroactive = 0;
		unsigned int i;
		for (i = 0; i < aCount; i++)
		{
			VoiceCommand &c = aBatch[i];
			// The voice may have ended, or its slot been reused, since the
			// change was queued: the handle check drops the change then.
			const int ch = getVoiceFromHandle_internal(c.mHandle);
			if (ch < 0)
			{
				c.mState = BATCH_DROPPED;
				continue;
			}
			const int param = c.mType == VoiceCommand::VOLUME ? RetroJournalEntry::PARAM_VOLUME :
				c.mType == VoiceCommand::PAN ? RetroJournalEntry::PARAM_PAN : RetroJournalEntry::PARAM_PAUSE;
			if (retroactiveParamEligible_internal((unsigned int)ch, param, (float)c.mValue))
			{
				c.mState = BATCH_RETROACTIVE;
				retroactive++;
			}
			else
			{
				c.mState = BATCH_IMMEDIATE;
			}
		}

		if (retroactive > 0)
		{
			const time playhead = playheadTimeLocked_internal();
			const int ci = retroactiveBegin_internal(playhead);
			for (i = 0; i < aCount; i++)
			{
				VoiceCommand &c = aBatch[i];
				if (c.mState != BATCH_RETROACTIVE)
					continue;
				const int ch = ci >= 0 ? getVoiceFromHandle_internal(c.mHandle) : -1;
				bool placed = false;
				if (ch >= 0)
				{
					switch (c.mType)
					{
					case VoiceCommand::VOLUME:
						placed = retroactiveParamInWindow_internal((unsigned int)ch, mVoice[ch]->mPlayIndex,
							RetroJournalEntry::PARAM_VOLUME, (float)c.mValue, 0, playhead);
						break;
					case VoiceCommand::PAN:
						placed = retroactiveParamInWindow_internal((unsigned int)ch, mVoice[ch]->mPlayIndex,
							RetroJournalEntry::PARAM_PAN, (float)c.mValue, 0, playhead);
						break;
					case VoiceCommand::PAUSE:
						placed = retroactiveParamInWindow_internal((unsigned int)ch, mVoice[ch]->mPlayIndex,
							RetroJournalEntry::PARAM_PAUSE, 1.0f, 0, playhead);
						break;
					}
				}
				if (!placed)
					c.mState = BATCH_IMMEDIATE;
			}
			if (ci >= 0)
				retroactiveEnd_internal(ci);
		}

		for (i = 0; i < aCount; i++)
		{
			const VoiceCommand &c = aBatch[i];
			if (c.mState != BATCH_IMMEDIATE)
				continue;
			const int ch = getVoiceFromHandle_internal(c.mHandle);
			if (ch < 0)
				continue;
			switch (c.mType)
			{
			case VoiceCommand::VOLUME:
				mVoice[ch]->mVolumeFader.mActive = 0;
				setVoiceVolume_internal((unsigned int)ch, (float)c.mValue);
				break;
			case VoiceCommand::PAN:
				mVoice[ch]->mPanFader.mActive = 0;
				setVoicePan_internal((unsigned int)ch, (float)c.mValue);
				break;
			case VoiceCommand::PAUSE:
				setVoicePause_internal((unsigned int)ch, c.mValue != 0);
				break;
			}
		}
	}

	// Queued seeks run here, and (but for cheap ones, below) only from two
	// places: the mixer at the start of a block (lockAudioMutex_internal(true)
	// in mix_internal()), and a control call on the idle-mixer path
	// (queueVoiceCommand_internal()), where no mixer would run them. Every
	// other call that takes the audio mutex applies the queued
	// volume/pan/pause changes but leaves those seeks queued, so a source's
	// seek work (decoding, file reads) never lands on a thread that merely
	// locked after it -- play() on the UI thread, or getActiveVoiceCount()
	// under the plugin's device-lifecycle mutex.
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
	// A voice has at most one seek queued: a newer one replaces its target
	// (see queueVoiceCommand_internal()), so only the newest runs. Nothing
	// plays between two seeks applied at the same block start, so the older
	// one could only cost time inside the block.
	//
	// The one exception to "mixer only": a voice whose instance reports
	// hasCheapSeek() (NativeStream: its seek only posts a request to its
	// decoder thread). Every other lock holder applies those seeks too
	// (aOnlyCheap), so the decoder starts on the target as soon as anything
	// takes the mutex; the rest stay queued, in order. Seek-cost-wise this is
	// safe for any caller; order-wise the reasoning above holds either way,
	// since a seek applied earlier is just what a locking call used to see
	// before seeks were queued.
	void Soloud::applyQueuedSeeks_internal(bool aOnlyCheap, bool aTryOnly)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		if (mSeekCommandPending.load(std::memory_order_acquire) == 0)
			return;
		unsigned int count = 0;
		{
			VoiceCommandLock lock(mVoiceCommandMutex, aTryOnly);
			if (!lock.locked())
				return; // a control call holds it: the next block applies them
			unsigned int kept = 0;
			unsigned int i;
			for (i = 0; i < mSeekCommandListCount; i++)
			{
				const unsigned int slot = mSeekCommandList[i];
				const PendingVoiceControl &q = mPendingVoice[slot];
				const unsigned long long ticket = q.mSeekTicket.load(std::memory_order_relaxed);
				const handle voiceHandle = q.mHandle.load(std::memory_order_relaxed);
				if (ticket != 0 && aOnlyCheap)
				{
					const int ch = getVoiceFromHandle_internal(voiceHandle);
					if (ch >= 0 && !mVoice[ch]->hasCheapSeek())
					{
						mSeekCommandList[kept++] = (unsigned short)slot; // left for the mixer
						continue;
					}
				}
				mVoiceCommandQueued[slot] &= (unsigned char)~(1u << VoiceCommand::SEEK);
				if (ticket == 0)
					continue; // reset for a newer voice in the slot
				VoiceCommand &c = mSeekCommandBatch[count++];
				c.mTicket = ticket;
				c.mHandle = voiceHandle;
				c.mType = VoiceCommand::SEEK;
				c.mValue = q.mSeek.load(std::memory_order_relaxed);
			}
			mSeekCommandListCount = kept;
			mSeekCommandPending.store(kept, std::memory_order_relaxed);
		}
		unsigned int i;
		for (i = 0; i < count; i++)
		{
			const VoiceCommand &c = mSeekCommandBatch[i];
			const int ch = getVoiceFromHandle_internal(c.mHandle);
			if (ch < 0)
				continue;
			applySeek_internal((unsigned int)ch, c.mValue);
			mVoiceSeekAppliedTicket[ch] = c.mTicket;
		}
	}
}
