/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "micro_interpreter.h"

#include "c_api_internal.h"
#include "api/flatbuffer_conversions.h"
#include "compatibility.h"

volatile uint32_t glb_op_tmr = 0;

namespace tflite {
namespace {
const int kStackDataAllocatorSize = 128;
class StackDataAllocator : public BuiltinDataAllocator {
 public:
  void* Allocate(size_t size) override {
    if (size > kStackDataAllocatorSize) {
      return nullptr;
    } else {
      return data_;
    }
  }
  void Deallocate(void* data) override {
    // Do nothing.
  }

 private:
  uint8_t data_[kStackDataAllocatorSize];

  TF_LITE_REMOVE_VIRTUAL_DELETE
};

const char* OpNameFromRegistration(const TfLiteRegistration* registration) {
  if (registration->builtin_code == BuiltinOperator_CUSTOM) {
    return registration->custom_name;
  } else {
    return EnumNameBuiltinOperator(BuiltinOperator(registration->builtin_code));
  }
}

void ReportOpError(struct TfLiteContext* context, const char* format, ...) {
  MicroInterpreter* interpreter =
      static_cast<MicroInterpreter*>(context->impl_);
  va_list args;
  va_start(args, format);
  interpreter->error_reporter()->Report(format, args);
  va_end(args);
}

}  // namespace

MicroInterpreter::MicroInterpreter(const Model* model,
                                   const OpResolver& op_resolver,
                                   SimpleTensorAllocator* tensor_allocator,
                                   ErrorReporter* error_reporter)
    : model_(model),
      op_resolver_(op_resolver),
      tensor_allocator_(tensor_allocator),
      error_reporter_(error_reporter),
      initialization_status_(kTfLiteOk),
      context_() {
  auto* subgraphs = model->subgraphs();
  if (subgraphs->size() != 1) {
    error_reporter->Report("Only 1 subgraph is currently supported.\n");
    initialization_status_ = kTfLiteError;
    return;
  }
  subgraph_ = (*subgraphs)[0];
  tensors_ = subgraph_->tensors();
  operators_ = subgraph_->operators();

  context_.tensors_size = tensors_->size();
  context_.tensors =
      reinterpret_cast<TfLiteTensor*>(tensor_allocator_->AllocateMemory(
          sizeof(TfLiteTensor) * context_.tensors_size, 4));
  context_.impl_ = static_cast<void*>(this);
  context_.ReportError = ReportOpError;
  context_.recommended_num_threads = 1;

  initialization_status_ = AllocateInputAndActTensors();
  if (initialization_status_ != kTfLiteOk) {
    return;
  }

  initialization_status_ = AllocateTemporaryTensors();
  if (initialization_status_ != kTfLiteOk) {
    return;
  }
}

TfLiteStatus MicroInterpreter::AllocateInputAndActTensors() {
  const flatbuffers::Vector<flatbuffers::Offset<Buffer>>* buffers =
      model_->buffers();
  for (flatbuffers::uoffset_t i = 0; i < subgraph_->inputs()->size(); ++i) {
    const int tensor_index = subgraph_->inputs()->Get(i);
    const auto* tensor = tensors_->Get(tensor_index);
    const TfLiteStatus status = tensor_allocator_->AllocateTensor(
        *tensor, 0, operators_->size(), buffers, error_reporter_,
        &context_.tensors[tensor_index]);
    if (status != kTfLiteOk) {
      return status;
    }
  }

  int* first_created = reinterpret_cast<int*>(tensor_allocator_->AllocateMemory(
      sizeof(int) * tensors_->size(), sizeof(int)));
  int* last_used = reinterpret_cast<int*>(tensor_allocator_->AllocateMemory(
      sizeof(int) * tensors_->size(), sizeof(int)));
  for (flatbuffers::uoffset_t i = 0; i < tensors_->size(); ++i) {
    first_created[i] = -1;
    last_used[i] = -1;
  }

  for (int i = (operators_->size() - 1); i >= 0; --i) {
    const auto* op = operators_->Get(i);
    for (flatbuffers::uoffset_t n = 0; n < op->inputs()->size(); ++n) {
      const int tensor_index = op->inputs()->Get(n);
      if ((last_used[tensor_index] == -1) || (last_used[tensor_index] < i)) {
        last_used[tensor_index] = i;
      }
    }
    for (flatbuffers::uoffset_t n = 0; n < op->outputs()->size(); ++n) {
      const int tensor_index = op->outputs()->Get(n);
      const int create_before = i;
      int destroy_after = last_used[tensor_index];
      if (destroy_after == -1) {
        destroy_after = operators_->size();
      }
      const auto* tensor = tensors_->Get(tensor_index);
      if (!tensor->is_variable()) {
        const TfLiteStatus status = tensor_allocator_->AllocateTensor(
            *tensor, create_before, destroy_after, buffers, error_reporter_,
            &context_.tensors[tensor_index]);
        if (status != kTfLiteOk) {
          return status;
        }
        first_created[tensor_index] = i;
      }
    }
  }

  for (flatbuffers::uoffset_t i = 0; i < tensors_->size(); ++i) {
    const auto* tensor = tensors_->Get(i);
    const bool is_read_only = (first_created[i] == -1) && (last_used[i] != -1);
    if (tensor->is_variable() || is_read_only) {
      const TfLiteStatus status = tensor_allocator_->AllocateTensor(
          *tensor, 0, operators_->size(), buffers, error_reporter_,
          &context_.tensors[i]);
      if (status != kTfLiteOk) {
        return status;
      }
    }
  }

  return kTfLiteOk;
}

TfLiteStatus MicroInterpreter::AllocateTemporaryTensors() {
  // TBD(wangtz) : Implement this method.
  return kTfLiteOk;
}

TfLiteStatus MicroInterpreter::Invoke() {
  if (initialization_status_ != kTfLiteOk) {
    error_reporter_->Report("Invoke() called after initialization failed\n");
    return kTfLiteError;
  }
  TfLiteStatus status = kTfLiteOk;
  auto opcodes = model_->operator_codes();
  for (flatbuffers::uoffset_t i = 0; i < operators_->size(); ++i) {
    /* reset timeout tmr */
    glb_op_tmr = 0;
    const auto* op = operators_->Get(i);
    flatbuffers::uoffset_t index = op->opcode_index();
    if (index < 0 || index >= opcodes->size()) {
      error_reporter_->Report("Missing registration for opcode_index %d\n",
                              index);
      return kTfLiteError;
    }
    auto opcode = (*opcodes)[index];
    const TfLiteRegistration* registration = nullptr;
    status = GetRegistrationFromOpCode(opcode, op_resolver_, error_reporter_,
                                       &registration);
    if (status != kTfLiteOk) {
      return status;
    }
    if (registration == nullptr) {
      error_reporter_->Report("Skipping op for opcode_index %d\n", index);
      return kTfLiteError;
    }
    BuiltinOperator op_type =
        static_cast<BuiltinOperator>(registration->builtin_code);

    if (op_type != BuiltinOperator_CUSTOM && op->custom_options()) {
      error_reporter_->Report(
          "Found builtin operator %s with custom options.\n",
          EnumNameBuiltinOperator(op_type));
    }

    StackDataAllocator stack_data_allocator;
    const char* custom_data = nullptr;
    size_t custom_data_size = 0;
    unsigned char* builtin_data = nullptr;
    if (op->custom_options()) {
      custom_data = reinterpret_cast<const char*>(op->custom_options()->data());
      custom_data_size = op->custom_options()->size();
    } else {
      TF_LITE_ENSURE_STATUS(ParseOpData(op, op_type, error_reporter_,
                                        &stack_data_allocator,
                                        (void**)(&builtin_data)));
    }

    const char* init_data;
    size_t init_data_size;
    if (registration->builtin_code == BuiltinOperator_CUSTOM) {
      init_data = custom_data;
      init_data_size = custom_data_size;
    } else {
      init_data = reinterpret_cast<const char*>(builtin_data);
      init_data_size = 0;
    }
    void* user_data = nullptr;
    if (registration->init) {
      user_data = registration->init(&context_, init_data, init_data_size);
    }

    // Disregard const qualifier to workaround with existing API.
    TfLiteIntArray* inputs_array = const_cast<TfLiteIntArray*>(
        reinterpret_cast<const TfLiteIntArray*>(op->inputs()));
    TfLiteIntArray* outputs_array = const_cast<TfLiteIntArray*>(
        reinterpret_cast<const TfLiteIntArray*>(op->outputs()));

    const int kMaxTemporaries = 16;
    int temporaries_data[kMaxTemporaries + 1];
    TfLiteIntArray* temporaries_array =
        reinterpret_cast<TfLiteIntArray*>(temporaries_data);
    temporaries_array->size = 0;

    TfLiteNode node;
    node.inputs = inputs_array;
    node.outputs = outputs_array;
    node.temporaries = temporaries_array;
    node.user_data = user_data;
    node.builtin_data = reinterpret_cast<void*>(builtin_data);
    node.custom_initial_data = custom_data;
    node.custom_initial_data_size = custom_data_size;
    node.delegate = nullptr;
    if (registration->prepare) {
      TfLiteStatus prepare_status = registration->prepare(&context_, &node);
      if (prepare_status != kTfLiteOk) {
        error_reporter_->Report(
            "Node %s (number %d) failed to prepare with status %d",
            OpNameFromRegistration(registration), i, prepare_status);
        return kTfLiteError;
      }
    }

    if (registration->invoke) {
      TfLiteStatus invoke_status = registration->invoke(&context_, &node);
      if (invoke_status != kTfLiteOk) {
        error_reporter_->Report(
            "Node %s (number %d) failed to invoke with status %d",
            OpNameFromRegistration(registration), i, invoke_status);
        return kTfLiteError;
      }
    }

    if (registration->free) {
      registration->free(&context_, user_data);
    }
    uint32_t stop_ms = glb_op_tmr;
    error_reporter_->Report("%s: %d msec\n", EnumNameBuiltinOperator(op_type), stop_ms);
  }
  return status;
}

TfLiteTensor* MicroInterpreter::input(int index) {
  const flatbuffers::Vector<int32_t>* inputs = subgraph_->inputs();
  const size_t length = inputs->size();
  if ((index < 0) || ((size_t)index >= length)) {
    error_reporter_->Report("Input index %d out of range (length is %d)", index,
                            length);
    return nullptr;
  }
  return &(context_.tensors[inputs->Get(index)]);
}

TfLiteTensor* MicroInterpreter::output(int index) {
  const flatbuffers::Vector<int32_t>* outputs = subgraph_->outputs();
  const size_t length = outputs->size();
  if ((index < 0) || ((size_t)index >= outputs->size())) {
    error_reporter_->Report("Output index %d out of range (length is %d)",
                            index, length);
    return nullptr;
  }
  return &(context_.tensors[outputs->Get(index)]);
}

}  // namespace tflite
