#include "mixer_output.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <unordered_map>
#include <array>

#include "../device_lifecycle_test_hooks.h"
#include "pcm_converter.h"
#include "wav_output_encoder.h"

namespace {
struct CapturePacket {
  uint8_t *bytes;
  size_t length;
  uint32_t captureId;
  uint64_t readThrough;
};
MixerOutputMutex gPacketMutex;
constexpr size_t kThresholdPacketBytes = 64 * 1024;
#ifndef __EMSCRIPTEN__
std::unordered_map<uintptr_t, CapturePacket> gPackets;
constexpr size_t kMaxPendingPackets = 64;
size_t gReservedPackets = 0;
uintptr_t gNextPacket = 0;

// Reserve registry capacity and allocate without holding the ring-copy lock.
// Retirement cannot free this private storage before publication.
class NativePacketBuffer {
 public:
  explicit NativePacketBuffer(size_t capacity) : mBytes(nullptr, &std::free) {
    {
      std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
      if (gPackets.size() + gReservedPackets >= kMaxPendingPackets ||
          gNextPacket == UINTPTR_MAX) return;
      mToken = ++gNextPacket;
      ++gReservedPackets;
    }
    mBytes.reset(static_cast<uint8_t *>(std::malloc(capacity)));
    SOLOUD_TEST_BARRIER(mixerPacketStorageAllocated);
  }
  ~NativePacketBuffer() {
    if (mToken != 0) {
      std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
      --gReservedPackets;
    }
  }
  uint8_t *data() const { return mBytes.get(); }
  uint8_t *publish(size_t length, uint32_t captureId, uint64_t readThrough) {
    std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
    const uintptr_t token = mToken;
    gPackets.emplace(token, CapturePacket{data(), length, captureId, readThrough});
    mBytes.release();
    --gReservedPackets;
    mToken = 0;
    return reinterpret_cast<uint8_t *>(token);
  }
 private:
  uintptr_t mToken = 0;
  std::unique_ptr<uint8_t, decltype(&std::free)> mBytes;
};
#else
// The worklet cannot allocate: a contended allocator can wait on a futex,
// which is forbidden there. Start prepares a bounded pool on the main thread.
// Slots stay immutable until claimed or retired, including across restarts.
struct WebCapturePacket {
  std::vector<uint8_t> storage;
  uintptr_t token = 0;
  CapturePacket packet{};
};
std::array<WebCapturePacket, 8> gWebPackets;
WebCapturePacket *findWebPacket(uintptr_t token) {
  if (token == 0) return nullptr;
  for (auto &slot : gWebPackets)
    if (slot.token == token) return &slot;
  return nullptr;
}
void prepareWebPackets(size_t size) {
  std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
  for (auto &slot : gWebPackets) {
    // Grow even occupied slots: vector growth preserves their payload, and
    // consumers only hold tokens. Otherwise a restart with larger chunks
    // could leave every subsequently freed slot too small to use.
    if (slot.storage.size() < size) slot.storage.resize(size);
    slot.packet.bytes = slot.storage.data();
  }
}
#endif
#ifdef __EMSCRIPTEN__
uintptr_t gNextPacket = 0;
#endif
}

uint8_t *MixerOutput::copyPacket(const uint8_t *data, size_t length,
                                uint32_t captureId, uint64_t readThrough) {
#ifdef __EMSCRIPTEN__
  std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
  if (gNextPacket == UINTPTR_MAX) return nullptr;
  for (auto &slot : gWebPackets) {
    if (slot.token != 0 || slot.storage.size() < length) continue;
    std::memcpy(slot.storage.data(), data, length);
    slot.token = ++gNextPacket;
    slot.packet = {slot.storage.data(), length, captureId, readThrough};
    return reinterpret_cast<uint8_t *>(slot.token);
  }
  return nullptr;
#else
  NativePacketBuffer packet(length);
  if (packet.data() == nullptr) return nullptr;
  std::memcpy(packet.data(), data, length);
  return packet.publish(length, captureId, readThrough);
#endif
}

