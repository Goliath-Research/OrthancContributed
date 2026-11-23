/* Spec for the VPI storage path built by OnStoredCallback.
   These checks fail while a DICOM sender can place instance bytes
   outside <configuration-directory>/VPI_Storage. A fix must reject an
   empty component, ".", or ".." before creating it, and must not
   create or write any path outside that storage directory. A name
   with no such component must still be stored as
   VPI_Storage/<PatientName>/<StudyDescription>/<SeriesNumber>-<SeriesDescription>/<n>.dcm
   containing the stored instance bytes. */

#include <orthanc/OrthancCPlugin.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

OrthancPluginErrorCode OnStoredCallback(OrthancPluginDicomInstance* instance,
                                        const char* instanceId);
extern "C" int32_t OrthancPluginInitialize(OrthancPluginContext* context);

struct Event
{
  int is_write;
  int created_new;
  std::string path;
};

static std::vector<Event> g_events;
static std::string g_config_file;
static std::string g_json;
static std::string g_bytes;
static int g_failures = 0;
static OrthancPluginOnStoredInstanceCallback g_registered = NULL;

extern "C" void vpi_test_note_mkdir(const char* absolute_path, int created_new)
{
  Event event;
  event.is_write = 0;
  event.created_new = created_new;
  event.path = absolute_path;
  g_events.push_back(event);
}

extern "C" FILE* vpi_test_fopen(const char* path, const char* mode)
{
  if (path != NULL && mode != NULL && (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL))
  {
    char resolved[4096];
    if (path[0] == '/')
    {
      snprintf(resolved, sizeof(resolved), "%s", path);
    }
    else
    {
      char cwd[4096];
      if (getcwd(cwd, sizeof(cwd)) != NULL)
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, path);
      else
        snprintf(resolved, sizeof(resolved), "%s", path);
    }
    Event event;
    event.is_write = 1;
    event.created_new = 1;
    event.path = resolved;
    g_events.push_back(event);
  }
  return fopen(path, mode);
}

static void plugin_free(void* buffer)
{
  free(buffer);
}

