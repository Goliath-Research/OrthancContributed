/* Spec for the VPI Reveal store callback.
   These checks fail while patient and study text is written to process
   output, and pass once that text is no longer written.

   The JSON fed to the callback is built by the same steps Orthanc 1.2.0
   uses after a C-STORE or POST /instances:
     DICOM string -> Latin1/ISO-8859-1 to UTF-8 (default encoding)
     -> full JSON node -> SimplifyTags(Human) -> jsoncpp 0.10.5 StyledWriter.
   The callback registered at startup is then invoked the way
   OrthancPlugins::SignalStoredInstance invokes it. */

#include <orthanc/OrthancCPlugin.h>

#include <assert.h>
#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

extern "C" OrthancPluginErrorCode OrthancPluginInitialize(OrthancPluginContext* context);

namespace {

std::string gSimplifiedJson;
std::string gWarnings;
const char* gInstanceBytes = "DICM";
OrthancPluginOnStoredInstanceCallback gStoredCallback = NULL;
OrthancPluginInstanceOrigin gOrigin = OrthancPluginInstanceOrigin_DicomProtocol;
const char* gRemoteAet = "MODALITY";

char* CopyCString(const std::string& text)
{
  char* copy = static_cast<char*>(malloc(text.size() + 1));
  if (copy == NULL)
  {
    abort();
  }
  memcpy(copy, text.c_str(), text.size() + 1);
  return copy;
}

void FreeBuffer(void* buffer)
{
  free(buffer);
}

/* Boost 1.60 iconv_to_utf opens iconv from the source charset to UTF-8.
   Orthanc's default Specific Character Set fallback is Latin1, whose
   Boost charset name is ISO-8859-1. */
std::string Latin1ToUtf8(const std::string& latin1)
{
  iconv_t converter = iconv_open("UTF-8", "ISO-8859-1");
  if (converter == (iconv_t) -1)
  {
    perror("iconv_open");
    exit(2);
  }

  std::string input = latin1;
  size_t inLeft = input.size();
  size_t outLeft = input.size() * 4 + 1;
  std::string output(outLeft, '\0');
  char* inPointer = &input[0];
  char* outPointer = &output[0];
  size_t converted = iconv(converter, &inPointer, &inLeft, &outPointer, &outLeft);
  iconv_close(converter);
  if (converted == (size_t) -1)
  {
    perror("iconv");
    exit(2);
  }
  output.resize(output.size() - outLeft);
  return output;
}

struct FullTag
{
  std::string name;
  std::string type;
  std::string value;
};

/* DicomTag::Format uses "%04x,%04x". */
std::string FormatTag(unsigned group, unsigned element)
{
  char formatted[16];
  sprintf(formatted, "%04x,%04x", group, element);
  return formatted;
}

/* ServerToolbox::SimplifyTags with DicomToJsonFormat_Human copies each
   string node's Name to the key and its Value to the string. */
std::map<std::string, std::string> SimplifyHuman(const std::map<std::string, FullTag>& source)
{
  std::map<std::string, std::string> simplified;
  for (std::map<std::string, FullTag>::const_iterator it = source.begin();
       it != source.end(); ++it)
  {
    if (it->second.type == "String")
    {
      simplified[it->second.name] = it->second.value;
    }
  }
  return simplified;
}

/* jsoncpp 0.10.5 StyledWriter for an object of strings: indent of three
   spaces, " : " between name and value, trailing newline. Member order
   follows std::map, which is the order jsoncpp's std::map returns. */
std::string StyledWriteStrings(const std::map<std::string, std::string>& fields)
{
  std::string document = "{";
  const std::string indent(3, ' ');
  bool first = true;
  for (std::map<std::string, std::string>::const_iterator it = fields.begin();
       it != fields.end(); ++it)
  {
    if (!first)
    {
      document += ",";
    }
    first = false;
    document += "\n";
    document += indent;
    document += "\"";
    document += it->first;
    document += "\" : \"";
    document += it->second;
    document += "\"";
  }
  document += "\n}\n";
  return document;
}

void AddStringTag(std::map<std::string, FullTag>& full,
                  unsigned group,
                  unsigned element,
                  const char* name,
                  const std::string& value)
{
  FullTag node;
  node.name = name;
  node.type = "String";
  node.value = value;
  full[FormatTag(group, element)] = node;
}

std::string SimplifiedJsonFor(const std::string& patientName,
                              const std::string& patientId,
                              const std::string& studyDescription,
                              const std::string& seriesNumber,
                              const std::string& seriesDescription)
{
  std::map<std::string, FullTag> full;
  /* Names: DicomTag::GetMainTagsName for PatientName and PatientID.
     DCMTK 3.6.0 dicom.dic for the other three, which FromDcmtkBridge
     returns via DcmTag::getTagName when GetMainTagsName is empty. */
  AddStringTag(full, 0x0010, 0x0010, "PatientName", patientName);
  AddStringTag(full, 0x0010, 0x0020, "PatientID", patientId);
  AddStringTag(full, 0x0008, 0x1030, "StudyDescription", studyDescription);
  AddStringTag(full, 0x0020, 0x0011, "SeriesNumber", seriesNumber);
  AddStringTag(full, 0x0008, 0x103e, "SeriesDescription", seriesDescription);
  return StyledWriteStrings(SimplifyHuman(full));
}

OrthancPluginErrorCode InvokeService(OrthancPluginContext* context,
                                     _OrthancPluginService service,
                                     const void* parameters)
{
  (void) context;
  switch (service)
  {
    case _OrthancPluginService_LogWarning:
    case _OrthancPluginService_LogError:
    case _OrthancPluginService_LogInfo:
      /* PluginsManager writes LogWarning to LOG(WARNING), whose default
         stream is stderr. Record the message that would be written. */
      gWarnings += reinterpret_cast<const char*>(parameters);
      gWarnings += "\n";
      return OrthancPluginErrorCode_Success;

    case _OrthancPluginService_GetOrthancPath:
    case _OrthancPluginService_GetOrthancDirectory:
    case _OrthancPluginService_GetConfiguration:
    {
      const _OrthancPluginRetrieveDynamicString* params =
        reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters);
      const char* value = (service == _OrthancPluginService_GetConfiguration) ? "{}" : "/opt/orthanc";
      *params->result = CopyCString(value);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetConfigurationPath:
    {
      const _OrthancPluginRetrieveDynamicString* params =
        reinterpret_cast<const _OrthancPluginRetrieveDynamicString*>(parameters);
      /* A backslash is required: the callback truncates at the last one. */
      *params->result = CopyCString("C:\\Orthanc\\Configuration.json");
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetCommandLineArgumentsCount:
    {
      const _OrthancPluginReturnSingleValue* params =
        reinterpret_cast<const _OrthancPluginReturnSingleValue*>(parameters);
      *params->resultUint32 = 0;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_RegisterOnStoredInstanceCallback:
    {
      const _OrthancPluginOnStoredInstanceCallback* params =
        reinterpret_cast<const _OrthancPluginOnStoredInstanceCallback*>(parameters);
      gStoredCallback = params->callback;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_RegisterOnChangeCallback:
    case _OrthancPluginService_RegisterRestCallback:
    case _OrthancPluginService_SetPluginProperty:
      return OrthancPluginErrorCode_Success;

    case _OrthancPluginService_RestApiGet:
    {
      const _OrthancPluginRestApiGet* params =
        reinterpret_cast<const _OrthancPluginRestApiGet*>(parameters);
      params->target->data = CopyCString("{}");
      params->target->size = 2;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSimplifiedJson:
    {
      const _OrthancPluginAccessDicomInstance* params =
        reinterpret_cast<const _OrthancPluginAccessDicomInstance*>(parameters);
      *params->resultStringToFree = CopyCString(gSimplifiedJson);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSize:
    {
      const _OrthancPluginAccessDicomInstance* params =
        reinterpret_cast<const _OrthancPluginAccessDicomInstance*>(parameters);
      *params->resultInt64 = 4;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceData:
    {
      const _OrthancPluginAccessDicomInstance* params =
        reinterpret_cast<const _OrthancPluginAccessDicomInstance*>(parameters);
      *params->resultString = gInstanceBytes;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceRemoteAet:
    {
      const _OrthancPluginAccessDicomInstance* params =
        reinterpret_cast<const _OrthancPluginAccessDicomInstance*>(parameters);
      *params->resultString = gRemoteAet;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceOrigin:
    {
      const _OrthancPluginAccessDicomInstance* params =
        reinterpret_cast<const _OrthancPluginAccessDicomInstance*>(parameters);
      *params->resultOrigin = gOrigin;
      return OrthancPluginErrorCode_Success;
    }

    default:
      fprintf(stderr, "unexpected plugin service %d\n", static_cast<int>(service));
      return OrthancPluginErrorCode_Plugin;
  }
}

struct Capture
{
  int saved;
  int pipes[2];
};

Capture BeginCapture(FILE* stream)
{
  Capture capture;
  fflush(stream);
  if (pipe(capture.pipes) != 0)
  {
    perror("pipe");
    exit(2);
  }
  capture.saved = dup(fileno(stream));
  if (capture.saved < 0 || dup2(capture.pipes[1], fileno(stream)) < 0)
  {
    perror("dup");
    exit(2);
  }
  /* A console session line-buffers stdout, so the newline in printf flushes. */
  setvbuf(stream, NULL, _IOLBF, 0);
  return capture;
}

std::string EndCapture(FILE* stream, Capture capture)
{
  fflush(stream);
  if (dup2(capture.saved, fileno(stream)) < 0)
  {
    perror("dup2");
    exit(2);
  }
  close(capture.saved);
  close(capture.pipes[1]);
  std::string data;
  char buffer[4096];
  ssize_t count;
  while ((count = read(capture.pipes[0], buffer, sizeof(buffer))) > 0)
  {
    data.append(buffer, static_cast<size_t>(count));
  }
  close(capture.pipes[0]);
  setvbuf(stream, NULL, _IOLBF, 0);
  return data;
}

struct StoredInstance
{
  const char* instanceId;
  const char* seriesNumber;
  const char* seriesDescription;
};

int failures = 0;

void Require(bool condition, const char* name, const std::string& detail)
{
  if (condition)
  {
    printf("PASS %s\n", name);
    return;
  }
  printf("FAIL %s\n%s\n", name, detail.c_str());
  failures++;
}

void StoreAndCheck(const char* name,
                   const StoredInstance& instance,
                   const std::string& patientName,
                   const std::string& patientId,
                   const std::string& studyDescription)
{
  gSimplifiedJson = SimplifiedJsonFor(patientName, patientId, studyDescription,
                                      instance.seriesNumber, instance.seriesDescription);
  gWarnings.clear();

  if (gSimplifiedJson.find(patientName) == std::string::npos ||
      gSimplifiedJson.find(patientId) == std::string::npos ||
      gSimplifiedJson.find(studyDescription) == std::string::npos ||
      gSimplifiedJson.find("\"PatientName\" : \"") == std::string::npos)
  {
    fprintf(stderr, "harness JSON is missing the stored identifiers\n%s\n",
            gSimplifiedJson.c_str());
    exit(2);
  }

  Capture stdoutCapture = BeginCapture(stdout);
  Capture stderrCapture = BeginCapture(stderr);
  OrthancPluginDicomInstance* dicom =
    reinterpret_cast<OrthancPluginDicomInstance*>(static_cast<intptr_t>(1));
  OrthancPluginErrorCode code = gStoredCallback(dicom, instance.instanceId);
  std::string stdoutText = EndCapture(stdout, stdoutCapture);
  std::string stderrText = EndCapture(stderr, stderrCapture);

  std::string processOutput = stdoutText + stderrText + gWarnings;
  std::vector<std::string> forbidden;
  forbidden.push_back(patientName);
  forbidden.push_back(patientId);
  forbidden.push_back(studyDescription);

  std::string detail;
  bool clean = (code == OrthancPluginErrorCode_Success);
  if (!clean)
  {
    detail += "callback did not succeed\n";
  }
  for (size_t i = 0; i < forbidden.size(); i++)
  {
    if (processOutput.find(forbidden[i]) != std::string::npos)
    {
      clean = false;
      detail += "process output contains " + forbidden[i] + "\n";
    }
  }
  if (!clean)
  {
    detail += "--- stdout ---\n" + stdoutText;
    detail += "--- stderr ---\n" + stderrText;
    detail += "--- warning log ---\n" + gWarnings;
  }
  Require(clean, name, detail);
}

}  // namespace

int main()
{
  char tempDirectory[] = "/tmp/vpi-phi-XXXXXX";
  if (mkdtemp(tempDirectory) == NULL)
  {
    perror("mkdtemp");
    return 2;
  }
  if (chdir(tempDirectory) != 0)
  {
    perror("chdir");
    return 2;
  }

  OrthancPluginContext context;
  memset(&context, 0, sizeof(context));
  context.orthancVersion = "1.2.0";
  context.Free = FreeBuffer;
  context.InvokeService = InvokeService;

  if (OrthancPluginInitialize(&context) != 0 || gStoredCallback == NULL)
  {
    fprintf(stderr, "plugin did not register an OnStoredInstance callback\n");
    return 2;
  }

  /* ASCII identifiers. iconv ISO-8859-1 -> UTF-8 is the conversion
     ConvertLeafElement applies for the default Latin1 encoding. */
  const std::string patientName = Latin1ToUtf8("DOE^JANE");
  const std::string patientId = Latin1ToUtf8("PID77821");
  const std::string studyDescription = Latin1ToUtf8("CHESTCT");
  if (patientName != "DOE^JANE" || patientId != "PID77821" || studyDescription != "CHESTCT")
  {
    fprintf(stderr, "ISO-8859-1 conversion changed an ASCII identifier\n");
    return 2;
  }

  gWarnings.clear();

  StoredInstance first = { "orthanc-instance-1", "4", "AXIAL" };
  StoreAndCheck("first_instance_of_series_does_not_write_patient_or_study_text",
                first, patientName, patientId, studyDescription);

  StoredInstance second = { "orthanc-instance-2", "4", "AXIAL" };
  StoreAndCheck("later_instance_of_same_series_does_not_write_patient_or_study_text",
                second, patientName, patientId, studyDescription);

  StoredInstance otherSeries = { "orthanc-instance-3", "5", "SAG" };
  StoreAndCheck("first_instance_of_another_series_does_not_write_patient_or_study_text",
                otherSeries, patientName, patientId, studyDescription);

  StoredInstance back = { "orthanc-instance-4", "4", "AXIAL" };
  StoreAndCheck("return_to_earlier_series_does_not_write_patient_or_study_text",
                back, patientName, patientId, studyDescription);

  if (failures != 0)
  {
    printf("%d disclosure check(s) failed\n", failures);
    return 1;
  }
  printf("all disclosure checks passed\n");
  return 0;
}
