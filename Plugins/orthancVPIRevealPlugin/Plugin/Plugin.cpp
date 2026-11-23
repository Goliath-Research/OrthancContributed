/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include <orthanc/OrthancCPlugin.h>

#include <string.h>
#include <stdio.h>
#include <direct.h>

static OrthancPluginContext* context = NULL;
static OrthancPluginErrorCode customError;


static OrthancPluginErrorCode CallbackCreateDicom(OrthancPluginRestOutput* output,
	const char* url,
	const OrthancPluginHttpRequest* request)
{
	OrthancPluginLogWarning(context, "+++ CallbackCreateDicom");

	const char* pathLocator = "\"Path\" : \"";
	char info[1024];
	char *id, *eos;
	OrthancPluginMemoryBuffer tmp;

	if (request->method != OrthancPluginHttpMethod_Post)
	{
		OrthancPluginSendMethodNotAllowed(context, output, "POST");
	}
	else
	{
		/* Make POST request to create a new DICOM instance */
		snprintf(info, sizeof(info), "{\"PatientName\":\"Test\"}");
		OrthancPluginRestApiPost(context, &tmp, "/tools/create-dicom", info, strlen(info));

		/**
		* Recover the ID of the created instance is constructed by a
		* quick-and-dirty parsing of a JSON string.
		**/
		id = strstr((char*)tmp.data, pathLocator) + strlen(pathLocator);
		eos = strchr(id, '\"');
		eos[0] = '\0';

		/* Delete the newly created DICOM instance. */
		OrthancPluginRestApiDelete(context, id);
		OrthancPluginFreeMemoryBuffer(context, &tmp);

		/* Set some cookie */
		OrthancPluginSetCookie(context, output, "hello", "world");

		/* Set some HTTP header */
		OrthancPluginSetHttpHeader(context, output, "Cache-Control", "max-age=0, no-cache");

		OrthancPluginAnswerBuffer(context, output, "OK\n", 3, "text/plain");
	}

	return OrthancPluginErrorCode_Success;
}


// True when snprintf wrote its whole output into a buffer of the given size.
static bool formatFits(int written, size_t size)
{
	return written >= 0 && (size_t) written < size;
}

// Getting the tags of the created instance is done by a quick-and-dirty parsing of a JSON string.
static void getDicomTag(char* json, const char* tag, char* tagValue, unsigned int len)
{
	char *jsonText = (char *)malloc((strlen(json) + 1) * sizeof(char));
	strcpy(jsonText, json);

	char tagText[100];
	if (formatFits(snprintf(tagText, sizeof(tagText), "\"%s\" : \"", tag), sizeof(tagText)))
	{

		//char buffer[10000];
		//sprintf(buffer, "tagText[%d]=%s", strlen(tagText), tagText);
		//OrthancPluginLogWarning(context, buffer);

		char* value = strstr(jsonText, tagText);
		int offset = strlen(tagText);
		value = value + offset;
		char* eos = strchr(value, '\"');
		eos[0] = '\0';
		//sprintf(buffer, "value[%d]=%s", strlen(value), value);
		//OrthancPluginLogWarning(context, buffer);

		if (strlen(value) < len-1)
			strcpy(tagValue, value);
		else
			strcpy(tagValue, "TAG ERROR");
	}
	else
		strcpy(tagValue, "VALUE ERROR");

	free(jsonText);
}

static bool createDir(char* dir)
{
	char buffer[1000];
	int ret = _mkdir(dir);
	errno_t err;

	if ( ret == 0)
	{
		snprintf(buffer, sizeof(buffer), "    Directory %s created", dir);
		OrthancPluginLogWarning(context, buffer);
		return true;
	}

	_get_errno(&err);

	if ( err == EEXIST)
	{
		//sprintf(buffer, "    Directory %s already exists", dir);
		//OrthancPluginLogWarning(context, buffer);
		return true;
	}

	snprintf(buffer, sizeof(buffer), "--- Problem creating directory %s", dir);
	OrthancPluginLogWarning(context, buffer);
	OrthancPluginExtendOrthancExplorer(context, buffer);
	OrthancPluginExtendOrthancExplorer(context, buffer);
	return false;
}

static bool appendDir(char* dir, size_t size, const char* part)
{
	size_t used = strlen(dir);
	return formatFits(snprintf(dir + used, size - used, "/%s", part), size - used);
}

