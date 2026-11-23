<?php
/**
 * Spec for the DICOM upload JSON injection.
 *
 * These tests fail while createDicom.php concatenates PatientName and
 * PatientID into Orthanc request bodies. They pass once those bodies are
 * produced by a JSON encoder that copies only the tags the script means to
 * set, or once a value that would break out of a string is rejected before
 * any Orthanc request or scanned.png write.
 *
 * Orthanc itself must already be listening on 127.0.0.1:18042. The script
 * under test posts to http://localhost:8042, so this file opens a recording
 * proxy on 8042 that forwards each byte to that Orthanc. Run:
 *
 *   php Applications/Php/DicomizeImage/tests/json_injection_test.php
 *
 * Exit status is 1 while the injection is still present.
 */

declare(strict_types=1);

const BACKEND = 'http://127.0.0.1:18042';
const APP_PORT = 18091;
const EXISTING_PATIENT_ID = 'REAL-1';
const EXISTING_STUDY_UID = '1.2.826.0.1.3680043.8.498.10001';
const EXISTING_SERIES_UID = '1.2.826.0.1.3680043.8.498.10003';
const EXISTING_SOP_UID = '1.2.826.0.1.3680043.8.498.10002';
const ATTACK_SOP_IN_STUDY = '1.2.826.0.1.3680043.8.498.10099';
const ATTACK_STUDY_UID = '1.2.826.0.1.3680043.8.498.30001';
const ATTACK_SOP_UID = '1.2.826.0.1.3680043.8.498.30002';
const ATTACK_STUDY_FROM_PATIENT_ID = '1.2.826.0.1.3680043.8.498.20001';
const ATTACK_SOP_FROM_PATIENT_ID = '1.2.826.0.1.3680043.8.498.20002';

$appDir = dirname(__DIR__);
$proxyScript = __DIR__ . '/orthanc_record_proxy.py';
$work = sys_get_temp_dir() . '/dicom-json-injection-' . getmypid();
mkdir($work, 0700, true);
$logPath = $work . '/requests.jsonl';
$prepend = $work . '/ymd-prepend.php';
file_put_contents($prepend, <<<'PHP'
<?php
// createDicom.php calls date(Ymd). PHP 8 throws on that undefined constant
// before the request is built. The bare word is the format the script has
// always passed to date(), so define it and let the real builder run.
if (!defined('Ymd')) {
    define('Ymd', 'Ymd');
}
PHP);

$children = [];
register_shutdown_function(function () use (&$children, $work, $appDir) {
    foreach ($children as $proc) {
        if (is_resource($proc)) {
            proc_terminate($proc);
        }
    }
    $png = $appDir . '/scanned.png';
    if (is_file($png)) {
        unlink($png);
    }
    if (is_dir($work)) {
        foreach (glob($work . '/*') ?: [] as $file) {
            if (is_file($file)) {
                unlink($file);
            }
        }
        @rmdir($work);
    }
});

$failures = 0;
$passes = 0;

function note(string $message): void
{
    fwrite(STDOUT, $message . "\n");
}

function fail(string $name, string $why): void
{
    global $failures;
    $failures++;
    fwrite(STDOUT, "FAIL {$name}: {$why}\n");
}

function pass(string $name): void
{
    global $passes;
    $passes++;
    fwrite(STDOUT, "PASS {$name}\n");
}

