<?php
//Requires php_curl and php_gd2
//Global params
$orthanc = "http://localhost:8042";

function reject($status, $message) {
	http_response_code($status);
	echo $message;
	exit;
	}

//Accept only plain text that fits the DICOM value representation: valid UTF-8, no control
//characters, no backslash (the DICOM multi-value delimiter), and at most $maxLength characters
//in each of at most $maxGroups "="-separated groups (PN allows 3 groups, LO allows 1).
function readTextField($name, $maxLength, $maxGroups) {
	if (!isset($_POST[$name]) || !is_string($_POST[$name])) {
		reject(400, "Missing " . $name);
		}
	$value = $_POST[$name];
	if (preg_match('//u', $value) !== 1 || preg_match('/[\x00-\x1F\x7F\\\\]/', $value) === 1) {
		reject(400, "Invalid " . $name);
		}
	$groups = ($maxGroups > 1) ? explode("=", $value) : array($value);
	if (count($groups) > $maxGroups) {
		reject(400, "Invalid " . $name);
		}
	foreach ($groups as $group) {
		if (preg_match_all('/./us', $group) > $maxLength) {
			reject(400, "Invalid " . $name);
			}
		}
	return $value;
	}

function encodeJson($value) {
	$json = json_encode($value);
	if ($json === false) {
		reject(400, "Could not encode request");
		}
	return $json;
	}

//Function for encode png to mime. I modified this to hard-select $_FILES.
function data_uri($mime) {  
	imagepng(imagecreatefromstring(file_get_contents($_FILES["file"]["tmp_name"])), "scanned.png");
	$contents = file_get_contents("scanned.png");
	$base64   = base64_encode($contents); 
	return ('data:' . $mime . ';base64,' . $base64);
	}

//Validate the posted fields before touching the upload or Orthanc
$patientName = readTextField('PatientName', 64, 3);
$patientID = readTextField('PatientID', 64, 1);

//Define the DICOM Tags & PNG File, build query
$dicomTags = array (
	"PatientName" => $patientName,
	"PatientID" => $patientID,
	"SOPClassUID" => "1.2.840.10008.5.1.4.1.1.7", //SC
	"Modality" => "SC",
	"SeriesDescription" => "Consent Form",
	"StudyDate" => date(Ymd),
	"SeriesDate" => date(Ymd),
	"StudyTime" => "000000",
	"SeriesTime" => "000000",
	"PixelData" => data_uri('image/png')
	);
$data = encodeJson($dicomTags);

//Create a DICOM instance
$curl = curl_init();
curl_setopt ($curl, CURLOPT_URL, $orthanc . '/tools/create-dicom');
curl_setopt ($curl, CURLOPT_RETURNTRANSFER, 1);
curl_setopt ($curl, CURLOPT_POST, 1);
curl_setopt ($curl, CURLOPT_POSTFIELDS, $data);
$resp = curl_exec($curl);
curl_close($curl);

$created = ($resp === false) ? null : json_decode($resp, true);
if (!is_array($created) || !isset($created['ID'])) {
	reject(502, "Orthanc did not create the instance");
	}

//Done
echo "Done";
?>
