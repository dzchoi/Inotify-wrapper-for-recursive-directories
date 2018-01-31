// To compile: g++ [-DDEBUG] -O2 test.cpp -lstdc++fs

#include <iostream>
#include "syslog.hpp"
#include "inotify.hpp"

Syslog<> log;  // logging function using syslog()

int main() {
    try {
	Inotify<> inotify { log };
	inotify.add_watch("/home/user1");   // meaning "/home/user1/**/"
	inotify.add_watch("/home/user2/");  // meaning "/home/user2/*/"
	for (;;) {
	    const inotify_event* eventp = inotify.read();
	    (std::cout << inotify.path(eventp->wd) << ": "
		).write(eventp->name, eventp->len)
		<< "\t(0x" << std::hex << eventp->mask << ")\n";
	}
    }

    catch (std::system_error& error) {
	std::cout << "Error: " << error.code() << " - " << error.what() << '\n';
    }
}