static bool createDir(const char* topDir, char* patientName, char* studyDescription, char* seriesDescription)
{
	char dir[1000];

	if (!formatFits(snprintf(dir, sizeof(dir), "%s", topDir), sizeof(dir))) return false;
	if (!createDir(dir)) return false;

	if (!appendDir(dir, sizeof(dir), patientName)) return false;
	if (!createDir(dir)) return false;

	if (!appendDir(dir, sizeof(dir), studyDescription)) return false;
	if (!createDir(dir)) return false;

	if (!appendDir(dir, sizeof(dir), seriesDescription)) return false;
	if (!createDir(dir)) return false;

	return true;
}


void cdTop(char* configurationPath)
{
	char buffer[1000];

	// Change directory to the top of the data storage
	snprintf(buffer, sizeof(buffer), "cd %s", configurationPath);
	//OrthancPluginLogWarning(context, buffer);
	int ret = _chdir(configurationPath);

	if (ret != 0)
	{
		snprintf(buffer, sizeof(buffer), "--- ERROR: _chdir fails");
		OrthancPluginLogWarning(context, buffer);
	}
}


void logCwd()
{
	// log the current working directory   
	const unsigned int maxDir = 1000;
	char cwd[maxDir];
	char buffer[maxDir];

	if (_getcwd(cwd, maxDir) == NULL)
	{
		snprintf(buffer, sizeof(buffer), "--- ERROR: _getcwd fails");
		OrthancPluginLogWarning(context, buffer);
	}
	else
	{
		snprintf(buffer, sizeof(buffer), "+++ CWD = %s", cwd);
		OrthancPluginLogWarning(context, buffer);
	}
}

void replaceIllegalChars(char* text)
{
	for (size_t i = 0; i < strlen(text); i++)
	{
		switch (text[i])
		{
		case '\\':
		case '/':
		case ':':
		case '*':
		case '?':
		case '\"':
		case '<':
		case '>':
		case '|':
			text[i] = '_';
			break;
		}
	}
}

