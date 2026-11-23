<?php
$orthancAddress = "http://10.2.150.33:8042";
// Public address of this script; Weasis downloads the manifest from here.
$launcherAddress = "http://10.1.20.127/pacs/loadWeasisX.php";
$jnlpTemplate = __DIR__ . "/weasisX.jnlp";
// Must not be inside the web root.
$manifestDirectory = sys_get_temp_dir() . DIRECTORY_SEPARATOR . "weasis-manifests";
$manifestLifetime = 300; // seconds

function fail($code, $message) {
	http_response_code($code);
	header("Content-Type: text/plain; charset=utf-8");
	header("Cache-Control: no-store");
	if ($code == 401) {
		header("WWW-Authenticate: Basic realm=\"Weasis launcher\"");
		}
	echo $message . "\n";
	exit;
	}

function xmlAttr($value) {
	return htmlspecialchars((string)$value, ENT_XML1 | ENT_QUOTES | ENT_SUBSTITUTE, "UTF-8");
	}

function isOrthancId($id) {
	return is_string($id) && preg_match('/^[A-Za-z0-9-]{1,128}$/', $id) === 1;
	}

function isManifestToken($token) {
	return is_string($token) && preg_match('/^[0-9a-f]{64}$/', $token) === 1;
	}

function manifestPath($directory, $token) {
	return $directory . DIRECTORY_SEPARATOR . hash("sha256", $token) . ".xml";
	}

// The caller's credentials, forwarded to Orthanc. Orthanc must require
// authentication: it decides whether this caller may read the patient.
function callerAuthorization() {
	$value = "";
	if (!empty($_SERVER["HTTP_AUTHORIZATION"])) {
		$value = $_SERVER["HTTP_AUTHORIZATION"];
		}
	elseif (!empty($_SERVER["REDIRECT_HTTP_AUTHORIZATION"])) {
		$value = $_SERVER["REDIRECT_HTTP_AUTHORIZATION"];
		}
	elseif (isset($_SERVER["PHP_AUTH_USER"])) {
		$value = "Basic " . base64_encode($_SERVER["PHP_AUTH_USER"] . ":" . (isset($_SERVER["PHP_AUTH_PW"]) ? $_SERVER["PHP_AUTH_PW"] : ""));
		}
	$value = trim($value);
	if ($value === "" || preg_match('/[\r\n]/', $value) || strpos($value, " ") === false) {
		return null;
		}
	return $value;
	}

function orthancGet($orthancAddress, $authorization, $path) {
	$context = stream_context_create(array("http" => array(
		"method" => "GET",
		"header" => "Authorization: " . $authorization . "\r\n",
		"ignore_errors" => true,
		"follow_location" => 0,
		"timeout" => 30,
		)));
	$body = @file_get_contents($orthancAddress . $path, false, $context);
	$headers = function_exists("http_get_last_response_headers") ? http_get_last_response_headers() : (isset($http_response_header) ? $http_response_header : null);
	$status = 0;
	if (is_array($headers) && isset($headers[0]) && preg_match('/^HTTP\/\S+\s+(\d{3})/', $headers[0], $match)) {
		$status = (int)$match[1];
		}
	if ($body === false || $status == 0) {
		fail(502, "Orthanc is unreachable");
		}
	if ($status == 401) {
		fail(401, "Orthanc refused these credentials");
		}
	if ($status == 403) {
		fail(403, "Orthanc refused access");
		}
	if ($status == 404) {
		fail(404, "Not found in Orthanc");
		}
	if ($status != 200) {
		fail(502, "Orthanc returned HTTP " . $status);
		}
	$json = json_decode($body, true);
	if (!is_array($json)) {
		fail(502, "Orthanc returned an unexpected answer");
		}
	return $json;
	}

function tag($resource, $name) {
	return isset($resource["MainDicomTags"][$name]) ? $resource["MainDicomTags"][$name] : "";
	}

function children($resource, $key) {
	$ids = isset($resource[$key]) && is_array($resource[$key]) ? $resource[$key] : array();
	foreach ($ids as $id) {
		if (!isOrthancId($id)) {
			fail(502, "Orthanc returned an unexpected identifier");
			}
		}
	return $ids;
	}

function privateDirectory($directory) {
	if (!is_dir($directory) && !@mkdir($directory, 0700) && !is_dir($directory)) {
		return false;
		}
	if (is_link($directory)) {
		return false;
		}
	if (!empty($_SERVER["DOCUMENT_ROOT"])) {
		$root = realpath($_SERVER["DOCUMENT_ROOT"]);
		$real = realpath($directory);
		if ($root !== false && $real !== false && ($real === $root || strpos($real, rtrim($root, DIRECTORY_SEPARATOR) . DIRECTORY_SEPARATOR) === 0)) {
			return false;
			}
		}
	if (DIRECTORY_SEPARATOR === "/") {
		$stat = stat($directory);
		if ($stat === false || ($stat["mode"] & 0077) != 0) {
			return false;
			}
		if (function_exists("posix_geteuid") && $stat["uid"] !== posix_geteuid()) {
			return false;
			}
		}
	return true;
	}

