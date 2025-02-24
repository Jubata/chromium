// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/language_settings_private/language_settings_private_api.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "base/feature_list.h"
#include "base/memory/ptr_util.h"
#include "base/stl_util.h"
#include "base/strings/string16.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/extensions/api/language_settings_private/language_settings_private_delegate.h"
#include "chrome/browser/extensions/api/language_settings_private/language_settings_private_delegate_factory.h"
#include "chrome/browser/language/language_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/spellchecker/spellcheck_factory.h"
#include "chrome/browser/spellchecker/spellcheck_service.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/translate/translate_service.h"
#include "chrome/common/extensions/api/language_settings_private.h"
#include "chrome/common/pref_names.h"
#include "components/language/core/browser/language_model.h"
#include "components/spellcheck/common/spellcheck_common.h"
#include "components/translate/core/browser/translate_download_manager.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "components/translate/core/common/translate_util.h"
#include "third_party/icu/source/i18n/unicode/coll.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/l10n_util_collator.h"

#if defined(OS_CHROMEOS)
#include "ui/base/ime/chromeos/component_extension_ime_manager.h"
#include "ui/base/ime/chromeos/extension_ime_util.h"
#include "ui/base/ime/chromeos/input_method_manager.h"
#include "ui/base/ime/chromeos/input_method_util.h"
#endif

