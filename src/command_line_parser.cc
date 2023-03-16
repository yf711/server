// Copyright 2022-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include "command_line_parser.h"

#ifdef _WIN32
int optind = 1;
const char* optarg = nullptr;

bool
end_of_long_opts(const struct option* longopts)
{
  return (
      (longopts->name_ == nullptr) && (longopts->has_arg_ == 0) &&
      (longopts->flag_ == nullptr) && (longopts->val_ == 0));
}

int
getopt_long(
    int argc, char* const argv[], const char* optstring,
    const struct option* longopts, int* longindex)
{
  if ((longindex != NULL) || (optind >= argc)) {
    return -1;
  }
  const struct option* curr_longopt = longopts;
  std::string argv_str = argv[optind];
  size_t found = argv_str.find_first_of("=");
  std::string key = argv_str.substr(
      2, (found == std::string::npos) ? std::string::npos : (found - 2));
  while (!end_of_long_opts(curr_longopt)) {
    if (key == curr_longopt->name_) {
      if (curr_longopt->has_arg_ == required_argument) {
        if (found == std::string::npos) {
          optind++;
          if (optind >= argc) {
            std::cerr << argv[0] << ": option '" << argv_str
                      << "' requires an argument" << std::endl;
            return '?';
          }
          optarg = argv[optind];
        } else {
          optarg = (argv[optind] + found + 1);
        }
      }
      optind++;
      return curr_longopt->val_;
    }
    curr_longopt++;
  }
  return -1;
}
#endif

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>

#include "common.h"

#define TRITONJSON_STATUSTYPE TRITONSERVER_Error*
#define TRITONJSON_STATUSRETURN(M) \
  return TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, (M).c_str())
#define TRITONJSON_STATUSSUCCESS nullptr
#include "triton/common/triton_json.h"


