alffs
=====

A FUSE filesystem for the Alfresco DMS repository server


Dependencies for building:
=========================

* ubuntu packages: libfuse-dev libcurl4-openssl-dev libjson0-dev libattr1-dev uthash-dev

Turn off syslog rate limiting:
add $SystemLogRateLimitInterval 0 to /etc/rsyslog.conf

Use cases:
* common standard file management
* access and modify metadata with extended attributes
* TODO: map any store as filesystem as a better node browser 

