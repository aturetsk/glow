/**
 * Copyright (c) 2017-present, Facebook, Inc.
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

#include "glow/Importer/ONNX.h"
#include "glow/Base/Tensor.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"

#include "llvm/Support/Casting.h"

#include "onnx.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace glow;
using llvm::cast;

using ArgumentDictionaryTy =
    std::unordered_map<std::string, const onnx::AttributeProto *>;

/// Translates the protocol buffer node \p op into a random access map.
static ArgumentDictionaryTy loadArgumentMap(const onnx::NodeProto &op) {
  ArgumentDictionaryTy dict;
  for (auto i = 0, e = op.attribute_size(); i < e; i++) {
    const onnx::AttributeProto &arg = op.attribute(i);
    dict[arg.name()] = &arg;
  }
  return dict;
}

bool ONNXModelLoader::getBroadcast(const ArgumentDictionaryTy &dict) {
  if (opsetVersion_ > 6)
    return true;
  return dict.count("broadcast") && (loadInt(dict.at("broadcast")) == 1);
}

void ONNXModelLoader::setVersion(onnx::ModelProto MP) {
  irVersion_ = MP.ir_version();
  opsetVersion_ = 0;
  GLOW_ASSERT(
      irVersion_ >= 3 &&
      "This ONNX model with ir_version < 3 is too old to be supported.");
  for (const auto &imp : MP.opset_import()) {
    if (!imp.has_domain() || imp.domain() == "") {
      opsetVersion_ = imp.version();
      break;
    }
  }
  GLOW_ASSERT(opsetVersion_ > 0 &&
              "The opset of this ONNX model is not supported.");
}

bool ONNXModelLoader::loadProto(
    onnx::GraphProto &net, google::protobuf::io::ZeroCopyInputStream &iStream) {
  // Construct and configure a Coded Input Stream
  google::protobuf::io::CodedInputStream codedStream(&iStream);

  // Don't warn about large file sizes.
  codedStream.SetTotalBytesLimit(MAX_PROTO_SIZE, MAX_PROTO_SIZE);
  onnx::ModelProto MP;
  bool parseNet = MP.ParseFromCodedStream(&codedStream);
  net = MP.graph();
  setVersion(MP);

  return parseNet;
}

bool ONNXModelLoader::loadProto(onnx::GraphProto &net, const void *onnxModel,
                                size_t onnxModelSize) {
  google::protobuf::io::ArrayInputStream arrayStream(onnxModel, onnxModelSize);
  return loadProto(net, arrayStream);
}

bool ONNXModelLoader::loadProto(onnx::GraphProto &net,
                                const std::string &filename) {
  std::ifstream ff(filename, std::ios::in | std::ios::binary);
  GLOW_ASSERT(ff && "Can't find the model or network files.");

  google::protobuf::io::IstreamInputStream fileStream(&ff);
  return loadProto(net, fileStream);
}

std::vector<size_t> getPads(const ArgumentDictionaryTy &dict) {
  if (dict.count("pads")) {
    return getShape(dict.at("pads"));
  }
  if (dict.count("auto_pad")) {
    auto padStr = loadStr(dict.at("auto_pad"));
    if (padStr == "VALID") {
      // Return default value 0 for pads.
      return {0, 0, 0, 0};
    }
    assert(false && "only auto_pad==VALID is supported");
  }
  // Return default value 0 for pads.
  return {0, 0, 0, 0};
}

/// Loads tensor \p T from the input \p in.
static void loadTensor(const onnx::TensorProto &in, Tensor *T) {
  std::vector<size_t> dim;
  for (auto d : in.dims()) {
    dim.push_back(d);
  }

  if (in.data_type() == onnx::TensorProto::FLOAT) {
    T->reset(ElemKind::FloatTy, dim);

    if (in.float_data_size() > 0) {
      auto TH = T->getHandle<>();
      size_t i = 0;
      for (auto f : in.float_data()) {
        TH.raw(i++) = f;
      }
    } else if (in.has_raw_data()) {
      std::istringstream inStream(in.raw_data(), std::stringstream::binary);
      inStream.read((char *)T->getRawDataPointer<float>(),
                    T->size() * sizeof(float));
    } else {
      assert(false && "Unsupported Tensor format.");
    }
  } else if (in.data_type() == onnx::TensorProto::INT64) {
    // TODO: either switch IndexTy to be 64 bit, or switch to another type here
    T->reset(ElemKind::IndexTy, dim);

    if (in.int64_data_size() > 0) {
      auto TH = T->getHandle<>();
      size_t i = 0;
      for (auto f : in.int64_data()) {
        TH.raw(i++) = f;
      }
    } else if (in.has_raw_data()) {
      std::istringstream inStream(in.raw_data(), std::stringstream::binary);
      inStream.read((char *)T->getRawDataPointer<size_t>(),
                    T->size() * sizeof(int64_t));
    } else {
      assert(false && "Unsupported Tensor format.");
    }
  } else {
    assert(false && "Only float and index tensors are supported");
  }
}

bool ONNXModelLoader::loadOperator(const onnx::NodeProto &op) {
  ArgumentDictionaryTy dict = loadArgumentMap(op);
  const std::string &typeName = op.op_type();

  // Check if operator is supported in parent class, CommonOperatorLoader.
  if (tryLoadCommonOperator(typeName, op, dict)) {
    // If operator is supported, CommonOperatorLoader loaded it to the Graph.
    return true;
  }

  const std::string &opName = loadOperatorName(op);

  // Load tensors with values:
  if (typeName == "Constant") {
    /*
      output: "Parameter6"
      name: "Parameter6"
      op_type: "Constant"
      attribute {
        name: "value"
        t {
          dims: 8
          data_type: FLOAT
          float_data: -0.161539719
          float_data: -0.433835655
          float_data: 0.091641359
          float_data: -0.0168522168
          float_data: -0.0650264397
          float_data: -0.131737873
          float_data: 0.0204175506
          float_data: -0.121110231
        }
        type: TENSOR
      }
      doc_string: ""
      domain: ""
    */

    const auto &name = op.output(0);
    // If the tensor is pre-populated by the user of this class then we don't
    // need to allocate a new tensor.
    if (tensors_.count(name)) {
      return true;
    }

    assert(dict["value"]->type() == onnx::AttributeProto::TENSOR &&
           "Only Tensor type constants are supported.");

    auto *T = new Tensor();
    loadTensor(dict["value"]->t(), T);
    tensors_[name] = T;
    return true;
  }

  if (typeName == "Conv") {
    // Load the inputs:
    int stride =
        dict.count("strides") ? getConstantArrayHead(dict["strides"]) : 1;
    unsigned group = dict.count("group") ? loadInt(dict["group"]) : 1;
    // Pads : {pad_top, pad_left, pad_bottom, pad_right}
    std::vector<size_t> pads = getPads(dict);

    auto *in = getOrCreateVariableByName(op.input(0));
    Tensor *w = getTensorByName(op.input(1));

    // Transpose the weights to the right format. Glow expects to read the
    // weights in the format CRSK. ONNX stores the operators as KCRS.
    // C - output_depth, R - filter_height, S - filter_width, K - input_depth.
    Tensor wtag;
    w->transpose(&wtag, NCHW2NHWC);

    // The structure of the conv weigts is: NHWC. We take the C, which is the
    // number of filters. We use this value to calculate the size of the bias
    // if it is not specified.
    size_t depth = wtag.dims()[0];

    // Construct the Filter field.
    auto *filter = G_.getParent()->createVariable("conv.filter", wtag);

    unsigned kernel;
    if (dict.count("kernel_shape")) {
      kernel = getConstantArrayHead(dict["kernel_shape"]);
    } else {
      assert(filter->dims()[1] == filter->dims()[2] &&
             "Only square kernels are supported");
      kernel = filter->dims()[1];
    }

    // Construct the Bias field.
    Tensor biasTensor(ElemKind::FloatTy, {depth});
    biasTensor.zero();

    // Check if we have a serialized bias vector.
    if (op.input_size() > 2) {
      auto &biasTensorName = op.input(2);
      if (tensors_.count(biasTensorName)) {
        // Load the serialized bias vector.
        Tensor *b = getTensorByName(biasTensorName);
        biasTensor.copyFrom(b);
      }
    }
    auto *bias = G_.getParent()->createVariable("conv.bias", biasTensor);

    // ONNX passes the input as NCHW, and we expect the input to be NHWC.
    auto *tr = G_.createTranspose(opName, in, NCHW2NHWC);

    // Calculate the size and allocate the output buffer.
    ShapeNHWC idim = ShapeNHWC(tr->getResult().dims());
    auto outSz =
        calculateConvPoolOutputDims(idim.h, idim.w, kernel, stride, pads);
    std::array<size_t, 4> outDims = {
        {idim.n, outSz.first, outSz.second, depth}};
    auto outTy = G_.getParent()->uniqueType(ElemKind::FloatTy, outDims);

    auto *node = G_.createConv(opName, tr, filter, bias, outTy, kernel, stride,
                               pads, group);

    // Transpose the output back.
    auto *N = G_.createTranspose(opName, node, NHWC2NCHW);
    addNodeAsOutput(op, N);
    return true;
  }

  if (typeName == "MaxPool" || typeName == "AveragePool") {
    // Load the inputs:
    auto *in = getOrCreateVariableByName(op.input(0));
    int stride =
        dict.count("strides") ? getConstantArrayHead(dict["strides"]) : 1;
    size_t kernel = getConstantArrayHead(dict["kernel_shape"]);

    std::vector<size_t> pads = getPads(dict);

    auto *tr = G_.createTranspose(opName, in, NCHW2NHWC);

    // If 'global_pooling' is set then the operation will pool over the size of
    // the input by doing: kernel = height/width.
    if (dict.count("global_pooling")) {
      auto Ty = in->getType(0);
      kernel = Ty->dims()[3];
    }

    Node *node = nullptr;
    if (typeName == "MaxPool") {
      node = G_.createPoolMax(opName, tr, kernel, stride, pads);
    } else {
      node = G_.createPoolAvg(opName, tr, kernel, stride, pads);
    }
    auto *N = G_.createTranspose(opName, node, NHWC2NCHW);
    addNodeAsOutput(op, N);
    return true;
  }

  if (typeName == "GlobalAveragePool") {
    // Load the inputs:
    auto *in = getOrCreateVariableByName(op.input(0));
    int stride =
        dict.count("strides") ? getConstantArrayHead(dict["strides"]) : 1;

    GLOW_ASSERT(in->dims(0)[2] == in->dims(0)[3] &&
                "For the image, height == weight is required");

    size_t kernel = in->dims(0)[2];
    std::vector<size_t> pads = getPads(dict);
    auto *tr = G_.createTranspose(opName, in, NCHW2NHWC);
    Node *node = G_.createPoolAvg(opName, tr, kernel, stride, pads);
    auto *N = G_.createTranspose(opName, node, NHWC2NCHW);
    addNodeAsOutput(op, N);
    return true;
  }

  if (typeName == "Squeeze") {
    auto *in = getOrCreateVariableByName(op.input(0));
    auto axes = getShape(dict["axes"]);
    Node *node = G_.createSqueeze(opName, in, axes);
    addNodeAsOutput(op, node);
    return true;
  }

  if (typeName == "Unsqueeze") {
    auto *in = getOrCreateVariableByName(op.input(0));
    auto axes = getShape(dict["axes"]);
    Node *node = G_.createExpandDims(opName, in, axes);
    addNodeAsOutput(op, node);
    return true;
  }

  if (typeName == "Dropout") {
    auto *in = getOrCreateVariableByName(op.input(0));
    // Save the identity operation:
    addNodeAsOutput(op, in);
    return true;
  }

  if (typeName == "BatchNormalization") {
    auto *in = getOrCreateVariableByName(op.input(0));
    auto *scale = getTensorByName(op.input(1));
    auto *bias = getTensorByName(op.input(2));
    auto *mean = getTensorByName(op.input(3));
    auto *var = getTensorByName(op.input(4));
    float epsilon = 1e-5f; // default
    auto epsilonIt = dict.find("epsilon");
    if (epsilonIt != dict.end()) {
      epsilon = loadFloat(epsilonIt->second);
    }

    auto *node = G_.createBatchNormalization(opName, in, 1, epsilon);

    // Load the weights.
    cast<Variable>(node->getScale())->copyFrom(scale);
    cast<Variable>(node->getBias())->copyFrom(bias);
    cast<Variable>(node->getMean())->copyFrom(mean);
    cast<Variable>(node->getVar())->copyFrom(var);
    addNodeAsOutput(op, node);
    return true;
  }

  if (typeName == "Concat") {
    const unsigned numInputs = op.input_size();
    llvm::SmallVector<NodeValue, 4> inputs;
    inputs.reserve(numInputs);
    for (unsigned i = 0; i < numInputs; i++) {
      inputs.push_back(getOrCreateVariableByName(op.input(i)));
    }

    auto axis = loadInt(dict["axis"]);
    Node *node = G_.createConcat(opName, inputs, axis);

    addNodeAsOutput(op, node);
    return true;
  }

  if (typeName == "Gemm") {
    Node *A = getOrCreateVariableByName(op.input(0));
    Node *B = getOrCreateVariableByName(op.input(1));
    Node *C = getOrCreateVariableByName(op.input(2));

    bool broadcastC = getBroadcast(dict);
    bool transA = dict.count("transA") && loadInt(dict["transA"]);
    bool transB = dict.count("transB") && loadInt(dict["transB"]);
    // TODO: support alpha * A * B + beta * C

    if (transA)
      A = G_.createTranspose(opName, A, {1, 0});
    if (transB)
      B = G_.createTranspose(opName, B, {1, 0});

    MatMulNode *mul = G_.createMatMul(opName, A, B);
    if (broadcastC) {
      int axis = mul->getResult().dims().size() - C->dims(0).size();
      C = G_.createBroadcast(opName, C, mul->getResult().dims(), axis);
    }

    Node *node = G_.createAdd(opName, mul, C);
    addNodeAsOutput(op, node);
    return true;
  }

  if (typeName == "Transpose") {
    loadTranspose(op, dict, "perm");
    return true;
  }

  return false;
}