function removeExpiredManifests($directory, $lifetime) {
	foreach (glob($directory . DIRECTORY_SEPARATOR . "*.xml") ?: array() as $file) {
		$modified = @filemtime($file);
		if ($modified !== false && $modified + $lifetime < time()) {
			@unlink($file);
			}
		}
	}

// Weasis downloads the manifest without the staff credentials, so the
// unguessable token in its address is what limits it to this launch.
if (isset($_GET["manifest"])) {
	$token = $_GET["manifest"];
	if (!isManifestToken($token)) {
		fail(404, "Unknown manifest");
		}
	$path = manifestPath($manifestDirectory, $token);
	$modified = @filemtime($path);
	if ($modified === false) {
		fail(404, "Unknown manifest");
		}
	if ($modified + $manifestLifetime < time()) {
		@unlink($path);
		fail(404, "Unknown manifest");
		}
	$xmlText = @file_get_contents($path);
	if ($xmlText === false) {
		fail(404, "Unknown manifest");
		}
	header("Content-Type: application/xml; charset=utf-8");
	header("Cache-Control: no-store");
	echo $xmlText;
	exit;
	}

$authorization = callerAuthorization();
if ($authorization === null) {
	fail(401, "Authentication required");
	}
if (!isset($_GET["PatientUUID"]) || !isOrthancId($_GET["PatientUUID"])) {
	fail(400, "Missing or invalid PatientUUID");
	}

$patient = orthancGet($orthancAddress, $authorization, "/patients/" . $_GET["PatientUUID"]);

$xmlText = "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<wado_query xmlns= \"http://www.weasis.org/xsd\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" wadoURL=\"" . xmlAttr($orthancAddress . "/instances/") . "\" requireOnlySOPInstanceUID=\"false\" additionnalParameters=\"\" overrideDicomTagsList=\"\" >\n  <Patient PatientID=\"" . xmlAttr(tag($patient, "PatientID")) . "\" PatientName=\"" . xmlAttr(tag($patient, "PatientName")) . "\" PatientBirthDate=\"" . xmlAttr(tag($patient, "PatientBirthDate")) . "\" >\n";

foreach (children($patient, "Studies") as $studyId) {
	$study = orthancGet($orthancAddress, $authorization, "/studies/" . $studyId);
	$xmlText = $xmlText . "    <Study StudyInstanceUID=\"" . xmlAttr(tag($study, "StudyInstanceUID")) . "\" StudyDescription=\"" . xmlAttr(tag($study, "StudyDescription")) . "\" StudyDate=\"" . xmlAttr(tag($study, "StudyDate")) . "\" StudyTime=\"" . xmlAttr(tag($study, "StudyTime")) . "\" >\n";

	foreach (children($study, "Series") as $seriesId) {
		$series = orthancGet($orthancAddress, $authorization, "/series/" . $seriesId);
		$xmlText = $xmlText . "      <Series SeriesInstanceUID=\"" . xmlAttr(tag($series, "SeriesInstanceUID")) . "\" SeriesDescription=\"" . xmlAttr(tag($series, "SeriesDescription")) . "\" SeriesNumber=\"" . xmlAttr(tag($series, "SeriesNumber")) . "\" Modality=\"" . xmlAttr(tag($series, "Modality")) . "\" >\n";

		foreach (children($series, "Instances") as $instanceId) {
			$instance = orthancGet($orthancAddress, $authorization, "/instances/" . $instanceId);
			$xmlText = $xmlText . "        <Instance SOPInstanceUID=\"" . xmlAttr(tag($instance, "SOPInstanceUID")) . "\" InstanceNumber=\"" . xmlAttr(tag($instance, "InstanceNumber")) . "\" DirectDownloadFile=\"" . xmlAttr($instanceId . "/file") . "\"/>\n";
			}
		$xmlText = $xmlText . "</Series>\n";
		}
	$xmlText = $xmlText . "</Study>\n";
	}
$xmlText = $xmlText . "</Patient>\n</wado_query>\n";

$jnlp = @file_get_contents($jnlpTemplate);
if ($jnlp === false || strpos($jnlp, "</application-desc>") === false) {
	fail(500, "Weasis JNLP template is missing");
	}
if (!privateDirectory($manifestDirectory)) {
	fail(500, "Manifest directory is not private");
	}
removeExpiredManifests($manifestDirectory, $manifestLifetime);

$token = bin2hex(random_bytes(32));
$fp = @fopen(manifestPath($manifestDirectory, $token), "x");
if ($fp === false) {
	fail(500, "Could not store the manifest");
	}
@chmod(manifestPath($manifestDirectory, $token), 0600);
$written = fwrite($fp, $xmlText);
fclose($fp);
if ($written !== strlen($xmlText)) {
	@unlink(manifestPath($manifestDirectory, $token));
	fail(500, "Could not store the manifest");
	}

$manifestUrl = $launcherAddress . "?manifest=" . $token;
$jnlp = str_replace("</application-desc>", "\t<argument>\$dicom:get -w " . xmlAttr($manifestUrl) . "</argument>\n\t</application-desc>", $jnlp);

header("Content-Type: application/x-java-jnlp-file");
header("Content-Disposition: attachment; filename=\"weasis.jnlp\"");
header("Cache-Control: no-store");
echo $jnlp;
