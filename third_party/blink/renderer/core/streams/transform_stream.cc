// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/streams/transform_stream.h"

#include "third_party/blink/renderer/bindings/core/v8/generated_code_helper.h"
#include "third_party/blink/renderer/bindings/core/v8/script_function.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise.h"
#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_script_runner.h"
#include "third_party/blink/renderer/core/streams/readable_stream.h"
#include "third_party/blink/renderer/core/streams/transform_stream_default_controller.h"
#include "third_party/blink/renderer/core/streams/transform_stream_transformer.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "third_party/blink/renderer/platform/bindings/v8_binding.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"

namespace blink {

// Base class for FlushAlgorithm and TransformAlgorithm. Contains common
// construction code and members.
class TransformStream::Algorithm : public ScriptFunction {
 public:
  // This is templated just to avoid having two identical copies of the
  // function.
  template <typename T>
  static v8::Local<v8::Function> Create(TransformStreamTransformer* transformer,
                                        ScriptState* script_state,
                                        ExceptionState& exception_state) {
    auto* algorithm = new T(transformer, script_state, exception_state);
    return algorithm->BindToV8Function();
  }

  void Trace(Visitor* visitor) override {
    visitor->Trace(transformer_);
    ScriptFunction::Trace(visitor);
  }

 protected:
  Algorithm(TransformStreamTransformer* transformer,
            ScriptState* script_state,
            ExceptionState& exception_state)
      : ScriptFunction(script_state),
        transformer_(transformer),
        interface_name_(exception_state.InterfaceName()),
        property_name_(exception_state.PropertyName()) {}

  // AlgorithmScope holds the stack-allocated objects used by the CallRaw()
  // methods for FlushAlgorithm and TransformAlgorithm.
  class AlgorithmScope {
    STACK_ALLOCATED();

   public:
    AlgorithmScope(Algorithm* owner,
                   const v8::FunctionCallbackInfo<v8::Value>& info,
                   v8::Local<v8::Value> controller)
        : controller_(owner->GetScriptState(), controller),
          exception_state_(owner->GetScriptState()->GetIsolate(),
                           // Using the original context would result in every
                           // exception claiming to have happened during
                           // construction. Since that is not helpful, don't
                           // annotate exceptions with where we think they came
                           // from.
                           ExceptionState::kUnknownContext,
                           owner->interface_name_,
                           owner->property_name_),
          reject_promise_scope_(info, exception_state_) {}

    TransformStreamDefaultController* GetController() { return &controller_; }

    ExceptionState* GetExceptionState() { return &exception_state_; }

   private:
    TransformStreamDefaultController controller_;
    ExceptionState exception_state_;
    ExceptionToRejectPromiseScope reject_promise_scope_;
  };

  Member<TransformStreamTransformer> transformer_;
  const char* const interface_name_;
  const char* const property_name_;
};

class TransformStream::FlushAlgorithm : public TransformStream::Algorithm {
 protected:
  using Algorithm::Algorithm;

 private:
  void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) override {
    DCHECK_EQ(info.Length(), 1);
    AlgorithmScope algorithm_scope(this, info, info[0]);
    ExceptionState& exception_state = *algorithm_scope.GetExceptionState();

    transformer_->Flush(algorithm_scope.GetController(), exception_state);
    if (exception_state.HadException())
      return;
    V8SetReturnValue(info,
                     ScriptPromise::CastUndefined(GetScriptState()).V8Value());
  }
};

class TransformStream::TransformAlgorithm : public TransformStream::Algorithm {
 protected:
  using Algorithm::Algorithm;

 private:
  void CallRaw(const v8::FunctionCallbackInfo<v8::Value>& info) override {
    DCHECK_EQ(info.Length(), 2);
    AlgorithmScope algorithm_scope(this, info, info[1]);
    ExceptionState& exception_state = *algorithm_scope.GetExceptionState();

    transformer_->Transform(info[0], algorithm_scope.GetController(),
                            exception_state);
    if (exception_state.HadException())
      return;
    V8SetReturnValue(info,
                     ScriptPromise::CastUndefined(GetScriptState()).V8Value());
  }
};

TransformStream::TransformStream() = default;

TransformStream::~TransformStream() = default;

TransformStream* TransformStream::Create(ScriptState* script_state,
                                         ExceptionState& exception_state) {
  ScriptValue undefined(script_state,
                        v8::Undefined(script_state->GetIsolate()));
  return Create(script_state, undefined, undefined, undefined, exception_state);
}

