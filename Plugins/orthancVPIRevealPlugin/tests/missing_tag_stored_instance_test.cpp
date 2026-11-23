/* Spec for the stored-instance crash in OnStoredCallback / getDicomTag.
 *
 * These tests fail while a missing DICOM tag, a JSON null, or a missing
 * closing quote makes the plugin dereference the failed search. They pass
 * when that callback returns an error and creates no export file or
 * directory. A complete instance must still be exported.
 *
 * The JSON is produced by JsonCpp 0.10.5's StyledWriter, the writer Orthanc
 * 1.2.0 uses for OrthancPluginGetInstanceSimplifiedJson. Tags that are not
 * in the set are omitted, which is what Orthanc's dataset walk does.
 */

#include <orthanc/OrthancCPlugin.h>

#include <json/json.h>

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

extern "C" int32_t OrthancPluginInitialize(OrthancPluginContext* context);

OrthancPluginErrorCode OnStoredCallback(OrthancPluginDicomInstance* instance,
                                        const char* instanceId);

namespace {

enum TagKind { TagAbsent, TagString, TagNull };

struct TagSpec {
  const char* name;
  TagKind kind;
  const char* value;
};

const char kInstancePayload[] = "DICOM-INSTANCE";
const char kRemoteAet[] = "CALLING";
char g_config_path[] = "C:\\orthanc\\Configuration.json";

std::string g_simplified_json;
std::string g_case_dir;

int Fail(const char* message)
{
  fprintf(stderr, "    %s\n", message);
  return 1;
}

bool DirectoryHasEntries(const char* path)
{
  DIR* dir = opendir(path);
  if (dir == NULL)
    return false;
  bool found = false;
  while (dirent* ent = readdir(dir)) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    found = true;
    break;
  }
  closedir(dir);
  return found;
}

/* Same member rules as Orthanc 1.2.0 ServerToolbox::SimplifyTags for a
 * human-readable string or null, then Json::StyledWriter as used by
 * AccessDicomInstance for GetInstanceSimplifiedJson.
 */
std::string SimplifiedJson(const std::vector<TagSpec>& tags)
{
  Json::Value simplified(Json::objectValue);
  for (size_t i = 0; i < tags.size(); i++) {
    if (tags[i].kind == TagAbsent)
      continue;
    if (tags[i].kind == TagNull)
      simplified[tags[i].name] = Json::Value();
    else
      simplified[tags[i].name] = tags[i].value;
  }
  Json::StyledWriter writer;
  return writer.write(simplified);
}

/* Drop the closing quote of one string value and everything after it, so
 * strchr cannot land on a later key's quote. The opening quote that the
 * plugin searches for stays in place.
 */
std::string WithoutClosingQuote(const std::string& json, const char* tag)
{
  const std::string needle = std::string("\"") + tag + "\" : \"";
  const size_t at = json.find(needle);
  if (at == std::string::npos)
    return std::string();
  return json.substr(0, at + needle.size()) + "UNCLOSED";
}

char* Dup(const char* text)
{
  char* copy = static_cast<char*>(malloc(strlen(text) + 1));
  memcpy(copy, text, strlen(text) + 1);
  return copy;
}