uint8_t *MixerOutput::takeStoppedPackets(uint32_t captureId, size_t &length) {
  length = 0;
  if (instance().isRunning() && instance().captureId() == captureId) return nullptr;
  // Reserve no heap storage under the registry lock. A packet may be retired
  // between these passes, but tokens are never reused, so it cannot be confused
  // with a replacement. Normal stop joins the publisher before this runs.
  std::array<uintptr_t, 64> tokens{};
  size_t count = 0, capacity = 0;
  {
    std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
#ifdef __EMSCRIPTEN__
    for (const auto &slot : gWebPackets) {
      if (slot.token == 0 || slot.packet.captureId != captureId) continue;
      tokens[count++] = slot.token;
      if (slot.packet.readThrough == 0) {
        if (slot.packet.length > SIZE_MAX - capacity) return nullptr;
        capacity += slot.packet.length;
      }
    }
#else
    for (const auto &entry : gPackets) {
      if (entry.second.captureId != captureId) continue;
      tokens[count++] = entry.first;
      if (entry.second.readThrough == 0) {
        if (entry.second.length > SIZE_MAX - capacity) return nullptr;
        capacity += entry.second.length;
      }
    }
#endif
  }
  std::sort(tokens.begin(), tokens.begin() + count);
  auto *bytes = capacity == 0 ? nullptr : static_cast<uint8_t *>(std::malloc(capacity));
  if (capacity != 0 && bytes == nullptr) return nullptr;
  {
    std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
    for (size_t i = 0; i < count; ++i) {
#ifdef __EMSCRIPTEN__
      auto *slot = findWebPacket(tokens[i]);
      if (slot == nullptr) continue;
      const auto &packet = slot->packet;
#else
      auto it = gPackets.find(tokens[i]);
      if (it == gPackets.end()) continue;
      const auto &packet = it->second;
#endif
      if (packet.readThrough == 0) {
        std::memcpy(bytes + length, packet.bytes, packet.length);
        length += packet.length;
      }
#ifdef __EMSCRIPTEN__
      slot->token = 0;
#else
      std::free(packet.bytes);
      gPackets.erase(it);
#endif
    }
  }
  return bytes;
}

uint8_t *MixerOutput::takePacket(uint8_t *token, uint32_t &captureId) {
  CapturePacket packet{};
#ifdef __EMSCRIPTEN__
  {
    std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
    auto *slot = findWebPacket(reinterpret_cast<uintptr_t>(token));
    if (slot == nullptr) return nullptr;
    packet.length = slot->packet.length;
  }
  // Allocation belongs to the main-thread consumer, outside the copy lock.
  // The worklet only ever waits for a bounded copy or token lookup.
  auto *bytes = static_cast<uint8_t *>(std::malloc(packet.length));
  if (bytes == nullptr) return nullptr;
  {
    std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
    auto *slot = findWebPacket(reinterpret_cast<uintptr_t>(token));
    if (slot != nullptr) {
      packet = slot->packet;
      std::memcpy(bytes, slot->storage.data(), packet.length);
      slot->token = 0;
    }
  }
  if (packet.bytes == nullptr) { std::free(bytes); return nullptr; }
  packet.bytes = bytes;
#else
  {
    std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
    auto it = gPackets.find(reinterpret_cast<uintptr_t>(token));
    if (it == gPackets.end()) return nullptr;
    packet = it->second;
    gPackets.erase(it);
  }
#endif
  captureId = packet.captureId;
  // An acknowledgement belongs to its original ring, never a replacement.
  // Admission also excludes start()/stop() while touching the ring's state.
  MixerOutput &capture = instance();
  const CapturePass pass(capture);
  if (pass && packet.readThrough != 0 && capture.captureId() == packet.captureId) {
    std::lock_guard<MixerOutputMutex> lock(capture.m_bufferMutex);
    // Overrun may already have dropped some or all of this packet's range.
    // Acknowledge its absolute end, not the same length from a newer read head.
    const uint64_t read = capture.m_readOffset.load(std::memory_order_relaxed);
    const uint64_t write = capture.m_writeOffset.load(std::memory_order_relaxed);
    capture.m_readOffset.store(std::max(read, std::min(write, packet.readThrough)),
                               std::memory_order_release);
    capture.m_notified.store(false, std::memory_order_release);
  }
  return packet.bytes;
}

