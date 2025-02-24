// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_TRANSLATE_CORE_BROWSER_TRANSLATE_DOWNLOAD_MANAGER_H_
#define COMPONENTS_TRANSLATE_CORE_BROWSER_TRANSLATE_DOWNLOAD_MANAGER_H_

#include <memory>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/sequence_checker.h"
#include "components/translate/core/browser/translate_language_list.h"
#include "components/translate/core/browser/translate_script.h"
#include "net/url_request/url_request_context_getter.h"

namespace base {
template <typename T> struct DefaultSingletonTraits;
}

class PrefService;

namespace translate {

// Manages the downloaded resources for Translate, such as the translate script
// and the language list.
class TranslateDownloadManager {
 public:
  // Returns the singleton instance.
  static TranslateDownloadManager* GetInstance();

  // The request context used to download the resources.
  // Should be set before this class can be used.
  net::URLRequestContextGetter* request_context() {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    return request_context_.get();
  }
  void set_request_context(net::URLRequestContextGetter* context) {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    request_context_ = context;
  }

  // The application locale.
  // Should be set before this class can be used.
  const std::string& application_locale() {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    return application_locale_;
  }
  void set_application_locale(const std::string& locale) {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    application_locale_ = locale;
  }

  // The language list.
  TranslateLanguageList* language_list() {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    return language_list_.get();
  }

  // The translate script.
  TranslateScript* script() {
    DCHECK(sequence_checker_.CalledOnValidSequence());
    return script_.get();
  }

  // Let the caller decide if and when we should fetch the language list from
  // the translate server. This is a NOOP if prefs::kOfferTranslateEnabled is
  // set to false.
  static void RequestLanguageList(PrefService* prefs);

  // Fills |languages| with the list of languages that the translate server can
  // translate to and from.
  static void GetSupportedLanguages(std::vector<std::string>* languages);

  // Returns the last-updated time when Chrome received a language list from a
  // Translate server. Returns null time if Chrome hasn't received any lists.
  static base::Time GetSupportedLanguagesLastUpdated();

  // Returns the language code that can be used with the Translate method for a
  // specified |language|. (ex. GetLanguageCode("en-US") will return "en", and
  // GetLanguageCode("zh-CN") returns "zh-CN")
  static std::string GetLanguageCode(const std::string& language);

  // Returns true if |language| is supported by the translation server.
  static bool IsSupportedLanguage(const std::string& language);

  // Must be called to shut Translate down. Cancels any pending fetches.
  void Shutdown();

  // Clears the translate script, so it will be fetched next time we translate.
  void ClearTranslateScriptForTesting();

  // Resets to its initial state as if newly created.
  void ResetForTesting();

  // Used by unit-tests to override some defaults:
  // Delay after which the translate script is fetched again from the
  // translation server.
  void SetTranslateScriptExpirationDelay(int delay_ms);

 private:
  friend struct base::DefaultSingletonTraits<TranslateDownloadManager>;
  TranslateDownloadManager();
  virtual ~TranslateDownloadManager();

  // Validates that accesses to the download manager are performed on the same
  // sequence.
  base::SequenceChecker sequence_checker_;

  std::unique_ptr<TranslateLanguageList> language_list_;

  // An instance of TranslateScript which manages JavaScript source for
  // Translate.
  std::unique_ptr<TranslateScript> script_;

  std::string application_locale_;
  scoped_refptr<net::URLRequestContextGetter> request_context_;
};

}  // namespace translate

#endif  // COMPONENTS_TRANSLATE_CORE_BROWSER_TRANSLATE_DOWNLOAD_MANAGER_H_