namespace triton { namespace server {

// [FIXME] expose following parse helpers for other type of parser
namespace {

// There must be specialization for the types to be parsed into so that
// the argument is properly validated and parsed. Attempted to use input
// opeartor (>>) but it will consume inproper argument without error
// (i.e. parse "1.4" to 'int' will return 1 but we want to report error).
template <typename T>
T ParseOption(const std::string& arg);

template <>
int
ParseOption(const std::string& arg)
{
  return std::stoi(arg);
}

template <>
uint64_t
ParseOption(const std::string& arg)
{
  return std::stoull(arg);
}

template <>
int64_t
ParseOption(const std::string& arg)
{
  return std::stoll(arg);
}

template <>
double
ParseOption(const std::string& arg)
{
  return std::stod(arg);
}

template <>
bool
ParseOption(const std::string& arg)
{
  // 'arg' need to comply with template declaration
  std::string larg = arg;
  std::transform(larg.begin(), larg.end(), larg.begin(), [](unsigned char c) {
    return std::tolower(c);
  });

  if ((larg == "true") || (larg == "on") || (larg == "1")) {
    return true;
  }
  if ((larg == "false") || (larg == "off") || (larg == "0")) {
    return false;
  }

  throw ParseException("invalid value for bool option: " + arg);
}

// Condition here merely to avoid compilation error, this function will
// be defined but not used otherwise.
#ifdef TRITON_ENABLE_LOGGING
int
ParseIntBoolOption(std::string arg)
{
  std::transform(arg.begin(), arg.end(), arg.begin(), [](unsigned char c) {
    return std::tolower(c);
  });

  if (arg == "true") {
    return 1;
  }
  if (arg == "false") {
    return 0;
  }

  return ParseOption<int>(arg);
}
#endif  // TRITON_ENABLE_LOGGING

std::string
PairsToJsonStr(std::vector<std::pair<std::string, std::string>> settings)
{
  triton::common::TritonJson::Value json(
      triton::common::TritonJson::ValueType::OBJECT);
  for (const auto& setting : settings) {
    const auto& key = setting.first;
    const auto& value = setting.second;
    json.SetStringObject(key.c_str(), value);
  }
  triton::common::TritonJson::WriteBuffer buffer;
  auto err = json.Write(&buffer);
  if (err != nullptr) {
    LOG_TRITONSERVER_ERROR(err, "failed to convert config to JSON");
  }
  return buffer.Contents();
}

template <typename T1, typename T2>
std::pair<T1, T2>
ParsePairOption(const std::string& arg, const std::string& delim_str)
{
  int delim = arg.find(delim_str);

  if ((delim < 0)) {
    std::stringstream ss;
    ss << "Cannot parse pair option due to incorrect number of inputs."
          "--<pair option> argument requires format <first>"
       << delim_str << "<second>. "
       << "Found: " << arg << std::endl;
    throw ParseException(ss.str());
  }

  std::string first_string = arg.substr(0, delim);
  std::string second_string = arg.substr(delim + delim_str.length());

  // Specific conversion from key-value string to actual key-value type,
  // should be extracted out of this function if we need to parse
  // more pair option of different types.
  return {ParseOption<T1>(first_string), ParseOption<T2>(second_string)};
}

}  // namespace

enum TritonOptionId {
  OPTION_HELP = 1000,
#ifdef TRITON_ENABLE_LOGGING
  OPTION_LOG_VERBOSE,
  OPTION_LOG_INFO,
  OPTION_LOG_WARNING,
  OPTION_LOG_ERROR,
  OPTION_LOG_FORMAT,
  OPTION_LOG_FILE,
#endif  // TRITON_ENABLE_LOGGING
  OPTION_ID,
  OPTION_MODEL_REPOSITORY,
  OPTION_EXIT_ON_ERROR,
  OPTION_DISABLE_AUTO_COMPLETE_CONFIG,
  OPTION_STRICT_MODEL_CONFIG,
  OPTION_STRICT_READINESS,
#if defined(TRITON_ENABLE_HTTP)
  OPTION_ALLOW_HTTP,
  OPTION_HTTP_PORT,
  OPTION_REUSE_HTTP_PORT,
  OPTION_HTTP_ADDRESS,
  OPTION_HTTP_THREAD_COUNT,
#endif  // TRITON_ENABLE_HTTP
#if defined(TRITON_ENABLE_GRPC)
  OPTION_ALLOW_GRPC,
  OPTION_GRPC_PORT,
  OPTION_REUSE_GRPC_PORT,
  OPTION_GRPC_ADDRESS,
  OPTION_GRPC_INFER_ALLOCATION_POOL_SIZE,
  OPTION_GRPC_USE_SSL,
  OPTION_GRPC_USE_SSL_MUTUAL,
  OPTION_GRPC_SERVER_CERT,
  OPTION_GRPC_SERVER_KEY,
  OPTION_GRPC_ROOT_CERT,
  OPTION_GRPC_RESPONSE_COMPRESSION_LEVEL,
  OPTION_GRPC_ARG_KEEPALIVE_TIME_MS,
  OPTION_GRPC_ARG_KEEPALIVE_TIMEOUT_MS,
  OPTION_GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
  OPTION_GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA,
  OPTION_GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
  OPTION_GRPC_ARG_HTTP2_MAX_PING_STRIKES,
#endif  // TRITON_ENABLE_GRPC
#if defined(TRITON_ENABLE_SAGEMAKER)
  OPTION_ALLOW_SAGEMAKER,
  OPTION_SAGEMAKER_PORT,
  OPTION_SAGEMAKER_SAFE_PORT_RANGE,
  OPTION_SAGEMAKER_THREAD_COUNT,
#endif  // TRITON_ENABLE_SAGEMAKER
#if defined(TRITON_ENABLE_VERTEX_AI)
  OPTION_ALLOW_VERTEX_AI,
  OPTION_VERTEX_AI_PORT,
  OPTION_VERTEX_AI_THREAD_COUNT,
  OPTION_VERTEX_AI_DEFAULT_MODEL,
#endif  // TRITON_ENABLE_VERTEX_AI
#ifdef TRITON_ENABLE_METRICS
  OPTION_ALLOW_METRICS,
  OPTION_ALLOW_GPU_METRICS,
  OPTION_ALLOW_CPU_METRICS,
  OPTION_METRICS_PORT,
  OPTION_METRICS_INTERVAL_MS,
#endif  // TRITON_ENABLE_METRICS
#ifdef TRITON_ENABLE_TRACING
  OPTION_TRACE_FILEPATH,
  OPTION_TRACE_LEVEL,
  OPTION_TRACE_RATE,
  OPTION_TRACE_COUNT,
  OPTION_TRACE_LOG_FREQUENCY,
#endif  // TRITON_ENABLE_TRACING
  OPTION_MODEL_CONTROL_MODE,
  OPTION_POLL_REPO_SECS,
  OPTION_STARTUP_MODEL,
  OPTION_RATE_LIMIT,
  OPTION_RATE_LIMIT_RESOURCE,
  OPTION_PINNED_MEMORY_POOL_BYTE_SIZE,
  OPTION_CUDA_MEMORY_POOL_BYTE_SIZE,
  OPTION_RESPONSE_CACHE_BYTE_SIZE,
  OPTION_CACHE_CONFIG,
  OPTION_CACHE_DIR,
  OPTION_MIN_SUPPORTED_COMPUTE_CAPABILITY,
  OPTION_EXIT_TIMEOUT_SECS,
  OPTION_BACKEND_DIR,
  OPTION_REPOAGENT_DIR,
  OPTION_BUFFER_MANAGER_THREAD_COUNT,
  OPTION_MODEL_LOAD_THREAD_COUNT,
  OPTION_BACKEND_CONFIG,
  OPTION_HOST_POLICY,
  OPTION_MODEL_LOAD_GPU_LIMIT,
  OPTION_MODEL_NAMESPACING
};

std::vector<Option> TritonParser::recognized_options_
{
  {OPTION_HELP, "help", Option::ArgNone, "Print usage"},
#ifdef TRITON_ENABLE_LOGGING
      {OPTION_LOG_VERBOSE, "log-verbose", Option::ArgInt,
       "Set verbose logging level. Zero (0) disables verbose logging and "
       "values >= 1 enable verbose logging."},
      {OPTION_LOG_INFO, "log-info", Option::ArgBool,
       "Enable/disable info-level logging."},
      {OPTION_LOG_WARNING, "log-warning", Option::ArgBool,
       "Enable/disable warning-level logging."},
      {OPTION_LOG_ERROR, "log-error", Option::ArgBool,
       "Enable/disable error-level logging."},
      {OPTION_LOG_FORMAT, "log-format", Option::ArgStr,
       "Set the logging format. Options are \"default\" and \"ISO8601\". "
       "The default is \"default\". For \"default\", the log severity (L) and "
       "timestamp will be logged as \"LMMDD hh:mm:ss.ssssss\". "
       "For \"ISO8601\", the log format will be \"YYYY-MM-DDThh:mm:ssZ L\"."},
      {OPTION_LOG_FILE, "log-file", Option::ArgStr,
       "Set the name of the log output file. If specified, log outputs will be "
       "saved to this file. If not specified, log outputs will stream to the "
       "console."},
#endif  // TRITON_ENABLE_LOGGING
      {OPTION_ID, "id", Option::ArgStr, "Identifier for this server."},
      {OPTION_MODEL_REPOSITORY, "model-store", Option::ArgStr,
       "Equivalent to --model-repository."},
      {OPTION_MODEL_REPOSITORY, "model-repository", Option::ArgStr,
       "Path to model repository directory. It may be specified multiple times "
       "to add multiple model repositories. Note that if a model is not unique "
       "across all model repositories at any time, the model will not be "
       "available."},
      {OPTION_EXIT_ON_ERROR, "exit-on-error", Option::ArgBool,
       "Exit the inference server if an error occurs during initialization."},
      {OPTION_DISABLE_AUTO_COMPLETE_CONFIG, "disable-auto-complete-config",
       Option::ArgNone,
       "If set, disables the triton and backends from auto completing model "
       "configuration files. Model configuration files must be provided and "
       "all required "
       "configuration settings must be specified."},
      {OPTION_STRICT_MODEL_CONFIG, "strict-model-config", Option::ArgBool,
       "DEPRECATED: If true model configuration files must be provided and all "
       "required "
       "configuration settings must be specified. If false the model "
       "configuration may be absent or only partially specified and the "
       "server will attempt to derive the missing required configuration."},
      {OPTION_STRICT_READINESS, "strict-readiness", Option::ArgBool,
       "If true /v2/health/ready endpoint indicates ready if the server "
       "is responsive and all models are available. If false "
       "/v2/health/ready endpoint indicates ready if server is responsive "
       "even if some/all models are unavailable."},
#if defined(TRITON_ENABLE_HTTP)
      {OPTION_ALLOW_HTTP, "allow-http", Option::ArgBool,
       "Allow the server to listen for HTTP requests."},
      {OPTION_HTTP_PORT, "http-port", Option::ArgInt,
       "The port for the server to listen on for HTTP requests."},
      {OPTION_REUSE_HTTP_PORT, "reuse-http-port", Option::ArgBool,
       "Allow multiple servers to listen on the same HTTP port when every "
       "server has this option set. If you plan to use this option as a way to "
       "load balance between different Triton servers, the same model "
       "repository or set of models must be used for every server."},
      {OPTION_HTTP_ADDRESS, "http-address", Option::ArgStr,
       "The address for the http server to binds to."},
      {OPTION_HTTP_THREAD_COUNT, "http-thread-count", Option::ArgInt,
       "Number of threads handling HTTP requests."},
#endif  // TRITON_ENABLE_HTTP
#if defined(TRITON_ENABLE_GRPC)
      {OPTION_ALLOW_GRPC, "allow-grpc", Option::ArgBool,
       "Allow the server to listen for GRPC requests."},
      {OPTION_GRPC_PORT, "grpc-port", Option::ArgInt,
       "The port for the server to listen on for GRPC requests."},
      {OPTION_REUSE_GRPC_PORT, "reuse-grpc-port", Option::ArgBool,
       "Allow multiple servers to listen on the same GRPC port when every "
       "server has this option set. If you plan to use this option as a way to "
       "load balance between different Triton servers, the same model "
       "repository or set of models must be used for every server."},
      {OPTION_GRPC_ADDRESS, "grpc-address", Option::ArgStr,
       "The address for the grpc server to binds to."},
      {OPTION_GRPC_INFER_ALLOCATION_POOL_SIZE,
       "grpc-infer-allocation-pool-size", Option::ArgInt,
       "The maximum number of inference request/response objects that remain "
       "allocated for reuse. As long as the number of in-flight requests "
       "doesn't exceed this value there will be no allocation/deallocation of "
       "request/response objects."},
      {OPTION_GRPC_USE_SSL, "grpc-use-ssl", Option::ArgBool,
       "Use SSL authentication for GRPC requests. Default is false."},
      {OPTION_GRPC_USE_SSL_MUTUAL, "grpc-use-ssl-mutual", Option::ArgBool,
       "Use mututal SSL authentication for GRPC requests. This option will "
       "preempt '--grpc-use-ssl' if it is also specified. Default is false."},
      {OPTION_GRPC_SERVER_CERT, "grpc-server-cert", Option::ArgStr,
       "File holding PEM-encoded server certificate. Ignored unless "
       "--grpc-use-ssl is true."},
      {OPTION_GRPC_SERVER_KEY, "grpc-server-key", Option::ArgStr,
       "File holding PEM-encoded server key. Ignored unless "
       "--grpc-use-ssl is true."},
      {OPTION_GRPC_ROOT_CERT, "grpc-root-cert", Option::ArgStr,
       "File holding PEM-encoded root certificate. Ignore unless "
       "--grpc-use-ssl is false."},
      {OPTION_GRPC_RESPONSE_COMPRESSION_LEVEL,
       "grpc-infer-response-compression-level", Option::ArgStr,
       "The compression level to be used while returning the infer response to "
       "the peer. Allowed values are none, low, medium and high. By default, "
       "compression level is selected as none."},
      {OPTION_GRPC_ARG_KEEPALIVE_TIME_MS, "grpc-keepalive-time", Option::ArgInt,
       "The period (in milliseconds) after which a keepalive ping is sent on "
       "the transport. Default is 7200000 (2 hours)."},
      {OPTION_GRPC_ARG_KEEPALIVE_TIMEOUT_MS, "grpc-keepalive-timeout",
       Option::ArgInt,
       "The period (in milliseconds) the sender of the keepalive ping waits "
       "for an acknowledgement. If it does not receive an acknowledgment "
       "within this time, it will close the connection. "
       "Default is 20000 (20 seconds)."},
      {OPTION_GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS,
       "grpc-keepalive-permit-without-calls", Option::ArgBool,
       "Allows keepalive pings to be sent even if there are no calls in flight "
       "(0 : false; 1 : true). Default is 0 (false)."},
      {OPTION_GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA,
       "grpc-http2-max-pings-without-data", Option::ArgInt,
       "The maximum number of pings that can be sent when there is no "
       "data/header frame to be sent. gRPC Core will not continue sending "
       "pings if we run over the limit. Setting it to 0 allows sending pings "
       "without such a restriction. Default is 2."},
      {OPTION_GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
       "grpc-http2-min-recv-ping-interval-without-data", Option::ArgInt,
       "If there are no data/header frames being sent on the transport, this "
       "channel argument on the server side controls the minimum time "
       "(in milliseconds) that gRPC Core would expect between receiving "
       "successive pings. If the time between successive pings is less than "
       "this time, then the ping will be considered a bad ping from the peer. "
       "Such a ping counts as a ‘ping strike’. Default is 300000 (5 minutes)."},
      {OPTION_GRPC_ARG_HTTP2_MAX_PING_STRIKES, "grpc-http2-max-ping-strikes",
       Option::ArgInt,
       "Maximum number of bad pings that the server will tolerate before "
       "sending an HTTP2 GOAWAY frame and closing the transport. Setting it to "
       "0 allows the server to accept any number of bad pings. Default is 2."},
#endif  // TRITON_ENABLE_GRPC
#if defined(TRITON_ENABLE_SAGEMAKER)
      {OPTION_ALLOW_SAGEMAKER, "allow-sagemaker", Option::ArgBool,
       "Allow the server to listen for Sagemaker requests. Default is false."},
      {OPTION_SAGEMAKER_PORT, "sagemaker-port", Option::ArgInt,
       "The port for the server to listen on for Sagemaker requests. Default "
       "is 8080."},
      {OPTION_SAGEMAKER_SAFE_PORT_RANGE, "sagemaker-safe-port-range",
       "<integer>-<integer>",
       "Set the allowed port range for endpoints other than the SageMaker "
       "endpoints."},
      {OPTION_SAGEMAKER_THREAD_COUNT, "sagemaker-thread-count", Option::ArgInt,
       "Number of threads handling Sagemaker requests. Default is 8."},
#endif  // TRITON_ENABLE_SAGEMAKER
#if defined(TRITON_ENABLE_VERTEX_AI)
      {OPTION_ALLOW_VERTEX_AI, "allow-vertex-ai", Option::ArgBool,
       "Allow the server to listen for Vertex AI requests. Default is true if "
       "AIP_MODE=PREDICTION, false otherwise."},
      {OPTION_VERTEX_AI_PORT, "vertex-ai-port", Option::ArgInt,
       "The port for the server to listen on for Vertex AI requests. Default "
       "is AIP_HTTP_PORT if set, 8080 otherwise."},
      {OPTION_VERTEX_AI_THREAD_COUNT, "vertex-ai-thread-count", Option::ArgInt,
       "Number of threads handling Vertex AI requests. Default is 8."},
      {OPTION_VERTEX_AI_DEFAULT_MODEL, "vertex-ai-default-model",
       Option::ArgStr,
       "The name of the model to use for single-model inference requests."},
#endif  // TRITON_ENABLE_VERTEX_AI
#ifdef TRITON_ENABLE_METRICS
      {OPTION_ALLOW_METRICS, "allow-metrics", Option::ArgBool,
       "Allow the server to provide prometheus metrics."},
      {OPTION_ALLOW_GPU_METRICS, "allow-gpu-metrics", Option::ArgBool,
       "Allow the server to provide GPU metrics. Ignored unless "
       "--allow-metrics is true."},
      {OPTION_ALLOW_CPU_METRICS, "allow-cpu-metrics", Option::ArgBool,
       "Allow the server to provide CPU metrics. Ignored unless "
       "--allow-metrics is true."},
      {OPTION_METRICS_PORT, "metrics-port", Option::ArgInt,
       "The port reporting prometheus metrics."},
      {OPTION_METRICS_INTERVAL_MS, "metrics-interval-ms", Option::ArgFloat,
       "Metrics will be collected once every <metrics-interval-ms> "
       "milliseconds. Default is 2000 milliseconds."},
#endif  // TRITON_ENABLE_METRICS
#ifdef TRITON_ENABLE_TRACING
      {OPTION_TRACE_FILEPATH, "trace-file", Option::ArgStr,
       "Set the file where trace output will be saved. If --trace-log-frequency"
       " is also specified, this argument value will be the prefix of the files"
       " to save the trace output. See --trace-log-frequency for detail."},
      {OPTION_TRACE_LEVEL, "trace-level", Option::ArgStr,
       "Specify a trace level. OFF to disable tracing, TIMESTAMPS to "
       "trace timestamps, TENSORS to trace tensors. It may be specified "
       "multiple times to trace multiple informations. Default is OFF."},
      {OPTION_TRACE_RATE, "trace-rate", Option::ArgInt,
       "Set the trace sampling rate. Default is 1000."},
      {OPTION_TRACE_COUNT, "trace-count", Option::ArgInt,
       "Set the number of traces to be sampled. If the value is -1, the number "
       "of traces to be sampled will not be limited. Default is -1."},
      {OPTION_TRACE_LOG_FREQUENCY, "trace-log-frequency", Option::ArgInt,
       "Set the trace log frequency. If the value is 0, Triton will only log "
       "the trace output to <trace-file> when shutting down. Otherwise, Triton "
       "will log the trace output to <trace-file>.<idx> when it collects the "
       "specified number of traces. For example, if the log frequency is 100, "
       "when Triton collects the 100-th trace, it logs the traces to file "
       "<trace-file>.0, and when it collects the 200-th trace, it logs the "
       "101-th to the 200-th traces to file <trace-file>.1. Default is 0."},
#endif  // TRITON_ENABLE_TRACING
      {OPTION_MODEL_CONTROL_MODE, "model-control-mode", Option::ArgStr,
       "Specify the mode for model management. Options are \"none\", \"poll\" "
       "and \"explicit\". The default is \"none\". "
       "For \"none\", the server will load all models in the model "
       "repository(s) at startup and will not make any changes to the load "
       "models after that. For \"poll\", the server will poll the model "
       "repository(s) to detect changes and will load/unload models based on "
       "those changes. The poll rate is controlled by 'repository-poll-secs'. "
       "For \"explicit\", model load and unload is initiated by using the "
       "model control APIs, and only models specified with --load-model will "
       "be loaded at startup."},
      {OPTION_POLL_REPO_SECS, "repository-poll-secs", Option::ArgInt,
       "Interval in seconds between each poll of the model repository to check "
       "for changes. Valid only when --model-control-mode=poll is "
       "specified."},
      {OPTION_STARTUP_MODEL, "load-model", Option::ArgStr,
       "Name of the model to be loaded on server startup. It may be specified "
       "multiple times to add multiple models. To load ALL models at startup, "
       "specify '*' as the model name with --load-model=* as the ONLY "
       "--load-model argument, this does not imply any pattern matching. "
       "Specifying --load-model=* in conjunction with another --load-model "
       "argument will result in error. Note that this option will only take "
       "effect if --model-control-mode=explicit is true."},
      // FIXME:  fix the default to execution_count once RL logic is complete.
      {OPTION_RATE_LIMIT, "rate-limit", Option::ArgStr,
       "Specify the mode for rate limiting. Options are \"execution_count\" "
       "and \"off\". The default is \"off\". For "
       "\"execution_count\", the server will determine the instance using "
       "configured priority and the number of time the instance has been "
       "used to run inference. The inference will finally be executed once "
       "the required resources are available. For \"off\", the server will "
       "ignore any rate limiter config and run inference as soon as an "
       "instance is ready."},
      {OPTION_RATE_LIMIT_RESOURCE, "rate-limit-resource",
       "<string>:<integer>:<integer>",
       "The number of resources available to the server. The format of this "
       "flag is --rate-limit-resource=<resource_name>:<count>:<device>. The "
       "<device> is optional and if not listed will be applied to every "
       "device. If the resource is specified as \"GLOBAL\" in the model "
       "configuration the resource is considered shared among all the devices "
       "in the system. The <device> property is ignored for such resources. "
       "This flag can be specified multiple times to specify each resources "
       "and their availability. By default, the max across all instances that "
       "list the resource is selected as its availability. The values for this "
       "flag is case-insensitive."},
      {OPTION_PINNED_MEMORY_POOL_BYTE_SIZE, "pinned-memory-pool-byte-size",
       Option::ArgInt,
       "The total byte size that can be allocated as pinned system memory. "
       "If GPU support is enabled, the server will allocate pinned system "
       "memory to accelerate data transfer between host and devices until it "
       "exceeds the specified byte size. If 'numa-node' is configured via "
       "--host-policy, the pinned system memory of the pool size will be "
       "allocated on each numa node. This option will not affect the "
       "allocation conducted by the backend frameworks. Default is 256 MB."},
      {OPTION_CUDA_MEMORY_POOL_BYTE_SIZE, "cuda-memory-pool-byte-size",
       "<integer>:<integer>",
       "The total byte size that can be allocated as CUDA memory for the GPU "
       "device. If GPU support is enabled, the server will allocate CUDA "
       "memory to minimize data transfer between host and devices until it "
       "exceeds the specified byte size. This option will not affect the "
       "allocation conducted by the backend frameworks. The argument should be "
       "2 integers separated by colons in the format "
       "<GPU device ID>:<pool byte size>. This option can be used multiple "
       "times, but only once per GPU device. Subsequent uses will overwrite "
       "previous uses for the same GPU device. Default is 64 MB."},
      {OPTION_RESPONSE_CACHE_BYTE_SIZE, "response-cache-byte-size",
       Option::ArgInt, "DEPRECATED: Please use --cache-config instead."},
      {OPTION_CACHE_CONFIG, "cache-config", "<string>,<string>=<string>",
       "Specify a cache-specific configuration setting. The format of this "
       "flag is --cache-config=<cache_name>,<setting>=<value>. Where "
       "<cache_name> is the name of the cache, such as 'local' or 'redis'. "
       "Example: --cache-config=local,size=1048576 will configure a 'local' "
       "cache implementation with a fixed buffer pool of size 1048576 bytes."},
      {OPTION_CACHE_DIR, "cache-directory", Option::ArgStr,
       "The global directory searched for cache shared libraries. Default is "
       "'/opt/tritonserver/caches'. This directory is expected to contain a "
       "cache implementation as a shared library with the name "
       "'libtritoncache.so'."},
      {OPTION_MIN_SUPPORTED_COMPUTE_CAPABILITY,
       "min-supported-compute-capability", Option::ArgFloat,
       "The minimum supported CUDA compute capability. GPUs that don't support "
       "this compute capability will not be used by the server."},
      {OPTION_EXIT_TIMEOUT_SECS, "exit-timeout-secs", Option::ArgInt,
       "Timeout (in seconds) when exiting to wait for in-flight inferences to "
       "finish. After the timeout expires the server exits even if inferences "
       "are still in flight."},
      {OPTION_BACKEND_DIR, "backend-directory", Option::ArgStr,
       "The global directory searched for backend shared libraries. Default is "
       "'/opt/tritonserver/backends'."},
      {OPTION_REPOAGENT_DIR, "repoagent-directory", Option::ArgStr,
       "The global directory searched for repository agent shared libraries. "
       "Default is '/opt/tritonserver/repoagents'."},
      {OPTION_BUFFER_MANAGER_THREAD_COUNT, "buffer-manager-thread-count",
       Option::ArgInt,
       "The number of threads used to accelerate copies and other operations "
       "required to manage input and output tensor contents. Default is 0."},
      {OPTION_MODEL_LOAD_THREAD_COUNT, "model-load-thread-count",
       Option::ArgInt,
       "The number of threads used to concurrently load models in "
       "model repositories. Default is 2*<num_cpu_cores>."},
      {OPTION_BACKEND_CONFIG, "backend-config", "<string>,<string>=<string>",
       "Specify a backend-specific configuration setting. The format of this "
       "flag is --backend-config=<backend_name>,<setting>=<value>. Where "
       "<backend_name> is the name of the backend, such as 'tensorrt'."},
      {OPTION_HOST_POLICY, "host-policy", "<string>,<string>=<string>",
       "Specify a host policy setting associated with a policy name. The "
       "format of this flag is --host-policy=<policy_name>,<setting>=<value>. "
       "Currently supported settings are 'numa-node', 'cpu-cores'. Note that "
       "'numa-node' setting will affect pinned memory pool behavior, see "
       "--pinned-memory-pool for more detail."},
      {OPTION_MODEL_LOAD_GPU_LIMIT, "model-load-gpu-limit",
       "<device_id>:<fraction>",
       "Specify the limit on GPU memory usage as a fraction. If model loading "
       "on the device is requested and the current memory usage exceeds the "
       "limit, the load will be rejected. If not specified, the limit will "
       "not be set."},
  {
    OPTION_MODEL_NAMESPACING, "model-namespacing", Option::ArgBool,
        "Whether model namespacing is enable or not. If true, models with the "
        "same name can be served if they are in different namespace."
  }
};

void
TritonServerParameters::CheckPortCollision()
{
  // [FIXME] try to make this function endpoint type agnostic
  // List of enabled services and their constraints
  std::vector<
      std::tuple<std::string, std::string, int32_t, bool, int32_t, int32_t>>
      ports;
#ifdef TRITON_ENABLE_HTTP
  if (allow_http_) {
    ports.emplace_back("HTTP", http_address_, http_port_, false, -1, -1);
  }
#endif  // TRITON_ENABLE_HTTP
#ifdef TRITON_ENABLE_GRPC
  if (allow_grpc_) {
    ports.emplace_back(
        "GRPC", grpc_options_.socket_.address_, grpc_options_.socket_.port_,
        false, -1, -1);
  }
#endif  // TRITON_ENABLE_GRPC
#ifdef TRITON_ENABLE_METRICS
  if (allow_metrics_) {
    ports.emplace_back(
        "metrics", metrics_address_, metrics_port_, false, -1, -1);
  }
#endif  // TRITON_ENABLE_METRICS
#ifdef TRITON_ENABLE_SAGEMAKER
  if (allow_sagemaker_) {
    ports.emplace_back(
        "SageMaker", sagemaker_address_, sagemaker_port_,
        sagemaker_safe_range_set_, sagemaker_safe_range_.first,
        sagemaker_safe_range_.second);
  }
#endif  // TRITON_ENABLE_SAGEMAKER
#ifdef TRITON_ENABLE_VERTEX_AI
  if (allow_vertex_ai_) {
    ports.emplace_back(
        "Vertex AI", vertex_ai_address_, vertex_ai_port_, false, -1, -1);
  }
#endif  // TRITON_ENABLE_VERTEX_AI

  for (auto curr_it = ports.begin(); curr_it != ports.end(); ++curr_it) {
    // If the current service doesn't specify the allow port range for other
    // services, then we don't need to revisit the checked services
    auto comparing_it = (std::get<3>(*curr_it)) ? ports.begin() : (curr_it + 1);
    for (; comparing_it != ports.end(); ++comparing_it) {
      if (comparing_it == curr_it) {
        continue;
      }
      if (std::get<1>(*curr_it) != std::get<1>(*comparing_it)) {
        continue;
      }
      // Set range and comparing service port is out of range
      if (std::get<3>(*curr_it) &&
          ((std::get<2>(*comparing_it) < std::get<4>(*curr_it)) ||
           (std::get<2>(*comparing_it) > std::get<5>(*curr_it)))) {
        std::stringstream ss;
        ss << "The server cannot listen to " << std::get<0>(*comparing_it)
           << " requests at port " << std::get<2>(*comparing_it)
           << ", allowed port range is [" << std::get<4>(*curr_it) << ", "
           << std::get<5>(*curr_it) << "]" << std::endl;
        throw ParseException(ss.str());
      }
      if (std::get<2>(*curr_it) == std::get<2>(*comparing_it)) {
        std::stringstream ss;
        ss << "The server cannot listen to " << std::get<0>(*curr_it)
           << " requests "
           << "and " << std::get<0>(*comparing_it)
           << " requests at the same address and port " << std::get<1>(*curr_it)
           << ":" << std::get<2>(*curr_it) << std::endl;
        throw ParseException(ss.str());
      }
    }
  }
}

TritonServerParameters::ManagedTritonServerOptionPtr
TritonServerParameters::BuildTritonServerOptions()
{
  TRITONSERVER_ServerOptions* loptions = nullptr;
  THROW_IF_ERR(
      ParseException, TRITONSERVER_ServerOptionsNew(&loptions),
      "creating server options");
  ManagedTritonServerOptionPtr managed_ptr(
      loptions, TRITONSERVER_ServerOptionsDelete);
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetServerId(loptions, server_id_.c_str()),
      "setting server ID");
  for (const auto& model_repository_path : model_repository_paths_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetModelRepositoryPath(
            loptions, model_repository_path.c_str()),
        "setting model repository path");
  }
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetModelControlMode(loptions, control_mode_),
      "setting model control mode");
  for (const auto& model : startup_models_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetStartupModel(loptions, model.c_str()),
        "setting startup model");
  }
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetRateLimiterMode(loptions, rate_limit_mode_),
      "setting rate limiter configuration");
  for (const auto& resource : rate_limit_resources_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsAddRateLimiterResource(
            loptions, std::get<0>(resource).c_str(), std::get<1>(resource),
            std::get<2>(resource)),
        "setting rate limiter resource");
  }
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetPinnedMemoryPoolByteSize(
          loptions, pinned_memory_pool_byte_size_),
      "setting total pinned memory byte size");
  for (const auto& cuda_pool : cuda_pools_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetCudaMemoryPoolByteSize(
            loptions, cuda_pool.first, cuda_pool.second),
        "setting total CUDA memory byte size");
  }
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetMinSupportedComputeCapability(
          loptions, min_supported_compute_capability_),
      "setting minimum supported CUDA compute capability");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetExitOnError(loptions, exit_on_error_),
      "setting exit on error");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetStrictModelConfig(
          loptions, strict_model_config_),
      "setting strict model configuration");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetStrictReadiness(loptions, strict_readiness_),
      "setting strict readiness");
  // [FIXME] std::max seems to be part of Parse()
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetExitTimeout(
          loptions, std::max(0, exit_timeout_secs_)),
      "setting exit timeout");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetBufferManagerThreadCount(
          loptions, std::max(0, buffer_manager_thread_count_)),
      "setting buffer manager thread count");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetModelLoadThreadCount(
          loptions, std::max(1u, model_load_thread_count_)),
      "setting model load thread count");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetModelNamespacing(
          loptions, enable_model_namespacing_),
      "setting model namespacing");

