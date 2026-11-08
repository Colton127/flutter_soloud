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

// Voice group operations

// ###### flutter_soloud local patch (non-blocking voice control) ######
// The voice group table (mVoiceGroup, mVoiceGroupCount) is written with both
// the audio mutex and mVoiceGroupMutex held, in that order, so it can be read
// with either one: the mixer and the FOR_ALL_VOICES_PRE operations read it
// under the audio mutex; isVoiceGroup() and the queued voice control calls
// (setVolume, seek, ...) read it under mVoiceGroupMutex alone, without
// waiting for a mix block. mVoiceGroupMutex is released before the audio
// mutex: unlocking that can dispatch voice-ended callbacks, which may call
// back into these functions.

namespace SoLoud
{
	// Create a voice group. Returns 0 if unable (out of voice groups / out of memory)
	handle Soloud::createVoiceGroup()
	{
		lockAudioMutex_internal();
		mVoiceGroupMutex.lock();

		handle result = 0;
		unsigned int i;
		bool reused = false;
		// Check if there's any deleted voice groups and re-use if found
		for (i = 0; i < mVoiceGroupCount; i++)
		{
			if (mVoiceGroup[i] == NULL)
			{
				reused = true;
				mVoiceGroup[i] = new unsigned int[17];
				if (mVoiceGroup[i] != NULL)
				{
					mVoiceGroup[i][0] = 16;
					mVoiceGroup[i][1] = 0;
					result = 0xfffff000 | i;
				}
				break;
			}
		}
		if (!reused && mVoiceGroupCount != 4096)
		{
			unsigned int oldcount = mVoiceGroupCount;
			if (mVoiceGroupCount == 0)
			{
				mVoiceGroupCount = 4;
			}
			mVoiceGroupCount *= 2;
			unsigned int **vg = new unsigned int * [mVoiceGroupCount];
			if (vg == NULL)
			{
				mVoiceGroupCount = oldcount;
			}
			else
			{
				for (i = 0; i < oldcount; i++)
				{
					vg[i] = mVoiceGroup[i];
				}

				for (; i < mVoiceGroupCount; i++)
				{
					vg[i] = NULL;
				}

				delete[] mVoiceGroup;
				mVoiceGroup = vg;
				i = oldcount;
				mVoiceGroup[i] = new unsigned int[17];
				if (mVoiceGroup[i] != NULL)
				{
					mVoiceGroup[i][0] = 16;
					mVoiceGroup[i][1] = 0;
					result = 0xfffff000 | i;
				}
			}
		}

		mVoiceGroupMutex.unlock();
		unlockAudioMutex_internal();
		return result;
	}

	// Destroy a voice group.
	result Soloud::destroyVoiceGroup(handle aVoiceGroupHandle)
	{
		lockAudioMutex_internal();
		mVoiceGroupMutex.lock();
		result res = INVALID_PARAMETER;
		if (isVoiceGroupLocked_internal(aVoiceGroupHandle))
		{
			int c = aVoiceGroupHandle & 0xfff;
			delete[] mVoiceGroup[c];
			mVoiceGroup[c] = NULL;
			res = SO_NO_ERROR;
		}
		mVoiceGroupMutex.unlock();
		unlockAudioMutex_internal();
		return res;
	}

	// Add a voice handle to a voice group
	result Soloud::addVoiceToGroup(handle aVoiceGroupHandle, handle aVoiceHandle)
	{
		lockAudioMutex_internal();
		mVoiceGroupMutex.lock();

		result res = SO_NO_ERROR;
		if (!isVoiceGroupLocked_internal(aVoiceGroupHandle))
		{
			res = INVALID_PARAMETER;
		}
		// Don't consider adding invalid voice handles as an error, since the voice may just have ended.
		else if (isValidVoiceHandle(aVoiceHandle))
		{
			trimVoiceGroupLocked_internal(aVoiceGroupHandle);

			int c = aVoiceGroupHandle & 0xfff;
			unsigned int i;
			bool done = false;

			for (i = 1; i < mVoiceGroup[c][0]; i++)
			{
				if (mVoiceGroup[c][i] == aVoiceHandle)
				{
					done = true; // already there
					break;
				}

				if (mVoiceGroup[c][i] == 0)
				{
					mVoiceGroup[c][i] = aVoiceHandle;
					mVoiceGroup[c][i + 1] = 0;
					done = true;
					break;
				}
			}

			if (!done)
			{
				// Full group, allocate more memory
				unsigned int * n = new unsigned int[mVoiceGroup[c][0] * 2 + 1];
				if (n == NULL)
				{
					res = OUT_OF_MEMORY;
				}
				else
				{
					for (i = 0; i < mVoiceGroup[c][0]; i++)
						n[i] = mVoiceGroup[c][i];
					n[n[0]] = aVoiceHandle;
					n[n[0]+1] = 0;
					n[0] *= 2;
					delete[] mVoiceGroup[c];
					mVoiceGroup[c] = n;
				}
			}
		}

		mVoiceGroupMutex.unlock();
		unlockAudioMutex_internal();
		return res;
	}

