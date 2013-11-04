#!/bin/bash

mvn -Dmaven.test.skip=true package

(
cd target
TMPDIR=tmp$$
mkdir $TMPDIR
(
cd $TMPDIR
unzip ../alffs.amp
(
cd lib
ls |grep -v alffs |xargs rm
)
zip -r ../alffs-nodep.amp .
)

rm -rf $TMPDIR
)




