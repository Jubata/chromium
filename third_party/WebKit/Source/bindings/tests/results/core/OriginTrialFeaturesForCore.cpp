// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file has been auto-generated from the Jinja2 template
// third_party/WebKit/Source/bindings/templates/OriginTrialFeaturesForCore.cpp.tmpl
// by the script generate_origin_trial_features.py.
// DO NOT MODIFY!

// clang-format off

#include "bindings/core/v8/OriginTrialFeaturesForCore.h"

#include "bindings/core/v8/V8TestObject.h"
#include "bindings/core/v8/V8Window.h"
#include "core/context_features/ContextFeatureSettings.h"
#include "core/dom/ExecutionContext.h"
#include "core/frame/Frame.h"
#include "core/origin_trials/origin_trials.h"
#include "platform/bindings/OriginTrialFeatures.h"
#include "platform/bindings/ScriptState.h"

namespace blink {

namespace {
InstallOriginTrialFeaturesFunction g_old_install_origin_trial_features_function =
    nullptr;
InstallOriginTrialFeaturesOnGlobalFunction
    g_old_install_origin_trial_features_on_global_function = nullptr;
InstallPendingOriginTrialFeatureFunction
    g_old_install_pending_origin_trial_feature_function = nullptr;

void InstallOriginTrialFeaturesForCore(
    const WrapperTypeInfo* wrapper_type_info,
    const ScriptState* script_state,
    v8::Local<v8::Object> prototype_object,
    v8::Local<v8::Function> interface_object) {
  (*g_old_install_origin_trial_features_function)(
      wrapper_type_info, script_state, prototype_object, interface_object);

  ExecutionContext* execution_context = ExecutionContext::From(script_state);
  if (!execution_context)
    return;
  v8::Isolate* isolate = script_state->GetIsolate();
  const DOMWrapperWorld& world = script_state->World();
  // TODO(iclelland): Unify ContextFeatureSettings with the rest of the
  // conditional features.
  if (wrapper_type_info == &V8Window::wrapperTypeInfo) {
    auto* settings = ContextFeatureSettings::From(
        execution_context,
        ContextFeatureSettings::CreationMode::kDontCreateIfNotExists);
    if (settings && settings->isMojoJSEnabled()) {
      v8::Local<v8::Object> instance_object =
          script_state->GetContext()->Global();
      V8Window::installMojoJS(isolate, world, instance_object, prototype_object,
                              interface_object);
    }
  }
  // TODO(iclelland): Extract this common code out of OriginTrialFeaturesForCore
  // and OriginTrialFeaturesForModules into a block.
  if (wrapper_type_info == &V8TestObject::wrapperTypeInfo) {
    if (OriginTrials::featureNameEnabled(execution_context)) {
      V8TestObject::installFeatureName(
          isolate, world, v8::Local<v8::Object>(), prototype_object, interface_object);
    }
  }
}

void InstallOriginTrialFeaturesOnGlobalForCore(
    const WrapperTypeInfo* wrapper_type_info,
    const ScriptState* script_state) {
  (*g_old_install_origin_trial_features_on_global_function)(wrapper_type_info,
                                                           script_state);

  // TODO(chasej): Generate this logic at compile-time, based on interfaces with
  // [SecureContext] attribute.
  if (wrapper_type_info == &V8Window::wrapperTypeInfo) {
    V8Window::InstallConditionalFeaturesOnGlobal(script_state->GetContext(),
                                                 script_state->World());
  }
}

void InstallPendingOriginTrialFeatureForCore(const String& feature,
                                             const ScriptState* script_state) {
  (*g_old_install_pending_origin_trial_feature_function)(feature, script_state);

  // TODO(iclelland): Extract this common code out of OriginTrialFeaturesForCore
  // and OriginTrialFeaturesForModules into a block.
  v8::Local<v8::Object> prototype_object;
  v8::Local<v8::Function> interface_object;
  v8::Isolate* isolate = script_state->GetIsolate();
  const DOMWrapperWorld& world = script_state->World();
  V8PerContextData* context_data = script_state->PerContextData();
  if (feature == OriginTrials::kFeatureNameTrialName) {
    if (context_data->GetExistingConstructorAndPrototypeForType(
            &V8TestObject::wrapperTypeInfo, &prototype_object, &interface_object)) {
      V8TestObject::installFeatureName(
          isolate, world, v8::Local<v8::Object>(), prototype_object, interface_object);
    }
  }
}

}  // namespace

void RegisterInstallOriginTrialFeaturesForCore() {
  g_old_install_origin_trial_features_function =
      SetInstallOriginTrialFeaturesFunction(&InstallOriginTrialFeaturesForCore);
  g_old_install_origin_trial_features_on_global_function =
      SetInstallOriginTrialFeaturesOnGlobalFunction(
          &InstallOriginTrialFeaturesOnGlobalForCore);
  g_old_install_pending_origin_trial_feature_function =
      SetInstallPendingOriginTrialFeatureFunction(
          &InstallPendingOriginTrialFeatureForCore);
}

}  // namespace blink
