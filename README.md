alffs
=====

A FUSE filesystem for the Alfresco DMS repository server

Use cases
=========

* common standard file management
* access and modify metadata with extended attributes
* TODO: map any store as filesystem as a better node browser 

Building
========

There is no automake setup (yet). It feels so overkill to have all that complicated automake stuff
for one source file. To simplify this, a simple script make.sh is provided that compiles the module.

Try it out
==========

* check out the project with git
* install the missing libraries below
* see the retest.sh script on mounting options
* build the Alfresco modules and install them to your Alfresco server

Note on the Alfresco modules
============================

Go the directory alfrescp/alffs-repo-amp and use the script build-repo-amp.sh to create the repository side AMP which needs to be installed to Alfresco.

Dependencies for building
=========================

* ubuntu packages: libfuse-dev libcurl4-openssl-dev libjson0-dev libattr1-dev uthash-dev

Turn off syslog rate limiting:
add $SystemLogRateLimitInterval 0 to /etc/rsyslog.conf


