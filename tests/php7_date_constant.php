<?php
// createDicom.php calls date(Ymd). PHP 7 and earlier treated an undefined
// constant as the string of its name, so that call was date("Ymd"). PHP 8
// raises Error before the script reaches Orthanc. Defining the constant
// restores the value those older interpreters used.
if (!defined("Ymd")) {
    define("Ymd", "Ymd");
}
