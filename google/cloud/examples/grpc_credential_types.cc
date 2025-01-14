// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/iam/iam_credentials_client.h"
#include "google/cloud/spanner/instance_admin_client.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/log.h"
#include "google/cloud/testing_util/example_driver.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"  // NOLINT(modernize-deprecated-headers)
#include <curl/curl.h>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {

auto constexpr kTokenValidationPeriod = std::chrono::seconds(30);

extern "C" size_t CurlOnWriteData(char* ptr, size_t size, size_t nmemb,
                                  void* userdata) {
  auto* buffer = reinterpret_cast<std::string*>(userdata);
  buffer->append(ptr, size * nmemb);
  return size * nmemb;
}

google::cloud::StatusOr<std::string> HttpGet(std::string const& url,
                                             std::string const& token) {
  static auto const kCurlInit = [] {
    return curl_global_init(CURL_GLOBAL_ALL);
  }();
  (void)kCurlInit;
  auto const authorization = "Authorization: Bearer " + token;
  using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
  auto const headers = Headers{
      curl_slist_append(nullptr, authorization.c_str()), curl_slist_free_all};
  using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
  auto curl = CurlHandle(curl_easy_init(), curl_easy_cleanup);
  if (!curl) throw std::runtime_error("Failed to create CurlHandle");
  std::string buffer;
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &CurlOnWriteData);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);

  CURLcode code = curl_easy_perform(curl.get());
  if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));
  long status;  // NOLINT(google-runtime-int)
  code = curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  if (code != CURLE_OK) throw std::runtime_error(curl_easy_strerror(code));

  // Handle common errors as Status, this is not exhaustive.
  using ::google::cloud::Status;
  using ::google::cloud::StatusCode;
  if (status == 400) return Status(StatusCode::kInvalidArgument, buffer);
  if (status == 401) return Status(StatusCode::kUnauthenticated, buffer);
  if (status == 403) return Status(StatusCode::kPermissionDenied, buffer);
  if (status >= 500) return Status(StatusCode::kInternal, buffer);
  if (status < 200 || status >= 300) {
    std::ostringstream os;
    os << "HTTP error [" << status << "]: " << buffer;
    return Status(StatusCode::kUnknown, buffer);
  }
  return buffer;
}

// TODO(#6185) - this should be done by the generated code
std::set<std::string> DefaultTracingComponents() {
  return absl::StrSplit(
      google::cloud::internal::GetEnv("GOOGLE_CLOUD_CPP_ENABLE_TRACING")
          .value_or(""),
      ',');
}

google::iam::credentials::v1::GenerateAccessTokenResponse UseAccessToken(
    google::cloud::iam::IAMCredentialsClient client,
    std::vector<std::string> const& argv) {
  namespace iam = google::cloud::iam;
  return [](iam::IAMCredentialsClient client,
            std::string const& service_account, std::string const& project_id) {
    google::protobuf::Duration duration;
    duration.set_seconds(
        std::chrono::seconds(2 * kTokenValidationPeriod).count());
    auto token = client.GenerateAccessToken(
        "projects/-/serviceAccounts/" + service_account, /*delegates=*/{},
        /*scope=*/{"https://www.googleapis.com/auth/cloud-platform"}, duration);
    if (!token) throw std::runtime_error(token.status().message());

    namespace spanner = google::cloud::spanner;
    auto credentials = grpc::CompositeChannelCredentials(
        grpc::SslCredentials({}),
        grpc::AccessTokenCredentials(token->access_token()));

    spanner::InstanceAdminClient admin(spanner::MakeInstanceAdminConnection(
        google::cloud::Options{}.set<google::cloud::GrpcCredentialOption>(
            credentials)));
    for (auto instance : admin.ListInstances(project_id, /*filter=*/{})) {
      if (!instance) throw std::runtime_error(instance.status().message());
      std::cout << "Instance: " << instance->name() << "\n";
    }

    return *std::move(token);
  }(std::move(client), argv.at(0), argv.at(1));
}

void UseAccessTokenUntilExpired(google::cloud::iam::IAMCredentialsClient client,
                                std::vector<std::string> const& argv) {
  auto token = UseAccessToken(std::move(client), argv);
  auto const project_id = argv.at(1);
  auto const expiration =
      std::chrono::system_clock::from_time_t(token.expire_time().seconds());
  auto const deadline = expiration + 4 * kTokenValidationPeriod;
  std::cout << "Running until " << absl::FromChrono(deadline)
            << ". This is past the access token expiration time ("
            << absl::FromChrono(expiration) << ")\n";

  auto iteration = [=](bool expired) {
    namespace spanner = google::cloud::spanner;
    auto credentials = grpc::CompositeChannelCredentials(
        grpc::SslCredentials({}),
        grpc::AccessTokenCredentials(token.access_token()));
    spanner::InstanceAdminClient admin(spanner::MakeInstanceAdminConnection(
        google::cloud::Options{}.set<google::cloud::GrpcCredentialOption>(
            credentials)));
    for (auto instance : admin.ListInstances(project_id, /*filter=*/{})) {
      // kUnauthenticated receives special treatment, it is the error received
      // when the token expires.
      if (instance.status().code() ==
          google::cloud::StatusCode::kUnauthenticated) {
        std::cout << "error [" << instance.status() << "]";
        if (!expired) {
          std::cout << ": unexpected, but could be a race condition."
                    << " Trying again\n";
          return true;
        }
        std::cout << ": this is expected as the token is expired\n";
        return false;
      }
      if (!instance) throw std::runtime_error(instance.status().message());
      std::cout << "success (" << instance->name() << ")\n";
      return true;
    }
    return false;
  };

  for (auto now = std::chrono::system_clock::now(); now < deadline;
       now = std::chrono::system_clock::now()) {
    auto const expired = (now > expiration);
    std::cout << absl::FromChrono(now) << ": running iteration with "
              << (expired ? "an expired" : "a valid") << " token ";
    if (!iteration(expired)) break;
    std::this_thread::sleep_for(kTokenValidationPeriod);
  }
}