namespace extensions {

namespace language_settings_private = api::language_settings_private;

#if defined(OS_CHROMEOS)
using chromeos::input_method::InputMethodDescriptor;
using chromeos::input_method::InputMethodDescriptors;
using chromeos::input_method::InputMethodManager;
using chromeos::input_method::InputMethodUtil;

namespace {

// Returns the set of IDs of all enabled IMEs.
std::unordered_set<std::string> GetEnabledIMEs(
    scoped_refptr<InputMethodManager::State> ime_state) {
  const std::vector<std::string>& ime_ids(ime_state->GetActiveInputMethodIds());
  return std::unordered_set<std::string>(ime_ids.begin(), ime_ids.end());
}

// Returns the set of IDs of enabled IMEs for the given pref.
std::unordered_set<std::string> GetIMEsFromPref(PrefService* prefs,
                                                const char* pref_name) {
  std::vector<std::string> enabled_imes =
      base::SplitString(prefs->GetString(pref_name), ",", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  return std::unordered_set<std::string>(enabled_imes.begin(),
                                         enabled_imes.end());
}

// Sorts the input methods by the order of their associated languages. For
// example, if the enabled language order is:
//  - French
//  - English
//  then the French keyboard should come before the English keyboard.
std::vector<std::string> GetSortedComponentIMEs(
    InputMethodManager* manager,
    scoped_refptr<InputMethodManager::State> ime_state,
    const std::unordered_set<std::string>& component_ime_set,
    PrefService* prefs) {
  std::vector<std::string> enabled_languages =
      base::SplitString(prefs->GetString(prefs::kLanguagePreferredLanguages),
                        ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  // Duplicate set for membership testing.
  std::unordered_set<std::string> available_component_imes(component_ime_set);
  std::vector<std::string> component_ime_list;

  for (const auto& language_code : enabled_languages) {
    // Get all input methods for this language.
    std::vector<std::string> input_method_ids;
    manager->GetInputMethodUtil()->GetInputMethodIdsFromLanguageCode(
        language_code, chromeos::input_method::kAllInputMethods,
        &input_method_ids);
    // Append the enabled ones to the new list. Also remove them from the set
    // so they aren't duplicated for other languages.
    for (const auto& input_method_id : input_method_ids) {
      if (available_component_imes.count(input_method_id)) {
        component_ime_list.push_back(input_method_id);
        available_component_imes.erase(input_method_id);
      }
    }
  }

  return component_ime_list;
}

// Sorts the third-party IMEs by the order of their associated languages.
std::vector<std::string> GetSortedExtensionIMEs(
    scoped_refptr<InputMethodManager::State> ime_state,
    const std::unordered_set<std::string>& extension_ime_set,
    PrefService* prefs) {
  std::vector<std::string> extension_ime_list;
  std::string preferred_languages =
      prefs->GetString(prefs::kLanguagePreferredLanguages);
  std::vector<std::string> enabled_languages =
      base::SplitString(preferred_languages, ",", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);

  InputMethodDescriptors descriptors;
  ime_state->GetInputMethodExtensions(&descriptors);

  // Filter out the IMEs not in |extension_ime_set|.
  auto it = descriptors.begin();
  while (it != descriptors.end()) {
    if (extension_ime_set.count(it->id()) == 0)
      it = descriptors.erase(it);
    else
      it++;
  }

  // For each language, add any candidate IMEs that support it.
  for (const auto& language : enabled_languages) {
    auto it = descriptors.begin();
    while (it != descriptors.end() && descriptors.size()) {
      if (extension_ime_set.count(it->id()) &&
          base::ContainsValue(it->language_codes(), language)) {
        extension_ime_list.push_back(it->id());
        // Remove the added descriptor from the candidate list.
        it = descriptors.erase(it);
      } else {
        it++;
      }
    }
  }

  return extension_ime_list;
}

}  // anonymous namespace
#endif

LanguageSettingsPrivateGetLanguageListFunction::
    LanguageSettingsPrivateGetLanguageListFunction() = default;

LanguageSettingsPrivateGetLanguageListFunction::
    ~LanguageSettingsPrivateGetLanguageListFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateGetLanguageListFunction::Run() {
  // Collect the language codes from the supported accept-languages.
  const std::string app_locale = g_browser_process->GetApplicationLocale();
  std::vector<translate::TranslateLanguageInfo> languages;

  translate::TranslatePrefs::GetLanguageInfoList(app_locale, &languages);

  // Get the list of available locales (display languages) and convert to a set.
  const std::vector<std::string>& locales = l10n_util::GetAvailableLocales();
  const std::unordered_set<std::string> locale_set(locales.begin(),
                                                   locales.end());

  // Get the list of spell check languages and convert to a set.
  std::vector<std::string> spellcheck_languages;
  spellcheck::SpellCheckLanguages(&spellcheck_languages);
  const std::unordered_set<std::string> spellcheck_language_set(
      spellcheck_languages.begin(), spellcheck_languages.end());

  // Build the language list.
  std::unique_ptr<base::ListValue> language_list(new base::ListValue);
  for (const auto& entry : languages) {
    language_settings_private::Language language;

    language.code = entry.code;
    language.display_name = entry.display_name;
    language.native_display_name = entry.native_display_name;

    // Set optional fields only if they differ from the default.
    if (locale_set.count(entry.code) > 0) {
      language.supports_ui.reset(new bool(true));
    }
    if (spellcheck_language_set.count(entry.code) > 0) {
      language.supports_spellcheck.reset(new bool(true));
    }
    if (entry.supports_translate) {
      language.supports_translate.reset(new bool(true));
    }

    language_list->Append(language.ToValue());
  }
  return RespondNow(OneArgument(std::move(language_list)));
}

LanguageSettingsPrivateEnableLanguageFunction::
    LanguageSettingsPrivateEnableLanguageFunction()
    : chrome_details_(this) {}

LanguageSettingsPrivateEnableLanguageFunction::
    ~LanguageSettingsPrivateEnableLanguageFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateEnableLanguageFunction::Run() {
  const auto parameters =
      language_settings_private::EnableLanguage::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters.get());
  const std::string& language_code = parameters->language_code;

  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(
          chrome_details_.GetProfile()->GetPrefs());

  std::vector<std::string> languages;
  translate_prefs->GetLanguageList(&languages);
  std::string chrome_language = language_code;
  translate::ToChromeLanguageSynonym(&chrome_language);

  if (base::ContainsValue(languages, chrome_language)) {
    LOG(ERROR) << "Language " << chrome_language << " already enabled";
    return RespondNow(NoArguments());
  }

  translate_prefs->AddToLanguageList(language_code, /*force_blocked=*/false);

  return RespondNow(NoArguments());
}

LanguageSettingsPrivateDisableLanguageFunction::
    LanguageSettingsPrivateDisableLanguageFunction()
    : chrome_details_(this) {}

LanguageSettingsPrivateDisableLanguageFunction::
    ~LanguageSettingsPrivateDisableLanguageFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateDisableLanguageFunction::Run() {
  const auto parameters =
      language_settings_private::DisableLanguage::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters.get());
  const std::string& language_code = parameters->language_code;

  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(
          chrome_details_.GetProfile()->GetPrefs());

  std::vector<std::string> languages;
  translate_prefs->GetLanguageList(&languages);
  std::string chrome_language = language_code;
  translate::ToChromeLanguageSynonym(&chrome_language);

  auto it = std::find(languages.begin(), languages.end(), chrome_language);
  if (it == languages.end()) {
    LOG(ERROR) << "Language " << chrome_language << " not enabled";
    return RespondNow(NoArguments());
  }

  translate_prefs->RemoveFromLanguageList(language_code);

