OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
rxx(pi/4) q[0], q[1];
ryy(-pi/2) q[0], q[1];