	// Is this handle a valid voice group?
	bool Soloud::isVoiceGroup(handle aVoiceGroupHandle)
	{
		if ((aVoiceGroupHandle & 0xfffff000) != 0xfffff000)
			return 0;
		// mVoiceGroupMutex alone: callers (the FFI layer's handle check
		// before every voice control call) must not wait for a mix block.
		std::lock_guard<std::mutex> groupLock(mVoiceGroupMutex);
		return isVoiceGroupLocked_internal(aVoiceGroupHandle);
	}

	bool Soloud::isVoiceGroupLocked_internal(handle aVoiceGroupHandle) const
	{
		if ((aVoiceGroupHandle & 0xfffff000) != 0xfffff000)
			return 0;
		unsigned int c = aVoiceGroupHandle & 0xfff;
		if (c >= mVoiceGroupCount)
			return 0;
		return mVoiceGroup[c] != NULL;
	}

	// Is this voice group empty?
	bool Soloud::isVoiceGroupEmpty(handle aVoiceGroupHandle)
	{
		lockAudioMutex_internal();
		mVoiceGroupMutex.lock();
		// If not a voice group, yeah, we're empty alright..
		bool res = 1;
		if (isVoiceGroupLocked_internal(aVoiceGroupHandle))
		{
			trimVoiceGroupLocked_internal(aVoiceGroupHandle);
			int c = aVoiceGroupHandle & 0xfff;
			res = mVoiceGroup[c][1] == 0;
		}
		mVoiceGroupMutex.unlock();
		unlockAudioMutex_internal();
		return res;
	}

	// Remove all non-active voices from group
	void Soloud::trimVoiceGroup_internal(handle aVoiceGroupHandle)
	{
		lockAudioMutex_internal();
		mVoiceGroupMutex.lock();
		if (isVoiceGroupLocked_internal(aVoiceGroupHandle))
			trimVoiceGroupLocked_internal(aVoiceGroupHandle);
		mVoiceGroupMutex.unlock();
		unlockAudioMutex_internal();
	}

	// Both locks held for the whole trim. isValidVoiceHandle() no longer takes
	// the audio mutex, so this no longer drops and re-takes it around every
	// check the way the original loop did.
	void Soloud::trimVoiceGroupLocked_internal(handle aVoiceGroupHandle)
	{
		SOLOUD_ASSERT(mInsideAudioThreadMutex);
		int c = aVoiceGroupHandle & 0xfff;

		// empty group
		if (mVoiceGroup[c][1] == 0)
		{
			return;
		}

		unsigned int i;
		// first item in voice group is number of allocated indices
		for (i = 1; i < mVoiceGroup[c][0]; i++)
		{
			// If we hit a voice in the group that's not set, we're done
			if (mVoiceGroup[c][i] == 0)
			{
				return;
			}

			while (!isValidVoiceHandle(mVoiceGroup[c][i]))
			{
				// current index is an invalid handle, move all following handles backwards
				unsigned int j;
				for (j = i; j < mVoiceGroup[c][0] - 1; j++)
				{
					mVoiceGroup[c][j] = mVoiceGroup[c][j + 1];
					// not a full group, we can stop copying
					if (mVoiceGroup[c][j] == 0)
						break;
				}
				// be sure to mark the last one as unused in any case
				mVoiceGroup[c][mVoiceGroup[c][0] - 1] = 0;
				// did we end up with an empty group? we're done then
				if (mVoiceGroup[c][i] == 0)
				{
					return;
				}
			}
		}
	}

	handle *Soloud::voiceGroupHandleToArray_internal(handle aVoiceGroupHandle) const
	{
		if ((aVoiceGroupHandle & 0xfffff000) != 0xfffff000)
			return NULL;
		unsigned int c = aVoiceGroupHandle & 0xfff;
		if (c >= mVoiceGroupCount)
			return NULL;
		if (mVoiceGroup[c] == NULL)
			return NULL;
		return mVoiceGroup[c] + 1;
	}

}
