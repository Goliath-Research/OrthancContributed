Contribution from Emsy Chan <emlscs@yahoo.com> on 2014-08-19
============================================================


Hi,

I feel that orthanc is a great mini-PACS and have an urge to contribute something. However, please forgive my very limited programming skills. 

I want to share my (very basic) method of implementing Weasis with Orthanc using PHP and XML.

Summary: 
1. Orthanc Patient UUID is passed to loadWeasisX.php (eg http://webserver:80/pacs/loadWeasisX.php?PatientUUID=2c713ae2-c924aea4-c86e9306-f7873fb5-fd8444d0)
2. loadWeasisX.php using file_get_contents from http://orthanc:8042/, parses the JSON, and writes a Weasis-compatible XML file for that launch only
3. loadWeasisX.php returns weasisX.jnlp with a dicom:get argument pointing at loadWeasisX.php?manifest=<random token>, which serves that launch's XML file

Note: 
1. The local addresses in the files attached are 10.1.20.127 (webserver with PHP) and 10.2.150.33:8042 (orthanc with default 8042 port for webserver).
2. Weasis is installed into the webserver/pacs/weasis (which is where the jnlp points to).
3. Requests to loadWeasisX.php must carry credentials (an Authorization header, e.g. HTTP Basic). They are forwarded to Orthanc, which decides who may read the patient, so Orthanc must have authentication enabled. Requests without credentials are refused.
4. Manifests are kept outside the web root (in the PHP temp directory, folder weasis-manifests, mode 0700) and expire after 5 minutes. Set $launcherAddress, $manifestDirectory and $manifestLifetime at the top of loadWeasisX.php if needed. Requires PHP 7 or later.

Hope this helps with anybody who needs it.

Regards,
Em