#ifdef TRITON_ENABLE_LOGGING
  TRITONSERVER_ServerOptionsSetLogFile(loptions, log_file_.c_str());
  THROW_IF_ERR(
      ParseException, TRITONSERVER_ServerOptionsSetLogInfo(loptions, log_info_),
      "setting log info enable");
  THROW_IF_ERR(
      ParseException, TRITONSERVER_ServerOptionsSetLogWarn(loptions, log_warn_),
      "setting log warn enable");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetLogError(loptions, log_error_),
      "setting log error enable");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetLogVerbose(loptions, log_verbose_),
      "setting log verbose level");
  switch (log_format_) {
    case triton::common::Logger::Format::kDEFAULT:
      THROW_IF_ERR(
          ParseException,
          TRITONSERVER_ServerOptionsSetLogFormat(
              loptions, TRITONSERVER_LOG_DEFAULT),
          "setting log format");
      break;
    case triton::common::Logger::Format::kISO8601:
      THROW_IF_ERR(
          ParseException,
          TRITONSERVER_ServerOptionsSetLogFormat(
              loptions, TRITONSERVER_LOG_ISO8601),
          "setting log format");
      break;
  }
#endif  // TRITON_ENABLE_LOGGING

#ifdef TRITON_ENABLE_METRICS
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetMetrics(loptions, allow_metrics_),
      "setting metrics enable");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetGpuMetrics(loptions, allow_gpu_metrics_),
      "setting GPU metrics enable");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetCpuMetrics(loptions, allow_cpu_metrics_),
      "setting CPU metrics enable");
  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetMetricsInterval(
          loptions, metrics_interval_ms_),
      "setting metrics interval");
