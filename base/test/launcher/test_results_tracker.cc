// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/launcher/test_results_tracker.h"

#include <stddef.h>

#include <utility>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/format_macros.h"
#include "base/json/json_file_value_serializer.h"
#include "base/json/string_escape.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/test/gtest_util.h"
#include "base/test/launcher/test_launcher.h"
#include "base/values.h"

namespace base {

namespace {

// The default output file for XML output.
const FilePath::CharType kDefaultOutputFile[] = FILE_PATH_LITERAL(
    "test_detail.xml");

// Converts the given epoch time in milliseconds to a date string in the ISO
// 8601 format, without the timezone information.
// TODO(xyzzyz): Find a good place in Chromium to put it and refactor all uses
// to point to it.
std::string FormatTimeAsIso8601(base::Time time) {
  base::Time::Exploded exploded;
  time.UTCExplode(&exploded);
  return StringPrintf("%04d-%02d-%02dT%02d:%02d:%02d",
                      exploded.year,
                      exploded.month,
                      exploded.day_of_month,
                      exploded.hour,
                      exploded.minute,
                      exploded.second);
}

struct TestSuiteResultsAggregator {
  TestSuiteResultsAggregator()
      : tests(0), failures(0), disabled(0), errors(0) {}

  void Add(const TestResult& result) {
    tests++;
    elapsed_time += result.elapsed_time;

    switch (result.status) {
      case TestResult::TEST_SUCCESS:
        break;
      case TestResult::TEST_FAILURE:
        failures++;
        break;
      case TestResult::TEST_EXCESSIVE_OUTPUT:
      case TestResult::TEST_FAILURE_ON_EXIT:
      case TestResult::TEST_TIMEOUT:
      case TestResult::TEST_CRASH:
      case TestResult::TEST_UNKNOWN:
        errors++;
        break;
      case TestResult::TEST_SKIPPED:
        disabled++;
        break;
    }
  }

  int tests;
  int failures;
  int disabled;
  int errors;

  base::TimeDelta elapsed_time;
};

}  // namespace

TestResultsTracker::TestResultsTracker() : iteration_(-1), out_(nullptr) {}

TestResultsTracker::~TestResultsTracker() {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!out_)
    return;

  // Maps test case names to test results.
  typedef std::map<std::string, std::vector<TestResult> > TestCaseMap;
  TestCaseMap test_case_map;

  TestSuiteResultsAggregator all_tests_aggregator;
  for (const PerIterationData::ResultsMap::value_type& i
           : per_iteration_data_[iteration_].results) {
    // Use the last test result as the final one.
    TestResult result = i.second.test_results.back();
    test_case_map[result.GetTestCaseName()].push_back(result);
    all_tests_aggregator.Add(result);
  }