  return RespondNow(NoArguments());
}

LanguageSettingsPrivateSetEnableTranslationForLanguageFunction::
    LanguageSettingsPrivateSetEnableTranslationForLanguageFunction()
    : chrome_details_(this) {}

LanguageSettingsPrivateSetEnableTranslationForLanguageFunction::
    ~LanguageSettingsPrivateSetEnableTranslationForLanguageFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateSetEnableTranslationForLanguageFunction::Run() {
  const auto parameters = language_settings_private::
      SetEnableTranslationForLanguage::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters.get());
  const std::string& language_code = parameters->language_code;
  // True if translation enabled, false if disabled.
  const bool enable = parameters->enable;

  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(
          chrome_details_.GetProfile()->GetPrefs());

  if (enable) {
    translate_prefs->UnblockLanguage(language_code);
  } else {
    translate_prefs->BlockLanguage(language_code);
  }

  return RespondNow(NoArguments());
}

LanguageSettingsPrivateMoveLanguageFunction::
    LanguageSettingsPrivateMoveLanguageFunction()
    : chrome_details_(this) {}

LanguageSettingsPrivateMoveLanguageFunction::
    ~LanguageSettingsPrivateMoveLanguageFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateMoveLanguageFunction::Run() {
  const auto parameters =
      language_settings_private::MoveLanguage::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(parameters.get());

  const std::string app_locale = g_browser_process->GetApplicationLocale();
  std::vector<std::string> supported_language_codes;
  l10n_util::GetAcceptLanguagesForLocale(app_locale, &supported_language_codes);

  const std::string& language_code = parameters->language_code;
  const language_settings_private::MoveType move_type = parameters->move_type;

  std::unique_ptr<translate::TranslatePrefs> translate_prefs =
      ChromeTranslateClient::CreateTranslatePrefs(
          chrome_details_.GetProfile()->GetPrefs());

  translate::TranslatePrefs::RearrangeSpecifier where =
      translate::TranslatePrefs::kNone;
  switch (move_type) {
    case language_settings_private::MOVE_TYPE_TOP:
      where = translate::TranslatePrefs::kTop;
      break;

    case language_settings_private::MOVE_TYPE_UP:
      where = translate::TranslatePrefs::kUp;
      break;

    case language_settings_private::MOVE_TYPE_DOWN:
      where = translate::TranslatePrefs::kDown;
      break;

    case language_settings_private::MOVE_TYPE_NONE:
    case language_settings_private::MOVE_TYPE_LAST:
      NOTREACHED();
  }

  translate_prefs->RearrangeLanguage(language_code, where,
                                     supported_language_codes);

  return RespondNow(NoArguments());
}

LanguageSettingsPrivateGetSpellcheckDictionaryStatusesFunction::
    LanguageSettingsPrivateGetSpellcheckDictionaryStatusesFunction() = default;

LanguageSettingsPrivateGetSpellcheckDictionaryStatusesFunction::
    ~LanguageSettingsPrivateGetSpellcheckDictionaryStatusesFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateGetSpellcheckDictionaryStatusesFunction::Run() {
  LanguageSettingsPrivateDelegate* delegate =
      LanguageSettingsPrivateDelegateFactory::GetForBrowserContext(
          browser_context());

  return RespondNow(OneArgument(
      language_settings_private::GetSpellcheckDictionaryStatuses::Results::
          Create(delegate->GetHunspellDictionaryStatuses())));
}

LanguageSettingsPrivateGetSpellcheckWordsFunction::
    LanguageSettingsPrivateGetSpellcheckWordsFunction() = default;

LanguageSettingsPrivateGetSpellcheckWordsFunction::
    ~LanguageSettingsPrivateGetSpellcheckWordsFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateGetSpellcheckWordsFunction::Run() {
  SpellcheckService* service =
      SpellcheckServiceFactory::GetForContext(browser_context());
  SpellcheckCustomDictionary* dictionary = service->GetCustomDictionary();

  if (dictionary->IsLoaded())
    return RespondNow(OneArgument(GetSpellcheckWords()));

  dictionary->AddObserver(this);
  AddRef();  // Balanced in OnCustomDictionaryLoaded().
  return RespondLater();
}

void
LanguageSettingsPrivateGetSpellcheckWordsFunction::OnCustomDictionaryLoaded() {
  SpellcheckService* service =
      SpellcheckServiceFactory::GetForContext(browser_context());
  service->GetCustomDictionary()->RemoveObserver(this);
  Respond(OneArgument(GetSpellcheckWords()));
  Release();
}

void
LanguageSettingsPrivateGetSpellcheckWordsFunction::OnCustomDictionaryChanged(
    const SpellcheckCustomDictionary::Change& dictionary_change) {
  NOTREACHED() << "SpellcheckCustomDictionary::Observer: "
                  "OnCustomDictionaryChanged() called before "
                  "OnCustomDictionaryLoaded()";
}

