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
#include <stdlib.h>
#include <direct.h>

static OrthancPluginContext* context = NULL;
static OrthancPluginErrorCode customError;


// Getting the tags of the created instance is done by a quick-and-dirty parsing of a JSON string.
// Returns false, leaving tagValue empty, if the tag is absent, is not a string (e.g. null),
// or its value has no closing quote.
static bool getDicomTag(const char* json, const char* tag, char* tagValue, unsigned int len)
{
	tagValue[0] = '\0';

	char tagText[100] = "\"";
	if (strlen(tag) + strlen("\"\" : \"") >= sizeof(tagText))
		return false;
	strcat(tagText, tag);
	strcat(tagText, "\" : \"");

	const char* value = strstr(json, tagText);
	if (value == NULL)
		return false;
	value += strlen(tagText);

	const char* eos = strchr(value, '\"');
	if (eos == NULL)
		return false;

	size_t valueLen = (size_t)(eos - value);
	if (valueLen < len - 1)
	{
		memcpy(tagValue, value, valueLen);
		tagValue[valueLen] = '\0';
	}
	else
		strcpy(tagValue, "TAG ERROR");

	return true;
}


static OrthancPluginErrorCode CallbackCreateDicom(OrthancPluginRestOutput* output,
	const char* url,
	const OrthancPluginHttpRequest* request)
{
	OrthancPluginLogWarning(context, "+++ CallbackCreateDicom");

	char info[1024];
	char id[1024];
	char* answer;
	bool hasId;
	OrthancPluginMemoryBuffer tmp;
	OrthancPluginErrorCode error;

	if (request->method != OrthancPluginHttpMethod_Post)
	{
		OrthancPluginSendMethodNotAllowed(context, output, "POST");
	}
	else
	{
		/* Make POST request to create a new DICOM instance */
		sprintf(info, "{\"PatientName\":\"Test\"}");
		memset(&tmp, 0, sizeof(tmp));
		error = OrthancPluginRestApiPost(context, &tmp, "/tools/create-dicom", info, strlen(info));
		if (error != OrthancPluginErrorCode_Success)
			return error;

		/* The answer body is not NUL-terminated. */
		answer = (char*)malloc(tmp.size + 1);
		if (answer == NULL)
		{
			OrthancPluginFreeMemoryBuffer(context, &tmp);
			return OrthancPluginErrorCode_NotEnoughMemory;
		}
		if (tmp.size > 0)
			memcpy(answer, tmp.data, tmp.size);
		answer[tmp.size] = '\0';
		OrthancPluginFreeMemoryBuffer(context, &tmp);

		/**
		* Recover the ID of the created instance is constructed by a
		* quick-and-dirty parsing of a JSON string.
		**/
		hasId = getDicomTag(answer, "Path", id, sizeof(id));
		free(answer);
		if (!hasId)
		{
			OrthancPluginLogError(context, "--- /tools/create-dicom answered without a Path");
			return OrthancPluginErrorCode_BadFileFormat;
		}

		/* Delete the newly created DICOM instance. */
		OrthancPluginRestApiDelete(context, id);

		/* Set some cookie */
		OrthancPluginSetCookie(context, output, "hello", "world");

		/* Set some HTTP header */
		OrthancPluginSetHttpHeader(context, output, "Cache-Control", "max-age=0, no-cache");

		OrthancPluginAnswerBuffer(context, output, "OK\n", 3, "text/plain");
	}

	return OrthancPluginErrorCode_Success;
}


