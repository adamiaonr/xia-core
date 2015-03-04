require(library xia_router_lib.click);
require(library xia_address.click);

// TOPOLOGY OVERVIEW
//
// LEGEND:
//	'ADx'           : Autonomous Domain (w/ ADx)
//	'Rx'            : Router (w/ RHIDx)
//	'Hx'            : Host (w/ HIDx)
//	'---' or '|'    : Link
//
// { AD0    }{ AD1  }{ AD2      }
//               H1     H4
//               |      |
// H0 --- R0 --- R1 --- R2 --- H3
//               |      |
//               H2     H5
// { AD0    }{ AD1  }{ AD2      }
//

// 1) HOSTS & ROUTERS

// 1.1) HOSTs <-> AD0
host0 :: XIAEndHost (RE AD0 HID0, HID0, 1500, 0, aa:aa:aa:aa:aa:aa);

// 1.2) HOSTs <-> AD1
host1 :: XIAEndHost (RE AD1 HID1, HID1, 1601, 1, aa:aa:aa:aa:aa:aa);
host2 :: XIAEndHost (RE AD1 HID2, HID2, 1602, 2, aa:aa:aa:aa:aa:aa);

// 1.3) HOSTs <-> AD2
host3 :: XIAEndHost (RE AD2 HID3, HID3, 1703, 3, aa:aa:aa:aa:aa:aa);
host4 :: XIAEndHost (RE AD2 HID4, HID4, 1704, 4, aa:aa:aa:aa:aa:aa);
host5 :: XIAEndHost (RE AD2 HID5, HID5, 1705, 5, aa:aa:aa:aa:aa:aa);

// 1.4) ROUTERs <-> ADx
router0 :: XIARouter2Port(RE AD0 RHID0, AD0, RHID0, 0.0.0.0, 2000, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa);
router1 :: XIARouter4Port(RE AD1 RHID1, AD1, RHID1, 0.0.0.0, 2100, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa);
router2 :: XIARouter4Port(RE AD2 RHID2, AD2, RHID2, 0.0.0.0, 2200, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa, aa:aa:aa:aa:aa:aa);

// The following line is required by the xianet script so it can determine the appropriate
// host/router pair to run the nameserver on
// host0 :: nameserver

// 2) HOSTS <-> ROUTERS INTERCONNECTIONS

// 2.1) AD0 (HID{0} <-> ROUTER0)
host0[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]router0;
router0[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]host0;

// 2.2) AD1 (HID{1,2} <-> ROUTER 1)
host1[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]router1;
router1[0] -> LinkUnqueue(0.005, 1 GB/s) ->[0]host1;

host2[0] -> LinkUnqueue(0.005, 1 GB/s) -> [1]router1;
router1[1] -> LinkUnqueue(0.005, 1 GB/s) ->[0]host2;

// 2.2) AD1 (HID{3,4,5} <-> ROUTER 2)
host3[0] -> LinkUnqueue(0.005, 1 GB/s) -> [0]router2;
router2[0] -> LinkUnqueue(0.005, 1 GB/s) ->[0]host3;

host4[0] -> LinkUnqueue(0.005, 1 GB/s) -> [1]router2;
router2[1] -> LinkUnqueue(0.005, 1 GB/s) ->[0]host4;

host5[0] -> LinkUnqueue(0.005, 1 GB/s) -> [2]router2;
router2[2] -> LinkUnqueue(0.005, 1 GB/s) ->[0]host5;

// 3) ROUTER <-> ROUTER (AD <-> AD) INTERCONNECTIONS

// 3.1) AD0 <-> AD1
router0[1] -> LinkUnqueue(0.005, 1 GB/s) ->[2]router1;
router1[2] -> LinkUnqueue(0.005, 1 GB/s) ->[1]router0;

// 3.1) AD1 <-> AD2
router1[3] -> LinkUnqueue(0.005, 1 GB/s) ->[3]router2;
router2[3] -> LinkUnqueue(0.005, 1 GB/s) ->[3]router1;

ControlSocket(tcp, 7777);