static char* dup_c(const std::string& value)
{
  char* copy = (char*) malloc(value.size() + 1);
  memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

static OrthancPluginErrorCode invoke_service(OrthancPluginContext* context,
                                             _OrthancPluginService service,
                                             const void* params)
{
  (void) context;
  switch (service)
  {
    case _OrthancPluginService_LogInfo:
    case _OrthancPluginService_LogWarning:
    case _OrthancPluginService_LogError:
    case _OrthancPluginService_SetPluginProperty:
    case _OrthancPluginService_RegisterRestCallback:
    case _OrthancPluginService_RegisterOnChangeCallback:
      return OrthancPluginErrorCode_Success;

    case _OrthancPluginService_RegisterOnStoredInstanceCallback:
    {
      const _OrthancPluginOnStoredInstanceCallback* callback =
        (const _OrthancPluginOnStoredInstanceCallback*) params;
      g_registered = callback->callback;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetOrthancPath:
    case _OrthancPluginService_GetOrthancDirectory:
    case _OrthancPluginService_GetConfiguration:
    {
      _OrthancPluginRetrieveDynamicString* out =
        (_OrthancPluginRetrieveDynamicString*) params;
      *out->result = dup_c(".");
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetConfigurationPath:
    {
      _OrthancPluginRetrieveDynamicString* out =
        (_OrthancPluginRetrieveDynamicString*) params;
      *out->result = dup_c(g_config_file);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetCommandLineArgumentsCount:
    {
      _OrthancPluginReturnSingleValue* out =
        (_OrthancPluginReturnSingleValue*) params;
      *out->resultUint32 = 0;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_RestApiGet:
    {
      _OrthancPluginRestApiGet* out = (_OrthancPluginRestApiGet*) params;
      out->target->data = malloc(1);
      out->target->size = 1;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceRemoteAet:
    {
      _OrthancPluginAccessDicomInstance* out =
        (_OrthancPluginAccessDicomInstance*) params;
      *out->resultString = "CALLINGAET";
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSize:
    {
      _OrthancPluginAccessDicomInstance* out =
        (_OrthancPluginAccessDicomInstance*) params;
      *out->resultInt64 = (int64_t) g_bytes.size();
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceData:
    {
      _OrthancPluginAccessDicomInstance* out =
        (_OrthancPluginAccessDicomInstance*) params;
      *out->resultString = g_bytes.data();
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSimplifiedJson:
    {
      _OrthancPluginAccessDicomInstance* out =
        (_OrthancPluginAccessDicomInstance*) params;
      *out->resultStringToFree = dup_c(g_json);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceOrigin:
    {
      _OrthancPluginAccessDicomInstance* out =
        (_OrthancPluginAccessDicomInstance*) params;
      *out->resultOrigin = OrthancPluginInstanceOrigin_DicomProtocol;
      return OrthancPluginErrorCode_Success;
    }

    default:
      fprintf(stderr, "unexpected plugin service %d\n", (int) service);
      return OrthancPluginErrorCode_Plugin;
  }
}

static void fail(const char* name, const std::string& detail)
{
  fprintf(stderr, "FAIL %s: %s\n", name, detail.c_str());
  g_failures++;
}

static void reset_events()
{
  g_events.clear();
}

static int ensure_dir(const std::string& path)
{
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
    return 0;
  return -1;
}

static std::string make_sandbox(const char* name)
{
  char tmpl[] = "/tmp/vpi-path-XXXXXX";
  char* dir = mkdtemp(tmpl);
  if (dir == NULL)
  {
    perror("mkdtemp");
    exit(2);
  }
  std::string root = dir;
  std::string config = root + "/config";
  if (ensure_dir(config) != 0)
  {
    perror("mkdir config");
    exit(2);
  }
  /* Orthanc 1.2.0 returns the configuration file path. The plugin
     keeps the prefix before the last backslash. */
  g_config_file = config + "\\orthanc.json";
  (void) name;
  return config;
}

/* Flat simplified-instance object, same spacing as Json::StyledWriter
   in jsoncpp 0.10.5 (indent 3, " : " between name and value). */
static std::string simplified_json(const char* patient,
                                   const char* study,
                                   const char* series_number,
                                   const char* series_description)
{
  const char* keys[4] = {
    "PatientName", "SeriesDescription", "SeriesNumber", "StudyDescription"
  };
  const char* values[4] = {
    patient, series_description, series_number, study
  };
  std::string document = "{";
  for (int i = 0; i < 4; i++)
  {
    document += "\n   \"";
    document += keys[i];
    document += "\" : \"";
    document += values[i];
    document += "\"";
    if (i + 1 != 4)
      document += ",";
  }
  document += "\n}\n";
  return document;
}

static int within(const std::string& path, const std::string& root)
{
  if (path.size() < root.size() || path.compare(0, root.size(), root) != 0)
    return 0;
  return path.size() == root.size() || path[root.size()] == '/';
}

static std::string outside_events(const std::string& storage)
{
  std::string detail;
  for (size_t i = 0; i < g_events.size(); i++)
  {
    const Event& event = g_events[i];
    int bad = 0;
    if (event.is_write)
      bad = !within(event.path, storage);
    else if (event.created_new)
      bad = !within(event.path, storage);
    if (!bad)
      continue;
    if (!detail.empty())
      detail += "; ";
    detail += event.is_write ? "wrote " : "created ";
    detail += event.path;
  }
  return detail;
}

static int read_file(const std::string& path, std::string* out)
{
  FILE* fp = fopen(path.c_str(), "rb");
  if (fp == NULL)
    return -1;
  char buffer[4096];
  size_t n = fread(buffer, 1, sizeof(buffer), fp);
  fclose(fp);
  out->assign(buffer, n);
  return 0;
}

static void store(const char* patient,
                  const char* study,
                  const char* series_number,
                  const char* series_description,
                  const std::string& bytes,
                  const char* instance_id)
{
  g_json = simplified_json(patient, study, series_number, series_description);
  g_bytes = bytes;
  OrthancPluginDicomInstance* instance = (OrthancPluginDicomInstance*) &g_bytes;
  OnStoredCallback(instance, instance_id);
}

static void test_registration()
{
  const char* name = "stored_callback_is_registered";
  if (g_registered != OnStoredCallback)
    fail(name, "OrthancPluginInitialize did not register OnStoredCallback");
}

static void test_two_parent_components()
{
  const char* name = "parent_components_do_not_escape_storage";
  std::string config = make_sandbox("two-parent");
  std::string storage = config + "/VPI_Storage";
  std::string bytes1 = std::string("INSTANCE-ONE-") + std::string(32, 'A');
  std::string bytes2 = std::string("INSTANCE-TWO-") + std::string(32, 'B');
  reset_events();
  store("..", "..", "1", "sender-folder", bytes1, "instance-1");
  store("..", "..", "1", "sender-folder", bytes2, "instance-2");

  std::string detail = outside_events(storage);
  std::string parent = config.substr(0, config.find_last_of('/'));
  std::string escaped1 = parent + "/1-sender-folder/1.dcm";
  std::string escaped2 = parent + "/1-sender-folder/2.dcm";
  std::string got;
  if (read_file(escaped1, &got) == 0)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "file " + escaped1 + " exists";
    if (got == bytes1)
      detail += " and contains the first instance";
  }
  if (read_file(escaped2, &got) == 0)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "file " + escaped2 + " exists";
    if (got == bytes2)
      detail += " and contains the second instance";
  }
  if (!detail.empty())
    fail(name, detail);
}

static void test_one_parent_component()
{
  const char* name = "single_parent_component_does_not_escape_storage";
  std::string config = make_sandbox("one-parent");
  std::string storage = config + "/VPI_Storage";
  std::string bytes = std::string("INSTANCE-PEER-") + std::string(24, 'C');
  reset_events();
  store("..", "KeptStudy", "9", "Peer", bytes, "instance-peer");

  std::string detail = outside_events(storage);
  std::string escaped = config + "/KeptStudy/9-Peer/1.dcm";
  std::string got;
  if (read_file(escaped, &got) == 0)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "file " + escaped + " exists";
    if (got == bytes)
      detail += " and contains the instance";
  }
  if (!detail.empty())
    fail(name, detail);
}

static void test_benign_name()
{
  const char* name = "benign_names_write_inside_storage";
  std::string config = make_sandbox("benign");
  std::string storage = config + "/VPI_Storage";
  std::string bytes = std::string("INSTANCE-OK-") + std::string(16, 'D');
  reset_events();
  store("Doe", "Head", "4", "Plain", bytes, "instance-ok");

  std::string detail = outside_events(storage);
  std::string expected = storage + "/Doe/Head/4-Plain/1.dcm";
  std::string got;
  if (read_file(expected, &got) != 0)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "missing " + expected;
  }
  else if (got != bytes)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "contents of " + expected + " are not the instance bytes";
  }
  if (!detail.empty())
    fail(name, detail);
}

static void test_separators_do_not_escape()
{
  const char* name = "separator_characters_do_not_escape_storage";
  std::string config = make_sandbox("separators");
  std::string storage = config + "/VPI_Storage";
  std::string bytes = std::string("INSTANCE-SEP-") + std::string(16, 'E');
  reset_events();
  store("..\\..\\loot", "a/../../b", "5", "ok", bytes, "instance-sep");

  std::string detail = outside_events(storage);
  std::string expected = storage + "/.._.._loot/a_.._.._b/5-ok/1.dcm";
  std::string got;
  if (read_file(expected, &got) != 0)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "missing sanitized file " + expected;
  }
  else if (got != bytes)
  {
    if (!detail.empty())
      detail += "; ";
    detail += "sanitized file contents differ";
  }
  if (!detail.empty())
    fail(name, detail);
}

int main()
{
  OrthancPluginContext context;
  memset(&context, 0, sizeof(context));
  context.orthancVersion = "1.2.0";
  context.Free = plugin_free;
  context.InvokeService = invoke_service;
  if (OrthancPluginInitialize(&context) != 0)
  {
    fprintf(stderr, "OrthancPluginInitialize failed\n");
    return 2;
  }

  test_registration();
  test_two_parent_components();
  test_one_parent_component();
  test_benign_name();
  test_separators_do_not_escape();

  if (g_failures != 0)
  {
    fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
