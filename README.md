# Inotify wrapper in C++ for recursive directories

<Working on...>

### Wraps inotify instances in a C++ class.

It wraps [`INOTIFY(7)`](http://man7.org/linux/man-pages/man7/inotify.7.html) API and its instances in a C++ class.
```cpp
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
```
This is a copy of `test.c` in this repository. Here, we adds two watches for "/home/user1" and "/home/user2/".

"/home/user1", as not ending with the trailing `'/'`, has a recursive watch. It and all its files and subdirectories in any level will be monitored. However, "/home/user2" has a non-recursive watch and will be monitored for only its immediate files and subdirectories.

### Can set up watches on existing directories.

We can set up watches on existing directories (but not files) and we must have read-permission for them. Otherwise, the watch will be ignored but will be logged with the given logging function.

Thus, an empty-string path, a non-existing path, a non-directory path, or a path without its read-permission will be simply ignored.

### Can handle recursive directory watches automatically and implicitly.

A watch on a directory name that does not end with `'/'` is regarded as a recursive watch, and another watch will be attached to every subdirectory in any level automatically and implicitly. That is, on the initial setup of the top directory watch, watches for all existing subdirectories will be also set up recursively, and when a new subdirectory is created or moved in after, it will also have a watch attached to it.

### Can handle moving watches dynamically and efficiently.

Watches can be moved using `rename` or `mv`. We have a basic principal: when a watched directory moves (or renames) the associated watch gets detached from the directory and is removed, and if the (non-watched) directory happens to moved in another watch and only if that watch is recursive the directory will have a watch attached to it.

It seems like inefficient and seems include the overhead of deleting and then recreating watches. Internally, however, watches are actually moved (or renamed) with a single operation. The principal is just for easy understanding.

Note that our focus is different from that of the legacy [`inotifywait(1)`](https://linux.die.net/man/1/inotifywait), which is also based on `INOTIFY(7)` but focuses on the watches rather than their associated directories. With `inotifywait`, if a watched directory is moved the watch is always retained but only changes its associated directory. On the other hand, we are focusing on the directory itself, the pathname of the directory to be exact, and we think its watch is associated with the pathname of the directory. So, if the directory is renamed or moved, it will lose its associated watch.

### Can handle copy of a directory tree perfectly.

Since the `INOTIFY(7)` is basically non-recursive and effective only on the immediate children of the watched directory, when a whole directory tree is copied into a watched directory we set watches for any subdirectories ourselves. However, the inotify system has a problem that we may miss events for some members in the tree.

For example, suppose a directory `a` that contains a file `A.txt` is copied into a (recursively-) watched directory `b` by using like `cp -r a b`. Then, since `b` is a recursive watch and monitoring its contents, it will automatically set up a watch for `a` when it sees `a` is created in it, while we are copying the directory `a` and the file `A.txt` into it. If the watch for `a` happens to be created, up and running before `A.txt` is copied, the inotify system will successfully get notified and will report to us. Otherwise, however, we are out of luck and will not get reported from the inotify system.

This problem is overcome with this library. If a directory tree is copied into a (recursively-) watched directory, we traverse the whole tree and we prepare all the files and subdirectories in it to be reported as an IN_CREATE event ourselves, not knowing whether or not those may get notified later and reported by the inotify system. So, in this case, we may get duplicated reports from our API `Inotify::read()` for some of them. Still better, however, than missing.

### Can handle Utf-8 encoded (such as Hangul) filenames well, thanks to C++ `std::string`.

### Can throw exceptions.

All run-time errors including system call errors are logged via user-provided logging function, and thrown as `std::system_error` exceptions. (The helper function `Inotify::path()` throws an `std::out_of_range` exception if the watch desciptor given as an argument does not exist.)

### Can accept user-provided logging functions.

When we define an inotify instance using `Inotify<>`, we can specify a logging function as the template parameter, which is by default a function object wrapping `syslog()` system call that is provided in `syslog.hpp` in this repository.

We can also use other logging functions such as:
```cpp
#include <cstdarg>  // va_list, va_start(), va_end()
#include <cstdio>  // vfprintf(), stderr
inline void log(const char* format...)
{
    va_list ap;  // arg startup
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    std::fputc('\n', stderr);  // append a newline character
    va_end(ap);  // arg cleanup
}
```

Then, we can use this `log()` function in defining an instance as:

`Inotify<void (*)(const char*...> inotify { log };`

or, if under C++17, simply as:

`Inotify inotify { log };`

### Can support timed wait in reading inotify events.

The API function `const inotify_event* Inotify::read(int timeout =(-1), int read_delay =0)` takes two arguments.

- `timeout` (in milliseconds): time to wait for an event, or (-1) to wait indefinitely. If timed out with no events, `nullptr` would return.
- `read_delay` (in milliseconds, \[0..1000\]): time to wait after the first event arrives before reading the kernel buffer. This allows further events to accumulate before reading, which allows the kernel to consolidate like events and can enhance performance when there are many similar events.

## To compile,

This library is only a single file `inotify.hpp`. (I like template programming so much, it even saves me from having to write class declaration and its member defintions in separate files. ^^) We just need to #include it and compile it. (`syslog.hpp` is optional, and will provide a function object wrapping `syslog()` system call.)

```
$ g++ -O2 test.cpp -lstdc++fs
```

- The compile option `-O2`, `-O3`, or `-foptimize-sibling-calls` is recommended, because some API recurse itself rather than jumps to itself for the simplicity reasons and will not take up unnecessry stack space under one of those compile options.
