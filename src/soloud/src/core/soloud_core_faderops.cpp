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

#include "soloud_internal.h"

// Core operations related to faders (not including filters)

namespace SoLoud
{
	void Soloud::schedulePause(handle aVoiceHandle, time aTime)
	{
		if (aTime <= 0)
		{
			setPause(aVoiceHandle, 1);
			return;
		}
		FOR_ALL_VOICES_PRE
		mVoice[ch]->mPauseScheduler.set(1, 0, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::scheduleStop(handle aVoiceHandle, time aTime)
	{
		if (aTime <= 0)
		{
			stop(aVoiceHandle);
			return;
		}
		FOR_ALL_VOICES_PRE
		mVoice[ch]->mStopScheduler.set(1, 0, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::scheduleStopAt(handle aVoiceHandle, time aEngineTime)
	{
		lockAudioMutex_internal();
		time rel = aEngineTime - mStreamTime;
		unlockAudioMutex_internal();
		// The countdown is in output samples and, like the delay countdown
		// used by playScheduled, starts from the first sample of the next
		// output buffer (whose position is exactly mStreamTime), so the
		// voice is stopped with sample accuracy.
		long long samples = rel > 0 ? (long long)floor(rel * mSamplerate + 0.5) : 0;
		FOR_ALL_VOICES_PRE
		// ###### flutter_soloud local patch (retroactive re-mix) ######
		// An in-window scheduled stop re-mixes the window so the voice goes
		// silent at exactly aEngineTime (clamped to the playhead when that is
		// already in the past) rather than surviving until the write head
		// catches up. Declines when the gate refuses, and for times beyond
		// the write head (ordinary future scheduling).
		if (!retroactiveStopVoiceAt_internal(ch, aEngineTime))
		{
			if (rel <= 0)
			{
				// The scheduled time is already in the past: stop immediately.
				stopVoice_internal(ch);
			}
			else
			{
				mVoice[ch]->mStopSamplesLeft = samples;
				journalDeath_internal(ch, aEngineTime);
			}
		}
		FOR_ALL_VOICES_POST
	}

	// ###### flutter_soloud local patch (fade starting values) ######
	// The public getters read the last published block without waiting for a
	// mix. A fader must instead start from the voice's live value after the
	// audio mutex has drained queued changes: the current block may already
	// have advanced an earlier fade. Read and decide under that same hold,
	// for each voice separately when the handle names a group.
	void Soloud::scheduleFadeAt(handle aVoiceHandle, time aEngineTime, float aTo, time aFadeTime, bool aThenStop)
	{
		lockAudioMutex_internal();
		const time playhead = playheadTimeLocked_internal();
		if (aEngineTime < playhead)
			aEngineTime = playhead;
		const time rel = aEngineTime - mStreamTime;
		const bool tryRetro = mRenderRing.isInited() && rel < 0;
		FOR_ALL_VOICES_LOCKED_PRE
			const float from = mVoice[ch]->mSetVolume;
			if (tryRetro && retroactiveParam_internal(ch, RetroJournalEntry::PARAM_VOLUME,
				aTo, aFadeTime, aEngineTime))
			{
				// The rollback placed the fade at its requested time.
			}
			else if (rel <= 0)
			{
				if (aFadeTime <= 0 || aTo == from)
				{
					mVoice[ch]->mVolumeFader.mActive = 0;
					setVoiceVolume_internal(ch, aTo);
				}
				else
					mVoice[ch]->mVolumeFader.set(from, aTo, aFadeTime, mVoice[ch]->mStreamTime);
			}
			else
				mVoice[ch]->mVolumeFader.setScheduled(from, aTo, aFadeTime, mVoice[ch]->mStreamTime + rel);
		FOR_ALL_VOICES_LOCKED_POST
		unlockAudioMutex_internal();
		if (aThenStop)
			scheduleStopAt(aVoiceHandle, aEngineTime + aFadeTime);
	}

	void Soloud::fadeVolume(handle aVoiceHandle, float aTo, time aTime)
	{
		if (aTime <= 0)
		{
			setVolume(aVoiceHandle, aTo);
			return;
		}

		FOR_ALL_VOICES_PRE
			const float from = mVoice[ch]->mSetVolume;
			if (aTo == from)
			{
				if (!retroactiveParam_internal(ch, RetroJournalEntry::PARAM_VOLUME, aTo, 0, playheadTimeLocked_internal()))
				{
					mVoice[ch]->mVolumeFader.mActive = 0;
					setVoiceVolume_internal(ch, aTo);
				}
			}
			else if (!retroactiveParam_internal(ch, RetroJournalEntry::PARAM_VOLUME, aTo, aTime, playheadTimeLocked_internal()))
				mVoice[ch]->mVolumeFader.set(from, aTo, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::fadePan(handle aVoiceHandle, float aTo, time aTime)
	{
		if (aTime <= 0)
		{
			setPan(aVoiceHandle, aTo);
			return;
		}

		FOR_ALL_VOICES_PRE
			const float from = mVoice[ch]->mPan;
			if (aTo == from)
			{
				if (!retroactiveParam_internal(ch, RetroJournalEntry::PARAM_PAN, aTo, 0, playheadTimeLocked_internal()))
				{
					mVoice[ch]->mPanFader.mActive = 0;
					setVoicePan_internal(ch, aTo);
				}
			}
			else if (!retroactiveParam_internal(ch, RetroJournalEntry::PARAM_PAN, aTo, aTime, playheadTimeLocked_internal()))
				mVoice[ch]->mPanFader.set(from, aTo, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::fadeRelativePlaySpeed(handle aVoiceHandle, float aTo, time aTime)
	{
		float from = getRelativePlaySpeed(aVoiceHandle);
		if (aTime <= 0 || aTo == from)
		{
			setRelativePlaySpeed(aVoiceHandle, aTo);
			return;
		}
		FOR_ALL_VOICES_PRE
		// ###### flutter_soloud local patch (retroactive re-mix) ######
		if (!retroactiveParam_internal(ch, RetroJournalEntry::PARAM_SPEED, aTo, aTime, playheadTimeLocked_internal()))
			mVoice[ch]->mRelativePlaySpeedFader.set(from, aTo, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::fadeGlobalVolume(float aTo, time aTime)
	{
		float from = getGlobalVolume();
		if (aTime <= 0 || aTo == from)
		{
			setGlobalVolume(aTo);
			return;
		}
		mGlobalVolumeFader.set(from, aTo, aTime, mStreamTime);
	}


	void Soloud::oscillateVolume(handle aVoiceHandle, float aFrom, float aTo, time aTime)
	{
		if (aTime <= 0 || aTo == aFrom)
		{
			setVolume(aVoiceHandle, aTo);
			return;
		}

		FOR_ALL_VOICES_PRE
		mVoice[ch]->mVolumeFader.setLFO(aFrom, aTo, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::oscillatePan(handle aVoiceHandle, float aFrom, float aTo, time aTime)
	{
		if (aTime <= 0 || aTo == aFrom)
		{
			setPan(aVoiceHandle, aTo);
			return;
		}

		FOR_ALL_VOICES_PRE
		mVoice[ch]->mPanFader.setLFO(aFrom, aTo, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::oscillateRelativePlaySpeed(handle aVoiceHandle, float aFrom, float aTo, time aTime)
	{
		if (aTime <= 0 || aTo == aFrom)
		{
			setRelativePlaySpeed(aVoiceHandle, aTo);
			return;
		}
		
		FOR_ALL_VOICES_PRE
		mVoice[ch]->mRelativePlaySpeedFader.setLFO(aFrom, aTo, aTime, mVoice[ch]->mStreamTime);
		FOR_ALL_VOICES_POST
	}

	void Soloud::oscillateGlobalVolume(float aFrom, float aTo, time aTime)
	{
		if (aTime <= 0 || aTo == aFrom)
		{
			setGlobalVolume(aTo);
			return;
		}
		mGlobalVolumeFader.setLFO(aFrom, aTo, aTime, mStreamTime);
	}
}
