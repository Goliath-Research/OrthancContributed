/* Drives OnStoredCallback with the simplified JSON Orthanc 1.2.0 hands
   the plugin after a stored instance.

   For an ASCII series number and series description of at most 256
   bytes, that JSON is the tag text itself:
   - DCMTK 3.6.0 DcmByteString::getString returns the stored bytes and
     does not apply the VR maximum (12 for IS, 64 for LO).
   - Orthanc keeps the value when its length is <= ORTHANC_MAXIMUM_TAG_LENGTH
     (256) and names it SeriesNumber / SeriesDescription.
   - JsonCpp 0.10.5 StyledWriter emits `"Name" : "value"` for those
     characters, which getDicomTag searches for.

   The callback then joins the two labels with a hyphen and copies the
   result into a 100-byte static buffer. These tests require that copy
   to stay inside the buffer, and that the series counter still tells
   two labels apart when they differ past the 99th character of the
   joined name. */

#include <orthanc/OrthancCPlugin.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern OrthancPluginErrorCode OnStoredCallback(OrthancPluginDicomInstance* instance,
                                                const char* instanceId);

extern "C" int32_t OrthancPluginInitialize(OrthancPluginContext* c);

static char g_storageRoot[512];
static const char* g_simplifiedJson;
static int g_instanceSerial = 0;

static const char kInstanceBytes[] = "TEST";

static char* Dup(const char* text)
{
  size_t n = strlen(text);
  char* copy = (char*) malloc(n + 1);
  if (copy == NULL)
  {
    abort();
  }
  memcpy(copy, text, n + 1);
  return copy;
}

static void Fill(char* dest, size_t n, char ch)
{
  memset(dest, ch, n);
  dest[n] = '\0';
}

/* Human simplified JSON, StyledWriter spacing, no extra escaping. */
static char* SimplifiedJson(const char* patient,
                            const char* study,
                            const char* seriesNumber,
                            const char* seriesDescription)
{
  const char* format =
      "{\n"
      "   \"PatientName\" : \"%s\",\n"
      "   \"SeriesDescription\" : \"%s\",\n"
      "   \"SeriesNumber\" : \"%s\",\n"
      "   \"StudyDescription\" : \"%s\"\n"
      "}\n";
  size_t needed = strlen(format) + strlen(patient) + strlen(study)
      + strlen(seriesNumber) + strlen(seriesDescription) + 1;
  char* json = (char*) malloc(needed);
  if (json == NULL)
  {
    abort();
  }
  snprintf(json, needed, format, patient, seriesDescription, seriesNumber, study);
  return json;
}

static void CoreFree(void* buffer)
{
  free(buffer);
}