OrthancPluginErrorCode InvokeService(OrthancPluginContext* /*context*/,
                                     _OrthancPluginService service,
                                     const void* params)
{
  switch (service) {
    case _OrthancPluginService_LogInfo:
    case _OrthancPluginService_LogWarning:
    case _OrthancPluginService_LogError:
    case _OrthancPluginService_SetPluginProperty:
    case _OrthancPluginService_RegisterRestCallback:
    case _OrthancPluginService_RegisterOnStoredInstanceCallback:
    case _OrthancPluginService_RegisterOnChangeCallback:
      return OrthancPluginErrorCode_Success;

    case _OrthancPluginService_GetOrthancPath:
    case _OrthancPluginService_GetOrthancDirectory:
    case _OrthancPluginService_GetConfiguration: {
      const _OrthancPluginRetrieveDynamicString* p =
          static_cast<const _OrthancPluginRetrieveDynamicString*>(params);
      *p->result = Dup("C:\\orthanc");
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetConfigurationPath: {
      const _OrthancPluginRetrieveDynamicString* p =
          static_cast<const _OrthancPluginRetrieveDynamicString*>(params);
      *p->result = Dup(g_config_path);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetCommandLineArgumentsCount: {
      const _OrthancPluginReturnSingleValue* p =
          static_cast<const _OrthancPluginReturnSingleValue*>(params);
      *p->resultUint32 = 0;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_RestApiGet: {
      const _OrthancPluginRestApiGet* p =
          static_cast<const _OrthancPluginRestApiGet*>(params);
      const char* body = "{}\n";
      p->target->data = Dup(body);
      p->target->size = static_cast<uint32_t>(strlen(body));
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceRemoteAet: {
      const _OrthancPluginAccessDicomInstance* p =
          static_cast<const _OrthancPluginAccessDicomInstance*>(params);
      *p->resultString = kRemoteAet;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSize: {
      const _OrthancPluginAccessDicomInstance* p =
          static_cast<const _OrthancPluginAccessDicomInstance*>(params);
      *p->resultInt64 = static_cast<int64_t>(sizeof(kInstancePayload) - 1);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceData: {
      const _OrthancPluginAccessDicomInstance* p =
          static_cast<const _OrthancPluginAccessDicomInstance*>(params);
      *p->resultString = kInstancePayload;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSimplifiedJson: {
      const _OrthancPluginAccessDicomInstance* p =
          static_cast<const _OrthancPluginAccessDicomInstance*>(params);
      *p->resultStringToFree = Dup(g_simplified_json.c_str());
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceOrigin: {
      const _OrthancPluginAccessDicomInstance* p =
          static_cast<const _OrthancPluginAccessDicomInstance*>(params);
      *p->resultOrigin = OrthancPluginInstanceOrigin_DicomProtocol;
      return OrthancPluginErrorCode_Success;
    }

    default:
      fprintf(stderr, "unexpected plugin service %d\n", static_cast<int>(service));
      return OrthancPluginErrorCode_UnknownPluginService;
  }
}

void FreeString(void* buffer)
{
  free(buffer);
}

std::vector<TagSpec> CompleteTags()
{
  const TagSpec tags[] = {
      {"PatientName", TagString, "ANN"},
      {"StudyDescription", TagString, "CHEST"},
      {"SeriesNumber", TagString, "3"},
      {"SeriesDescription", TagString, "LOCAL"},
  };
  return std::vector<TagSpec>(tags, tags + 4);
}

std::vector<TagSpec> TagsMissing(const char* name)
{
  std::vector<TagSpec> tags = CompleteTags();
  for (size_t i = 0; i < tags.size(); i++) {
    if (strcmp(tags[i].name, name) == 0)
      tags[i].kind = TagAbsent;
  }
  return tags;
}

int ExpectExport(const char* json)
{
  g_simplified_json = json;
  if (chdir(g_case_dir.c_str()) != 0)
    return Fail("cannot enter case directory");

  int marker = 0;
  OrthancPluginDicomInstance* instance =
      reinterpret_cast<OrthancPluginDicomInstance*>(&marker);
  OrthancPluginErrorCode code = OnStoredCallback(instance, "instance-1");
  if (code != OrthancPluginErrorCode_Success)
    return Fail("complete instance was not stored");

  /* OnStoredCallback changes the process directory to the series folder
   * and the configuration-path change back is a Windows path. Look up
   * the export from the case directory, not from the final cwd.
   */
  const std::string exported =
      g_case_dir + "/VPI_Storage/ANN/CHEST/3-LOCAL/1.dcm";
  FILE* fp = fopen(exported.c_str(), "rb");
  if (fp == NULL)
    return Fail("export file was not written");
  char buf[sizeof(kInstancePayload)] = {0};
  size_t n = fread(buf, 1, sizeof(buf), fp);
  fclose(fp);
  if (n != sizeof(kInstancePayload) - 1 ||
      memcmp(buf, kInstancePayload, n) != 0)
    return Fail("export file contents do not match the stored instance");
  return 0;
}

int ExpectRefused(const char* json)
{
  g_simplified_json = json;
  if (chdir(g_case_dir.c_str()) != 0)
    return Fail("cannot enter case directory");

  int marker = 0;
  OrthancPluginDicomInstance* instance =
      reinterpret_cast<OrthancPluginDicomInstance*>(&marker);
  OrthancPluginErrorCode code = OnStoredCallback(instance, "instance-1");
  if (code == OrthancPluginErrorCode_Success)
    return Fail("callback returned success for a tag it could not read");
  if (DirectoryHasEntries(g_case_dir.c_str()))
    return Fail("export created a file or directory before refusing");
  return 0;
}

int RunChild(int (*fn)(const char*), const char* json)
{
  struct rlimit core;
  core.rlim_cur = 0;
  core.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &core);
  alarm(5);
  int rc = fn(json);
  _exit(rc == 0 ? 0 : 2);
}

int RunCase(const char* name, int (*fn)(const char*), const char* json)
{
  char templ[] = "/tmp/vpi-missing-tag-XXXXXX";
  char* dir = mkdtemp(templ);
  if (dir == NULL) {
    fprintf(stderr, "FAIL %s: mkdtemp: %s\n", name, strerror(errno));
    return 1;
  }
  g_case_dir = dir;

  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "FAIL %s: fork: %s\n", name, strerror(errno));
    return 1;
  }
  if (pid == 0)
    RunChild(fn, json);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    fprintf(stderr, "FAIL %s: waitpid: %s\n", name, strerror(errno));
    return 1;
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "FAIL %s: process died with signal %d\n", name, WTERMSIG(status));
    return 1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "FAIL %s: exit status %d\n", name,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 1;
  }
  printf("PASS %s\n", name);
  return 0;
}

}  // namespace

int main()
{
  OrthancPluginContext context;
  memset(&context, 0, sizeof(context));
  context.orthancVersion = "1.2.0";
  context.Free = FreeString;
  context.InvokeService = InvokeService;
  if (OrthancPluginInitialize(&context) != 0) {
    fprintf(stderr, "plugin initialization failed\n");
    return 1;
  }

  int failed = 0;
  const std::string complete = SimplifiedJson(CompleteTags());
  failed += RunCase("complete_instance_is_exported", ExpectExport, complete.c_str());

  const char* optional_tags[] = {
      "PatientName", "StudyDescription", "SeriesNumber", "SeriesDescription"};
  for (size_t i = 0; i < 4; i++) {
    const std::string json = SimplifiedJson(TagsMissing(optional_tags[i]));
    char name[128];
    snprintf(name, sizeof(name), "missing_%s_returns_error_without_export",
             optional_tags[i]);
    failed += RunCase(name, ExpectRefused, json.c_str());
  }

  std::vector<TagSpec> study_null = CompleteTags();
  study_null[1].kind = TagNull;
  failed += RunCase("null_study_description_returns_error_without_export",
                    ExpectRefused, SimplifiedJson(study_null).c_str());

  std::vector<TagSpec> series_null = CompleteTags();
  series_null[3].kind = TagNull;
  failed += RunCase("null_series_description_returns_error_without_export",
                    ExpectRefused, SimplifiedJson(series_null).c_str());

  failed += RunCase("study_description_without_closing_quote_returns_error_without_export",
                    ExpectRefused,
                    WithoutClosingQuote(complete, "StudyDescription").c_str());

  /* SeriesDescription is not the last key StyledWriter emits, so cutting
   * its quote would leave a later quote for strchr. Keep the other tags
   * closed, then append the unclosed series description at the end.
   */
  std::string series_quote_case = SimplifiedJson(TagsMissing("SeriesDescription"));
  series_quote_case += "\"SeriesDescription\" : \"LOCAL";
  failed += RunCase("series_description_without_closing_quote_returns_error_without_export",
                    ExpectRefused, series_quote_case.c_str());

  return failed == 0 ? 0 : 1;
}