#endif  // TRITON_ENABLE_METRICS

  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetBackendDirectory(
          loptions, backend_dir_.c_str()),
      "setting backend directory");

  // Enable cache and configure it if a cache CLI arg is passed,
  // this will allow for an empty configuration.
  if (enable_cache_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetCacheDirectory(
            loptions, cache_dir_.c_str()),
        "setting cache directory");

    for (const auto& cache_pair : cache_config_settings_) {
      const auto& cache_name = cache_pair.first;
      const auto& settings = cache_pair.second;
      const auto& json_config_str = PairsToJsonStr(settings);
      THROW_IF_ERR(
          ParseException,
          TRITONSERVER_ServerOptionsSetCacheConfig(
              loptions, cache_name.c_str(), json_config_str.c_str()),
          "setting cache configurtion");
    }
  }

  THROW_IF_ERR(
      ParseException,
      TRITONSERVER_ServerOptionsSetRepoAgentDirectory(
          loptions, repoagent_dir_.c_str()),
      "setting repository agent directory");
  for (const auto& bcs : backend_config_settings_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetBackendConfig(
            loptions, std::get<0>(bcs).c_str(), std::get<1>(bcs).c_str(),
            std::get<2>(bcs).c_str()),
        "setting backend configurtion");
  }
  for (const auto& limit : load_gpu_limit_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetModelLoadDeviceLimit(
            loptions, TRITONSERVER_INSTANCEGROUPKIND_GPU, limit.first,
            limit.second),
        "setting model load GPU limit");
  }
  for (const auto& hp : host_policies_) {
    THROW_IF_ERR(
        ParseException,
        TRITONSERVER_ServerOptionsSetHostPolicy(
            loptions, std::get<0>(hp).c_str(), std::get<1>(hp).c_str(),
            std::get<2>(hp).c_str()),
        "setting host policy");
  }
  return managed_ptr;
}