void UseIdToken(google::cloud::iam::IAMCredentialsClient client,
                std::vector<std::string> const& argv) {
  namespace iam = google::cloud::iam;
  [](iam::IAMCredentialsClient client, std::string const& service_account,
     std::string const& hello_world_url) {
    auto token = client.GenerateIdToken(
        "projects/-/serviceAccounts/" + service_account, /*delegates=*/{},
        /*audience=*/{hello_world_url},
        /*include_email=*/true);
    if (!token) throw std::runtime_error(token.status().message());

    auto text = HttpGet(hello_world_url, token->token());
    if (!text) throw std::runtime_error(token.status().message());
    std::cout << "Server says: " << *text << "\n";
  }(std::move(client), argv.at(0), argv.at(1));
}

void AutoRun(std::vector<std::string> const& argv) {
  namespace examples = ::google::cloud::testing_util;
  using google::cloud::internal::GetEnv;

  if (!argv.empty()) throw examples::Usage{"auto"};
  examples::CheckEnvironmentVariablesAreSet({
      "GOOGLE_CLOUD_PROJECT",
      "GOOGLE_CLOUD_CPP_IAM_TEST_SERVICE_ACCOUNT",
      "GOOGLE_CLOUD_CPP_TEST_HELLO_WORLD_SERVICE_ACCOUNT",
      "GOOGLE_CLOUD_CPP_TEST_HELLO_WORLD_HTTP_URL",
  });
  auto project_id = GetEnv("GOOGLE_CLOUD_PROJECT").value();
  auto const test_iam_service_account =
      GetEnv("GOOGLE_CLOUD_CPP_IAM_TEST_SERVICE_ACCOUNT").value_or("");
  auto const hello_world_service_account =
      GetEnv("GOOGLE_CLOUD_CPP_TEST_HELLO_WORLD_SERVICE_ACCOUNT").value_or("");
  auto const hello_world_url =
      GetEnv("GOOGLE_CLOUD_CPP_TEST_HELLO_WORLD_HTTP_URL").value_or("");

  auto client = google::cloud::iam::IAMCredentialsClient(
      google::cloud::iam::MakeIAMCredentialsConnection(
          google::cloud::Options{}.set<google::cloud::TracingComponentsOption>(
              DefaultTracingComponents())));

  std::cout << "\nRunning UseAccessToken() example" << std::endl;
  UseAccessToken(client, {test_iam_service_account, project_id});

  std::cout << "\nRunning UseAccessTokenUntilExpired() example" << std::endl;
  UseAccessTokenUntilExpired(client, {test_iam_service_account, project_id});

  std::cout << "\nRunning UseIdToken() example" << std::endl;
  UseIdToken(client, {hello_world_service_account, hello_world_url});
}

}  // namespace

int main(int argc, char* argv[]) {  // NOLINT(bugprone-exception-escape)
  using ::google::cloud::testing_util::Example;

  using ClientCommand = std::function<void(
      google::cloud::iam::IAMCredentialsClient, std::vector<std::string> argv)>;

  auto make_entry = [](std::string name,
                       std::vector<std::string> const& arg_names,
                       ClientCommand const& command) {
    auto adapter = [=](std::vector<std::string> argv) {
      if ((argv.size() == 1 && argv[0] == "--help") ||
          argv.size() != arg_names.size()) {
        std::string usage = name;
        for (auto const& a : arg_names) usage += " <" + a + ">";
        throw google::cloud::testing_util::Usage{std::move(usage)};
      }
      auto client = google::cloud::iam::IAMCredentialsClient(
          google::cloud::iam::MakeIAMCredentialsConnection(
              google::cloud::Options{}
                  .set<google::cloud::TracingComponentsOption>(
                      DefaultTracingComponents())));
      command(client, std::move(argv));
    };
    return google::cloud::testing_util::Commands::value_type(std::move(name),
                                                             adapter);
  };

  Example example({
      make_entry("use-access-token", {"service-account", "project-id"},
                 UseAccessToken),
      make_entry("use-access-token-until-expired",
                 {"service-account", "project-id"}, UseAccessTokenUntilExpired),
      make_entry("use-id-token", {"service-account", "hello-world-http-url"},
                 UseIdToken),
      {"auto", AutoRun},
  });
  return example.Run(argc, argv);
}
