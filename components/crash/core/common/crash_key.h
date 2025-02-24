// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_CRASH_CORE_COMMON_CRASH_KEY_H_
#define COMPONENTS_CRASH_CORE_COMMON_CRASH_KEY_H_

#include <stdint.h>

#include "base/macros.h"
#include "base/strings/string_piece.h"
#include "build/build_config.h"

// The crash key interface exposed by this file is the same as the Crashpad
// Annotation interface. Because not all platforms use Crashpad yet, a
// source-compatible interface is provided on top of the older Breakpad
// storage mechanism.
#if (defined(OS_MACOSX) && !defined(OS_IOS)) || defined(OS_WIN)
#define USE_CRASHPAD_ANNOTATION 1
#endif

#if defined(USE_CRASHPAD_ANNOTATION)
#include "third_party/crashpad/crashpad/client/annotation.h"
#endif

namespace crash_reporter {

class CrashKeyBreakpadTest;

// A CrashKeyString stores a name-value pair that will be recorded within a
// crash report.
//
// The crash key name must be a constant string expression, and the value
// should be unique and identifying. The maximum size for the value is
// specified as the template argument, and values greater than this are
// truncated. Crash keys should be declared with static storage duration.
//
// Example:
//    namespace {
//    crash_reporter::CrashKeyString<256> g_active_url("current-page-url");
//    }
//
//    void DidNaviagate(GURL new_url) {
//      g_active_url.Set(new_url.ToString());
//    }
#if defined(USE_CRASHPAD_ANNOTATION)

template <crashpad::Annotation::ValueSizeType MaxLength>
using CrashKeyString = crashpad::StringAnnotation<MaxLength>;

#else  // Crashpad-compatible crash key interface:

namespace internal {

constexpr size_t kCrashKeyStorageNumEntries = 200;
constexpr size_t kCrashKeyStorageValueSize = 128;

// Base implementation of a CrashKeyString for non-Crashpad clients. A separate
// base class is used to avoid inlining complex logic into the public template
// API.
class CrashKeyStringImpl {
 public:
  constexpr explicit CrashKeyStringImpl(const char name[],
                                        size_t* index_array,
                                        size_t index_array_count)
      : name_(name),
        index_array_(index_array),
        index_array_count_(index_array_count) {}

  void Set(base::StringPiece value);
  void Clear();

  bool is_set() const;

 private:
  friend class crash_reporter::CrashKeyBreakpadTest;

  // The name of the crash key.
  const char* const name_;

  // If the crash key is set, this is the index into the storage that can be
  // used to set/clear the key without requiring a linear scan of the storage
  // table. This will be |num_entries| if unset.
  size_t* index_array_;
  size_t index_array_count_;

  DISALLOW_COPY_AND_ASSIGN(CrashKeyStringImpl);
};

// This type creates a C array that is initialized with a specific default
// value, rather than the standard zero-initialized default.
template <typename T,
          size_t TotalSize,
          T DefaultValue,
          size_t Count,
          T... Values>
struct InitializedArrayImpl {
  using Type = typename InitializedArrayImpl<T,
                                             TotalSize,
                                             DefaultValue,
                                             Count - 1,
                                             DefaultValue,
                                             Values...>::Type;
};

template <typename T, size_t TotalSize, T DefaultValue, T... Values>
struct InitializedArrayImpl<T, TotalSize, DefaultValue, 0, Values...> {
  using Type = InitializedArrayImpl<T, TotalSize, DefaultValue, 0, Values...>;
  T data[TotalSize]{Values...};
};

template <typename T, size_t ArraySize, T DefaultValue>
using InitializedArray =
    typename InitializedArrayImpl<T, ArraySize, DefaultValue, ArraySize>::Type;

}  // namespace internal

template <uint32_t MaxLength>
class CrashKeyString : public internal::CrashKeyStringImpl {
 public:
  constexpr static size_t chunk_count =
      (MaxLength / internal::kCrashKeyStorageValueSize) + 1;

  constexpr explicit CrashKeyString(const char name[])
      : internal::CrashKeyStringImpl(name, indexes_.data, chunk_count) {}

 private:
  // Indexes into the TransitionalCrashKeyStorage for when a value is set.
  // See the comment in CrashKeyStringImpl for details.
  // An unset index in the storage is represented by a sentinel value, which
  // is the total number of entries. This will initialize the array with
  // that sentinel value as a compile-time expression.
  internal::InitializedArray<size_t,
                             chunk_count,
                             internal::kCrashKeyStorageNumEntries>
      indexes_;

  DISALLOW_COPY_AND_ASSIGN(CrashKeyString);
};

#endif

// This scoper clears the specified annotation's value when it goes out of
// scope.
//
// Example:
//    void DoSomething(const std::string& data) {
//      static crash_reporter::CrashKeyString<32> crash_key("DoSomething-data");
//      crash_reporter::ScopedCrashKeyString auto_clear(&crash_key, data);
//
//      DoSomethignImpl(data);
//    }
class ScopedCrashKeyString {
 public:
#if defined(USE_CRASHPAD_ANNOTATION)
  using CrashKeyType = crashpad::Annotation;
#else
  using CrashKeyType = internal::CrashKeyStringImpl;
#endif

  template <class T>
  ScopedCrashKeyString(T* crash_key, base::StringPiece value)
      : crash_key_(crash_key) {
    crash_key->Set(value);
  }

  ~ScopedCrashKeyString() { crash_key_->Clear(); }

 private:
  CrashKeyType* const crash_key_;
  DISALLOW_COPY_AND_ASSIGN(ScopedCrashKeyString);
};

// Initializes the crash key subsystem if it is required.
void InitializeCrashKeys();

}  // namespace crash_reporter

#undef USE_CRASHPAD_ANNOTATION

#endif  // COMPONENTS_CRASH_CORE_COMMON_CRASH_KEY_H_
