import socket, select, xia_pb2
import struct

hasGotCID = 0; 


def read_write(bsoc, soc_rpc, max_idling=20):
        iw = [bsoc, soc_rpc]
        ow = []
        count = 0
	msg_response = xia_pb2.msg()
        while 1:
            count += 1
            (ins, _, exs) = select.select(iw, ow, iw, 3)
            if exs: break
            if ins:
                for i in ins:
                    if i is soc_rpc:
                        out = bsoc
                    else:
                        out = soc_rpc
                    data = i.recv(4)
		    print "sizesizesize: %d" % len(data)
                    size = struct.unpack('!i', data)[0]
		    data = i.recv(size)
		    
		    msg_response.ParseFromString(data) 
                    if data:
			    if (i == soc_rpc): #and (msg_response.appid == bsoc.fileno())):
				    print "payload len (recv): %d" % len(msg_response.payload)
				    print "payload (recv): %s" % msg_response.payload
				    #out.send(msg_response.payload) # add http header and send to browser
				    #getCID('','0000000000000000000000000000000000000000',bsoc, soc_rpc)
			    #else:
				    out.send(msg_response.payload)  # ???
				    #print "out(soc_rpc): %d" % soc_rpc.fileno() 
			    count = 0
            else:
                print "\t" "idle", count
            if count == max_idling: break
                                                                                                                                                                                                  

def printFunc(value):
	print value
	return


def sendSIDRequest(netloc, payload, bsoc, sock_rpc):
	print "in SID function - net location = " + netloc  
	msg = xia_pb2.msg()
        #msg.appid = bsoc.fileno()  # change 0 -> bsoc
	#print "bsoc: %d" % bsoc.fileno()
	#msg.xid = '00000000000000000000'
	msg.type = xia_pb2.msg.CONNECTSID
	payload = ''     # to be removed
        msg.payload = payload
        msg.xiapath_src = 'AD:1000000000000000000000000000000000000000 HID:0000000000000000000000000000000000000000'
        msg.xiapath_dst = 'AD:1000000000000000000000000000000000000001 HID:0000000000000000000000000000000000000001'  # sp SID: xxx
        serialized_msg = msg.SerializeToString()
	#print ''.join(map(lambda x: '%02x' % ord(x), serialized_msg))
        size = struct.pack('!i', len(serialized_msg))
        print "payload len %d " % len(payload)
        sock_rpc.send(size)
        sock_rpc.send(serialized_msg)
	read_write(bsoc, sock_rpc)
	
	return


def getCID(netloc, CID, bsoc, sock_rpc):
#############	global hasGotCID
	#if (hasGotCID == 1):
	#	return
	#hasGotCID = 1;
	print "in getCID function - net location = " + netloc  
	msg = xia_pb2.msg()
        # msg.appid = bsoc.fileno()  
	msg.xid = '00000000000000000000'
	msg.type = xia_pb2.msg.GETCID
        msg.payload = ''
        msg.xiapath_src = 'AD:1000000000000000000000000000000000000000 HID:0000000000000000000000000000000000000000'
        msg.xiapath_dst = '( AD:1000000000000000000000000000000000000001 HID:0000000000000000000000000000000000000001 ) CID:0000000000000000000000000000000000000000'
        serialized_msg = msg.SerializeToString()
	#print ''.join(map(lambda x: '%02x' % ord(x), serialized_msg))
        size = struct.pack('!i', len(serialized_msg))
        #print "payload len %d " % len(payload)
	print "protobufmsg len %d " % len(serialized_msg)

        sock_rpc.send(size)
        sock_rpc.send(serialized_msg)
	read_write(bsoc, sock_rpc)
	
	return


def xiaHandler(control, payload, bsock, sock_rpc):
	print "in XIA code\n" + control + "\n" + payload
	control=control[4:]
	if control.find('sid') == 0:
		print "SID request"
		sendSIDRequest(control[4:], payload);
	elif control.find('cid') == 0:
		print "cid request"
		getCID('','0000000000000000000000000000000000000000',bsock, sock_rpc)
		#getCID(control[4:], payload, bsock, sock_rpc);
	elif control.find('socket') == 0:
		sendSIDRequest(control[7:], payload, bsock, sock_rpc);
		
	return;