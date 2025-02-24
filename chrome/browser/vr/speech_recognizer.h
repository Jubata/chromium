// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_VR_SPEECH_RECOGNIZER_H_
#define CHROME_BROWSER_VR_SPEECH_RECOGNIZER_H_

#include <string>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"

namespace content {
class SpeechRecognitionManager;
}

namespace net {
class URLRequestContextGetter;
}

namespace vr {

// Note that speech recognition is activated on VR UI thread. This means it
// usually involves 3 threads. In the simplest case, the thread communication
// looks like the following:
//     VR UI thread        Browser thread         IO thread
//          |                    |                    |
//          |----ActivateVS----->|                    |
//          |                    |------Start------>  |
//          |                    |                    |
//          |                    |<-NotifyStateChange-|
//          |<--OnSRStateChanged-|                    |
//          |                    |                    |
//          |                    |<--OnSpeechResult---|
//          |<--OnSRStateChanged-|                    |
//          |                 navigate                |
//          |                    |                    |
// VS = voice search, SR = speech recognition

enum SpeechRecognitionState {
  SPEECH_RECOGNITION_OFF = 0,
  SPEECH_RECOGNITION_READY,
  SPEECH_RECOGNITION_END,
  SPEECH_RECOGNITION_RECOGNIZING,
  SPEECH_RECOGNITION_IN_SPEECH,
  SPEECH_RECOGNITION_TRY_AGAIN,
  SPEECH_RECOGNITION_NETWORK_ERROR,
};

class VoiceResultDelegate {
 public:
  virtual void OnVoiceResults(const base::string16& result) = 0;
};

class BrowserUiInterface;
class SpeechRecognizerOnIO;

// An interface for IO to communicate with browser UI thread.
// This is used by SpeechRecognizerOnIO class who lives on IO thread.
class IOBrowserUIInterface {
 public:
  // Receive a speech recognition result. |is_final| indicated whether the
  // result is an intermediate or final result. If |is_final| is true, then the
  // recognizer stops and no more results will be returned.
  virtual void OnSpeechResult(const base::string16& query, bool is_final) = 0;

  // Invoked regularly to indicate the average sound volume.
  virtual void OnSpeechSoundLevelChanged(float level) = 0;

  // Invoked when the state of speech recognition is changed.
  virtual void OnSpeechRecognitionStateChanged(
      SpeechRecognitionState new_state) = 0;

 protected:
  virtual ~IOBrowserUIInterface() {}
};

// SpeechRecognizer is a wrapper around the speech recognition engine that
// simplifies its use from the UI thread. This class handles all setup/shutdown,
// collection of results, error cases, and threading.
class SpeechRecognizer : public IOBrowserUIInterface {
 public:
  SpeechRecognizer(VoiceResultDelegate* delegate,
                   BrowserUiInterface* ui,
                   net::URLRequestContextGetter* url_request_context_getter,
                   const std::string& locale);
  ~SpeechRecognizer() override;

  // Start/stop the speech recognizer.
  // Must be called on the UI thread.
  void Start();
  void Stop();

  // Overridden from vr::IOBrowserUIInterface:
  void OnSpeechResult(const base::string16& query, bool is_final) override;
  void OnSpeechSoundLevelChanged(float level) override;
  void OnSpeechRecognitionStateChanged(
      vr::SpeechRecognitionState new_state) override;

  void GetSpeechAuthParameters(std::string* auth_scope,
                               std::string* auth_token);

  static void SetManagerForTest(content::SpeechRecognitionManager* manager);
  void SetSpeechTimerForTest(std::unique_ptr<base::Timer> speech_timer);

 private:
  VoiceResultDelegate* delegate_;
  BrowserUiInterface* ui_;
  scoped_refptr<net::URLRequestContextGetter> url_request_context_getter_;
  std::string locale_;

  // Note that this object is destroyed on IO thread.
  std::unique_ptr<SpeechRecognizerOnIO> speech_recognizer_on_io_;
  base::WeakPtrFactory<SpeechRecognizer> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(SpeechRecognizer);
};

}  // namespace vr

#endif  // CHROME_BROWSER_VR_SPEECH_RECOGNIZER_H_