std::pair<TritonServerParameters, std::vector<char*>>
TritonParser::Parse(int argc, char** argv)
{
  //
  // Step 1. Before parsing setup
  //
  TritonServerParameters lparams;
  bool strict_model_config_present{false};
  bool disable_auto_complete_config{false};
  bool cache_size_present{false};
  bool cache_config_present{false};
#ifdef TRITON_ENABLE_TRACING
  bool explicit_disable_trace{false};
#endif  // TRITON_ENABLE_TRACING

#ifdef TRITON_ENABLE_GRPC
  triton::server::grpc::Options& lgrpc_options = lparams.grpc_options_;
#endif  // TRITON_ENABLE_GRPC

#ifdef TRITON_ENABLE_VERTEX_AI
  // Set different default value if specific flag is set
  {
    auto aip_mode =
        triton::server::GetEnvironmentVariableOrDefault("AIP_MODE", "");
    // Enable Vertex AI service and disable HTTP / GRPC service by default
    // if detecting Vertex AI environment
    if (aip_mode == "PREDICTION") {
      lparams.allow_vertex_ai_ = true;
#ifdef TRITON_ENABLE_HTTP
      lparams.allow_http_ = false;
#endif  // TRITON_ENABLE_HTTP
#ifdef TRITON_ENABLE_GRPC
      lparams.allow_grpc_ = false;
#endif  // TRITON_ENABLE_GRPC
    }
    auto port = triton::server::GetEnvironmentVariableOrDefault(
        "AIP_HTTP_PORT", "8080");
    lparams.vertex_ai_port_ = ParseOption<int>(port);
  }
#endif  // TRITON_ENABLE_VERTEX_AI

  //
  // Step 2. parse options
  //
  std::vector<struct option> long_options;
  for (const auto& o : recognized_options_) {
    long_options.push_back(o.GetLongOption());
  }
  long_options.push_back({nullptr, 0, nullptr, 0});

  int flag;
  while ((flag = getopt_long(argc, argv, "", &long_options[0], NULL)) != -1) {
    switch (flag) {
      case OPTION_HELP:
        // [FIXME] how help is printed?
      case '?':
        // [FIXME] fall through when seeing this, currently consumes all options
        // [FIXME] disable stderr output of `getopt_long`
        throw ParseException();
#ifdef TRITON_ENABLE_LOGGING
      case OPTION_LOG_VERBOSE:
        lparams.log_verbose_ = ParseIntBoolOption(optarg);
        break;
      case OPTION_LOG_INFO:
        lparams.log_info_ = ParseOption<bool>(optarg);
        break;
      case OPTION_LOG_WARNING:
        lparams.log_warn_ = ParseOption<bool>(optarg);
        break;
      case OPTION_LOG_ERROR:
        lparams.log_error_ = ParseOption<bool>(optarg);
        break;
      case OPTION_LOG_FORMAT: {
        std::string format_str(optarg);
        if (format_str == "default") {
          lparams.log_format_ = triton::common::Logger::Format::kDEFAULT;
        } else if (format_str == "ISO8601") {
          lparams.log_format_ = triton::common::Logger::Format::kISO8601;
        } else {
          throw ParseException("invalid argument for --log-format");
        }
        break;
      }
      case OPTION_LOG_FILE:
        lparams.log_file_ = optarg;
        break;
#endif  // TRITON_ENABLE_LOGGING

      case OPTION_ID:
        lparams.server_id_ = optarg;
        break;
      case OPTION_MODEL_REPOSITORY:
        lparams.model_repository_paths_.insert(optarg);
        break;
      case OPTION_EXIT_ON_ERROR:
        lparams.exit_on_error_ = ParseOption<bool>(optarg);
        break;
      case OPTION_DISABLE_AUTO_COMPLETE_CONFIG:
        disable_auto_complete_config = true;
        break;
      case OPTION_STRICT_MODEL_CONFIG:
        std::cerr << "Warning: '--strict-model-config' has been deprecated! "
                     "Please use '--disable-auto-complete-config' instead."
                  << std::endl;
        strict_model_config_present = true;
        lparams.strict_model_config_ = ParseOption<bool>(optarg);
        break;
      case OPTION_STRICT_READINESS:
        lparams.strict_readiness_ = ParseOption<bool>(optarg);
        break;

#ifdef TRITON_ENABLE_HTTP
      case OPTION_ALLOW_HTTP:
        lparams.allow_http_ = ParseOption<bool>(optarg);
        break;
      case OPTION_HTTP_PORT:
        lparams.http_port_ = ParseOption<int>(optarg);
        break;
      case OPTION_REUSE_HTTP_PORT:
        lparams.reuse_http_port_ = ParseOption<int>(optarg);
        break;
      case OPTION_HTTP_ADDRESS:
        lparams.http_address_ = optarg;
#ifdef TRITON_ENABLE_METRICS
        lparams.metrics_address_ = optarg;
#endif  // TRITON_ENABLE_METRICS
        break;
      case OPTION_HTTP_THREAD_COUNT:
        lparams.http_thread_cnt_ = ParseOption<int>(optarg);
        break;
#endif  // TRITON_ENABLE_HTTP

#ifdef TRITON_ENABLE_SAGEMAKER
      case OPTION_ALLOW_SAGEMAKER:
        lparams.allow_sagemaker_ = ParseOption<bool>(optarg);
        break;
      case OPTION_SAGEMAKER_PORT:
        lparams.sagemaker_port_ = ParseOption<int>(optarg);
        break;
      case OPTION_SAGEMAKER_SAFE_PORT_RANGE:
        lparams.sagemaker_safe_range_set_ = true;
        lparams.sagemaker_safe_range_ = ParsePairOption<int, int>(optarg, "-");
        break;
      case OPTION_SAGEMAKER_THREAD_COUNT:
        lparams.sagemaker_thread_cnt_ = ParseOption<int>(optarg);
        break;
#endif  // TRITON_ENABLE_SAGEMAKER

#ifdef TRITON_ENABLE_VERTEX_AI
      case OPTION_ALLOW_VERTEX_AI:
        lparams.allow_vertex_ai_ = ParseOption<bool>(optarg);
        break;
      case OPTION_VERTEX_AI_PORT:
        lparams.vertex_ai_port_ = ParseOption<int>(optarg);
        break;
      case OPTION_VERTEX_AI_THREAD_COUNT:
        lparams.vertex_ai_thread_cnt_ = ParseOption<int>(optarg);
        break;
      case OPTION_VERTEX_AI_DEFAULT_MODEL:
        lparams.vertex_ai_default_model_ = optarg;
        break;
#endif  // TRITON_ENABLE_VERTEX_AI

#ifdef TRITON_ENABLE_GRPC
      case OPTION_ALLOW_GRPC:
        lparams.allow_grpc_ = ParseOption<bool>(optarg);
        break;
      case OPTION_GRPC_PORT:
        lgrpc_options.socket_.port_ = ParseOption<int>(optarg);
        break;
      case OPTION_REUSE_GRPC_PORT:
        lgrpc_options.socket_.reuse_port_ = ParseOption<int>(optarg);
        break;
      case OPTION_GRPC_ADDRESS:
        lgrpc_options.socket_.address_ = optarg;
        break;
      case OPTION_GRPC_INFER_ALLOCATION_POOL_SIZE:
        lgrpc_options.infer_allocation_pool_size_ = ParseOption<int>(optarg);
        break;
      case OPTION_GRPC_USE_SSL:
        lgrpc_options.ssl_.use_ssl_ = ParseOption<bool>(optarg);
        break;
      case OPTION_GRPC_USE_SSL_MUTUAL:
        lgrpc_options.ssl_.use_mutual_auth_ = ParseOption<bool>(optarg);
        lgrpc_options.ssl_.use_ssl_ = true;
        break;
      case OPTION_GRPC_SERVER_CERT:
        lgrpc_options.ssl_.server_cert_ = optarg;
        break;
      case OPTION_GRPC_SERVER_KEY:
        lgrpc_options.ssl_.server_key_ = optarg;
        break;
      case OPTION_GRPC_ROOT_CERT:
        lgrpc_options.ssl_.root_cert_ = optarg;
        break;
      case OPTION_GRPC_RESPONSE_COMPRESSION_LEVEL: {
        std::string mode_str(optarg);
        std::transform(
            mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
        if (mode_str == "none") {
          lgrpc_options.infer_compression_level_ = GRPC_COMPRESS_LEVEL_NONE;
        } else if (mode_str == "low") {
          lgrpc_options.infer_compression_level_ = GRPC_COMPRESS_LEVEL_LOW;
        } else if (mode_str == "medium") {
          lgrpc_options.infer_compression_level_ = GRPC_COMPRESS_LEVEL_MED;
        } else if (mode_str == "high") {
          lgrpc_options.infer_compression_level_ = GRPC_COMPRESS_LEVEL_HIGH;
        } else {
          throw ParseException(
              "invalid argument for --grpc_infer_response_compression_level");
        }
        break;
      }
      case OPTION_GRPC_ARG_KEEPALIVE_TIME_MS:
        lgrpc_options.keep_alive_.keepalive_time_ms_ = ParseOption<int>(optarg);
        break;
      case OPTION_GRPC_ARG_KEEPALIVE_TIMEOUT_MS:
        lgrpc_options.keep_alive_.keepalive_timeout_ms_ =
            ParseOption<int>(optarg);
        break;
      case OPTION_GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS:
        lgrpc_options.keep_alive_.keepalive_permit_without_calls_ =
            ParseOption<bool>(optarg);
        break;
      case OPTION_GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA:
        lgrpc_options.keep_alive_.http2_max_pings_without_data_ =
            ParseOption<int>(optarg);
        break;
      case OPTION_GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS:
        lgrpc_options.keep_alive_
            .http2_min_recv_ping_interval_without_data_ms_ =
            ParseOption<int>(optarg);
        break;
      case OPTION_GRPC_ARG_HTTP2_MAX_PING_STRIKES:
        lgrpc_options.keep_alive_.http2_max_ping_strikes_ =
            ParseOption<int>(optarg);
        break;
#endif  // TRITON_ENABLE_GRPC

#ifdef TRITON_ENABLE_METRICS
      case OPTION_ALLOW_METRICS:
        lparams.allow_metrics_ = ParseOption<bool>(optarg);
        break;
      case OPTION_ALLOW_GPU_METRICS:
        lparams.allow_gpu_metrics_ = ParseOption<bool>(optarg);
        break;
      case OPTION_ALLOW_CPU_METRICS:
        lparams.allow_cpu_metrics_ = ParseOption<bool>(optarg);
        break;
      case OPTION_METRICS_PORT:
        lparams.metrics_port_ = ParseOption<int>(optarg);
        break;
      case OPTION_METRICS_INTERVAL_MS:
        lparams.metrics_interval_ms_ = ParseOption<int>(optarg);
        break;
#endif  // TRITON_ENABLE_METRICS

#ifdef TRITON_ENABLE_TRACING
      case OPTION_TRACE_FILEPATH:
        lparams.trace_filepath_ = optarg;
        break;
      case OPTION_TRACE_LEVEL: {
        auto parsed_level = ParseTraceLevelOption(optarg);
        explicit_disable_trace |=
            (parsed_level == TRITONSERVER_TRACE_LEVEL_DISABLED);
        lparams.trace_level_ = static_cast<TRITONSERVER_InferenceTraceLevel>(
            lparams.trace_level_ | parsed_level);
        break;
      }
      case OPTION_TRACE_RATE:
        lparams.trace_rate_ = ParseOption<int>(optarg);
        break;
      case OPTION_TRACE_COUNT:
        lparams.trace_count_ = ParseOption<int>(optarg);
        break;
      case OPTION_TRACE_LOG_FREQUENCY:
        lparams.trace_log_frequency_ = ParseOption<int>(optarg);
        break;
#endif  // TRITON_ENABLE_TRACING

      case OPTION_POLL_REPO_SECS:
        lparams.repository_poll_secs_ = ParseOption<int>(optarg);
        break;
      case OPTION_STARTUP_MODEL:
        lparams.startup_models_.insert(optarg);
        break;
      case OPTION_MODEL_CONTROL_MODE: {
        std::string mode_str(optarg);
        std::transform(
            mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
        if (mode_str == "none") {
          lparams.control_mode_ = TRITONSERVER_MODEL_CONTROL_NONE;
        } else if (mode_str == "poll") {
          lparams.control_mode_ = TRITONSERVER_MODEL_CONTROL_POLL;
        } else if (mode_str == "explicit") {
          lparams.control_mode_ = TRITONSERVER_MODEL_CONTROL_EXPLICIT;
        } else {
          throw ParseException("invalid argument for --model-control-mode");
        }
        break;
      }
      case OPTION_RATE_LIMIT: {
        std::string rate_limit_str(optarg);
        std::transform(
            rate_limit_str.begin(), rate_limit_str.end(),
            rate_limit_str.begin(), ::tolower);
        if (rate_limit_str == "execution_count") {
          lparams.rate_limit_mode_ = TRITONSERVER_RATE_LIMIT_EXEC_COUNT;
        } else if (rate_limit_str == "off") {
          lparams.rate_limit_mode_ = TRITONSERVER_RATE_LIMIT_OFF;
        } else {
          throw ParseException("invalid argument for --rate-limit");
        }
        break;
      }
      case OPTION_RATE_LIMIT_RESOURCE: {
        std::string rate_limit_resource_str(optarg);
        std::transform(
            rate_limit_resource_str.begin(), rate_limit_resource_str.end(),
            rate_limit_resource_str.begin(), ::tolower);
        // [FIXME] directly throw ParseException from parse helper
        try {
          lparams.rate_limit_resources_.push_back(
              ParseRateLimiterResourceOption(optarg));
        }
        catch (const std::invalid_argument& ia) {
          throw ParseException(
              std::string("failed to parse '") + optarg +
              "' as <str>:<int>:<int>");
        }
        break;
      }
      case OPTION_PINNED_MEMORY_POOL_BYTE_SIZE:
        lparams.pinned_memory_pool_byte_size_ = ParseOption<int64_t>(optarg);
        break;
      case OPTION_CUDA_MEMORY_POOL_BYTE_SIZE:
        lparams.cuda_pools_.push_back(
            ParsePairOption<int, uint64_t>(optarg, ":"));
        break;
      case OPTION_RESPONSE_CACHE_BYTE_SIZE: {
        cache_size_present = true;
        const auto byte_size = std::to_string(ParseOption<int64_t>(optarg));
        lparams.cache_config_settings_["local"] = {{"size", byte_size}};
        std::cerr
            << "Warning: '--response-cache-byte-size' has been deprecated! "
               "This will default to the 'local' cache implementation with "
               "the provided byte size for its config. Please use "
               "'--cache-config' instead. The equivalent "
               "--cache-config CLI args would be: "
               "'--cache-config=local,size=" +
                   byte_size + "'"
            << std::endl;
        break;
      }
      case OPTION_CACHE_CONFIG: {
        cache_config_present = true;
        const auto cache_setting = ParseCacheConfigOption(optarg);
        const auto& cache_name = std::get<0>(cache_setting);
        const auto& key = std::get<1>(cache_setting);
        const auto& value = std::get<2>(cache_setting);
        lparams.cache_config_settings_[cache_name].push_back({key, value});
        break;
      }
      case OPTION_CACHE_DIR:
        lparams.cache_dir_ = optarg;
        break;
      case OPTION_MIN_SUPPORTED_COMPUTE_CAPABILITY:
        lparams.min_supported_compute_capability_ = ParseOption<double>(optarg);
        break;
      case OPTION_EXIT_TIMEOUT_SECS:
        lparams.exit_timeout_secs_ = ParseOption<int>(optarg);
        break;
      case OPTION_BACKEND_DIR:
        lparams.backend_dir_ = optarg;
        break;
      case OPTION_REPOAGENT_DIR:
        lparams.repoagent_dir_ = optarg;
        break;
      case OPTION_BUFFER_MANAGER_THREAD_COUNT:
        lparams.buffer_manager_thread_count_ = ParseOption<int>(optarg);
        break;
      case OPTION_MODEL_LOAD_THREAD_COUNT:
        lparams.model_load_thread_count_ = ParseOption<int>(optarg);
        break;
      case OPTION_BACKEND_CONFIG:
        lparams.backend_config_settings_.push_back(
            ParseBackendConfigOption(optarg));
        break;
      case OPTION_HOST_POLICY:
        lparams.host_policies_.push_back(ParseHostPolicyOption(optarg));
        break;
      case OPTION_MODEL_LOAD_GPU_LIMIT:
        lparams.load_gpu_limit_.emplace(
            ParsePairOption<int, double>(optarg, ":"));
        break;
      case OPTION_MODEL_NAMESPACING:
        lparams.enable_model_namespacing_ = ParseOption<bool>(optarg);
        break;
    }
  }

  if (optind < argc) {
    throw ParseException(std::string("Unexpected argument: ") + argv[optind]);
  }

  //
  // Step 3. Post parsing validation, usually for options that depend on the
  // others which are not determined until after parsing.
  //

  // [FIXME] check at parsing?
  if (lparams.control_mode_ != TRITONSERVER_MODEL_CONTROL_POLL) {
    lparams.repository_poll_secs_ = 0;
  }

#ifdef TRITON_ENABLE_VERTEX_AI
  // Set default model repository if specific flag is set, postpone the
  // check to after parsing so we only monitor the default repository if
  // Vertex service is allowed
  if (lparams.model_repository_paths_.empty()) {
    auto aip_storage_uri =
        triton::server::GetEnvironmentVariableOrDefault("AIP_STORAGE_URI", "");
    if (!aip_storage_uri.empty()) {
      lparams.model_repository_paths_.insert(aip_storage_uri);
    }
  }
#endif  // TRITON_ENABLE_VERTEX_AI

#ifdef TRITON_ENABLE_METRICS
  lparams.allow_gpu_metrics_ &= lparams.allow_metrics_;
  lparams.allow_cpu_metrics_ &= lparams.allow_metrics_;
#endif  // TRITON_ENABLE_METRICS

#ifdef TRITON_ENABLE_TRACING
  if (explicit_disable_trace) {
    lparams.trace_level_ = TRITONSERVER_TRACE_LEVEL_DISABLED;
  }
#endif  // TRITON_ENABLE_TRACING

  // Check if there is a conflict between --disable-auto-complete-config
  // and --strict-model-config
  if (disable_auto_complete_config) {
    if (strict_model_config_present && !lparams.strict_model_config_) {
      std::cerr
          << "Warning: Overriding deprecated '--strict-model-config' from "
             "False to True in favor of '--disable-auto-complete-config'!"
          << std::endl;
    }
    lparams.strict_model_config_ = true;
  }

  // Check if there is a conflict between --response-cache-byte-size
  // and --cache-config
  if (cache_size_present && cache_config_present) {
    throw ParseException(
        "Error: Incompatible flags --response-cache-byte-size and "
        "--cache-config both provided. Please provide one or the other.");
  }
  lparams.enable_cache_ = (cache_size_present || cache_config_present);
  return {lparams, {}};
}

std::string
TritonParser::FormatUsageMessage(std::string str, int offset)
{
  int width = 60;
  int current_pos = offset;
  while (current_pos + width < int(str.length())) {
    int n = str.rfind(' ', current_pos + width);
    if (n != int(std::string::npos)) {
      str.replace(n, 1, "\n\t");
      current_pos += (width + 9);
    }
  }

  return str;
}

std::string
TritonParser::Usage()
{
  std::stringstream ss;
  for (const auto& o : recognized_options_) {
    if (!o.arg_desc_.empty()) {
      ss << "  --" << o.flag_ << " <" << o.arg_desc_ << ">" << std::endl
         << "\t" << FormatUsageMessage(o.desc_, 0) << std::endl;
    } else {
      ss << "  --" << o.flag_ << std::endl
         << "\t" << FormatUsageMessage(o.desc_, 0) << std::endl;
    }
  }
  return ss.str();
}

std::tuple<std::string, std::string, std::string>
TritonParser::ParseCacheConfigOption(const std::string& arg)
{
  // Format is "<cache_name>,<setting>=<value>" for specific
  // config/settings and "<setting>=<value>" for cache agnostic
  // configs/settings
  int delim_name = arg.find(",");
  int delim_setting = arg.find("=", delim_name + 1);

  std::string name_string = std::string();
  if (delim_name > 0) {
    name_string = arg.substr(0, delim_name);
  }
  // No cache-agnostic global settings are currently supported
  else {
    std::stringstream ss;
    ss << "No cache specified. --cache-config option format is "
       << "<cache name>,<setting>=<value>. Got " << arg << std::endl;
    throw ParseException(ss.str());
  }

  if (delim_setting < 0) {
    std::stringstream ss;
    ss << "--cache-config option format is '<cache "
          "name>,<setting>=<value>'. Got "
       << arg << std::endl;
    throw ParseException(ss.str());
  }
  std::string setting_string =
      arg.substr(delim_name + 1, delim_setting - delim_name - 1);
  std::string value_string = arg.substr(delim_setting + 1);

  if (setting_string.empty() || value_string.empty()) {
    std::stringstream ss;
    ss << "--cache-config option format is '<cache "
          "name>,<setting>=<value>'. Got "
       << arg << std::endl;
    throw ParseException(ss.str());
  }

  return {name_string, setting_string, value_string};
}

std::tuple<std::string, int, int>
TritonParser::ParseRateLimiterResourceOption(const std::string& arg)
{
  std::string error_string(
      "--rate-limit-resource option format is "
      "'<resource_name>:<count>:<device>' or '<resource_name>:<count>'. Got " +
      arg);

  std::string name_string("");
  int count = -1;
  int device_id = -1;

  size_t delim_first = arg.find(":");
  size_t delim_second = arg.find(":", delim_first + 1);

  if (delim_second != std::string::npos) {
    // Handle format `<resource_name>:<count>:<device>'
    size_t delim_third = arg.find(":", delim_second + 1);
    if (delim_third != std::string::npos) {
      throw ParseException(error_string);
    }
    name_string = arg.substr(0, delim_first);
    count = ParseOption<int>(
        arg.substr(delim_first + 1, delim_second - delim_first - 1));
    device_id = ParseOption<int>(arg.substr(delim_second + 1));
  } else if (delim_first != std::string::npos) {
    // Handle format `<resource_name>:<count>'
    name_string = arg.substr(0, delim_first);
    count = ParseOption<int>(arg.substr(delim_first + 1));
  } else {
    // If no colons found
    throw ParseException(error_string);
  }

  return {name_string, count, device_id};
}

std::tuple<std::string, std::string, std::string>
TritonParser::ParseBackendConfigOption(const std::string& arg)
{
  // Format is "<backend_name>,<setting>=<value>" for specific
  // config/settings and "<setting>=<value>" for backend agnostic
  // configs/settings
  int delim_name = arg.find(",");
  int delim_setting = arg.find("=", delim_name + 1);

  std::string name_string = std::string();
  if (delim_name > 0) {
    name_string = arg.substr(0, delim_name);
  } else if (delim_name == 0) {
    std::stringstream ss;
    ss << "No backend specified. --backend-config option format is "
       << "<backend name>,<setting>=<value> or "
       << "<setting>=<value>. Got " << arg << std::endl;
    throw ParseException(ss.str());
  }  // else global backend config

  if (delim_setting < 0) {
    std::stringstream ss;
    ss << "--backend-config option format is '<backend "
          "name>,<setting>=<value>'. Got "
       << arg << std::endl;
    throw ParseException(ss.str());
  }
  std::string setting_string =
      arg.substr(delim_name + 1, delim_setting - delim_name - 1);
  std::string value_string = arg.substr(delim_setting + 1);

  if (setting_string.empty() || value_string.empty()) {
    std::stringstream ss;
    ss << "--backend-config option format is '<backend "
          "name>,<setting>=<value>'. Got "
       << arg << std::endl;
    throw ParseException(ss.str());
  }

  return {name_string, setting_string, value_string};
}

std::tuple<std::string, std::string, std::string>
TritonParser::ParseHostPolicyOption(const std::string& arg)
{
  // Format is "<policy_name>,<setting>=<value>"
  int delim_name = arg.find(",");
  int delim_setting = arg.find("=", delim_name + 1);

  // Check for 2 semicolons
  if ((delim_name < 0) || (delim_setting < 0)) {
    std::stringstream ss;
    ss << "--host-policy option format is '<policy "
          "name>,<setting>=<value>'. Got "
       << arg << std::endl;
    throw ParseException(ss.str());
  }

  std::string name_string = arg.substr(0, delim_name);
  std::string setting_string =
      arg.substr(delim_name + 1, delim_setting - delim_name - 1);
  std::string value_string = arg.substr(delim_setting + 1);

  if (name_string.empty() || setting_string.empty() || value_string.empty()) {
    std::stringstream ss;
    ss << "--host-policy option format is '<policy "
          "name>,<setting>=<value>'. Got "
       << arg << std::endl;
    throw ParseException(ss.str());
  }

  return {name_string, setting_string, value_string};
}

#ifdef TRITON_ENABLE_TRACING
TRITONSERVER_InferenceTraceLevel
TritonParser::ParseTraceLevelOption(std::string arg)
{
  std::transform(arg.begin(), arg.end(), arg.begin(), [](unsigned char c) {
    return std::tolower(c);
  });

  if ((arg == "false") || (arg == "off")) {
    return TRITONSERVER_TRACE_LEVEL_DISABLED;
  }
  if ((arg == "true") || (arg == "on") || (arg == "min") || (arg == "max") ||
      (arg == "timestamps")) {
    return TRITONSERVER_TRACE_LEVEL_TIMESTAMPS;
  }
  if (arg == "tensors") {
    return TRITONSERVER_TRACE_LEVEL_TENSORS;
  }

  throw ParseException("invalid value for trace level option: " + arg);
}
#endif  // TRITON_ENABLE_TRACING

}}  // namespace triton::server