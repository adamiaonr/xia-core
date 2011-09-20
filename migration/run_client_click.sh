#!/bin/bash

PREFIX=`dirname $0`
PREFIX=`readlink -f $PREFIX`
source $PREFIX/common.sh

ssh $HOST_A_IP bash -c "sudo killall click; $CLICK_PATH/userlevel/click $CLICK_PATH/conf/xia/xia_vm_hostA.click"

ssh $HOST_B_IP bash -c "sudo killall click; $CLICK_PATH/userlevel/click $CLICK_PATH/conf/xia/xia_vm_hostB.click"
