#include "core/framework/inference_session.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>

#include "core/common/logging/logging.h"
#include "core/framework/execution_provider.h"
#include "core/framework/op_kernel.h"
#include "core/framework/session_state.h"
#include "core/graph/graph.h"
#include "core/graph/model.h"
#include "core/graph/op.h"
#include "core/providers/cpu/cpu_execution_provider.h"
#include "core/providers/cpu/math/element_wise_ops.h"
#include "core/framework/tensorprotoutils.h"

#include "test/capturing_sink.h"
#include "test/test_environment.h"
#include "test_utils.h"
#include "gtest/gtest.h"

using namespace std;
using namespace Lotus::Logging;

namespace Lotus {
namespace Test {
static bool Compare(const InputDefList& f_arg, const InputDefList& s_arg);
static const std::string MODEL_URI = "testdata/mul_1.pb";
//static const std::string MODEL_URI = "./testdata/squeezenet/model.onnx"; // TODO enable this after we've weights?

void RunModel(InferenceSession& session_object,
              const RunOptions& run_options,
              bool is_preallocate_output_vec = false) {
  // prepare inputs
  std::vector<int64_t> dims_mul_x = {3, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  MLValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), dims_mul_x, values_mul_x, &ml_value);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("Y");
  std::vector<MLValue> fetches;

  if (is_preallocate_output_vec) {
    fetches.resize(output_names.size());
    for (auto& elem : fetches) {
      CreateMLValue<float>(TestCPUExecutionProvider()->GetAllocator(), dims_mul_x, values_mul_x, &elem);
    }
  }

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_mul_y = {3, 2};
  std::vector<float> expected_values_mul_y = {1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f};

  // Now run
  Common::Status st = session_object.Run(run_options, feeds, output_names, &fetches);
  std::cout << "Run returned status: " << st.ErrorMessage() << std::endl;
  ASSERT_TRUE(st.IsOK());
  ASSERT_EQ(1, fetches.size());
  auto& rtensor = fetches.front().Get<Tensor>();
  TensorShape expected_shape(expected_dims_mul_y);
  ASSERT_EQ(expected_shape, rtensor.Shape());
  const std::vector<float> found(rtensor.Data<float>(), rtensor.Data<float>() + expected_shape.Size());
  ASSERT_EQ(expected_values_mul_y, found);
}

TEST(InferenceSessionTests, NoTimeout) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.NoTimeout";

  InferenceSession session_object{so, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "one session/one tag";
  RunModel(session_object, run_options);
}

static bool Compare(const InputDefList& f_arg, const InputDefList& s_arg) {
  if (f_arg.size() != s_arg.size()) {
    cout << "Sizes differ: f_arg size: " << f_arg.size() << " s_arg size: " << s_arg.size() << endl;
    return false;
  }

  for (auto i = 0; i < f_arg.size(); ++i) {
    const LotusIR::NodeArg* x = f_arg[i];
    const LotusIR::NodeArg* y = s_arg[i];
    if ((x->Shape() == nullptr) ^ (y->Shape() == nullptr)) {
      return false;
    } else {
      if (!x->Shape()) {
        continue;
      } else {
        vector<int64_t> x_shape = Utils::GetTensorShapeFromTensorShapeProto(*x->Shape());
        vector<int64_t> y_shape = Utils::GetTensorShapeFromTensorShapeProto(*y->Shape());
        if (x->Name() == y->Name() && x_shape == y_shape && *x->Type() == *y->Type()) {
          continue;
        } else {
          return false;
        }
      }
    }
  }

  return true;
}

TEST(InferenceSessionTests, ModelMetadata) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.ModelMetadata";
  InferenceSession session_object{so, &DefaultLoggingManager()};
  string model_uri = "testdata/squeezenet/model.onnx";
  ASSERT_TRUE(session_object.Load(model_uri).IsOK());