/// Creates tensor \p T from the input \p in. Note, there is no data associated
/// with the Tensor. This method makes sure that the tensor is created with the
/// proper share and element type.
static void loadShape(const onnx::TypeProto &in, Tensor *T) {
  std::vector<size_t> dim;
  for (auto d : in.tensor_type().shape().dim()) {
    dim.push_back(d.dim_value());
  }

  if (in.tensor_type().elem_type() == onnx::TensorProto::FLOAT) {
    T->reset(ElemKind::FloatTy, dim);
  } else if (in.tensor_type().elem_type() == onnx::TensorProto::INT64) {
    // TODO: either switch IndexTy to be 64 bit, or switch to another type here
    T->reset(ElemKind::IndexTy, dim);
  } else {
    assert(false && "Only float and index tensors are supported");
  }
}

/// Loads tensor \p T from the input \p in.
void ONNXModelLoader::loadInputs(onnx::GraphProto &net) {
  for (const auto &in : net.input()) {
    Tensor *T = new Tensor();
    loadShape(in.type(), T);
    tensors_[in.name()] = T;
  }
}

void ONNXModelLoader::loadInitializers(onnx::GraphProto &net) {
  // Load the network initializaers:
  for (const auto &in : net.initializer()) {
    Tensor *T = new Tensor();
    loadTensor(in, T);
    tensors_[in.name()] = T;
  }
}

