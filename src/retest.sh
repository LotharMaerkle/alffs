#!/bin/bash
set -e

if ! ./make.sh
then
  echo "MAKE FAILED"
  exit 1 
fi


ALF_HOST=192.168.100.1
ALF_USER=admin
ALF_PASSWORD=admin
ALF_PORT=8080
ALF_MOUNTPOINT=/mnt/alf
ALF_MAX_WRITE=$((1024 * 1024))
ALF_MAX_READ=$((1024 * 1024))
ALF_CONN_CACHE=10
ALF_HK_SLEEP=3
ALF_PROTOCOL=http
ALF_HK_CACHE_MAX=3600

. ../local.config

if mount |grep " $ALF_MOUNTPOINT" >/dev/null
then
  echo "unmounting..."
  sudo umount $ALF_MOUNTPOINT
fi

sudo ./alf $@ -oallow_other,big_writes,max_write=$ALF_MAX_WRITE,max_read=$ALF_MAX_READ,uid=`id -u`,gid=`id -g`,alfuserid=$ALF_USER,alfpassword=$ALF_PASSWORD,alfprotocol=$ALF_PROTOCOL,alfhost=$ALF_HOST,alfport=$ALF_PORT,alfiobase=/alfresco/service/alffs,alfmountbase=workspace://SpacesStore/app:company_home,alftimeout=100,alfcachedir=/tmp,alfconcachemax=$ALF_CONN_CACHE,alfhksleep=$ALF_HK_SLEEP,alfhkcachemax=$ALF_HK_CACHE_MAX $ALF_MOUNTPOINT

#sudo ls $ALF_MOUNTPOINT