void MixerOutput::discardPacket(uint8_t *token) {
  // Discard without acknowledging ring data (it may still be flushed on stop).
  std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
#ifdef __EMSCRIPTEN__
  for (auto &slot : gWebPackets)
    if (slot.token == reinterpret_cast<uintptr_t>(token)) slot.token = 0;
#else
  auto it = gPackets.find(reinterpret_cast<uintptr_t>(token));
  if (it == gPackets.end()) return;
  std::free(it->second.bytes);
  gPackets.erase(it);
#endif
}

void MixerOutput::discardPackets() {
  std::lock_guard<MixerOutputMutex> lock(gPacketMutex);
#ifdef __EMSCRIPTEN__
  for (auto &slot : gWebPackets) slot.token = 0;
#else
  for (const auto &entry : gPackets) std::free(entry.second.bytes);
  gPackets.clear();
#endif
  instance().m_notified.store(false, std::memory_order_release);
}

MixerOutput &MixerOutput::instance() {
  static MixerOutput instance;
  return instance;
}

MixerOutput::~MixerOutput() { stop(); discardPackets(); }

bool MixerOutput::isRunning() const { return m_running.load(); }

uint64_t MixerOutput::enterAudioCallback() {
  const uint64_t session = m_activeSession.load(std::memory_order_acquire);
  if (session == kNoCaptureSession) {
    return kNoCaptureSession;
  }

  m_audioCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);

  // Re-read after registering. If a stop slipped in between the two loads it
  // either sees this callback in the count and waits for it, or it retired
  // before the increment -- in which case the session no longer matches and
  // the callback backs out without touching anything.
  if (m_activeSession.load(std::memory_order_acquire) != session) {
    m_audioCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
    return kNoCaptureSession;
  }

  return session;
}