  fprintf(out_, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  fprintf(out_, "<testsuites name=\"AllTests\" tests=\"%d\" failures=\"%d\""
          " disabled=\"%d\" errors=\"%d\" time=\"%.3f\" timestamp=\"%s\">\n",
          all_tests_aggregator.tests,
          all_tests_aggregator.failures,
          all_tests_aggregator.disabled,
          all_tests_aggregator.errors,
          all_tests_aggregator.elapsed_time.InSecondsF(),
          FormatTimeAsIso8601(base::Time::Now()).c_str());

  for (const TestCaseMap::value_type& i : test_case_map) {
    const std::string testsuite_name = i.first;
    const std::vector<TestResult>& results = i.second;

    TestSuiteResultsAggregator aggregator;
    for (const TestResult& result : results) {
      aggregator.Add(result);
    }
    fprintf(out_, "  <testsuite name=\"%s\" tests=\"%d\" "
            "failures=\"%d\" disabled=\"%d\" errors=\"%d\" time=\"%.3f\" "
            "timestamp=\"%s\">\n",
            testsuite_name.c_str(),
            aggregator.tests, aggregator.failures,
            aggregator.disabled, aggregator.errors,
            aggregator.elapsed_time.InSecondsF(),
            FormatTimeAsIso8601(base::Time::Now()).c_str());

    for (const TestResult& result : results) {
      fprintf(out_, "    <testcase name=\"%s\" status=\"run\" time=\"%.3f\""
              " classname=\"%s\">\n",
              result.GetTestName().c_str(),
              result.elapsed_time.InSecondsF(),
              result.GetTestCaseName().c_str());
      if (result.status != TestResult::TEST_SUCCESS) {
        // The actual failure message is not propagated up to here, as it's too
        // much work to escape it properly, and in case of failure, almost
        // always one needs to look into full log anyway.
        fprintf(out_, "      <failure message=\"\" type=\"\"></failure>\n");
      }
      fprintf(out_, "    </testcase>\n");
    }
    fprintf(out_, "  </testsuite>\n");
  }

  fprintf(out_, "</testsuites>\n");
  fclose(out_);
}

bool TestResultsTracker::Init(const CommandLine& command_line) {
  DCHECK(thread_checker_.CalledOnValidThread());

  // Prevent initializing twice.
  if (out_) {
    NOTREACHED();
    return false;
  }

  if (!command_line.HasSwitch(kGTestOutputFlag))
    return true;

  std::string flag = command_line.GetSwitchValueASCII(kGTestOutputFlag);
  size_t colon_pos = flag.find(':');
  FilePath path;
  if (colon_pos != std::string::npos) {
    FilePath flag_path =
        command_line.GetSwitchValuePath(kGTestOutputFlag);
    FilePath::StringType path_string = flag_path.value();
    path = FilePath(path_string.substr(colon_pos + 1));
    // If the given path ends with '/', consider it is a directory.
    // Note: This does NOT check that a directory (or file) actually exists
    // (the behavior is same as what gtest does).
    if (path.EndsWithSeparator()) {
      FilePath executable = command_line.GetProgram().BaseName();
      path = path.Append(executable.ReplaceExtension(
                             FilePath::StringType(FILE_PATH_LITERAL("xml"))));
    }
  }
  if (path.value().empty())
    path = FilePath(kDefaultOutputFile);
  FilePath dir_name = path.DirName();
  if (!DirectoryExists(dir_name)) {
    LOG(WARNING) << "The output directory does not exist. "
                 << "Creating the directory: " << dir_name.value();
    // Create the directory if necessary (because the gtest does the same).
    if (!base::CreateDirectory(dir_name)) {
      LOG(ERROR) << "Failed to created directory " << dir_name.value();
      return false;
    }
  }
  out_ = OpenFile(path, "w");
  if (!out_) {
    LOG(ERROR) << "Cannot open output file: "
               << path.value() << ".";
    return false;
  }

  return true;
}

void TestResultsTracker::OnTestIterationStarting() {
  DCHECK(thread_checker_.CalledOnValidThread());

  // Start with a fresh state for new iteration.
  iteration_++;
  per_iteration_data_.push_back(PerIterationData());
}

void TestResultsTracker::AddTest(const std::string& test_name) {
  // Record disabled test names without DISABLED_ prefix so that they are easy
  // to compare with regular test names, e.g. before or after disabling.
  all_tests_.insert(TestNameWithoutDisabledPrefix(test_name));
}

void TestResultsTracker::AddDisabledTest(const std::string& test_name) {
  // Record disabled test names without DISABLED_ prefix so that they are easy
  // to compare with regular test names, e.g. before or after disabling.
  disabled_tests_.insert(TestNameWithoutDisabledPrefix(test_name));
}

void TestResultsTracker::AddTestLocation(const std::string& test_name,
                                         const std::string& file,
                                         int line) {
  test_locations_.insert(std::make_pair(test_name, CodeLocation(file, line)));
}

void TestResultsTracker::AddTestResult(const TestResult& result) {
  DCHECK(thread_checker_.CalledOnValidThread());

  // Record disabled test names without DISABLED_ prefix so that they are easy
  // to compare with regular test names, e.g. before or after disabling.
  per_iteration_data_[iteration_].results[
      TestNameWithoutDisabledPrefix(result.full_name)].test_results.push_back(
          result);
}

void TestResultsTracker::PrintSummaryOfCurrentIteration() const {
  TestStatusMap tests_by_status(GetTestStatusMapForCurrentIteration());

  PrintTests(tests_by_status[TestResult::TEST_FAILURE].begin(),
             tests_by_status[TestResult::TEST_FAILURE].end(),
             "failed");
  PrintTests(tests_by_status[TestResult::TEST_FAILURE_ON_EXIT].begin(),
             tests_by_status[TestResult::TEST_FAILURE_ON_EXIT].end(),
             "failed on exit");
  PrintTests(tests_by_status[TestResult::TEST_EXCESSIVE_OUTPUT].begin(),
             tests_by_status[TestResult::TEST_EXCESSIVE_OUTPUT].end(),
             "produced excessive output");
  PrintTests(tests_by_status[TestResult::TEST_TIMEOUT].begin(),
             tests_by_status[TestResult::TEST_TIMEOUT].end(),
             "timed out");
  PrintTests(tests_by_status[TestResult::TEST_CRASH].begin(),
             tests_by_status[TestResult::TEST_CRASH].end(),
             "crashed");
  PrintTests(tests_by_status[TestResult::TEST_SKIPPED].begin(),
             tests_by_status[TestResult::TEST_SKIPPED].end(),
             "skipped");
  PrintTests(tests_by_status[TestResult::TEST_UNKNOWN].begin(),
             tests_by_status[TestResult::TEST_UNKNOWN].end(),
             "had unknown result");
}

void TestResultsTracker::PrintSummaryOfAllIterations() const {
  DCHECK(thread_checker_.CalledOnValidThread());

  TestStatusMap tests_by_status(GetTestStatusMapForAllIterations());

  fprintf(stdout, "Summary of all test iterations:\n");
  fflush(stdout);

  PrintTests(tests_by_status[TestResult::TEST_FAILURE].begin(),
             tests_by_status[TestResult::TEST_FAILURE].end(),
             "failed");
  PrintTests(tests_by_status[TestResult::TEST_FAILURE_ON_EXIT].begin(),
             tests_by_status[TestResult::TEST_FAILURE_ON_EXIT].end(),
             "failed on exit");
  PrintTests(tests_by_status[TestResult::TEST_EXCESSIVE_OUTPUT].begin(),
             tests_by_status[TestResult::TEST_EXCESSIVE_OUTPUT].end(),
             "produced excessive output");
  PrintTests(tests_by_status[TestResult::TEST_TIMEOUT].begin(),
             tests_by_status[TestResult::TEST_TIMEOUT].end(),
             "timed out");
  PrintTests(tests_by_status[TestResult::TEST_CRASH].begin(),
             tests_by_status[TestResult::TEST_CRASH].end(),
             "crashed");
  PrintTests(tests_by_status[TestResult::TEST_SKIPPED].begin(),
             tests_by_status[TestResult::TEST_SKIPPED].end(),
             "skipped");
  PrintTests(tests_by_status[TestResult::TEST_UNKNOWN].begin(),
             tests_by_status[TestResult::TEST_UNKNOWN].end(),
             "had unknown result");

  fprintf(stdout, "End of the summary.\n");
  fflush(stdout);
}

void TestResultsTracker::AddGlobalTag(const std::string& tag) {
  global_tags_.insert(tag);
}

bool TestResultsTracker::SaveSummaryAsJSON(
    const FilePath& path,
    const std::vector<std::string>& additional_tags) const {
  std::unique_ptr<DictionaryValue> summary_root(new DictionaryValue);

  std::unique_ptr<ListValue> global_tags(new ListValue);
  for (const auto& global_tag : global_tags_) {
    global_tags->AppendString(global_tag);
  }
  for (const auto& tag : additional_tags) {
    global_tags->AppendString(tag);
  }
  summary_root->Set("global_tags", std::move(global_tags));

  std::unique_ptr<ListValue> all_tests(new ListValue);
  for (const auto& test : all_tests_) {
    all_tests->AppendString(test);
  }
  summary_root->Set("all_tests", std::move(all_tests));

  std::unique_ptr<ListValue> disabled_tests(new ListValue);
  for (const auto& disabled_test : disabled_tests_) {
    disabled_tests->AppendString(disabled_test);
  }
  summary_root->Set("disabled_tests", std::move(disabled_tests));

  std::unique_ptr<ListValue> per_iteration_data(new ListValue);

  for (int i = 0; i <= iteration_; i++) {
    std::unique_ptr<DictionaryValue> current_iteration_data(
        new DictionaryValue);

    for (PerIterationData::ResultsMap::const_iterator j =
             per_iteration_data_[i].results.begin();
         j != per_iteration_data_[i].results.end();
         ++j) {
      std::unique_ptr<ListValue> test_results(new ListValue);

      for (size_t k = 0; k < j->second.test_results.size(); k++) {
        const TestResult& test_result = j->second.test_results[k];

        std::unique_ptr<DictionaryValue> test_result_value(new DictionaryValue);

        test_result_value->SetString("status", test_result.StatusAsString());
        test_result_value->SetInteger(
            "elapsed_time_ms",
            static_cast<int>(test_result.elapsed_time.InMilliseconds()));

        bool lossless_snippet = false;
        if (IsStringUTF8(test_result.output_snippet)) {
          test_result_value->SetString(
              "output_snippet", test_result.output_snippet);
          lossless_snippet = true;
        } else {
          test_result_value->SetString(
              "output_snippet",
              "<non-UTF-8 snippet, see output_snippet_base64>");
        }

        // TODO(phajdan.jr): Fix typo in JSON key (losless -> lossless)
        // making sure not to break any consumers of this data.
        test_result_value->SetBoolean("losless_snippet", lossless_snippet);

        // Also include the raw version (base64-encoded so that it can be safely
        // JSON-serialized - there are no guarantees about character encoding
        // of the snippet). This can be very useful piece of information when
        // debugging a test failure related to character encoding.
        std::string base64_output_snippet;
        Base64Encode(test_result.output_snippet, &base64_output_snippet);
        test_result_value->SetString("output_snippet_base64",
                                     base64_output_snippet);

        std::unique_ptr<ListValue> test_result_parts(new ListValue);
        for (const TestResultPart& result_part :
             test_result.test_result_parts) {
          std::unique_ptr<DictionaryValue> result_part_value(
              new DictionaryValue);
          result_part_value->SetString("type", result_part.TypeAsString());
          result_part_value->SetString("file", result_part.file_name);
          result_part_value->SetInteger("line", result_part.line_number);

          bool lossless_summary = IsStringUTF8(result_part.summary);
          if (lossless_summary) {
            result_part_value->SetString("summary", result_part.summary);
          } else {
            result_part_value->SetString(
                "summary", "<non-UTF-8 snippet, see summary_base64>");
          }
          result_part_value->SetBoolean("lossless_summary", lossless_summary);

          std::string encoded_summary;
          Base64Encode(result_part.summary, &encoded_summary);
          result_part_value->SetString("summary_base64", encoded_summary);

          bool lossless_message = IsStringUTF8(result_part.message);
          if (lossless_message) {
            result_part_value->SetString("message", result_part.message);
          } else {
            result_part_value->SetString(
                "message", "<non-UTF-8 snippet, see message_base64>");
          }
          result_part_value->SetBoolean("lossless_message", lossless_message);

          std::string encoded_message;
          Base64Encode(result_part.message, &encoded_message);
          result_part_value->SetString("message_base64", encoded_message);

          test_result_parts->Append(std::move(result_part_value));
        }
        test_result_value->Set("result_parts", std::move(test_result_parts));

        test_results->Append(std::move(test_result_value));
      }

      current_iteration_data->SetWithoutPathExpansion(j->first,
                                                      std::move(test_results));
    }
    per_iteration_data->Append(std::move(current_iteration_data));
  }
  summary_root->Set("per_iteration_data", std::move(per_iteration_data));

  std::unique_ptr<DictionaryValue> test_locations(new DictionaryValue);
  for (const auto& item : test_locations_) {
    std::string test_name = item.first;
    CodeLocation location = item.second;
    std::unique_ptr<DictionaryValue> location_value(new DictionaryValue);
    location_value->SetString("file", location.file);
    location_value->SetInteger("line", location.line);
    test_locations->SetWithoutPathExpansion(test_name,
                                            std::move(location_value));
  }
  summary_root->Set("test_locations", std::move(test_locations));

  JSONFileValueSerializer serializer(path);
  return serializer.Serialize(*summary_root);
}

TestResultsTracker::TestStatusMap
    TestResultsTracker::GetTestStatusMapForCurrentIteration() const {
  TestStatusMap tests_by_status;
  GetTestStatusForIteration(iteration_, &tests_by_status);
  return tests_by_status;
}

TestResultsTracker::TestStatusMap
    TestResultsTracker::GetTestStatusMapForAllIterations() const {
  TestStatusMap tests_by_status;
  for (int i = 0; i <= iteration_; i++)
    GetTestStatusForIteration(i, &tests_by_status);
  return tests_by_status;
}

void TestResultsTracker::GetTestStatusForIteration(
    int iteration, TestStatusMap* map) const {
  for (PerIterationData::ResultsMap::const_iterator j =
           per_iteration_data_[iteration].results.begin();
       j != per_iteration_data_[iteration].results.end();
       ++j) {
    // Use the last test result as the final one.
    const TestResult& result = j->second.test_results.back();
    (*map)[result.status].insert(result.full_name);
  }
}

// Utility function to print a list of test names. Uses iterator to be
// compatible with different containers, like vector and set.
template<typename InputIterator>
void TestResultsTracker::PrintTests(InputIterator first,
                                    InputIterator last,
                                    const std::string& description) const {
  size_t count = std::distance(first, last);
  if (count == 0)
    return;

  fprintf(stdout,
          "%" PRIuS " test%s %s:\n",
          count,
          count != 1 ? "s" : "",
          description.c_str());
  for (InputIterator i = first; i != last; ++i) {
    fprintf(stdout,
            "    %s (%s:%d)\n",
            (*i).c_str(),
            test_locations_.at(*i).file.c_str(),
            test_locations_.at(*i).line);
  }
  fflush(stdout);
}


TestResultsTracker::AggregateTestResult::AggregateTestResult() {
}

TestResultsTracker::AggregateTestResult::AggregateTestResult(
    const AggregateTestResult& other) = default;

TestResultsTracker::AggregateTestResult::~AggregateTestResult() {
}

TestResultsTracker::PerIterationData::PerIterationData() {
}

TestResultsTracker::PerIterationData::PerIterationData(
    const PerIterationData& other) = default;

TestResultsTracker::PerIterationData::~PerIterationData() {
}

}  // namespace base