std::unique_ptr<base::ListValue>
LanguageSettingsPrivateGetSpellcheckWordsFunction::GetSpellcheckWords() const {
  SpellcheckService* service =
      SpellcheckServiceFactory::GetForContext(browser_context());
  SpellcheckCustomDictionary* dictionary = service->GetCustomDictionary();
  DCHECK(dictionary->IsLoaded());

  // TODO(michaelpg): Sort using app locale.
  std::unique_ptr<base::ListValue> word_list(new base::ListValue());
  const std::set<std::string>& words = dictionary->GetWords();
  for (const std::string& word : words)
    word_list->AppendString(word);
  return word_list;
}

LanguageSettingsPrivateAddSpellcheckWordFunction::
    LanguageSettingsPrivateAddSpellcheckWordFunction() = default;

LanguageSettingsPrivateAddSpellcheckWordFunction::
    ~LanguageSettingsPrivateAddSpellcheckWordFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateAddSpellcheckWordFunction::Run() {
  const auto params =
      language_settings_private::AddSpellcheckWord::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(params.get());

  SpellcheckService* service =
      SpellcheckServiceFactory::GetForContext(browser_context());
  bool success = service->GetCustomDictionary()->AddWord(params->word);

  return RespondNow(OneArgument(base::MakeUnique<base::Value>(success)));
}

LanguageSettingsPrivateRemoveSpellcheckWordFunction::
    LanguageSettingsPrivateRemoveSpellcheckWordFunction() = default;

LanguageSettingsPrivateRemoveSpellcheckWordFunction::
    ~LanguageSettingsPrivateRemoveSpellcheckWordFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateRemoveSpellcheckWordFunction::Run() {
  const auto params =
      language_settings_private::RemoveSpellcheckWord::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(params.get());

  SpellcheckService* service =
      SpellcheckServiceFactory::GetForContext(browser_context());
  bool success = service->GetCustomDictionary()->RemoveWord(params->word);

  return RespondNow(OneArgument(base::MakeUnique<base::Value>(success)));
}

LanguageSettingsPrivateGetTranslateTargetLanguageFunction::
    LanguageSettingsPrivateGetTranslateTargetLanguageFunction()
    : chrome_details_(this) {
}

LanguageSettingsPrivateGetTranslateTargetLanguageFunction::
    ~LanguageSettingsPrivateGetTranslateTargetLanguageFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateGetTranslateTargetLanguageFunction::Run() {
  Profile* profile = chrome_details_.GetProfile();
  language::LanguageModel* language_model =
      LanguageModelFactory::GetForBrowserContext(profile);
  return RespondNow(OneArgument(
      base::MakeUnique<base::Value>(TranslateService::GetTargetLanguage(
          profile->GetPrefs(), language_model))));
}

#if defined(OS_CHROMEOS)
// Populates the vector of input methods using information in the list of
// descriptors. Used for languageSettingsPrivate.getInputMethodLists().
void PopulateInputMethodListFromDescriptors(
    const InputMethodDescriptors& descriptors,
    std::vector<language_settings_private::InputMethod>* input_methods) {
  InputMethodManager* manager = InputMethodManager::Get();
  InputMethodUtil* util = manager->GetInputMethodUtil();
  scoped_refptr<InputMethodManager::State> ime_state =
      manager->GetActiveIMEState();
  if (!ime_state.get())
    return;

  const std::unordered_set<std::string> active_ids(GetEnabledIMEs(ime_state));
  for (const auto& descriptor : descriptors) {
    language_settings_private::InputMethod input_method;
    input_method.id = descriptor.id();
    input_method.display_name = util->GetLocalizedDisplayName(descriptor);
    input_method.language_codes = descriptor.language_codes();
    bool enabled = active_ids.count(input_method.id) > 0;
    if (enabled)
      input_method.enabled.reset(new bool(true));
    if (descriptor.options_page_url().is_valid())
      input_method.has_options_page.reset(new bool(true));
    input_methods->push_back(std::move(input_method));
  }
}
#endif

LanguageSettingsPrivateGetInputMethodListsFunction::
    LanguageSettingsPrivateGetInputMethodListsFunction() = default;