OrthancPluginErrorCode OnStoredCallback(OrthancPluginDicomInstance* instance,
	const char* instanceId)
{
  const unsigned int maxLen = 100;
  const unsigned int maxJson = 10000;
  const unsigned int maxDir = 1000;

  char buffer[maxJson];
  char patientName[maxLen];
  char studyDescription[maxLen];
  char seriesNumber[maxLen];
  char seriesDescription[maxLen];
  char seriesDir[maxDir];
  char location[maxDir];

  static char previousSeriesDir[maxDir];
  static int count;
  int ret;
  OrthancPluginErrorCode returnCode;

  FILE* fp;
  char* json;

  snprintf(buffer, sizeof(buffer), "+++ OnStoredCallback: Received DICOM instance of size %d and ID %s from origin %d (AET %s)", 
          (int) OrthancPluginGetInstanceSize(context, instance), instanceId, 
          OrthancPluginGetInstanceOrigin(context, instance),
          OrthancPluginGetInstanceRemoteAet(context, instance));
  OrthancPluginLogWarning(context, buffer);  

  json = OrthancPluginGetInstanceSimplifiedJson(context, instance);
  //OrthancPluginLogWarning(context, "+++ json=");
  //OrthancPluginLogWarning(context, json);

  // Getting the tags of the created instance is done by a quick-and-dirty parsing of a JSON string.
  getDicomTag(json, "PatientName", patientName, maxLen);
  getDicomTag(json, "StudyDescription", studyDescription, maxLen);
  getDicomTag(json, "SeriesNumber", seriesNumber, maxLen);
  getDicomTag(json, "SeriesDescription", seriesDescription, maxLen);

  snprintf(buffer, sizeof(buffer), "    PatientName=%s", patientName);
  OrthancPluginLogWarning(context, buffer);
  snprintf(buffer, sizeof(buffer), "    StudyDescription=%s", studyDescription);
  OrthancPluginLogWarning(context, buffer);
  snprintf(buffer, sizeof(buffer), "    SeriesNumber=%s", seriesNumber);
  OrthancPluginLogWarning(context, buffer);
  snprintf(buffer, sizeof(buffer), "    SeriesDescription=%s", seriesDescription);
  OrthancPluginLogWarning(context, buffer);

  replaceIllegalChars(patientName);
  replaceIllegalChars(studyDescription);
  replaceIllegalChars(seriesNumber);
  replaceIllegalChars(seriesDescription);

  if (!formatFits(snprintf(seriesDir, sizeof(seriesDir), "%s-%s", seriesNumber, seriesDescription), sizeof(seriesDir)))
  {
	  OrthancPluginLogError(context, "--- Series directory name is too long");
	  OrthancPluginFreeString(context, json);
	  return OrthancPluginErrorCode_CannotWriteFile;
  }

  snprintf(buffer, sizeof(buffer), "    previousSeriesDir=%s", previousSeriesDir);
  OrthancPluginLogWarning(context, buffer);
  snprintf(buffer, sizeof(buffer), "    seriesDir=%s", seriesDir);
  OrthancPluginLogWarning(context, buffer);

  if (strcmp(previousSeriesDir, seriesDir) != 0)
	  count = 1;	// new series

  if (count == 1)	// Only for the first DICOM instance of the series
  {
    printf("[%s]\n", json);

	snprintf(buffer, sizeof(buffer), "    PatientName=%s", patientName);
	OrthancPluginLogWarning(context, buffer);
	snprintf(buffer, sizeof(buffer), "    StudyDescription=%s", studyDescription);
	OrthancPluginLogWarning(context, buffer);
	snprintf(buffer, sizeof(buffer), "    SeriesNumber=%s", seriesNumber);
	OrthancPluginLogWarning(context, buffer);
	snprintf(buffer, sizeof(buffer), "    SeriesDescription=%s", seriesDescription);
	OrthancPluginLogWarning(context, buffer);
  }

  // Get directory to the top of the data storage
  char* configurationPath = OrthancPluginGetConfigurationPath(context);
  char* lastSlash = strrchr(configurationPath, '\\');
  lastSlash[0] = '\0';		// remove file name

  cdTop(configurationPath);	// Change directory to the top of the data storage
  logCwd();					// log the current working directory

  if (!formatFits(snprintf(location, sizeof(location), "VPI_Storage/%s/%s/%s", patientName, studyDescription, seriesDir), sizeof(location)))
  {
	  OrthancPluginLogError(context, "--- Storage location is too long");
	  OrthancPluginFreeString(context, json);
	  return OrthancPluginErrorCode_CannotWriteFile;
  }

  snprintf(buffer, sizeof(buffer), "+++ Writing instance %s\\%d.dcm", location, count);
  OrthancPluginLogWarning(context, buffer);

  if (createDir("VPI_Storage", patientName, studyDescription, seriesDir))
  {
	  // Change directory
	  snprintf(buffer, sizeof(buffer), "cd %s", location);
	  OrthancPluginLogWarning(context, buffer);
	  ret = _chdir(location);

	  if (ret != 0)
	  {
		  snprintf(buffer, sizeof(buffer), "--- ERROR: _chdir fails");
		  OrthancPluginLogWarning(context, buffer);
	  }

	  logCwd();   // log the current working directory

	  // write DICOM file to disc
	  snprintf(buffer, sizeof(buffer), "%d.dcm", count);
	  fp = fopen(buffer, "wb");
	  fwrite(OrthancPluginGetInstanceData(context, instance),
		  (size_t)OrthancPluginGetInstanceSize(context, instance), 1, fp);
	  fclose(fp);
	  returnCode = OrthancPluginErrorCode_Success;
  }
  else
  {
	  OrthancPluginLogError(context, "--- Failure to create directory path. Create it by hand and try again.");
	  returnCode = OrthancPluginErrorCode_CannotWriteFile;
  }

  snprintf(previousSeriesDir, sizeof(previousSeriesDir), "%s", seriesDir);
  count++;
  OrthancPluginLogWarning(context, ">>> cont incremented");
  OrthancPluginFreeString(context, json);

  cdTop(configurationPath);	// Change directory back to the top of the data storage
  logCwd();   // log the current working directory
  return returnCode;
}


