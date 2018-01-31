// Wrapper for syslog() system call in C++
// 01/20/18, dzchoi, feature complete

// How to use:
// - Define a global (or local) instance of Syslog object:
//   Syslog<LOG_INFO> log;
//   Syslog<> log;	(setting LOG_ERR as a default priority)
//   Syslog<> log("backupd", LOG_PID, LOG_DAEMON);
// - Write a log:
//   log(LOG_WARNING, "errno=%d, strerror(errno)=%m", errno);
//   log("...");		(using the default priority and the default facility)
//   log(LOG_DAEMON, "...");	(using the default priority)
//   log(LOG_INFO|LOG_DAEMON, "...");

// See http://hant.ask.helplib.com/c++/post_707188 for wrapping with the error stream



#ifndef SYSLOG_HPP
#define SYSLOG_HPP

#include <cstdarg>  // va_list, va_start(), va_end()
extern "C" {
#include <syslog.h> // LOG_*, openlog(), closelog(), vsyslog(), setlogmask()
}

template <int Priority =LOG_ERR>
// Priority is the default priority to log with.
class Syslog {	// being declared as a function object
// Should be used to define a single global object (though not forced), since the C 
// openlog() keeps all its settings (ident, option, and facility, but not Priority) 
// globally.

public:
    Syslog(const char *ident, int option =LOG_ODELAY, int facility =LOG_USER) {
	// The contents pointed to by ident is not backed up, so assumed to be unchanged 
	// across the entire program.
	openlog(ident, option, facility);
    }
    Syslog() {}
	// does not call openlog(), retaining any previous settings of openlog() from 
	// when constructing the last Syslog<> object.
    ~Syslog() { closelog(); }

    void operator()(int priority, const char *format...) const;
    // Note, the priority argument is formed by ORing together a facility value (like 
    // LOG_AUTH) and a level value (like LOG_ERR). If no facility value is ORed into the 
    // priority, then the default value set by openlog() is used, or, if there was no 
    // preceding openlog() call, a default of LOG_USER is employed.

    template <typename... Args>  // uses the default Priority
    void operator()(const char *format, Args... args) const {
	operator()(Priority, format, args...);
    }

    int setlogmask(int mask) { return ::setlogmask(mask); }
};

template <int Priority>
void Syslog<Priority>::operator()(int priority, const char *format...) const {
    if ( LOG_PRI(priority) == 0 )
	priority |= LOG_PRI(Priority);

    va_list ap;  // arg startup
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);  // arg cleanup
}

#endif /* SYSLOG_HPP */
