/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#if defined(__ANDROID__)
#include <sys/resource.h>
#include <android/log.h>
#include <cerrno>
#include <unistd.h>
#include <sys/syscall.h>
#endif
#include <algorithm>
#include <array>
#include <optional>
#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/diagnostics/gta4_transition.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>
#include <rex/platform.h>

#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
#include <pthread/qos.h>
#endif

// Each guest audio block is 256 samples (~5.3 ms at 48 kHz) and the queue
// runs full, so the depth is the audio latency: 64 blocks lagged ~340 ms
// behind the picture on Android. 32 (~170 ms) removes the noticeable delay;
// 16 (~85 ms) was lower still but stuttered briefly with the game's frame
// hitches, on the Retroid Pocket 5 and on a Snapdragon 8 Elite phone.
REXCVAR_DEFINE_INT32(
    audio_maxqframes, REX_PLATFORM_ANDROID ? 32 : 64, "Audio",
    "Maximum buffered guest audio blocks (range 1-64). The backend selects the initial depth.");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

#include "handoff_trace.inc"

namespace rex::audio {

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)),
               static_cast<uint32_t>(1)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
  handoff::Shutdown();
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  handoff::Initialize();
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
  // The guest mixer callback is the producer for the native output ring. Give
  // it interactive QoS so graphics and code-generation load cannot starve
  // audio production long enough to drain the reliability buffer.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

#if defined(__ANDROID__)
  // Same reason as the macOS QoS above: render threads saturate the big
  // cores, and an equal-priority audio producer underruns (crackling).
  // -16 is Android's ANDROID_PRIORITY_AUDIO, which apps may request.
  {
    pid_t tid = syscall(SYS_gettid);
    const int result = setpriority(PRIO_PROCESS, tid, -16);
    __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "audio-priority %s tid=%d result=%d errno=%d",
                        "audio_system.cpp", int(tid), result, errno);
  }
#endif

  // Main execution loop for the Audio Worker
  while (worker_running_) {
    // Process guest audio queue frames and stream them to native output
    this->Update();
    
    // Safety check to prevent absolute 100% CPU lock when idle
    rex::thread::Sleep(std::chrono::milliseconds(2));
  }
}

}  // namespace rex::audio
