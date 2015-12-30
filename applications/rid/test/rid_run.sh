#!/bin/bash

# the purpose of this script is to test the RID principal type on a locally 
# emulated network w/ Click.

# 1) TOPOLOGY OVERVIEW
#
# the topology used in this example is based on the file /etc/click/xia_custom_topology.click
#
# our topology is composed by 2 RID requesters (running the rid_requester app) 
# at H0 and H3. 4 different RID producers (running the rid_producer app) serve 
# different parts of the namespace, in the following manner:
#
# | Host | Served prefix |
# | H1   | /cmu/ece/ph   |
# | H2   | /cmu/cs/ghc   |
# | H4   | /upitt/cs/ss  |
# | H5   | /upitt/ece/bh |
#
# LEGEND:
#	'ADx'           : Autonomous Domain (w/ ADx)
#	'Rx'            : Router (w/ RHIDx)
#	'Hx'            : Host (w/ HIDx)
#	'---' or '|'    : Link
#
# { AD0    }{ AD1  }{ AD2      }
#               H1     H4
#               |      |
# H0 --- R0 --- R1 --- R2 --- H3
#               |      |
#               H2     H5
#

# path to xia root dir (should be an argument ?)
XIA_DIR=/home/adamiaonr/Workbench/xia/xia-core

# paths to binaries for xcache and rid apps
XCACHE_BINPATH=${XIA_DIR}/daemons/xcache/bin
RID_BINPATH=${XIA_DIR}/applications/rid/bin

function cleanHost() {
    host=$1
    rid_app_type=$2

    rm -f xcache.${host}

    if [ ! -z "${rid_app_type}" ]; then
        rm -f ${rid_app_type}.${host}
    fi

    rm -f xsockconf.ini
}

function makeHost() {
    host=$1
    rid_app_type=$2

    cp ${XCACHE_BINPATH}/xcache xcache.${host}
    
    echo "[xcache.${host}]" >> xsockconf.ini
    echo "host=${host}" >> xsockconf.ini
    echo "" >> xsockconf.ini

    if [ ! -z "${rid_app_type}" ]; then
        cp ${RID_BINPATH}/${rid_app_type} ${rid_app_type}.${host}

        echo "[${rid_app_type}.${host}]" >> xsockconf.ini
        echo "host=${host}" >> xsockconf.ini
        echo "" >> xsockconf.ini
    fi
}

# add the diff xia library paths to LD_LIBRARY_PATH (librid, libXsocket & co., 
# libxcache)
export LD_LIBRARY_PATH=${XIA_DIR}api/rid/bin:${XIA_DIR}/api/lib:${XIA_DIR}/daemons/xcache/api

# start the XIA network (xia_local_topology.click)
${XIA_DIR}/bin/xianet -V start

cleanHost host0
cleanHost host1
cleanHost host2
cleanHost host3
cleanHost host4
cleanHost host5

cleanHost router0
cleanHost router1
cleanHost router2

makeHost host0 rid_requester
makeHost host1 rid_producer
makeHost host2 rid_producer
makeHost host3 rid_requester
makeHost host4 rid_producer
makeHost host5 rid_producer

makeHost router0
makeHost router1
makeHost router2

if [ "$1" = "run" ]; then

    # start xcache at each host[x]
    ./xcache.host0 -h host0 -l 3 -m 0xFF&
    ./xcache.host1 -h host1 -l 3 -m 0xFF&
    ./xcache.host2 -h host1 -l 3 -m 0xFF&
    ./xcache.host3 -h host1 -l 3 -m 0xFF&
    ./xcache.host4 -h host1 -l 3 -m 0xFF&
    ./xcache.host5 -h host1 -l 3 -m 0xFF&

    # what do we do with the routers?
    # ./xcache.router0 -h router0 -l 3 -m 0x0&
    # ./xcache.router1 -h router1 -l 3 -m 0x0&
    # ./xcache.router1 -h router2 -l 3 -m 0x0&

    # start the rid producers (rid requesters are ran manually)
    ./rid_producer.host1 -n /cmu/ece/ph
    ./rid_producer.host2 -n /cmu/cs/ghc
    ./rid_producer.host4 -n /upitt/cs/ss
    ./rid_producer.host5 -n /upitt/ece/bh
fi

if [ "$1" = "stop" ]; then
    killall xcache.host0
    killall xcache.host1
    killall xcache.router0
    killall xcache.router1
fi
