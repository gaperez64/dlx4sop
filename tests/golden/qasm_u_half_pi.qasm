OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
u(pi/2,-pi/8,-3*pi/4) q[0];
u(-pi/2,pi/8,3*pi/8) q[1];