void ONNXModelLoader::setOutputNodes(onnx::GraphProto &net) {
  assert(net.output_size() && "Network needs external outputs defined.");

  for (int i = 0; i < net.output_size(); i++) {
    auto &outputName = net.output(i).name();
    auto *r = getNodeByName(outputName);
    outputsByName_[outputName] = G_.createSave("save_" + outputName, r);
  }
}

bool ONNXModelLoader::loadNetwork(onnx::GraphProto &net) {
  /// Load the network operators:
  for (int i = 0; i < net.node_size(); i++) {
    auto &op = net.node(i);
    if (!loadOperator(op)) {
      unexpectedNodeError(op, "Unsupported operator.");
      return false;
    }
  }

  return true;
}

std::unique_ptr<ONNXModelLoader> ONNXModelLoader::parse(const void *onnxModel,
                                                        size_t onnxModelSize,
                                                        Function &F) {
  std::unique_ptr<ONNXModelLoader> onnxLoader(new ONNXModelLoader(F));

  onnx::GraphProto modelDef;
  if (!onnxLoader->loadProto(modelDef, onnxModel, onnxModelSize)) {
    return nullptr;
  }

  onnxLoader->loadInputs(modelDef);
  if (!onnxLoader->loadNetwork(modelDef)) {
    return nullptr;
  }

  return onnxLoader;
}

ONNXModelLoader::ONNXModelLoader(Function &F)
    : CommonOperatorLoader({}, {}, F) {}

ONNXModelLoader::ONNXModelLoader(const std::string &modelDescFilename,
                                 llvm::ArrayRef<const char *> tensorNames,
                                 llvm::ArrayRef<Tensor *> tensors, Function &F)
    : CommonOperatorLoader(tensorNames, tensors, F) {
  // The ONNX model that we are deserializing.
  onnx::GraphProto modelDef;
  if (!loadProto(modelDef, modelDescFilename)) {
    GLOW_ASSERT("Cannot load the network.");
  }

  loadInitializers(modelDef);
  if (loadNetwork(modelDef)) {
    setOutputNodes(modelDef);
  }
}
