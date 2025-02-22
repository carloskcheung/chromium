// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_PEERCONNECTION_RTC_QUIC_STREAM_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_PEERCONNECTION_RTC_QUIC_STREAM_H_

#include "third_party/blink/renderer/core/dom/context_lifecycle_observer.h"
#include "third_party/blink/renderer/core/typed_arrays/array_buffer_view_helpers.h"
#include "third_party/blink/renderer/core/typed_arrays/dom_typed_array.h"
#include "third_party/blink/renderer/modules/event_target_modules.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/quic_stream_proxy.h"

namespace blink {

class RTCQuicTransport;

enum class RTCQuicStreamState { kNew, kOpening, kOpen, kClosing, kClosed };

class MODULES_EXPORT RTCQuicStream final : public EventTargetWithInlineData,
                                           public ContextClient,
                                           public QuicStreamProxy::Delegate {
  DEFINE_WRAPPERTYPEINFO();

 public:
  // TODO(steveanton): These maybe should be adjustable.
  static const uint32_t kWriteBufferSize;

  RTCQuicStream(ExecutionContext* context,
                RTCQuicTransport* transport,
                QuicStreamProxy* stream_proxy);
  ~RTCQuicStream() override;

  // Called from the RTCQuicTransport when it is being stopped.
  void Stop();
  bool IsClosed() const { return state_ == RTCQuicStreamState::kClosed; }

  // rtc_quic_stream.idl
  RTCQuicTransport* transport() const;
  String state() const;
  uint32_t readBufferedAmount() const;
  uint32_t writeBufferedAmount() const;
  uint32_t maxWriteBufferedAmount() const;
  void write(NotShared<DOMUint8Array> data, ExceptionState& exception_state);
  void finish();
  void reset();
  DEFINE_ATTRIBUTE_EVENT_LISTENER(statechange, kStatechange);

  // EventTarget overrides.
  const AtomicString& InterfaceName() const override;
  ExecutionContext* GetExecutionContext() const override;

  // For garbage collection.
  void Trace(blink::Visitor* visitor) override;

 private:
  // Closes the stream. This will change the state to kClosed and deregister it
  // from the RTCQuicTransport. The QuicStreamProxy can no longer be used after
  // this point.
  void Close();

  // QuicStreamProxy::Delegate overrides.
  void OnRemoteReset() override;
  void OnDataReceived(Vector<uint8_t> data, bool fin) override;
  void OnWriteDataConsumed(uint32_t amount) override;

  Member<RTCQuicTransport> transport_;
  RTCQuicStreamState state_ = RTCQuicStreamState::kOpen;
  bool readable_ = true;
  uint32_t read_buffered_amount_ = 0;
  uint32_t write_buffered_amount_ = 0;
  bool wrote_fin_ = false;
  QuicStreamProxy* proxy_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_PEERCONNECTION_RTC_QUIC_STREAM_H_