static OrthancPluginErrorCode InvokeService(OrthancPluginContext* context,
                                             _OrthancPluginService service,
                                             const void* params)
{
  (void) context;

  switch (service)
  {
    case _OrthancPluginService_LogError:
    case _OrthancPluginService_LogWarning:
    case _OrthancPluginService_LogInfo:
      return OrthancPluginErrorCode_Success;

    case _OrthancPluginService_GetOrthancPath:
    case _OrthancPluginService_GetOrthancDirectory:
    case _OrthancPluginService_GetConfiguration:
    {
      _OrthancPluginRetrieveDynamicString* p =
          (_OrthancPluginRetrieveDynamicString*) params;
      *p->result = Dup("unused");
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetConfigurationPath:
    {
      /* OnStoredCallback writes a NUL over the last backslash. */
      _OrthancPluginRetrieveDynamicString* p =
          (_OrthancPluginRetrieveDynamicString*) params;
      char path[600];
      snprintf(path, sizeof(path), "%s\\Configuration.json", g_storageRoot);
      *p->result = Dup(path);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetCommandLineArgumentsCount:
    {
      _OrthancPluginReturnSingleValue* p = (_OrthancPluginReturnSingleValue*) params;
      *p->resultUint32 = 0;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_RegisterRestCallback:
    case _OrthancPluginService_RegisterOnStoredInstanceCallback:
    case _OrthancPluginService_RegisterOnChangeCallback:
    case _OrthancPluginService_SetPluginProperty:
      return OrthancPluginErrorCode_Success;

    case _OrthancPluginService_RestApiGet:
    {
      _OrthancPluginRestApiGet* p = (_OrthancPluginRestApiGet*) params;
      p->target->data = Dup("{}");
      p->target->size = 2;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceRemoteAet:
    {
      _OrthancPluginAccessDicomInstance* p =
          (_OrthancPluginAccessDicomInstance*) params;
      *p->resultString = "STORESCU";
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSize:
    {
      _OrthancPluginAccessDicomInstance* p =
          (_OrthancPluginAccessDicomInstance*) params;
      *p->resultInt64 = (int64_t) sizeof(kInstanceBytes) - 1;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceData:
    {
      _OrthancPluginAccessDicomInstance* p =
          (_OrthancPluginAccessDicomInstance*) params;
      *p->resultString = kInstanceBytes;
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceSimplifiedJson:
    {
      _OrthancPluginAccessDicomInstance* p =
          (_OrthancPluginAccessDicomInstance*) params;
      *p->resultStringToFree = Dup(g_simplifiedJson);
      return OrthancPluginErrorCode_Success;
    }

    case _OrthancPluginService_GetInstanceOrigin:
    {
      _OrthancPluginAccessDicomInstance* p =
          (_OrthancPluginAccessDicomInstance*) params;
      *p->resultOrigin = OrthancPluginInstanceOrigin_DicomProtocol;
      return OrthancPluginErrorCode_Success;
    }

    default:
      fprintf(stderr, "unexpected plugin service %d\n", (int) service);
      return OrthancPluginErrorCode_Plugin;
  }
}

static int Store(const char* seriesNumber, const char* seriesDescription)
{
  char* json = SimplifiedJson("Pat", "Study", seriesNumber, seriesDescription);
  g_simplifiedJson = json;

  char instanceId[32];
  snprintf(instanceId, sizeof(instanceId), "instance-%d", ++g_instanceSerial);

  OrthancPluginErrorCode code = OnStoredCallback(NULL, instanceId);
  free(json);
  g_simplifiedJson = NULL;
  return code == OrthancPluginErrorCode_Success ? 0 : 1;
}

static int FileExists(const char* seriesDir, const char* leaf)
{
  char path[1200];
  snprintf(path, sizeof(path), "%s/VPI_Storage/Pat/Study/%s/%s",
           g_storageRoot, seriesDir, leaf);
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int ExpectFile(const char* seriesDir, const char* leaf, int present)
{
  int found = FileExists(seriesDir, leaf);
  if (found != present)
  {
    fprintf(stderr, "expected %s/%s to be %s\n",
            seriesDir, leaf, present ? "present" : "absent");
    return 1;
  }
  return 0;
}

static void Joined(char* dest, size_t destSize,
                   const char* seriesNumber, const char* seriesDescription)
{
  snprintf(dest, destSize, "%s-%s", seriesNumber, seriesDescription);
}

/* Labels that fit in the 100-byte series buffer, including the hyphen. */
static int TestFittingLabels(void)
{
  char number[41];
  char description[41];
  char otherDescription[41];
  char seriesA[90];
  char seriesB[90];
  int failed = 0;

  Fill(number, 40, '1');
  Fill(description, 40, 'A');
  Fill(otherDescription, 40, 'B');
  Joined(seriesA, sizeof(seriesA), number, description);
  Joined(seriesB, sizeof(seriesB), number, otherDescription);

  failed |= Store(number, description);
  failed |= Store(number, description);
  failed |= Store(number, otherDescription);

  failed |= ExpectFile(seriesA, "1.dcm", 1);
  failed |= ExpectFile(seriesA, "2.dcm", 1);
  failed |= ExpectFile(seriesA, "3.dcm", 0);
  failed |= ExpectFile(seriesB, "1.dcm", 1);
  failed |= ExpectFile(seriesB, "2.dcm", 0);
  return failed;
}

/* Each label is 98 characters, under the per-tag cap of strlen < 99.
   The joined folder name is 197 characters and must not disturb the
   series counter or be confused with a label that differs at the end. */
static int TestMaxAcceptedLabels(void)
{
  char number[99];
  char descriptionA[99];
  char descriptionB[99];
  char seriesA[220];
  char seriesB[220];
  int failed = 0;

  Fill(number, 98, '1');
  Fill(descriptionA, 98, 'A');
  Fill(descriptionB, 98, 'A');
  descriptionB[97] = 'B';
  Joined(seriesA, sizeof(seriesA), number, descriptionA);
  Joined(seriesB, sizeof(seriesB), number, descriptionB);

  failed |= Store(number, descriptionA);
  failed |= Store(number, descriptionA);
  failed |= Store(number, descriptionB);

  failed |= ExpectFile(seriesA, "1.dcm", 1);
  failed |= ExpectFile(seriesA, "2.dcm", 1);
  failed |= ExpectFile(seriesA, "3.dcm", 0);
  failed |= ExpectFile(seriesB, "1.dcm", 1);
  failed |= ExpectFile(seriesB, "2.dcm", 0);
  return failed;
}

/* Smallest joined name that does not fit: 98 + '-' + 1 = 100 characters
   plus the terminating NUL. A second label that shares the first 99
   characters must still start its own series at 1.dcm. */
static int TestJoinedNameOnePastBuffer(void)
{
  char number[99];
  char seriesX[120];
  char seriesY[120];
  int failed = 0;

  Fill(number, 98, '1');
  Joined(seriesX, sizeof(seriesX), number, "X");
  Joined(seriesY, sizeof(seriesY), number, "Y");

  failed |= Store(number, "X");
  failed |= Store(number, "X");
  failed |= Store(number, "Y");

  failed |= ExpectFile(seriesX, "1.dcm", 1);
  failed |= ExpectFile(seriesX, "2.dcm", 1);
  failed |= ExpectFile(seriesX, "3.dcm", 0);
  failed |= ExpectFile(seriesY, "1.dcm", 1);
  failed |= ExpectFile(seriesY, "2.dcm", 0);
  return failed;
}

static OrthancPluginContext g_context;

static int PrepareStorage(void)
{
  char templ[] = "/tmp/vpi-series-label-XXXXXX";
  char* dir = mkdtemp(templ);
  if (dir == NULL)
  {
    perror("mkdtemp");
    return 1;
  }
  snprintf(g_storageRoot, sizeof(g_storageRoot), "%s", dir);

  memset(&g_context, 0, sizeof(g_context));
  g_context.orthancVersion = "1.2.0";
  g_context.Free = CoreFree;
  g_context.InvokeService = InvokeService;

  if (OrthancPluginInitialize(&g_context) != 0)
  {
    fprintf(stderr, "plugin initialization failed\n");
    return 1;
  }
  return 0;
}

int main(int argc, char** argv)
{
  int failed;

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s fitting|max-accepted|one-past\n", argv[0]);
    return 2;
  }

  if (PrepareStorage() != 0)
  {
    return 1;
  }

  if (strcmp(argv[1], "fitting") == 0)
  {
    failed = TestFittingLabels();
  }
  else if (strcmp(argv[1], "max-accepted") == 0)
  {
    failed = TestMaxAcceptedLabels();
  }
  else if (strcmp(argv[1], "one-past") == 0)
  {
    failed = TestJoinedNameOnePastBuffer();
  }
  else
  {
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
  }

  if (failed)
  {
    fprintf(stderr, "case %s failed under %s\n", argv[1], g_storageRoot);
    return 1;
  }

  printf("case %s passed\n", argv[1]);
  return 0;
}
