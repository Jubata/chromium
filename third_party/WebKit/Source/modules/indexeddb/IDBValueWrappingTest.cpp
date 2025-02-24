// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/indexeddb/IDBValueWrapping.h"

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "bindings/core/v8/V8BindingForTesting.h"
#include "core/fileapi/Blob.h"
#include "modules/indexeddb/IDBKey.h"
#include "modules/indexeddb/IDBKeyPath.h"
#include "modules/indexeddb/IDBValue.h"
#include "platform/wtf/PtrUtil.h"
#include "platform/wtf/Vector.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8.h"

namespace blink {

TEST(IDBValueUnwrapperTest, IsWrapped) {
  V8TestingScope scope;
  NonThrowableExceptionState non_throwable_exception_state;
  v8::Local<v8::Value> v8_true = v8::True(scope.GetIsolate());
  IDBValueWrapper wrapper(scope.GetIsolate(), v8_true,
                          SerializedScriptValue::SerializeOptions::kSerialize,
                          non_throwable_exception_state);
  wrapper.WrapIfBiggerThan(0);
  Vector<scoped_refptr<BlobDataHandle>> blob_data_handles;
  wrapper.ExtractBlobDataHandles(&blob_data_handles);
  Vector<WebBlobInfo>& blob_infos = wrapper.WrappedBlobInfo();
  scoped_refptr<SharedBuffer> wrapped_marker_buffer =
      wrapper.ExtractWireBytes();
  IDBKey* key = IDBKey::CreateNumber(42.0);
  IDBKeyPath key_path(String("primaryKey"));

  scoped_refptr<IDBValue> wrapped_value = IDBValue::Create(
      wrapped_marker_buffer,
      std::make_unique<Vector<scoped_refptr<BlobDataHandle>>>(
          blob_data_handles),
      std::make_unique<Vector<WebBlobInfo>>(blob_infos), key, key_path);
  EXPECT_TRUE(IDBValueUnwrapper::IsWrapped(wrapped_value.get()));

  Vector<char> wrapped_marker_bytes(wrapped_marker_buffer->size());
  ASSERT_TRUE(wrapped_marker_buffer->GetBytes(wrapped_marker_bytes.data(),
                                              wrapped_marker_bytes.size()));

  // IsWrapped() looks at the first 3 bytes in the value's byte array.
  // Truncating the array to fewer than 3 bytes should cause IsWrapped() to
  // return false.
  ASSERT_LT(3U, wrapped_marker_bytes.size());
  for (size_t i = 0; i < 3; ++i) {
    scoped_refptr<IDBValue> mutant_value = IDBValue::Create(
        SharedBuffer::Create(wrapped_marker_bytes.data(), i),
        std::make_unique<Vector<scoped_refptr<BlobDataHandle>>>(
            blob_data_handles),
        std::make_unique<Vector<WebBlobInfo>>(blob_infos), key, key_path);

    EXPECT_FALSE(IDBValueUnwrapper::IsWrapped(mutant_value.get()));
  }

  // IsWrapped() looks at the first 3 bytes in the value. Flipping any bit in
  // these 3 bytes should cause IsWrapped() to return false.
  ASSERT_LT(3U, wrapped_marker_bytes.size());
  for (size_t i = 0; i < 3; ++i) {
    for (int j = 0; j < 8; ++j) {
      char mask = 1 << j;
      wrapped_marker_bytes[i] ^= mask;
      scoped_refptr<IDBValue> mutant_value = IDBValue::Create(
          SharedBuffer::Create(wrapped_marker_bytes.data(),
                               wrapped_marker_bytes.size()),
          std::make_unique<Vector<scoped_refptr<BlobDataHandle>>>(
              blob_data_handles),
          std::make_unique<Vector<WebBlobInfo>>(blob_infos), key, key_path);
      EXPECT_FALSE(IDBValueUnwrapper::IsWrapped(mutant_value.get()));

      wrapped_marker_bytes[i] ^= mask;
    }
  }
}

}  // namespace blink
