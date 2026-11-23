Contribution from Emsy Chan <emlscs@yahoo.com> on 2014-08-21
============================================================


I've managed to get create-dicom to accept an upload of an image file to be DICOMized! :)

Summary:
1. Make a POST to createDicom.php with PatientID, PatientName and image file to be uploaded (uploadDicom.html). (Be aware that php default max file upload size is 2Mb).
2. createDicom.php uses php_gd2 to convert incoming file to PNG, does a base64 encode, and passes it to orthanc/tools/create-dicom, together with PatientName and various DICOM tags.
3. create-dicom makes a new instance with the POSTed PatientName and PatientID.
4. PatientName and PatientID are rejected unless they are plain DICOM text (no control characters or backslash, at most 64 characters per component group), and the request body is built with json_encode from a fixed list of tags.

(Under Orthanc 0.8.2, create-dicom could not set PatientID, so the original version patched it in afterwards with /instances/{id}/modify. Current Orthanc accepts PatientID in create-dicom directly.)

Issues: 
My hospital uses Siemens Syngo Imaging XS (an old PACs) which likely needs more DICOM tags defined before it will accept the DICOMized image. Will be looking into this.
However, my hospital also uses Syngo.Via and OSIRIX, which have no trouble accepting the DICOMized image with the tags I had already defined in the current createDicom.php.

Hope this helps anybody who needs it.

Regards,
Em
