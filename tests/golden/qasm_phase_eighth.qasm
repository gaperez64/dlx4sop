OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
p(pi/8) q[0];
cp(-7*pi/8) q[0], q[1];
