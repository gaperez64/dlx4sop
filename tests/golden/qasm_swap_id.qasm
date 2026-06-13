OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
h q[0];
t q[0];
id q[0];
swap q[0], q[1];
h q[1];
