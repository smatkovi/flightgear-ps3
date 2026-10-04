// SGSerialPort for PS3: there are no serial devices, a port never opens.

#include <simgear/compiler.h>
#include STL_STRING
#include <simgear/serial/serial.hxx>

SGSerialPort::SGSerialPort() : dev_open(false) {}
SGSerialPort::SGSerialPort(const string&, int) : dev_open(false) {}
SGSerialPort::~SGSerialPort() {}
bool SGSerialPort::open_port(const string&) { return false; }
bool SGSerialPort::close_port() { dev_open = false; return true; }
bool SGSerialPort::set_baud(int) { return false; }
string SGSerialPort::read_port() { return ""; }
int SGSerialPort::read_port(char *, int) { return 0; }
int SGSerialPort::write_port(const string&) { return 0; }
int SGSerialPort::write_port(const char *, int) { return 0; }