ORTHANC_PLUGINS_API OrthancPluginErrorCode OnChangeCallback(OrthancPluginChangeType changeType,
                                                            OrthancPluginResourceType resourceType,
                                                            const char* resourceId)
{
  char info[1024];
  OrthancPluginMemoryBuffer tmp;

  snprintf(info, sizeof(info), "+++ OnChangeCallback: Change %d on resource %s of type %d", changeType, resourceId, resourceType);
  OrthancPluginLogWarning(context, info);

  if (changeType == OrthancPluginChangeType_NewInstance)
  {
    snprintf(info, sizeof(info), "/instances/%s/metadata/AnonymizedFrom", resourceId);
    if (OrthancPluginRestApiGet(context, &tmp, info) == 0)
    {
      snprintf(info, sizeof(info), "  Instance %s comes from the anonymization of instance", resourceId);
      size_t used = strlen(info);
      size_t room = sizeof(info) - used - 1;
      size_t n = tmp.size < room ? (size_t) tmp.size : room;
      memcpy(info + used, tmp.data, n);
      info[used + n] = '\0';
      OrthancPluginLogWarning(context, info);
      OrthancPluginFreeMemoryBuffer(context, &tmp);
    }
  }

  return OrthancPluginErrorCode_Success;
}


extern "C"
{
	ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion()
	{
		return "1.0.0";
	}


	ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* c)
	{
		OrthancPluginMemoryBuffer tmp;
		char info[1024], *s;
		int counter, i;

		context = c;
		OrthancPluginLogWarning(context, "VPI Plugin: initializing");

		/* Check the version of the Orthanc core */
		if (OrthancPluginCheckVersion(c) == 0)
		{
			snprintf(info, sizeof(info), "Your version of Orthanc (%s) must be above %d.%d.%d to run this plugin",
				c->orthancVersion,
				ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
				ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
				ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
			OrthancPluginLogError(context, info);
			return -1;
		}

		/* Print some information about Orthanc */
		snprintf(info, sizeof(info), "The version of Orthanc is '%s'", context->orthancVersion);
		OrthancPluginLogWarning(context, info);

		s = OrthancPluginGetOrthancPath(context);
		snprintf(info, sizeof(info), "  Path to Orthanc: %s", s);
		OrthancPluginLogWarning(context, info);
		OrthancPluginFreeString(context, s);

		s = OrthancPluginGetOrthancDirectory(context);
		snprintf(info, sizeof(info), "  Directory of Orthanc: %s", s);
		OrthancPluginLogWarning(context, info);
		OrthancPluginFreeString(context, s);

		s = OrthancPluginGetConfiguration(context);
		snprintf(info, sizeof(info), "  Content of the configuration file:\n");
		OrthancPluginLogWarning(context, info);
		OrthancPluginLogWarning(context, s);
		OrthancPluginFreeString(context, s);

		/* Print the command-line arguments of Orthanc */
		counter = OrthancPluginGetCommandLineArgumentsCount(context);
		for (i = 0; i < counter; i++)
		{
			s = OrthancPluginGetCommandLineArgument(context, i);
			snprintf(info, sizeof(info), "  Command-line argument %d: \"%s\"", i, s);
			OrthancPluginLogWarning(context, info);
			OrthancPluginFreeString(context, s);
		}

		/* Register the callbacks */
		OrthancPluginLogWarning(context, "VPI Plugin: Register the callbacks");
		OrthancPluginRegisterRestCallback(context, "/plugin/create", CallbackCreateDicom);

		OrthancPluginRegisterOnStoredInstanceCallback(context, OnStoredCallback);
		OrthancPluginRegisterOnChangeCallback(context, OnChangeCallback);

		/* Declare several properties of the plugin */
		OrthancPluginLogWarning(context, "VPI Plugin: Declare properties");
		OrthancPluginSetRootUri(context, "/plugin/hello");
		OrthancPluginSetDescription(context, "The VPI Reveal plugin intercepts any retrieved DICOM records and writes them into a hierarchical structure for use by VIP Reveal.");
		snprintf(info, sizeof(info), "alert('The VPI Reveal plugin %s has been loaded.');", OrthancPluginGetVersion());
		OrthancPluginExtendOrthancExplorer(context, info);

		/* Make REST requests to the built-in Orthanc API */
		OrthancPluginLogWarning(context, "VPI Plugin: REST requests");
		memset(&tmp, 0, sizeof(tmp));
		OrthancPluginRestApiGet(context, &tmp, "/changes");
		OrthancPluginFreeMemoryBuffer(context, &tmp);
		OrthancPluginRestApiGet(context, &tmp, "/changes?limit=1");
		OrthancPluginFreeMemoryBuffer(context, &tmp);

		return 0;
	}


	ORTHANC_PLUGINS_API void OrthancPluginFinalize()
	{
		OrthancPluginLogWarning(context, "VPI Reveal plugin is finalizing");
	}


	ORTHANC_PLUGINS_API const char* OrthancPluginGetName()
	{
		return "VPI Reveal plugin";
	}
}
