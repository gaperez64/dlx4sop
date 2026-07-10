OPENQASM 2.0;
include "qelib1.inc";
qreg q[1];
u3(pi,-pi,-pi/8) q[0];