  std::shared_ptr<LotusIR::Model> p_model;
  Status st = LotusIR::Model::Load(model_uri, &p_model);
  ASSERT_TRUE(st.IsOK());
  const LotusIR::Graph* p_graph = p_model->MainGraph();
  ASSERT_TRUE(p_graph != nullptr);

  // 1. first test the model meta
  {
    auto retval = session_object.GetModelMetadata();
    ASSERT_TRUE(retval.first.IsOK());
    const ModelMetadata* m = retval.second;
    ASSERT_TRUE(m->custom_metadata_map == p_model->MetaData() &&
                m->description == p_model->DocString() &&
                m->domain == p_model->Domain() &&
                m->graph_name == p_graph->Name() &&
                m->producer_name == p_model->ProducerName() &&
                m->version == p_model->ModelVersion());
  }

  {
    // 2. test inputs
    auto& inputs = p_graph->GetInputs();
    auto weights = p_graph->GetAllInitializedTensors();

    // skip the weights
    InputDefList inputs_no_weights;
    for (auto& elem : inputs) {
      if (weights.find(elem->Name()) != weights.end()) {
        continue;
      } else {
        inputs_no_weights.push_back(elem);
      }
    }

    auto retval = session_object.GetInputs();
    cout << "weights size: " << weights.size()
         << " inputs.size(): " << inputs.size()
         << " from session: " << retval.second->size() << endl;
    ASSERT_TRUE(retval.first.IsOK());
    ASSERT_TRUE(Compare(inputs_no_weights, *retval.second));
  }

  // 3. test outputs
  {
    auto retval = session_object.GetOutputs();
    ASSERT_TRUE(retval.first.IsOK());

    auto& outputs = p_graph->GetOutputs();
    retval = session_object.GetOutputs();
    ASSERT_TRUE(retval.first.IsOK());
    ASSERT_TRUE(Compare(outputs, *retval.second));
  }
}