static bool createDir(char* dir)
{
	char buffer[1000];
	int ret = _mkdir(dir);
	errno_t err;

	if ( ret == 0)
	{
		sprintf(buffer, "    Directory %s created", dir);
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

	sprintf(buffer, "--- Problem creating directory %s", dir);
	OrthancPluginLogWarning(context, buffer);
	OrthancPluginExtendOrthancExplorer(context, buffer);
	OrthancPluginExtendOrthancExplorer(context, buffer);
	return false;
}

static bool createDir(char* topDir, char* patientName, char* studyDescription, char* seriesDescription)
{
	char dir[1000];

	strcpy(dir, topDir);
	if (!createDir(dir)) return false;

	strcat(dir, "/");
	strcat(dir, patientName);
	if (!createDir(dir)) return false;

	strcat(dir, "/");
	strcat(dir, studyDescription);
	if (!createDir(dir)) return false;

	strcat(dir, "/");
	strcat(dir, seriesDescription);
	if (!createDir(dir)) return false;

	return true;
}


void cdTop(char* configurationPath)
{
	char buffer[1000];

	// Change directory to the top of the data storage
	sprintf(buffer, "cd %s", configurationPath);
	//OrthancPluginLogWarning(context, buffer);
	int ret = _chdir(configurationPath);

	if (ret != 0)
	{
		sprintf(buffer, "--- ERROR: _chdir fails");
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
		sprintf(buffer, "--- ERROR: _getcwd fails");
		OrthancPluginLogWarning(context, buffer);
	}
	else
	{
		sprintf(buffer, "+++ CWD = %s", cwd);
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

  sprintf(buffer, "+++ OnStoredCallback: Received DICOM instance of size %d and ID %s from origin %d (AET %s)", 
          (int) OrthancPluginGetInstanceSize(context, instance), instanceId, 
          OrthancPluginGetInstanceOrigin(context, instance),
          OrthancPluginGetInstanceRemoteAet(context, instance));
  OrthancPluginLogWarning(context, buffer);  

  json = OrthancPluginGetInstanceSimplifiedJson(context, instance);
  if (json == NULL)
  {
	  OrthancPluginLogError(context, "--- Cannot read the tags of the instance, it is not exported.");
	  return OrthancPluginErrorCode_BadFileFormat;
  }
  //OrthancPluginLogWarning(context, "+++ json=");
  //OrthancPluginLogWarning(context, json);

  // Getting the tags of the created instance is done by a quick-and-dirty parsing of a JSON string.
  bool hasTags = getDicomTag(json, "PatientName", patientName, maxLen);
  hasTags = getDicomTag(json, "StudyDescription", studyDescription, maxLen) && hasTags;
  hasTags = getDicomTag(json, "SeriesNumber", seriesNumber, maxLen) && hasTags;
  hasTags = getDicomTag(json, "SeriesDescription", seriesDescription, maxLen) && hasTags;

  if (!hasTags)
  {
	  sprintf(buffer, "--- Instance %s lacks PatientName, StudyDescription, SeriesNumber or SeriesDescription as a string, it is not exported.", instanceId);
	  OrthancPluginLogError(context, buffer);
	  OrthancPluginFreeString(context, json);
	  return OrthancPluginErrorCode_BadFileFormat;
  }

  sprintf(buffer, "    PatientName=%s", patientName);
  OrthancPluginLogWarning(context, buffer);
  sprintf(buffer, "    StudyDescription=%s", studyDescription);
  OrthancPluginLogWarning(context, buffer);
  sprintf(buffer, "    SeriesNumber=%s", seriesNumber);
  OrthancPluginLogWarning(context, buffer);
  sprintf(buffer, "    SeriesDescription=%s", seriesDescription);
  OrthancPluginLogWarning(context, buffer);

  replaceIllegalChars(patientName);
  replaceIllegalChars(studyDescription);
  replaceIllegalChars(seriesNumber);
  replaceIllegalChars(seriesDescription);

  strcpy(seriesDir, seriesNumber);
  strcat(seriesDir, "-");
  strcat(seriesDir, seriesDescription);

  sprintf(buffer, "    previousSeriesDir=%s", previousSeriesDir);
  OrthancPluginLogWarning(context, buffer);
  sprintf(buffer, "    seriesDir=%s", seriesDir);
  OrthancPluginLogWarning(context, buffer);

  if (strcmp(previousSeriesDir, seriesDir) != 0)
	  count = 1;	// new series

  if (count == 1)	// Only for the first DICOM instance of the series
  {
    printf("[%s]\n", json);

	sprintf(buffer, "    PatientName=%s", patientName);
	OrthancPluginLogWarning(context, buffer);
	sprintf(buffer, "    StudyDescription=%s", studyDescription);
	OrthancPluginLogWarning(context, buffer);
	sprintf(buffer, "    SeriesNumber=%s", seriesNumber);
	OrthancPluginLogWarning(context, buffer);
	sprintf(buffer, "    SeriesDescription=%s", seriesDescription);
	OrthancPluginLogWarning(context, buffer);
  }

  // Get directory to the top of the data storage
  char* configurationPath = OrthancPluginGetConfigurationPath(context);
  char* lastSlash = (configurationPath == NULL) ? NULL : strrchr(configurationPath, '\\');
  if (lastSlash == NULL)
  {
	  OrthancPluginLogError(context, "--- Cannot find the directory of the configuration file, the instance is not exported.");
	  if (configurationPath != NULL)
		  OrthancPluginFreeString(context, configurationPath);
	  OrthancPluginFreeString(context, json);
	  return OrthancPluginErrorCode_InternalError;
  }
  lastSlash[0] = '\0';		// remove file name

  cdTop(configurationPath);	// Change directory to the top of the data storage
  logCwd();					// log the current working directory

  strcpy(location, "VPI_Storage/");
  strcat(location, patientName);
  strcat(location, "/");
  strcat(location, studyDescription);
  strcat(location, "/");
  strcat(location, seriesDir);

  sprintf(buffer, "+++ Writing instance %s\\%d.dcm", location, count);
  OrthancPluginLogWarning(context, buffer);

  if (createDir("VPI_Storage", patientName, studyDescription, seriesDir))
  {
	  // Change directory
	  sprintf(buffer, "cd %s", location);
	  OrthancPluginLogWarning(context, buffer);
	  ret = _chdir(location);

	  if (ret != 0)
	  {
		  sprintf(buffer, "--- ERROR: _chdir fails");
		  OrthancPluginLogWarning(context, buffer);
	  }

	  logCwd();   // log the current working directory

	  // write DICOM file to disc
	  sprintf(buffer, "%d.dcm", count);
	  fp = fopen(buffer, "wb");
	  if (fp == NULL)
	  {
		  OrthancPluginLogError(context, "--- Cannot open the export file for writing.");
		  returnCode = OrthancPluginErrorCode_CannotWriteFile;
	  }
	  else
	  {
		  fwrite(OrthancPluginGetInstanceData(context, instance),
			  (size_t)OrthancPluginGetInstanceSize(context, instance), 1, fp);
		  fclose(fp);
		  returnCode = OrthancPluginErrorCode_Success;
	  }
  }
  else
  {
	  OrthancPluginLogError(context, "--- Failure to create directory path. Create it by hand and try again.");
	  returnCode = OrthancPluginErrorCode_CannotWriteFile;
  }

  strcpy(previousSeriesDir, seriesDir);
  count++;
  OrthancPluginLogWarning(context, ">>> cont incremented");
  OrthancPluginFreeString(context, json);

  cdTop(configurationPath);	// Change directory back to the top of the data storage
  OrthancPluginFreeString(context, configurationPath);
  logCwd();   // log the current working directory
  return returnCode;
}


ORTHANC_PLUGINS_API OrthancPluginErrorCode OnChangeCallback(OrthancPluginChangeType changeType,
                                                            OrthancPluginResourceType resourceType,
                                                            const char* resourceId)
{
  char info[1024];
  OrthancPluginMemoryBuffer tmp;

  sprintf(info, "+++ OnChangeCallback: Change %d on resource %s of type %d", changeType, resourceId, resourceType);
  OrthancPluginLogWarning(context, info);

  if (changeType == OrthancPluginChangeType_NewInstance)
  {
    sprintf(info, "/instances/%s/metadata/AnonymizedFrom", resourceId);
    if (OrthancPluginRestApiGet(context, &tmp, info) == 0)
    {
      sprintf(info, "  Instance %s comes from the anonymization of instance", resourceId);
      strncat(info, (const char*) tmp.data, tmp.size);
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
			sprintf(info, "Your version of Orthanc (%s) must be above %d.%d.%d to run this plugin",
				c->orthancVersion,
				ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
				ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
				ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
			OrthancPluginLogError(context, info);
			return -1;
		}

		/* Print some information about Orthanc */
		sprintf(info, "The version of Orthanc is '%s'", context->orthancVersion);
		OrthancPluginLogWarning(context, info);

		s = OrthancPluginGetOrthancPath(context);
		sprintf(info, "  Path to Orthanc: %s", s);
		OrthancPluginLogWarning(context, info);
		OrthancPluginFreeString(context, s);

		s = OrthancPluginGetOrthancDirectory(context);
		sprintf(info, "  Directory of Orthanc: %s", s);
		OrthancPluginLogWarning(context, info);
		OrthancPluginFreeString(context, s);

		s = OrthancPluginGetConfiguration(context);
		sprintf(info, "  Content of the configuration file:\n");
		OrthancPluginLogWarning(context, info);
		OrthancPluginLogWarning(context, s);
		OrthancPluginFreeString(context, s);

		/* Print the command-line arguments of Orthanc */
		counter = OrthancPluginGetCommandLineArgumentsCount(context);
		for (i = 0; i < counter; i++)
		{
			s = OrthancPluginGetCommandLineArgument(context, i);
			sprintf(info, "  Command-line argument %d: \"%s\"", i, s);
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
		sprintf(info, "alert('The VPI Reveal plugin %s has been loaded.');", OrthancPluginGetVersion());
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
