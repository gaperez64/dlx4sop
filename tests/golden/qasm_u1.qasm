OPENQASM 2.0;
include "qelib1.inc";
qreg q[1];
h q[0];
u1(3*pi/4) q[0];
h q[0];