TEST(InferenceSessionTests, CheckRunLogger) {
  SessionOptions so;

  so.session_logid = "CheckRunLogger";

  // create CapturingSink. LoggingManager will own it, but as long as the logging_manager
  // is around our pointer stays valid.
  auto capturing_sink = new CapturingSink();

  auto logging_manager = std::make_unique<Logging::LoggingManager>(
      std::unique_ptr<ISink>(capturing_sink), Logging::Severity::kVERBOSE, false, LoggingManager::InstanceType::Temporal);

  InferenceSession session_object{so, logging_manager.get()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "RunTag";
  RunModel(session_object, run_options);

#ifdef _DEBUG
  // check for some VLOG output to make sure tag was correct. VLOG is not enabled in release build
  auto& msgs = capturing_sink->Messages();
  std::copy(msgs.begin(), msgs.end(), std::ostream_iterator<std::string>(std::cout, "\n"));
  bool have_log_entry_with_run_tag =
      (std::find_if(msgs.begin(), msgs.end(),
                    [&run_options](std::string msg) { return msg.find(run_options.run_tag) != string::npos; }) != msgs.end());

  ASSERT_TRUE(have_log_entry_with_run_tag);
#endif
}

TEST(InferenceSessionTests, MultipleSessionsNoTimeout) {
  SessionOptions session_options;

  session_options.session_logid = "InferenceSessionTests.MultipleSessionsNoTimeout";
  InferenceSession session_object{session_options, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  std::thread thread1{[&session_object]() {
    RunOptions run_options;
    run_options.run_tag = "one session/thread 1";
    RunModel(session_object, run_options);
  }};

  std::thread thread2{[&session_object]() {
    RunOptions run_options;
    run_options.run_tag = "one session/thread 2";
    RunModel(session_object, run_options);
  }};

  thread1.join();
  thread2.join();
}

TEST(InferenceSessionTests, PreAllocateOutputVector) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.PreAllocateOutputVector";

  InferenceSession session_object{so, &DefaultLoggingManager()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.PreAllocateOutputVector";
  bool is_preallocate_output_vec = true;
  RunModel(session_object, run_options, is_preallocate_output_vec);
}

TEST(InferenceSessionTests, ConfigureVerbosityLevel) {
  SessionOptions so;

  so.session_logid = "ConfigureVerbosityLevel";
  so.session_log_verbosity_level = 1;

  // create CapturingSink. LoggingManager will own it, but as long as the logging_manager
  // is around our pointer stays valid.
  auto capturing_sink = new CapturingSink();

  auto logging_manager = std::make_unique<Logging::LoggingManager>(
      std::unique_ptr<ISink>(capturing_sink),
      Logging::Severity::kVERBOSE,
      false,
      LoggingManager::InstanceType::Temporal);

  InferenceSession session_object{so, logging_manager.get()};
  ASSERT_TRUE(session_object.Load(MODEL_URI).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "ConfigureVerbosityLevel";
  run_options.run_log_verbosity_level = 1;
  RunModel(session_object, run_options);

#ifdef _DEBUG
  // check for some VLOG output to make sure tag was correct. VLOG is not enabled in release build
  auto& msgs = capturing_sink->Messages();
  std::copy(msgs.begin(), msgs.end(), std::ostream_iterator<std::string>(std::cout, "\n"));
  bool have_log_entry_with_vlog_session_msg =
      (std::find_if(msgs.begin(), msgs.end(),
                    [&run_options](std::string msg) { return msg.find("Adding input argument with name") != string::npos; }) !=
       msgs.end());

  ASSERT_TRUE(have_log_entry_with_vlog_session_msg);

  bool have_log_entry_with_vlog_run_msg =
      (std::find_if(msgs.begin(), msgs.end(),
                    [&run_options](std::string msg) { return msg.find("Size of execution plan vector") != string::npos; }) !=
       msgs.end());

  ASSERT_TRUE(have_log_entry_with_vlog_run_msg);
#endif
}

TEST(InferenceSessionTests, TestWithIstream) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestWithIstream";

  InferenceSession session_object{so};

  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ASSERT_TRUE(model_file_stream.good());
  ASSERT_TRUE(session_object.Load(model_file_stream).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestWithIstream";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, TestRegisterExecutionProvider) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestWithIstream";

  InferenceSession session_object{so};
  CPUExecutionProviderInfo epi;
  ASSERT_TRUE(session_object.RegisterExecutionProvider(std::make_unique<CPUExecutionProvider>(epi)).IsOK());

  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ASSERT_TRUE(model_file_stream.good());
  ASSERT_TRUE(session_object.Load(model_file_stream).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestWithIstream";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, TestModelProtoInterface) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestModelProtoInterface";

  InferenceSession session_object{so};
  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ASSERT_TRUE(model_file_stream.good());

  ModelProto model_proto;
  ASSERT_TRUE(model_proto.ParseFromIstream(&model_file_stream));
  ASSERT_TRUE(session_object.Load(model_proto).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestModelProtoInterface";
  RunModel(session_object, run_options);
}

TEST(InferenceSessionTests, TestModelProtoInterfaceMultipleLoadFailure) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.TestModelProtoInterfaceMultipleLoadFailure";

  InferenceSession session_object{so};
  std::ifstream model_file_stream(MODEL_URI, ios::in | ios::binary);
  ASSERT_TRUE(model_file_stream.good());

  ModelProto model_proto;
  ASSERT_TRUE(model_proto.ParseFromIstream(&model_file_stream));
  ASSERT_TRUE(session_object.Load(model_proto).IsOK());
  ASSERT_FALSE(session_object.Load(model_proto).IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());
  ASSERT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "InferenceSessionTests.TestModelProtoInterfaceMultipleLoadFailure";
  RunModel(session_object, run_options);
}

}  // namespace Test
}  // namespace Lotus
