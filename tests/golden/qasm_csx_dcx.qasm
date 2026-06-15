OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
csx q[0], q[1];
csxdg q[1], q[0];
dcx q[0], q[1];
