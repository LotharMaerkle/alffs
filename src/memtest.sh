#!/bin/bash

./make.sh

ALF_HOST=192.168.100.1
ALF_USER=admin
ALF_PASSWORD=admin
ALF_PORT=8080

. ../local.config

if mount |grep /mnt/alf >/dev/null
then
  sudo umount /mnt/alf
fi

sudo valgrind --tool=memcheck --leak-check=yes --track-origins=yes --show-reachable=yes --num-callers=20 --track-fds=yes ./alf -d -oallow_other,uid=`id -u`,gid=`id -g`,alfuserid=$ALF_USER,alfpassword=$ALF_PASSWORD,alfprotocol=http,alfhost=$ALF_HOST,alfport=$ALF_PORT,alfiobase=/alfresco/service/ecm4u/io,alfmountbase=workspace://SpacesStore/app:company_home,alfdebug=1,alftimeout=10 /mnt/alf