function http_json(string $method, string $url, ?string $body = null, array $headers = []): array
{
    $curl = curl_init($url);
    curl_setopt($curl, CURLOPT_CUSTOMREQUEST, $method);
    curl_setopt($curl, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($curl, CURLOPT_HEADER, true);
    if ($body !== null) {
        curl_setopt($curl, CURLOPT_POSTFIELDS, $body);
        $headers[] = 'Content-Type: application/json';
    }
    if ($headers !== []) {
        curl_setopt($curl, CURLOPT_HTTPHEADER, $headers);
    }
    $raw = curl_exec($curl);
    if ($raw === false) {
        $error = curl_error($curl);
        curl_close($curl);
        throw new RuntimeException($method . ' ' . $url . ' failed: ' . $error);
    }
    $status = curl_getinfo($curl, CURLINFO_HTTP_CODE);
    $headerSize = curl_getinfo($curl, CURLINFO_HEADER_SIZE);
    curl_close($curl);
    $decoded = json_decode(substr($raw, $headerSize), true);
    return [$status, $decoded];
}

function wait_for(string $url, int $attempts = 50): void
{
    for ($i = 0; $i < $attempts; $i++) {
        $curl = curl_init($url);
        curl_setopt($curl, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($curl, CURLOPT_TIMEOUT, 1);
        $body = curl_exec($curl);
        $status = curl_getinfo($curl, CURLINFO_HTTP_CODE);
        curl_close($curl);
        if ($body !== false && $status >= 200 && $status < 500) {
            return;
        }
        usleep(100000);
    }
    throw new RuntimeException('timed out waiting for ' . $url);
}

function start_process(array $command, string $cwd, array &$children, string $stdoutPath, string $stderrPath): void
{
    $proc = proc_open(
        $command,
        [
            0 => ['file', '/dev/null', 'r'],
            1 => ['file', $stdoutPath, 'w'],
            2 => ['file', $stderrPath, 'w'],
        ],
        $pipes,
        $cwd
    );
    if (!is_resource($proc)) {
        throw new RuntimeException('could not start ' . implode(' ', $command));
    }
    $children[] = $proc;
}

function backend_up(): bool
{
    $curl = curl_init(BACKEND . '/system');
    curl_setopt($curl, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($curl, CURLOPT_TIMEOUT, 2);
    $body = curl_exec($curl);
    $status = curl_getinfo($curl, CURLINFO_HTTP_CODE);
    curl_close($curl);
    return $body !== false && $status === 200 && strpos($body, 'Orthanc') !== false;
}

function wipe_orthanc(): void
{
    [$status, $patients] = http_json('GET', BACKEND . '/patients');
    if ($status !== 200 || !is_array($patients)) {
        throw new RuntimeException('could not list Orthanc patients');
    }
    foreach ($patients as $patient) {
        http_json('DELETE', BACKEND . '/patients/' . rawurlencode($patient));
    }
}

function tiny_png(): string
{
    $image = imagecreatetruecolor(8, 8);
    $color = imagecolorallocate($image, 255, 0, 0);
    imagefill($image, 0, 0, $color);
    ob_start();
    imagepng($image);
    $png = ob_get_clean();
    imagedestroy($image);
    if ($png === false || $png === '') {
        throw new RuntimeException('could not build a PNG');
    }
    return $png;
}

function seed_existing_study(string $png): void
{
    $body = json_encode([
        'PatientName' => 'Real^Patient',
        'PatientID' => EXISTING_PATIENT_ID,
        'StudyInstanceUID' => EXISTING_STUDY_UID,
        'SeriesInstanceUID' => EXISTING_SERIES_UID,
        'SOPInstanceUID' => EXISTING_SOP_UID,
        'SOPClassUID' => '1.2.840.10008.5.1.4.1.1.7',
        'Modality' => 'OT',
        'PixelData' => 'data:image/png;base64,' . base64_encode($png),
    ], JSON_UNESCAPED_SLASHES);
    [$status, $decoded] = http_json('POST', BACKEND . '/tools/create-dicom', $body);
    if ($status !== 200 || !is_array($decoded) || ($decoded['Status'] ?? '') !== 'Success') {
        throw new RuntimeException('could not seed the existing study: HTTP ' . $status . ' ' . json_encode($decoded));
    }
}

function upload(string $patientName, string $patientId, string $pngPath): string
{
    $curl = curl_init('http://127.0.0.1:' . APP_PORT . '/createDicom.php');
    $file = new CURLFile($pngPath, 'image/png', 'tiny.png');
    curl_setopt($curl, CURLOPT_POST, true);
    curl_setopt($curl, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($curl, CURLOPT_POSTFIELDS, [
        'PatientName' => $patientName,
        'PatientID' => $patientId,
        'file' => $file,
    ]);
    $body = curl_exec($curl);
    if ($body === false) {
        $error = curl_error($curl);
        curl_close($curl);
        throw new RuntimeException('upload failed: ' . $error);
    }
    curl_close($curl);
    return $body;
}

function read_log(string $logPath, int $fromByte): array
{
    clearstatcache(true, $logPath);
    if (!is_file($logPath)) {
        return [];
    }
    $raw = file_get_contents($logPath);
    if ($raw === false || strlen($raw) < $fromByte) {
        return [];
    }
    $slice = substr($raw, $fromByte);
    $records = [];
    foreach (preg_split("/\n/", trim($slice)) as $line) {
        if ($line === '') {
            continue;
        }
        $decoded = json_decode($line, true);
        if (is_array($decoded)) {
            $records[] = $decoded;
        }
    }
    return $records;
}

function log_size(string $logPath): int
{
    clearstatcache(true, $logPath);
    return is_file($logPath) ? filesize($logPath) : 0;
}

function requests_to(array $records, string $suffix): array
{
    $matched = [];
    foreach ($records as $record) {
        $path = $record['path'] ?? '';
        if ($path === $suffix || substr($path, -strlen($suffix)) === $suffix) {
            $matched[] = $record;
        }
    }
    return $matched;
}

function instances(): array
{
    [$status, $ids] = http_json('GET', BACKEND . '/instances');
    if ($status !== 200 || !is_array($ids)) {
        throw new RuntimeException('could not list instances');
    }
    $out = [];
    foreach ($ids as $id) {
        [$tagStatus, $tags] = http_json('GET', BACKEND . '/instances/' . rawurlencode($id) . '/simplified-tags');
        if ($tagStatus !== 200 || !is_array($tags)) {
            throw new RuntimeException('could not read tags for ' . $id);
        }
        $tags['OrthancId'] = $id;
        $out[] = $tags;
    }
    return $out;
}

function instances_with_sop(string $sop): array
{
    $found = [];
    foreach (instances() as $tags) {
        if (($tags['SOPInstanceUID'] ?? '') === $sop) {
            $found[] = $tags;
        }
    }
    return $found;
}

function study_instance_count(string $patientId, string $studyUid): int
{
    [$status, $studies] = http_json('GET', BACKEND . '/studies');
    if ($status !== 200 || !is_array($studies)) {
        throw new RuntimeException('could not list studies');
    }
    $count = 0;
    $matched = false;
    foreach ($studies as $studyId) {
        [$studyStatus, $study] = http_json('GET', BACKEND . '/studies/' . rawurlencode($studyId));
        if ($studyStatus !== 200 || !is_array($study)) {
            continue;
        }
        $uid = $study['MainDicomTags']['StudyInstanceUID'] ?? '';
        $pid = $study['PatientMainDicomTags']['PatientID'] ?? '';
        if ($uid !== $studyUid || $pid !== $patientId) {
            continue;
        }
        $matched = true;
        foreach ($study['Series'] as $seriesId) {
            [$seriesStatus, $series] = http_json('GET', BACKEND . '/series/' . rawurlencode($seriesId));
            if ($seriesStatus === 200 && is_array($series)) {
                $count += count($series['Instances']);
            }
        }
    }
    if (!$matched) {
        throw new RuntimeException('seeded study disappeared');
    }
    return $count;
}

function assert_no_attacker_sop(string $name, string $sop): bool
{
    $found = instances_with_sop($sop);
    if ($found !== []) {
        $one = $found[0];
        fail($name, 'Orthanc stored SOPInstanceUID ' . $sop
            . ' on patient ' . ($one['PatientID'] ?? '')
            . ' study ' . ($one['StudyInstanceUID'] ?? '')
            . ' name ' . json_encode($one['PatientName'] ?? null));
        return false;
    }
    return true;
}

function assert_create_body_is_literal(string $name, ?array $record, string $rawName): bool
{
    if ($record === null) {
        return true;
    }
    $json = json_decode($record['body'] ?? '', true);
    if (!is_array($json)) {
        fail($name, 'create-dicom body is not a JSON object');
        return false;
    }
    $allowed = [
        'PatientName',
        'PatientID',
        'SOPClassUID',
        'Modality',
        'SeriesDescription',
        'StudyDate',
        'SeriesDate',
        'StudyTime',
        'SeriesTime',
        'PixelData',
    ];
    foreach (array_keys($json) as $tag) {
        if (!in_array($tag, $allowed, true)) {
            fail($name, 'create-dicom body sets ' . $tag . ' from the posted patient fields');
            return false;
        }
    }
    if (($json['PatientName'] ?? null) !== $rawName) {
        fail($name, 'create-dicom PatientName is ' . json_encode($json['PatientName'] ?? null)
            . ' instead of the posted text ' . json_encode($rawName));
        return false;
    }
    return true;
}

function assert_modify_body_is_literal(string $name, ?array $record, string $rawId): bool
{
    if ($record === null) {
        return true;
    }
    $json = json_decode($record['body'] ?? '', true);
    if (!is_array($json)) {
        fail($name, 'modify body is not a JSON object: ' . substr($record['body'] ?? '', 0, 300));
        return false;
    }
    if (array_key_exists('Force', $json)) {
        fail($name, 'modify body sets Force from the patient id field');
        return false;
    }
    $replace = $json['Replace'] ?? null;
    if (!is_array($replace)) {
        fail($name, 'modify body has no Replace object');
        return false;
    }
    $extra = array_diff(array_keys($replace), ['PatientID']);
    if ($extra !== []) {
        fail($name, 'modify Replace sets ' . implode(', ', $extra));
        return false;
    }
    if (($replace['PatientID'] ?? null) !== $rawId) {
        fail($name, 'modify PatientID is ' . json_encode($replace['PatientID'] ?? null)
            . ' instead of the posted text ' . json_encode($rawId));
        return false;
    }
    return true;
}

function assert_refusal_wrote_no_png(string $name, ?array $create, string $png, ?int $mtimeBefore, string $workPng): bool
{
    if ($create !== null) {
        return true;
    }
    $wrote = false;
    if (is_file($png) && filemtime($png) !== $mtimeBefore) {
        $wrote = true;
    }
    if (is_file($workPng)) {
        $wrote = true;
    }
    if ($wrote) {
        fail($name, 'scanned.png was written before the upload was refused');
        return false;
    }
    return true;
}

function last_request(array $records, string $suffix): ?array
{
    $matched = requests_to($records, $suffix);
    if ($matched === []) {
        return null;
    }
    return $matched[count($matched) - 1];
}

if (!backend_up()) {
    fwrite(STDERR, "Orthanc is not answering on " . BACKEND . "/system.\n");
    fwrite(STDERR, "Start it on port 18042, then re-run this file. The upload script posts to port 8042,\n");
    fwrite(STDERR, "and this test binds that port to record the JSON before forwarding it.\n");
    exit(2);
}

$proxyOut = $work . '/proxy.out';
$proxyErr = $work . '/proxy.err';
start_process(['python3', $proxyScript, $logPath], $work, $children, $proxyOut, $proxyErr);
wait_for('http://127.0.0.1:8042/system');

$phpOut = $work . '/php.out';
$phpErr = $work . '/php.err';
start_process(
    [
        'php',
        '-d', 'auto_prepend_file=' . $prepend,
        '-d', 'display_errors=1',
        '-S', '127.0.0.1:' . APP_PORT,
        '-t', $appDir,
    ],
    $work,
    $children,
    $phpOut,
    $phpErr
);
wait_for('http://127.0.0.1:' . APP_PORT . '/uploadDicom.html');

$png = tiny_png();
$pngPath = $work . '/tiny.png';
file_put_contents($pngPath, $png);
$appPng = $appDir . '/scanned.png';
$workPng = $work . '/scanned.png';

function run_case(string $name, callable $body): void
{
    try {
        $ok = $body();
        if ($ok) {
            pass($name);
        }
    } catch (Throwable $e) {
        fail($name, $e->getMessage());
    }
}

run_case('patient_name_injection_is_not_filed_in_existing_study', function () use ($png, $pngPath, $logPath, $appPng, $workPng) {
    wipe_orthanc();
    seed_existing_study($png);
    $rawName = 'Forged^Name","PatientID":"' . EXISTING_PATIENT_ID
        . '","StudyInstanceUID":"' . EXISTING_STUDY_UID
        . '","SOPInstanceUID":"' . ATTACK_SOP_IN_STUDY;
    $mark = log_size($logPath);
    $pngMtime = is_file($appPng) ? filemtime($appPng) : null;
    upload($rawName, 'IGNORED', $pngPath);
    $records = read_log($logPath, $mark);
    $create = last_request($records, '/tools/create-dicom');
    $ok = assert_no_attacker_sop('patient_name_injection_is_not_filed_in_existing_study', ATTACK_SOP_IN_STUDY);
    $ok = assert_create_body_is_literal('patient_name_injection_is_not_filed_in_existing_study', $create, $rawName) && $ok;
    $ok = assert_refusal_wrote_no_png('patient_name_injection_is_not_filed_in_existing_study', $create, $appPng, $pngMtime, $workPng) && $ok;
    $count = study_instance_count(EXISTING_PATIENT_ID, EXISTING_STUDY_UID);
    if ($count !== 1) {
        fail('patient_name_injection_is_not_filed_in_existing_study', 'existing study has ' . $count . ' instances after the upload');
        $ok = false;
    }
    return $ok;
});

run_case('patient_name_injection_cannot_choose_study_or_instance_uid', function () use ($pngPath, $logPath, $appPng, $workPng) {
    wipe_orthanc();
    $rawName = 'Forged^Name","StudyInstanceUID":"' . ATTACK_STUDY_UID
        . '","SOPInstanceUID":"' . ATTACK_SOP_UID;
    $mark = log_size($logPath);
    $pngMtime = is_file($appPng) ? filemtime($appPng) : null;
    upload($rawName, 'PID-PLAIN', $pngPath);
    $records = read_log($logPath, $mark);
    $create = last_request($records, '/tools/create-dicom');
    $ok = assert_no_attacker_sop('patient_name_injection_cannot_choose_study_or_instance_uid', ATTACK_SOP_UID);
    $ok = assert_create_body_is_literal('patient_name_injection_cannot_choose_study_or_instance_uid', $create, $rawName) && $ok;
    $ok = assert_refusal_wrote_no_png('patient_name_injection_cannot_choose_study_or_instance_uid', $create, $appPng, $pngMtime, $workPng) && $ok;
    foreach (instances() as $tags) {
        if (($tags['StudyInstanceUID'] ?? '') === ATTACK_STUDY_UID) {
            fail('patient_name_injection_cannot_choose_study_or_instance_uid', 'Orthanc stored StudyInstanceUID ' . ATTACK_STUDY_UID);
            $ok = false;
        }
    }
    return $ok;
});

run_case('patient_id_injection_cannot_choose_study_or_instance_uid', function () use ($pngPath, $logPath, $appPng, $workPng) {
    wipe_orthanc();
    $rawId = 'x","StudyInstanceUID":"' . ATTACK_STUDY_FROM_PATIENT_ID
        . '","SOPInstanceUID":"' . ATTACK_SOP_FROM_PATIENT_ID
        . '"},"Force":true,"Z":"';
    $mark = log_size($logPath);
    $pngMtime = is_file($appPng) ? filemtime($appPng) : null;
    upload('Plain^Name', $rawId, $pngPath);
    $records = read_log($logPath, $mark);
    $modify = null;
    foreach (requests_to($records, '/modify') as $record) {
        $modify = $record;
    }
    $ok = assert_no_attacker_sop('patient_id_injection_cannot_choose_study_or_instance_uid', ATTACK_SOP_FROM_PATIENT_ID);
    $ok = assert_modify_body_is_literal('patient_id_injection_cannot_choose_study_or_instance_uid', $modify, $rawId) && $ok;
    $create = last_request($records, '/tools/create-dicom');
    $ok = assert_refusal_wrote_no_png('patient_id_injection_cannot_choose_study_or_instance_uid', $create, $appPng, $pngMtime, $workPng) && $ok;
    foreach (instances() as $tags) {
        if (($tags['StudyInstanceUID'] ?? '') === ATTACK_STUDY_FROM_PATIENT_ID) {
            fail('patient_id_injection_cannot_choose_study_or_instance_uid', 'Orthanc stored StudyInstanceUID ' . ATTACK_STUDY_FROM_PATIENT_ID);
            $ok = false;
        }
    }
    return $ok;
});

run_case('plain_patient_fields_keep_the_posted_text', function () use ($pngPath, $logPath) {
    wipe_orthanc();
    $rawName = 'Doe^Jane';
    $rawId = 'PID-100';
    $mark = log_size($logPath);
    upload($rawName, $rawId, $pngPath);
    $records = read_log($logPath, $mark);
    $create = last_request($records, '/tools/create-dicom');
    $modify = null;
    foreach (requests_to($records, '/modify') as $record) {
        $modify = $record;
    }
    $name = 'plain_patient_fields_keep_the_posted_text';
    $ok = assert_create_body_is_literal($name, $create, $rawName);
    $ok = assert_modify_body_is_literal($name, $modify, $rawId) && $ok;
    $storedName = false;
    foreach (instances() as $tags) {
        if (($tags['PatientName'] ?? '') === $rawName && ($tags['SeriesDescription'] ?? '') === 'Consent Form') {
            $storedName = true;
            if (($tags['StudyInstanceUID'] ?? '') === ATTACK_STUDY_UID) {
                fail($name, 'plain upload received an attacker study uid');
                $ok = false;
            }
        }
    }
    if (!$storedName) {
        fail($name, 'plain upload did not store PatientName Doe^Jane');
        $ok = false;
    }
    return $ok;
});

note("{$passes} passed, {$failures} failed");
exit($failures === 0 ? 0 : 1);