void MixerOutput::leaveAudioCallback() {
  m_audioCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void MixerOutput::retireSessionAndDrain() {
  m_activeSession.store(kNoCaptureSession, std::memory_order_release);

  // Spin rather than block: this runs on a control thread, the audio callback
  // holds the count for a few microseconds at most, and there is nothing here
  // a real-time thread could be made to wait on.
  while (m_audioCallbacksInFlight.load(std::memory_order_acquire) != 0) {
    std::this_thread::yield();
  }
}

bool MixerOutput::isCompressedFormat() const {
  return m_format == MIXER_OUTPUT_OPUS || m_format == MIXER_OUTPUT_VORBIS ||
         m_format == MIXER_OUTPUT_FLAC || m_format == MIXER_OUTPUT_WAV;
}

PlayerErrors MixerOutput::start(MixerOutputFormat format, int sampleRate,
                                int channels, size_t bufferSizeBytes,
                                size_t notificationThresholdBytes,
                                int chunkPCMFrames) {
  if (m_running.load()) {
    return playerAlreadyInited;
  }

  if (bufferSizeBytes == 0 || notificationThresholdBytes == 0 ||
      notificationThresholdBytes > bufferSizeBytes) {
    return invalidParameter;
  }

  if (sampleRate <= 0 || channels <= 0) {
    return invalidParameter;
  }

#if defined(NO_XIPH_LIBS)
  // Without the Xiph libraries only the PCM formats (and the WAV encoder,
  // which does not depend on them) are supported.
  if (format == MIXER_OUTPUT_OPUS || format == MIXER_OUTPUT_VORBIS ||
      format == MIXER_OUTPUT_FLAC) {
    return xiphLibsNotFound;
  }
#endif

  const bool isCompressed = format == MIXER_OUTPUT_OPUS ||
                            format == MIXER_OUTPUT_VORBIS ||
                            format == MIXER_OUTPUT_FLAC ||
                            format == MIXER_OUTPUT_WAV;

  const size_t bps = bytesPerSample(format);
  if (!isCompressed && bps == 0) {
    return audioFormatNotSupported;
  }
  const size_t bytesPerFrame = bps * channels;
  if (!isCompressed) {
    // Defaults need not divide evenly into surround frames. Drop only the
    // unused tail bytes, keeping every wrap and overrun on a frame boundary.
    bufferSizeBytes -= bufferSizeBytes % bytesPerFrame;
    if (bufferSizeBytes < bytesPerFrame ||
        (chunkPCMFrames <= 0 && notificationThresholdBytes > bufferSizeBytes))
      return invalidParameter;
  }

  if (isCompressed && chunkPCMFrames > 0) {
    return invalidParameter;
  }

  if (chunkPCMFrames > 0) {
    if (chunkPCMFrames < 2048) {
      return invalidParameter;
    }
    const size_t chunkBytes =
        static_cast<size_t>(chunkPCMFrames) * bytesPerFrame;
    if (chunkBytes == 0 || chunkBytes > bufferSizeBytes) {
      return invalidParameter;
    }
  }

#ifdef __EMSCRIPTEN__
  prepareWebPackets(chunkPCMFrames > 0
      ? static_cast<size_t>(chunkPCMFrames) * bytesPerFrame
      : std::min(bufferSizeBytes, kThresholdPacketBytes));
#endif

  m_format = format;
  m_sampleRate = sampleRate;
  m_channels = channels;
  m_bufferSize = bufferSizeBytes;
  m_notificationThreshold = notificationThresholdBytes;
  m_wavHeader.clear();

  // Increment the capture id so any in-flight callbacks from a previous
  // capture session can be identified and discarded by the Dart side.
  m_captureId.fetch_add(1);

  if (isCompressedFormat()) {
    m_encoder.reset(MixerOutputEncoder::create(format));
    if (m_encoder == nullptr || !m_encoder->initialize(sampleRate, channels)) {
      m_encoder.reset();
      m_format = MIXER_OUTPUT_PCM_F32LE;
      return audioFormatNotSupported;
    }

    m_bytesPerSample = m_encoder->inputBytesPerSample();
    m_bytesPerFrame = m_bytesPerSample * channels;

#ifndef __EMSCRIPTEN__
    m_pcmQueue = std::make_unique<PcmChunkQueue>();
#endif
    m_buffer.assign(m_bufferSize, 0);
    m_writeOffset.store(0);
    m_readOffset.store(0);
    m_notified.store(false);
    m_shouldStop.store(false);
    m_running.store(true);

    m_chunkPCMFrames = 0;
    m_chunkBytes = 0;
    m_chunkBuffer.clear();

#ifdef __EMSCRIPTEN__
    // On web there are no worker threads: encoding runs inline in
    // onAudioData(), which is called on the main browser thread (the audio
    // thread there). Write the stream header up front like the encoder
    // thread does on native.
    std::vector<uint8_t> header;
    if (m_encoder->writeCurrentHeader(header) && !header.empty()) {
      writeToBuffer(header.data(), header.size());
    }
#else
    m_encoderThread = std::thread(&MixerOutput::encoderThreadFunc, this);
    m_notificationThread =
        std::thread(&MixerOutput::notificationThreadFunc, this);
#endif

    // Published last, and only now: every buffer, pointer and worker this
    // session needs is in place, so an audio callback admitted from here on
    // cannot observe half-built state.
    m_activeSession.store(m_nextSession.fetch_add(1, std::memory_order_acq_rel),
                          std::memory_order_release);
  } else {
    m_bytesPerSample = bps;
    m_bytesPerFrame = bytesPerFrame;

    if (chunkPCMFrames > 0) {
      m_chunkPCMFrames = static_cast<size_t>(chunkPCMFrames);
      m_chunkBytes = m_chunkPCMFrames * m_bytesPerFrame;
      m_chunkBuffer.assign(m_chunkBytes, 0);
    } else {
      m_chunkPCMFrames = 0;
      m_chunkBytes = 0;
      m_chunkBuffer.clear();
    }

    m_buffer.assign(m_bufferSize, 0);
    m_writeOffset.store(0);
    m_readOffset.store(0);
    m_notified.store(false);
    m_shouldStop.store(false);
    m_running.store(true);

#ifndef __EMSCRIPTEN__
    m_notificationThread =
        std::thread(&MixerOutput::notificationThreadFunc, this);
#endif

    // See the compressed branch above: publish last.
    m_activeSession.store(m_nextSession.fetch_add(1, std::memory_order_acq_rel),
                          std::memory_order_release);
  }

  return noError;
}

void MixerOutput::stop() {
  if (!m_running.load()) {
    return;
  }

  // Retire the session and wait out any audio callback already inside it
  // *before* touching the state it is using. Everything below this line --
  // the encoder, the PCM queue, the buffers -- is owned by the session that
  // was just retired.
  retireSessionAndDrain();

  m_running.store(false);
  m_shouldStop.store(true);

  if (m_pcmQueue != nullptr) {
    m_pcmQueue->stop();
  }

#ifdef __EMSCRIPTEN__
  // On web there is no encoder thread: flush the encoder tail inline so the
  // circular buffer is complete when this returns. The Dart side reads the
  // remaining data synchronously right after stop (_flushRemaining), and
  // joining a pthread here would block the main browser thread.
  if (m_encoder != nullptr) {
    m_encodeScratch.clear();
    if (m_encoder->finalize(m_encodeScratch) && !m_encodeScratch.empty()) {
      writeToBuffer(m_encodeScratch.data(), m_encodeScratch.size());
    }
  }
#endif

  if (m_encoderThread.joinable()) {
    m_encoderThread.join();
  }

  if (m_notificationThread.joinable()) {
    m_notificationThread.join();
  }

  // Cache the final WAV header after the encoder thread has finished so the
  // total sample count reflects all queued PCM data.
  if (m_encoder != nullptr) {
    m_encoder->writeCurrentHeader(m_wavHeader);
  }

  m_encoder.reset();
  m_pcmQueue.reset();

  // Keep the circular buffer and offsets intact so the Dart side can read
  // any tail data that did not reach the notification threshold.
  m_notified.store(false);
}

void MixerOutput::onAudioData(const float *data, unsigned int frames) {
  // Admission, not a bare `m_running` check: everything read below belongs to
  // the capture session and stop() must be able to wait this callback out
  // before destroying it.
  const CapturePass pass(*this);
  if (!pass) {
    return;
  }

  SOLOUD_TEST_BARRIER(mixerCaptureCallbackAdmitted);

  const size_t totalSamples = static_cast<size_t>(frames) * m_channels;

  if (totalSamples == 0) {
    return;
  }

  if (isCompressedFormat()) {
#ifdef __EMSCRIPTEN__
    // On web the audio callback runs on the main browser thread, so there
    // are no queue/encoder threads: encode inline and notify directly.
    if (m_encoder != nullptr) {
      m_encodeScratch.clear();
      if (m_encoder->encode(data, totalSamples, m_encodeScratch) &&
          !m_encodeScratch.empty()) {
        writeToBuffer(m_encodeScratch.data(), m_encodeScratch.size());
      }
    }
    processNotifications();
#else
    std::vector<float> chunk(data, data + totalSamples);
    m_pcmQueue->push(std::move(chunk));
#endif
    return;
  }

  const size_t bytesToWrite = totalSamples * m_bytesPerSample;
  {
    std::lock_guard<MixerOutputMutex> lock(m_bufferMutex);
    const uint64_t oldWrite = m_writeOffset.load(std::memory_order_relaxed);
    const uint64_t newWrite = oldWrite + bytesToWrite;
    const size_t kept = std::min(bytesToWrite, m_bufferSize);
    const size_t skipped = bytesToWrite - kept;
    const size_t offset = (oldWrite + skipped) % m_bufferSize;
    const size_t first = std::min(kept, m_bufferSize - offset);
    // A large device callback retains only its newest complete frames. Both
    // copies are bounded by the allocation, even after an uneven wrap.
    convertPcm(data + skipped / m_bytesPerSample, m_buffer.data() + offset,
               first / m_bytesPerSample, m_format);
    if (first < kept)
      convertPcm(data + (skipped + first) / m_bytesPerSample, m_buffer.data(),
                 (kept - first) / m_bytesPerSample, m_format);
    const uint64_t read = m_readOffset.load(std::memory_order_relaxed);
    m_readOffset.store(std::max(read, newWrite > m_bufferSize ? newWrite - m_bufferSize : 0),
                       std::memory_order_release);
    m_writeOffset.store(newWrite, std::memory_order_release);
  }

#ifdef __EMSCRIPTEN__
  processNotifications();
#endif
}

bool MixerOutput::notifyOnePacket(size_t minimum, size_t fixedBytes) {
  const auto callback = m_callback.load(std::memory_order_acquire);
  if (callback == nullptr) return false;
  uint8_t *token;
  size_t length;
#ifndef __EMSCRIPTEN__
  // Only bounded copies and offset accounting run under m_bufferMutex.
  // The audio thread can wait for that copy, never for allocation, registry
  // insertion, packet retirement or a consumer callback.
  const uint64_t observedRead = m_readOffset.load(std::memory_order_acquire);
  if (m_writeOffset.load(std::memory_order_acquire) - observedRead < minimum ||
      (fixedBytes == 0 && m_notified.load())) return false;
  NativePacketBuffer packet(fixedBytes > 0 ? fixedBytes :
                           std::min(m_bufferSize, kThresholdPacketBytes));
  if (packet.data() == nullptr) return false;
  uint64_t readThrough = 0;
#endif
  {
    std::lock_guard<MixerOutputMutex> lock(m_bufferMutex);
    const uint64_t read = m_readOffset.load(std::memory_order_relaxed);
    const size_t available = m_writeOffset.load(std::memory_order_relaxed) - read;
    if (available < minimum || (fixedBytes == 0 && m_notified.load())) return false;
    const size_t offset = read % m_bufferSize;
    length = fixedBytes > 0 ? fixedBytes : std::min(available, m_bufferSize - offset);
    // Threshold packets need not span the entire ring. Bound this copy so
    // the audio producer cannot wait for an arbitrarily large snapshot.
    if (fixedBytes == 0) length = std::min(length, kThresholdPacketBytes);
    // Each queued notification owns a distinct snapshot. Neither the next
    // chunk nor a capture restart can change bytes an event has already sent.
#ifdef __EMSCRIPTEN__
    if (fixedBytes > 0) {
      const size_t first = std::min(length, m_bufferSize - offset);
      std::memcpy(m_chunkBuffer.data(), m_buffer.data() + offset, first);
      if (first < length)
        std::memcpy(m_chunkBuffer.data() + first, m_buffer.data(), length - first);
      token = copyPacket(m_chunkBuffer.data(), length, captureId());
    } else {
      token = copyPacket(m_buffer.data() + offset, length, captureId(), read + length);
    }
    if (token == nullptr) return false;
#else
    const size_t first = std::min(length, m_bufferSize - offset);
    std::memcpy(packet.data(), m_buffer.data() + offset, first);
    if (first < length)
      std::memcpy(packet.data() + first, m_buffer.data(), length - first);
    if (fixedBytes == 0) readThrough = read + length;
#endif
    if (fixedBytes > 0)
      m_readOffset.store(read + length, std::memory_order_release);
    else
      m_notified.store(true, std::memory_order_release);
  }
#ifndef __EMSCRIPTEN__
  token = packet.publish(length, captureId(), readThrough);
#endif
  callback(token, length);
  return true;
}

#ifdef __EMSCRIPTEN__
void MixerOutput::processNotifications() {
  if (m_chunkBytes > 0) {
    while (notifyOnePacket(m_chunkBytes, m_chunkBytes)) {}
  } else {
    notifyOnePacket(m_notificationThreshold, 0);
  }
}
#endif

void MixerOutput::writeToBuffer(const uint8_t *data, size_t bytes) {
  if (bytes == 0 || data == nullptr || m_bufferSize == 0) {
    return;
  }

  std::lock_guard<MixerOutputMutex> lock(m_bufferMutex);
  const uint64_t oldWrite = m_writeOffset.load(std::memory_order_relaxed);
  const uint64_t newWrite = oldWrite + bytes;
  const size_t kept = std::min(bytes, m_bufferSize);
  const size_t skipped = bytes - kept;
  const size_t offset = (oldWrite + skipped) % m_bufferSize;
  const size_t first = std::min(kept, m_bufferSize - offset);
  std::memcpy(m_buffer.data() + offset, data + skipped, first);
  if (first < kept)
    std::memcpy(m_buffer.data(), data + skipped + first, kept - first);
  const uint64_t read = m_readOffset.load(std::memory_order_relaxed);
  m_readOffset.store(std::max(read, newWrite > m_bufferSize ? newWrite - m_bufferSize : 0),
                     std::memory_order_release);
  m_writeOffset.store(newWrite, std::memory_order_release);
}

void MixerOutput::encoderThreadFunc() {
  if (m_encoder == nullptr || m_pcmQueue == nullptr) {
    return;
  }

  std::vector<float> chunk;
  std::vector<uint8_t> encoded;

  // Write the WAV header at the start of the stream so the file begins with
  // a valid-ish RIFF/WAVE header. The size fields are patched after capture.
  std::vector<uint8_t> header;
  if (m_encoder->writeCurrentHeader(header) && !header.empty()) {
    writeToBuffer(header.data(), header.size());
  }

  while (m_pcmQueue->pop(chunk)) {
    if (!m_encoder->encode(chunk.data(), chunk.size(), encoded)) {
      continue;
    }

    if (!encoded.empty()) {
      writeToBuffer(encoded.data(), encoded.size());
      encoded.clear();
    }
  }

  // Flush remaining encoded data.
  if (m_encoder->finalize(encoded) && !encoded.empty()) {
    writeToBuffer(encoded.data(), encoded.size());
  }
}

uint8_t *MixerOutput::getBufferPointer() {
  if (m_buffer.empty()) {
    return nullptr;
  }
  return m_buffer.data();
}

size_t MixerOutput::getBufferSize() const { return m_bufferSize; }

size_t MixerOutput::getAvailableBytes() const {
  std::lock_guard<MixerOutputMutex> lock(m_bufferMutex);
  return m_writeOffset.load(std::memory_order_relaxed) -
         m_readOffset.load(std::memory_order_relaxed);
}

size_t MixerOutput::getReadOffset() const {
  return m_bufferSize == 0 ? 0 : m_readOffset.load(std::memory_order_acquire) % m_bufferSize;
}

void MixerOutput::advanceReadPosition(size_t bytes) {
  if (bytes == 0 || m_bufferSize == 0) {
    return;
  }

  std::lock_guard<MixerOutputMutex> lock(m_bufferMutex);
  const uint64_t readOffset = m_readOffset.load(std::memory_order_relaxed);
  const size_t available = m_writeOffset.load(std::memory_order_relaxed) - readOffset;
  const size_t toAdvance = std::min(bytes, available);
  m_readOffset.store(readOffset + toAdvance, std::memory_order_release);

  if (toAdvance > 0) {
    m_notified.store(false, std::memory_order_release);
  }
}

void MixerOutput::setDataCallback(MixerOutputDataCallback callback) {
  m_callback.store(callback, std::memory_order_release);
}

void MixerOutput::notificationThreadFunc() {
  while (!m_shouldStop.load()) {
    if (m_chunkBytes > 0) {
      // A fixed chunk is consumed here, so it needs no Dart acknowledgement.
      // Drain every complete chunk; a threshold latch would strand a backlog.
      while (!m_shouldStop.load() && notifyOnePacket(m_chunkBytes, m_chunkBytes)) {}
    } else {
      notifyOnePacket(m_notificationThreshold, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

size_t MixerOutput::contiguousAvailable() const {
  const size_t available = getAvailableBytes();
  const size_t readOffset = getReadOffset();
  return std::min(available, m_bufferSize - readOffset);
}
