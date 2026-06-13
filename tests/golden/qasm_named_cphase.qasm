OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
cs q[0], q[1];
ctdg q[0], q[1];
