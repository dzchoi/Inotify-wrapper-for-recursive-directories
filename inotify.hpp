// C++ Wrapper for inotify instances
//
// dzchoi,
// 01/19/18, defined using template, which is good for putting .cpp and .hpp together 
//           into a single .hpp file.
// 01/23/18, implemented .read().
// 01/25/18, implemented .add_watch() that is aware of duplicate or overlapping path 
//           names.
// 01/30/18, .add_watch() now has the rearrangement based on its in_move argument.

// Features:
// - Wraps inotify watch instances in a C++ class.
// - Can set up inotify watches on existing directories (not files).
// - Can handle recursive subdirectory watches (with pathnames not ending with '/') 
//   automatically.
// - Can handle moving watches dynamically and efficiently; watches are removed from all 
//   moving-out directories and added to all moving-in directories.
// - If a whole directory tree is copied into a watch at once, it will not miss reporting 
//   any member of the tree, which was an intrinsic problem of inotify system calls.
// - Utf-8 (such as hangul) pathnames handle well.
// - All run-time errors including system call errors are logged and thrown as 
//   std::system_error exception.
// - Can accept user-provided logging functions, like fprintf(stderr, ...) and syslog().
// - Supports timed waits in reading inotify events.

// references:
// http://inotify-simple.readthedocs.io/en/latest/
// https://lwn.net/Articles/605128/
// https://www.ibm.com/developerworks/library/l-inotify/index.html
// https://jdennis.fedorapeople.org/lwatch/html/InotifyOverview.html
// https://github.com/rvoicilas/inotify-tools



#ifndef INOTIFY_HPP
#define INOTIFY_HPP

#include <algorithm>  // mismatch()
#include <chrono>  // system_clock::now(), duration_cast<>
#include <cstring>  // strerror()
#include <experimental/filesystem>
    // path, path::filename(), directory_iterator(), is_directory(), is_other()
    // Todo: "experimental/" and "-lstdc++fs" will be no longer needed since gcc 8.0; see 
    // https://www.reddit.com/r/cpp/comments/7o9kg6.
#include <string>  // basic_string<>, string
#include <system_error>  // errno, system_error, system_category
#include <unordered_map>  // unordered_map<>, .find(), .emplace(), .erase(), .at()
#include "syslog.hpp"  // LOG_*, Syslog<>, log()
#ifdef DEBUG
#include <cstdio>  // printf()
#endif
extern "C" {
#include <poll.h>  // pollfd, POLLIN
#include <sys/inotify.h>  // inotify_*(), IN_*, inotify_event
#include <unistd.h>  // read(), close(), usleep()
}

namespace fs = std::experimental::filesystem;
    // Todo: "::experimental" will not be necessary since gcc 8.0.



// Helper function

// Simple wrapper of std::printf() that compiles only under DEBUG.
#ifdef DEBUG
#define printf(...) std::printf(__VA_ARGS__)
#else
#define printf(...)
#endif
/*
// The inline version may not be so cheap because it still evaluates its parameters for 
// any possible side effects.
template <typename... Args>
void printf(const char* format, Args... args) const {
#ifdef DEBUG
    std::printf(format, args...);
#endif
}
*/

template <typename CharT, typename Traits, typename Alloc>
inline std::basic_string<CharT, Traits, Alloc> operator/(
    const std::basic_string<CharT, Traits, Alloc>& path1, const CharT* path2)
// Todo: Improve it.
// inline std::string operator/(std::string&& path1, const std::string& path2)
{
    return (fs::path(path1)/fs::path(path2)).string();
    // Todo:
    // If either path1 or path2 is empty, simple return the other side without '/' 
    // intervening in-between.
}

// Class for an inotify instance that monitors (only) directories (possibly recursively)
template <typename Log =Syslog<LOG_ERR>>
    // The parameter Log is type of a function (object), void (*)(const char*...), that 
    // is used for logging.
class Inotify {
    const Log& log;  // a function (object) for logging

    const int fd;  // inotify file descriptor associated with this inotify instance
    pollfd fds;  // internal struct for calling poll()
    uint32_t mask;  // the common mask to monitor all watches with
	// Although the inotify_add_watch() can set a separate mask for each watch, we 
	// provide here only a single global mask, because we are supporting only 
	// directory watches and new directories can be created recursively and 
	// implicitly, or existing watch directories can be moved into another watch. 
	// Though such directories could inherit the masks of their parent directory 
	// watches, it makes more sense to get them a default global mask.

    struct Watch { std::string path; bool in_move; };
    std::unordered_map<int, Watch> watches;  // dictionary that holds all watches

