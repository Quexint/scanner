/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/eval/caffe/caffe_cpu_evaluator.h"

#include "scanner/util/common.h"
#include "scanner/util/util.h"

namespace scanner {

CaffeCPUEvaluator::CaffeCPUEvaluator(
  const EvaluatorConfig& config,
  const NetDescriptor& descriptor,
  CaffeInputTransformer* transformer,
  int device_id)
  : config_(config),
    descriptor_(descriptor),
    transformer_(transformer),
    device_id_(device_id)
{
  caffe::Caffe::set_mode(device_type_to_caffe_mode(DeviceType::CPU));

  // Initialize our network
  net_.reset(new Net<float>(descriptor_.model_path, caffe::TEST));
  net_->CopyTrainedLayersFrom(descriptor_.model_weights_path);

  const boost::shared_ptr<caffe::Blob<float>> input_blob{
    net_->blob_by_name(descriptor.input_layer_name)};

  // Get output blobs that we will extract net evaluation results from
  for (const std::string& output_layer_name : descriptor.output_layer_names)
  {
    const boost::shared_ptr<caffe::Blob<float>> output_blob{
      net_->blob_by_name(output_layer_name)};
    size_t output_size_per_frame = output_blob->count(1) * sizeof(float);
    output_sizes_.push_back(output_size_per_frame);
  }
}

void CaffeCPUEvaluator::configure(const DatasetItemMetadata& metadata) {
  metadata_ = metadata;

  // Dimensions of network input image
  int net_input_height = input_blob->shape(2);
  int net_input_width = input_blob->shape(3);

  const boost::shared_ptr<caffe::Blob<float>> input_blob{
    net_->blob_by_name(descriptor.input_layer_name)};
  if (input_blob->shape(0) != config_.max_batch_size) {
    input_blob->Reshape(
      {config_.max_batch_size, 3, net_input_height, net_input_width});
  }

  transformer_->configure(metadata);
}

void CaffeCPUEvaluator::evaluate(
  char* input_buffer,
  std::vector<char*> output_buffers,
  int batch_size)
{
  size_t frame_size =
    av_image_get_buffer_size(AV_PIX_FMT_RGB24,
                             metadata_.width,
                             metadata_.height,
                             1);

  const boost::shared_ptr<caffe::Blob<float>> input_blob{
    net_->blob_by_name(descriptor_.input_layer_name)};

  // Dimensions of network input image
  int net_input_height = input_blob->shape(2);
  int net_input_width = input_blob->shape(3);

  if (input_blob->shape(0) != batch_size) {
    input_blob->Reshape({batch_size, 3, net_input_height, net_input_width});
  }

  float* net_input_buffer = input_blob->mutable_cpu_data();

  // Process batch of frames
  auto cv_start = now();
  transformer_->transform_input(input_buffer, net_input_buffer, batch_size);
  args.profiler.add_interval("caffe:transform_input", cv_start, now());

  // Compute features
  auto net_start = now();
  net->Forward();
  args.profiler.add_interval("caffe:net", net_start, now());

  // Save batch of frames
  for (size_t i = 0; i < output_sizes_.size(); ++i) {
    const std::string& output_layer_name = descriptor_.output_layer_names[i];
    const boost::shared_ptr<caffe::Blob<float>> output_blob{
      net_->blob_by_name(output_layer_name)};
    memcpy(output_buffers[i],
           output_blob->cpu_data(),
           batch_size * output_sizes_[i]);
  }
}

CaffeCPUEvaluatorConstructor::CaffeCPUEvaluatorConstructor(
  const NetDescriptor& net_descriptor,
  CaffeInputTransformerFactory* transformer_factory)
  : net_descriptor_(net_descriptor),
    transformer_factory_(transformer_factory)
{
}

int CaffeCPUEvaluatorConstructor::get_number_of_devices() {
  return 1;
}

DeviceType CaffeCPUEvaluatorConstructor::get_input_buffer_type() {
  return DeviceType::CPU;
}

DeviceType CaffeCPUEvaluatorConstructor::get_output_buffer_type() {
  return DeviceType::CPU;
}

int CaffeCPUEvaluatorConstructor::get_number_of_outputs() {
  return static_cast<int>(net_descriptor_.output_layer_names.size());
}

std::vector<std::string> CaffeCPUEvaluatorConstructor::get_output_names() {
  return net_descriptor_.output_layer_names;
}

char* CaffeCPUEvaluatorConstructor::new_input_buffer(
  const EvaluatorConfig& config)
{
  return new char[
    config.max_batch_size *
    config.max_frame_width *
    config.max_frame_height *
    3 *
    sizeof(char)];
}

void CaffeCPUEvaluatorConstructor::delete_input_buffer(
  const EvaluatorConfig& config,
  char* buffer)
{
  delete[] buffer;
}

void CaffeCPUEvaluatorConstructor::delete_output_buffer(
  const EvaluatorConfig& config,
  char* buffer)
{
  delete[] buffer;
}

Evaluator* CaffeCPUEvaluatorConstructor::new_evaluator(
  const EvaluatorConfig& config)
{
  CaffeInputTransformer* transformer =
    transformer_factory_->construct(config, net_descriptor_);
  return new CaffeCPUEvaluator(config, net_descriptor_, transformer, 0);
}

}