TransformStream* TransformStream::Create(
    ScriptState* script_state,
    ScriptValue transform_stream_transformer,
    ExceptionState& exception_state) {
  ScriptValue undefined(script_state,
                        v8::Undefined(script_state->GetIsolate()));
  return Create(script_state, transform_stream_transformer, undefined,
                undefined, exception_state);
}

TransformStream* TransformStream::Create(
    ScriptState* script_state,
    ScriptValue transform_stream_transformer,
    ScriptValue writable_strategy,
    ExceptionState& exception_state) {
  ScriptValue undefined(script_state,
                        v8::Undefined(script_state->GetIsolate()));
  return Create(script_state, transform_stream_transformer, writable_strategy,
                undefined, exception_state);
}

TransformStream* TransformStream::Create(ScriptState* script_state,
                                         ScriptValue transformer,
                                         ScriptValue writable_strategy,
                                         ScriptValue readable_strategy,
                                         ExceptionState& exception_state) {
  auto* ts = MakeGarbageCollected<TransformStream>();

  v8::Local<v8::Value> args[] = {transformer.V8Value(),
                                 writable_strategy.V8Value(),
                                 readable_strategy.V8Value()};
  v8::Local<v8::Value> stream;
  {
    v8::TryCatch block(script_state->GetIsolate());
    if (!V8ScriptRunner::CallExtra(script_state, "createTransformStream", args)
             .ToLocal(&stream)) {
      DCHECK(block.HasCaught());
      exception_state.RethrowV8Exception(block.Exception());
      return nullptr;
    }
  }
  DCHECK(stream->IsObject());
  ts->InitInternal(script_state, stream.As<v8::Object>(), exception_state);
  return ts->stream_.IsEmpty() ? nullptr : ts;
}

void TransformStream::Init(TransformStreamTransformer* transformer,
                           ScriptState* script_state,
                           ExceptionState& exception_state) {
  auto transform_algorithm = Algorithm::Create<TransformAlgorithm>(
      transformer, script_state, exception_state);
  auto flush_algorithm = Algorithm::Create<FlushAlgorithm>(
      transformer, script_state, exception_state);
  v8::Local<v8::Value> args[] = {transform_algorithm, flush_algorithm};
  v8::Local<v8::Value> stream;
  {
    v8::TryCatch block(script_state->GetIsolate());
    if (!V8ScriptRunner::CallExtra(script_state, "createTransformStreamSimple",
                                   args)
             .ToLocal(&stream)) {
      DCHECK(block.HasCaught());
      exception_state.RethrowV8Exception(block.Exception());
      return;
    }
  }
  DCHECK(stream->IsObject());
  InitInternal(script_state, stream.As<v8::Object>(), exception_state);
}

ScriptValue TransformStream::Writable(ScriptState* script_state,
                                      ExceptionState& exception_state) const {
  v8::Local<v8::Value> result;
  v8::Local<v8::Value> args[] = {stream_.NewLocal(script_state->GetIsolate())};
  DCHECK(args[0]->IsObject());
  v8::TryCatch block(script_state->GetIsolate());
  if (!V8ScriptRunner::CallExtra(script_state, "getTransformStreamWritable",
                                 args)
           .ToLocal(&result)) {
    exception_state.RethrowV8Exception(block.Exception());
    return ScriptValue();
  }
  DCHECK(!block.HasCaught());
  return ScriptValue(script_state, result);
}

void TransformStream::Trace(Visitor* visitor) {
  visitor->Trace(stream_);
  visitor->Trace(readable_);
  ScriptWrappable::Trace(visitor);
}

void TransformStream::InitInternal(ScriptState* script_state,
                                   v8::Local<v8::Object> stream,
                                   ExceptionState& exception_state) {
  v8::Local<v8::Value> readable;
  v8::Local<v8::Value> args[] = {stream};
  v8::TryCatch block(script_state->GetIsolate());
  if (!V8ScriptRunner::CallExtra(script_state, "getTransformStreamReadable",
                                 args)
           .ToLocal(&readable)) {
    DCHECK(block.HasCaught());
    exception_state.RethrowV8Exception(block.Exception());
    return;
  }

  DCHECK(readable->IsObject());
  readable_ = ReadableStream::CreateFromInternalStream(
      script_state, readable.As<v8::Object>(), exception_state);

  if (!readable_)
    return;

  stream_.Set(script_state->GetIsolate(), stream);
}

}  // namespace blink