LanguageSettingsPrivateGetInputMethodListsFunction::
    ~LanguageSettingsPrivateGetInputMethodListsFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateGetInputMethodListsFunction::Run() {
#if !defined(OS_CHROMEOS)
  EXTENSION_FUNCTION_VALIDATE(false);
  return RespondNow(NoArguments());
#else
  language_settings_private::InputMethodLists input_method_lists;
  InputMethodManager* manager = InputMethodManager::Get();

  chromeos::ComponentExtensionIMEManager* component_extension_manager =
      manager->GetComponentExtensionIMEManager();
  PopulateInputMethodListFromDescriptors(
      component_extension_manager->GetAllIMEAsInputMethodDescriptor(),
      &input_method_lists.component_extension_imes);

  scoped_refptr<InputMethodManager::State> ime_state =
      manager->GetActiveIMEState();
  if (ime_state.get()) {
    InputMethodDescriptors ext_ime_descriptors;
    ime_state->GetInputMethodExtensions(&ext_ime_descriptors);
    PopulateInputMethodListFromDescriptors(
        ext_ime_descriptors,
        &input_method_lists.third_party_extension_imes);
  }

  return RespondNow(OneArgument(input_method_lists.ToValue()));
#endif
}

LanguageSettingsPrivateAddInputMethodFunction::
    LanguageSettingsPrivateAddInputMethodFunction()
    : chrome_details_(this) {}

LanguageSettingsPrivateAddInputMethodFunction::
    ~LanguageSettingsPrivateAddInputMethodFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateAddInputMethodFunction::Run() {
#if !defined(OS_CHROMEOS)
  EXTENSION_FUNCTION_VALIDATE(false);
#else
  const auto params =
      language_settings_private::AddInputMethod::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(params.get());

  InputMethodManager* manager = InputMethodManager::Get();
  scoped_refptr<InputMethodManager::State> ime_state =
      manager->GetActiveIMEState();
  if (!ime_state.get())
    return RespondNow(NoArguments());

  std::string new_input_method_id = params->input_method_id;
  bool is_extension_ime =
      chromeos::extension_ime_util::IsExtensionIME(new_input_method_id);

  PrefService* prefs = chrome_details_.GetProfile()->GetPrefs();
  const char* pref_name = is_extension_ime
                              ? prefs::kLanguageEnabledExtensionImes
                              : prefs::kLanguagePreloadEngines;

  // Get the input methods we are adding to.
  std::unordered_set<std::string> input_method_set(
      GetIMEsFromPref(prefs, pref_name));

  // Add the new input method.
  input_method_set.insert(new_input_method_id);

  // Sort the new input method list by preferred languages.
  std::vector<std::string> input_method_list;
  if (is_extension_ime) {
    input_method_list =
        GetSortedExtensionIMEs(ime_state, input_method_set, prefs);
  } else {
    input_method_list =
        GetSortedComponentIMEs(manager, ime_state, input_method_set, prefs);
  }

  std::string input_methods = base::JoinString(input_method_list, ",");
  prefs->SetString(pref_name, input_methods);
#endif
  return RespondNow(NoArguments());
}

LanguageSettingsPrivateRemoveInputMethodFunction::
    LanguageSettingsPrivateRemoveInputMethodFunction()
    : chrome_details_(this) {}

LanguageSettingsPrivateRemoveInputMethodFunction::
    ~LanguageSettingsPrivateRemoveInputMethodFunction() = default;

ExtensionFunction::ResponseAction
LanguageSettingsPrivateRemoveInputMethodFunction::Run() {
#if !defined(OS_CHROMEOS)
  EXTENSION_FUNCTION_VALIDATE(false);
#else
  const auto params =
      language_settings_private::RemoveInputMethod::Params::Create(*args_);
  EXTENSION_FUNCTION_VALIDATE(params.get());

  InputMethodManager* manager = InputMethodManager::Get();
  scoped_refptr<InputMethodManager::State> ime_state =
      manager->GetActiveIMEState();
  if (!ime_state.get())
    return RespondNow(NoArguments());

  std::string input_method_id = params->input_method_id;

  // Use the pref for the corresponding input method type.
  PrefService* prefs = chrome_details_.GetProfile()->GetPrefs();
  const char* pref_name =
      chromeos::extension_ime_util::IsExtensionIME(input_method_id)
          ? prefs::kLanguageEnabledExtensionImes
          : prefs::kLanguagePreloadEngines;

  std::string input_method_ids = prefs->GetString(pref_name);
  std::vector<std::string> input_method_list = base::SplitString(
      input_method_ids, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  // Find and remove the matching input method id.
  const auto& pos = std::find(input_method_list.begin(),
                              input_method_list.end(), input_method_id);
  if (pos != input_method_list.end()) {
    input_method_list.erase(pos);
    prefs->SetString(pref_name, base::JoinString(input_method_list, ","));
  }
#endif
  return RespondNow(NoArguments());
}

}  // namespace extensions
