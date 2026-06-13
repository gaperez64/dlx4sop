OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
h q;
u1(pi/4) q;
h q;
