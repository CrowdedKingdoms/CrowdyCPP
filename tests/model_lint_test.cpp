#include <string>
#include <vector>

#include "crowdy/studio/model_lint.hpp"
#include "test_util.hpp"

using crowdy::studio::CrowdyModelLintFinding;
using crowdy::studio::CrowdyModelLintLog;
using crowdy::studio::CrowdyModelRefusal;
using crowdy::studio::CrowdyStudioDiagnosticSeverity;
using crowdy::studio::CrowdyStudioDiagnosticSource;
using crowdy::studio::isModelRefusalCode;
using crowdy::studio::modelLintDiagnostics;
using crowdy::studio::modelLintSubjectPath;

/**
 * Game-model findings reaching the developer, mirroring CrowdyJS's model-lint test.
 *
 * The two SDKs are meant to agree on this surface, and the way they stop agreeing is one
 * of them growing a case the other does not have. Keeping the cases parallel is what makes
 * that visible in review.
 */

namespace {

CrowdyModelLintFinding orphanFinding() {
  CrowdyModelLintFinding finding;
  finding.code = "container_type_undefined";
  finding.severity = CrowdyStudioDiagnosticSeverity::Error;
  finding.subjectKind = "container_type";
  finding.subject = "TitanAssaultTeamAssignment";
  finding.message = "3 container(s) name a type this app does not define";
  finding.remedy = "Define it, or correct the name your client sends.";
  return finding;
}

void findingsBecomeDiagnostics() {
  const std::vector<CrowdyModelLintFinding> findings{orphanFinding()};
  const auto diagnostics = modelLintDiagnostics(findings);

  CHECK_EQ(diagnostics.size(), std::size_t{1});
  CHECK_EQ(diagnostics[0].severity, CrowdyStudioDiagnosticSeverity::Error);
  CHECK_EQ(diagnostics[0].code.value_or(""), std::string{"container_type_undefined"});
  CHECK_EQ(diagnostics[0].source, CrowdyStudioDiagnosticSource::ModelLint);
  CHECK_EQ(diagnostics[0].path,
           std::string{"container_type/TitanAssaultTeamAssignment"});
  // The remedy is joined into the message rather than dropped: a Problems list is
  // one line per entry, and a finding the developer cannot act on from where they
  // are reading it is the failure this surface exists to fix.
  CHECK(diagnostics[0].message.find("Define it, or correct the name") !=
        std::string::npos);
  // No file and no line to claim, so the location is the subject and line 1. An
  // invented location would send a click into a file with nothing to do with it.
  CHECK_EQ(diagnostics[0].line, std::uint32_t{1});
}

void subjectPathIsPrefixedByKind() {
  // An automation and a function may both be called on_join; a Problems list
  // showing both as "on_join" would be misleading about which one is broken.
  CrowdyModelLintFinding fn;
  fn.subjectKind = "function";
  fn.subject = "on_join";
  CrowdyModelLintFinding automation;
  automation.subjectKind = "automation";
  automation.subject = "on_join";

  CHECK_EQ(modelLintSubjectPath(fn), std::string{"function/on_join"});
  CHECK_EQ(modelLintSubjectPath(automation), std::string{"automation/on_join"});
}

void cleanLintRendersNothing() {
  const std::vector<CrowdyModelLintFinding> none;
  CHECK_EQ(modelLintDiagnostics(none).size(), std::size_t{0});
}

void refusalCodesAreRecognisedByCode() {
  CHECK(isModelRefusalCode("CONTAINER_TYPE_UNDEFINED"));
  CHECK(isModelRefusalCode("OBJECT_QUARANTINED"));
  // Never by message text, and never for an unrelated failure.
  CHECK(!isModelRefusalCode("FORBIDDEN"));
  CHECK(!isModelRefusalCode(""));
}

void sameRefusalIsRecordedOnce() {
  // The one that matters. A bind is attempted per entity per frame, and a warning
  // at that rate is indistinguishable from noise -- which is how the original
  // incident stayed invisible for a day while the client logged continuously.
  CrowdyModelLintLog log;
  const CrowdyModelRefusal refusal{"CONTAINER_TYPE_UNDEFINED", "not defined",
                                   "Foo"};

  CHECK(log.record(refusal));
  for (int i = 0; i < 1000; ++i) {
    CHECK(!log.record(refusal));
  }
  CHECK_EQ(log.collected().size(), std::size_t{1});
}

void differentSubjectIsADifferentProblem() {
  CrowdyModelLintLog log;
  CHECK(log.record({"CONTAINER_TYPE_UNDEFINED", "a", "Foo"}));
  CHECK(log.record({"CONTAINER_TYPE_UNDEFINED", "b", "Bar"}));
  CHECK(log.record({"OBJECT_QUARANTINED", "c", "Foo"}));
  CHECK_EQ(log.collected().size(), std::size_t{3});
}

void resetReportsAfresh() {
  CrowdyModelLintLog log;
  const CrowdyModelRefusal refusal{"OBJECT_QUARANTINED", "q", "f"};
  CHECK(log.record(refusal));
  log.reset();
  CHECK(log.record(refusal));
  CHECK_EQ(log.collected().size(), std::size_t{1});
}

}  // namespace

int main() {
  findingsBecomeDiagnostics();
  subjectPathIsPrefixedByKind();
  cleanLintRendersNothing();
  refusalCodesAreRecognisedByCode();
  sameRefusalIsRecordedOnce();
  differentSubjectIsADifferentProblem();
  resetReportsAfresh();
  return 0;
}