    inotify_event buffer[(4 *1024+sizeof(inotify_event)-1) / sizeof(inotify_event)];
	// buffer to read in inotify events data from kernel.
	// Its size is ~4K, but does not need more since inotify also has in-kernel 
	// buffer, which has the size specified in /proc/sys/fs/inotify/max_queued_events.
    int bytes_in_buffer =0;
    int bytes_handled =0;

public:
    // Note, member functions that are not specified as noexcept may throw an 
    // system_error exception, which results from system call errors.

    Inotify(const Log& log, uint32_t mask =IN_ALL_EVENTS):
	log { log }, fd { inotify_init1(IN_NONBLOCK) }, fds { fd, POLLIN }, mask { mask }
    {
	if ( fd == -1 )
	    throw std::system_error(errno, std::system_category());
    }

    ~Inotify() { close(fd); }

    const std::string& path(int wd) const {
	// will throw an out_of_range exception if wd is not existing.
	return watches.at(wd).path;
    }

    int add_watch(const std::string&, bool =true);
    void rm_watch(int wd) noexcept;
    void rm_all_watches() noexcept;

    const inotify_event* read(int timeout =(-1), int read_delay =0);
};

template <typename Log>
int Inotify<Log>::add_watch(const std::string& path, bool in_move)
// The given path is required to be non-empty string for an existing directory. 
// Otherwise, it will be ignored, but with an error logged.
// If the path already contains child files and subdirectories in it, the in_move flag 
// comes in play. If true and if the path is recursive (that is, not ending with '/'), 
// every subdirectory (but not file) will have its own watch set up recursively and 
// implicitly without notifying. If false, regardless of whether the path is recursive or 
// not, every (immediate) child file and subdirectory will be prepared to be reported as 
// IN_CREATE by the next call to read() (but, in this case, read() may duplicately report 
// the same IN_CREATE event for some files or directories, depending on how fast the path 
// watch kicks in).
// Note that the in_move flag is not just for IN_MOVED_TO'd watches, but also can be used 
// for initial setup of watches on existing directories, which is why its default 
// argument is set true.
// Todo: watch for non-existing directory/file yet.
// Todo: negative watch specification.
{
    const bool recursive = path.back() != '/';
	// will be always true if called from read().
    const int wd = inotify_add_watch(fd, path.c_str(),
	mask | IN_ONLYDIR | IN_MOVE_SELF | (recursive ? IN_CREATE | IN_MOVED_TO : 0));
	// IN_ONLYDIR is to set up a watch on directory only.

    if ( wd == -1 ) {  // if non-directory, non-existing, or without read-permission,
	//log("Warning: Cannot watch \"%s\": %m", path.c_str());
	    // We can use "%m" for strerror(errno) if log is Syslog<> function object.
	log("Warning: Cannot watch \"%s\": %s", path.c_str(), std::strerror(errno));
	return wd;
    }

    const auto it = watches.find(wd);
    if ( it == watches.end() ) {
	printf("[%d] %s created\n", wd, path.c_str());
	watches.emplace(wd, Watch { path, false });
    }

    else {  // if the watch was already registered,
	// The above inotify_add_watch() will return wd of an existing watch if there 
	// already exists a watch for the given path, in which case we determine the 
	// watch is moved rather than created newly.

	const std::string& path0 = it->second.path;
	const auto& diff = std::mismatch(path0.begin(), path0.end(), path.begin());
	    // finds the first mismatched characters in comparing path0 and path.
	if ( diff.first == path0.end() &&
	    (diff.second == path.end() || !recursive && diff.second+1 == path.end()) ) {
	    // Do nothing if path == path0 or path == path0 + "/".
	    printf("[%d] %s ignored as a duplicate\n", wd, path.c_str());
	    return wd;
	}
	printf("[%d] %s %s\n", wd, path.c_str(),
	    //in_move ? "moved" : "changed to recursive");
		// commented-out because in_move can be true for initial setup of watches 
		// on existing directories.
	    diff.second == path.end() && diff.first+1 == path0.end() &&
		*diff.first == '/' ? "changed to recursive" : "moved");
	it->second.path = path;
	//it->second.in_move = false;  // not necessary
    }

    // Note, when a directory that is either already a watch or not is moved into another 
    // watch, its children come into existence at the same time of its move and we will 
    // never get a report from it for the existence of any its children although its 
    // watch (if it is a watch) kicks in immediately on its move, and even if it has any 
    // subdirectories that are also a watch we will not get any report for its 
    // grand-children since they are also moved at the same time as it is moved. If it is 
    // not a watch, we can only set up its watch, no matter how fast we do, after all its 
    // children have been moved. So, we will get notified of only the top directory in 
    // this case.

    // When a non-empty directory is copied into a watch together with its children, 
    // however, we may get notified of the existence of any its children, depending on 
    // how fast we set up its watch. If some children are already copied before we set up 
    // the watch, we will miss reports for those children. If it has any grand-children, 
    // we will get their reports only after we set up a watch for their corresponding 
    // parent and only for such grand-children that are copied after their parent watch 
    // kicks in. This is because the inotify watches are basically non-recursive and 
    // effective only for their immediate children. So, in this case, we may get reports 
    // for some children and may miss for other children.

    // In case we are IN_MOVED_To'd, we traverse all the subdirectories (but not files) 
    // and set up their watches recursively and implicitly, without reporting to the 
    // read().
    if ( in_move ) {  // if we are IN_MOVED_TO'd,
	if ( recursive )
	    // We create a watch for every subdirectory down below, but without reporting 
	    // it to read().
	    for ( const fs::path& subdir: fs::directory_iterator(path) )
		// The directory_iterator() here will always succeed, since the previous 
		// check for "wd == -1" filters out what is not this case.
		if ( fs::is_directory(subdir) /* && !fs::is_symlink(subdir) */ )
		    // The is_directory() and the is_symlink() are mutually exclusive.
		    add_watch(subdir.string(), true);
    }

    // If we are IN_CREATEd, we traverse our immediate children (both files and 
    // subdirectories) and we prepare them to be reported as IN_CREATEd by the read(), 
    // which may possibly get a duplicate event for some of them from the system but will 
    // not miss any. We do not recurse into any child subdirectories here because read() 
    // will do, but a duplicate recurse of read() due to a duplicate event from the 
    // system will be checked out by the previous "ignored as a duplicate" filtering.
    else {  // if we are IN_CREATEd,
	for ( const fs::path& path: fs::directory_iterator(path) )
	    if ( !fs::is_other(path) ) {
		// if a directory, a regular file, or a symlink, but not device file, 
		// fifo, or socket,

		// Make a new IN_CREATE event to be read().
		const bool isdir = fs::is_directory(path);
		if ( mask & IN_CREATE || isdir && recursive ) {
		    const std::string& name = path.filename().string();
		    const uint32_t len =
			(name.size()+sizeof(int))/sizeof(int)*sizeof(int);
			// length including '\0' that fits in word boundary
		    inotify_event& event =
			*(inotify_event*)((char*)buffer + bytes_in_buffer);
		    bytes_in_buffer += sizeof(inotify_event) + len;
		    if ( bytes_in_buffer > sizeof(buffer) ) {
			log("Error: add_watch() - Buffer underruns");
			throw std::system_error(EINVAL, std::system_category());
		    }
		    event.wd = wd;
		    event.mask = isdir ? IN_ISDIR | IN_CREATE : IN_CREATE;
		    event.cookie = 0;
		    event.len = len;
		    name.copy(event.name, len);
		    event.name[name.size()] = '\0';
		}
	    }
    }

    return wd;  // return wd of only the top directory
}

