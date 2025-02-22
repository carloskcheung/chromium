/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/modules/indexeddb/idb_cursor_with_value.h"

#include <memory>
#include "third_party/blink/renderer/modules/indexeddb/idb_key.h"

using blink::WebIDBCursor;

namespace blink {

IDBCursorWithValue* IDBCursorWithValue::Create(
    std::unique_ptr<WebIDBCursor> backend,
    mojom::IDBCursorDirection direction,
    IDBRequest* request,
    const Source& source,
    IDBTransaction* transaction) {
  return new IDBCursorWithValue(std::move(backend), direction, request, source,
                                transaction);
}

IDBCursorWithValue::IDBCursorWithValue(std::unique_ptr<WebIDBCursor> backend,
                                       mojom::IDBCursorDirection direction,
                                       IDBRequest* request,
                                       const Source& source,
                                       IDBTransaction* transaction)
    : IDBCursor(std::move(backend), direction, request, source, transaction) {}

IDBCursorWithValue::~IDBCursorWithValue() = default;

}  // namespace blink
