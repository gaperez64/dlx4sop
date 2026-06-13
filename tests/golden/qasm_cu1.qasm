OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
cu1(pi/4) q[0], q[1];