template <typename Log>
void Inotify<Log>::rm_watch(int wd) noexcept
// Remove the watch associated with wd, and then IN_IGNORED event will be generated for 
// this wd.
{
    if ( inotify_rm_watch(fd, wd) )  // == -1
	log("Warning: inotify_rm_watch():%d - %s", errno, std::strerror(errno));
}

template <typename Log>
void Inotify<Log>::rm_all_watches() noexcept
// Delete all watches.
// Unlike ~Inotify(), we can continue to use .add_watch() and .read().
{
    for ( const auto& it: watches )
	rm_watch(it.first);  // calls rm_watch() for each key in watches
}

template <typename Log>
const inotify_event* Inotify<Log>::read(int timeout, int read_delay)
// Read one inotify event from fd, or return nullptr if timed out.
// Will throw an exception when an error is returned from poll() or read().

// The read_delay: The time in milliseconds to wait after the first event arrives before 
// reading the buffer. This allows further events to accumulate before reading, which 
// allows the kernel to consolidate like events and can enhance performance when there 
// are many similar events. The read_delay should be in [0..1000], or may cause a 
// run-time exception.
// If no watches are set up, read() will still run ok and will wait for nothing.
// Todo: Handle IN_Q_OVERFLOW and restart the daemon.
{
    const auto then = std::chrono::system_clock::now();  // check starting time

    // If the buffer underruns, (re)fill it by reading more events from read().
    if ( bytes_in_buffer == 0 ) {
	const char* where;

	where = "poll()";
	switch ( poll(&fds, 1, timeout) ) {
	    case 0:  // timed out!
		return nullptr;

	    default:  // or, "case 1:" and events are ready!
		where = "usleep()";
		if ( usleep(read_delay*1000u) != -1 ) {
		    where = "read()";
		    bytes_in_buffer = ::read(fd, buffer, sizeof(buffer));
		    if ( bytes_in_buffer > 0 )
			break;
		    if ( bytes_in_buffer == 0 )
			// EOF reached. Possibly too many events occurred at once?
			errno = EIO;
		}
		// intentional fall-through

	    case -1:  // error in system call
		log("Error: %s:%d - %s", where, errno, std::strerror(errno));
		throw std::system_error(errno, std::system_category());
	}
    }

    do {
	const inotify_event& event =
	    *(inotify_event*)((char*)buffer + bytes_handled);

	bytes_handled += sizeof(inotify_event) + event.len;
	if ( bytes_handled >= bytes_in_buffer ) {
	    if ( bytes_handled > bytes_in_buffer ) {  // sanity check
		// An incomplete event was read from read(). This is not to happen, but 
		// we just report the same failure that read() would do in case the 
		// buffer is too small to hold an event.
		log("Error: read() - Incomplete event returned");
		throw std::system_error(EINVAL, std::system_category());
	    }
	    bytes_handled = bytes_in_buffer = 0;
	}

	const auto it = watches.find(event.wd);
	if ( it == watches.end() ) {  // sanity check
	    log("Error: read() - Event for unknown wd [%d] possibly due to IN_Q_OVERFLOW",
		event.wd);
	    throw std::system_error(EINVAL, std::system_category());
	}
	Watch& watch = it->second;
	printf("- [%d] %s (%#x)\n", event.wd,
	    (event.len ? watch.path/event.name : watch.path).c_str(), event.mask);

	// A new subdirectory was created or moved in.
	if ( event.mask & (IN_CREATE | IN_MOVED_TO) &&
	    event.mask & IN_ISDIR &&
	    watch.path.back() != '/' ) {
	    const bool in_move = event.mask & IN_MOVED_TO;  // will cast to 0 or 1.
	    const int wd = add_watch(watch.path/event.name, in_move);
	    // The event.name here will be non-empty for IN_CREATE and IN_MOVED_TO.
	    // When a watch is moved into another directory, the watch is retained only 
	    // if that directory is also a watch and recursive, or deleted otherwise (at 
	    // the next IN_MOVE_SELF event). And, even if the watch was not recursive, it 
	    // turns into a recursive one and all its subdirectories as well. This way, 
	    // every subdirectory of a recursive watch directory is always a recursive 
	    // watch.

	    // Actually, we should think of the IN_MOVED_TO event as two separate events; 
	    // deleting the old watch, and creating a new watch because put under a 
	    // recursive watch directory. However, the inotify simply reuses (recycles) 
	    // the same watch in this case for efficiency reasons, and we just mark this 
	    // recycle on the in-struct in_move flag. If a watch is marked with this 
	    // recycle, it will survive the next IN_MOVE_SELF and will not be deleted.
	    if ( in_move && wd >= 0 /* != -1 */ )
		watches.at(wd).in_move = true;
		// In case of in_move, we here mark only the top directory of the moved 
		// tree as in_move, because the IN_MOVED_TO event and the corresponding 
		// IN_MOVE_SELF event (if any) arrive only with the top directory, but 
		// not recursively.
	}

	// Delete a watch recursively unless in move.
	if ( event.mask & IN_MOVE_SELF ) {
	    // We do not need to check here if it is a directory because only directory 
	    // watches were allowed.
	    if ( watch.in_move )
		// Do not delete if marked as in_move.
		watch.in_move = false;

	    else if ( watch.path.back() == '/' )
		rm_watch(event.wd);

	    else
		// We recursively delete this MOVE_SELF'd watch and all the watches for 
		// its subdirectories (whether or not they are recursive watches).
		for ( const auto& it: watches )
		    if ( it.second.path.compare(0, watch.path.size(), watch.path) == 0 )
			// checks if it.second.path starts with watch_path. Even if 
			// it.second.path is a prefix of watch.path on the other hand, it 
			// will be handled ok and compare() will yield non-zero.
			rm_watch(it.first);
		    // We do not actually remove members from the watches map here, which 
		    // will be done at the IN_IGNORED event later. That is why we could 
		    // declare watch.path to be a string reference rather than a copy of 
		    // string.
	}

	// A watch was deleted implicitly, so delete it from the dictionary too.
	if ( event.mask & IN_IGNORED ) {
	    printf("[%d] %s deleted\n", event.wd, watch.path.c_str());
	    watches.erase(event.wd);
	}

	// If a matching event is found, return it.
	if ( event.mask & mask )
	    return &event;

    // Repeat until all bytes in the buffer are handled.
    } while ( bytes_handled > 0 );

    // So far, We got some events and handled all of them, but we could not report any 
    // events of interest to the caller. We will take another oppertunity to read more 
    // events only if we have enough time left though, or report nothing otherwise.

    const int time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	std::chrono::system_clock::now() - then).count();
    if ( timeout < 0 /* == -1 */ || (timeout -= time_elapsed) > 0 )
	return read(timeout, read_delay);  // The tail-call optimization will work here.

    return nullptr;  // timed out!
}

#endif /* INOTIFY_HPP */
