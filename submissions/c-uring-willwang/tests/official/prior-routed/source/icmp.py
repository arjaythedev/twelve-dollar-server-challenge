"""Measure ICMP RTT with an unprivileged Linux ping socket and kernel RX timestamps."""
import json
import socket
import struct
import sys
import time

results=[]
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_ICMP) as sock:
    sock.settimeout(1)
    so_timestampns=getattr(socket, "SO_TIMESTAMPNS",35)
    sock.setsockopt(socket.SOL_SOCKET,so_timestampns,1)
    sock.connect((sys.argv[1],0))
    for seq in range(25):
        body=struct.pack("!BBHHH",8,0,0,0,seq)+bytes(56)
        start=time.time_ns()
        sock.send(body)
        reply,anc,flags,addr=sock.recvmsg(128,128)
        if addr[0]!=sys.argv[1] or reply[0]!=0 or struct.unpack_from("!H",reply,6)[0]!=seq:
            raise ValueError("unexpected ICMP echo response")
        ts=next(data for level,name,data in anc if level==socket.SOL_SOCKET and name==so_timestampns)
        seconds,nanos=struct.unpack("@ll", ts[:struct.calcsize("@ll")])
        results.append((seconds*1_000_000_000+nanos-start)/1_000_000)
        time.sleep(.02)
print(json.dumps(results))
