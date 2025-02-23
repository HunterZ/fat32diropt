# `fat32diropt` - FAT32 Directory Optimizer
## Hello
This is currently a prototype Linux command-line application that moves all directory data to the beginning of a FAT32 partition stored in a file or block device, via standard C++ file I/O operations.

## _**DANGER!**_
This is only a prototype. This may corrupt your stuff if something goes wrong in the middle of optimizing!

It does try to handle a fair amount of error conditions, and traps SIGINT and SIGTERM in order to attempt a graceful shutdown. It does _not_ try to undo any operations, so things could be left in a very bad state if something goes wrong.

## What?
Directories are really just files on a FAT32 partition, whose data is a table of 32-byte entries describing their contents (volume labels, devices, subdirectories, files, long filename data, etc.).

This means that if you have a spinning-rust HDD formatted as FAT32, directories can get scattered all over the disk. This can cause problems like high seek times, fragmentation of large files that have to fit between the scattered directory data, etc.

Unfortunately, Microsoft decided to chicken out when designing their modern Windows defragmentation API by omitting the ability to move that directory data.

Other methods of achieving this such as moving everything off of and back onto the drive can only work until directories are added/removed, at which point chaos sets in again.

## How?
This currently uses C++ `std::fstream` to open a specified file or block device for binary read and write. **NOTE:** This does mean you probably need `sudo` to operate on a block device (e.g. `/dev/sdb1`).

It performs a breadth-first walk of the directory tree, and attempts to arrange all data clusters for all directory clusters in an ordered, contiguous arrangement. Any regular file clusters in the target locations are moved to the first free clusters after what will ultimately be the "directory area".

Moving FAT32 directories is only slightly more difficult than moving regular files: When moving the first cluster of one, it's also necessary to update the start cluster of its `.` entry, as well as the start cluster of all of its subdirectories' `..` entries.

## Why?
I have a spinning-rust USB HDD with lots of files on it, some of which are very large, and many small. My attempts to do fancy arrangements of these files to avoid fragmentation were thwarted by the realization that directory data was getting in the way.

## No!
This project _only_ arranges directory data clusters. It does _not_ change the order of files/directories in the directory tree (there are other projects that do this). It does _not_ optimize/degragment the entire disk - in fact, it will likely create fragmentation of non-directories while moving them out of the "directory area", so you should consider running a defragmenter afterwards.

This currently only runs in Linux, because it's easy to modify physical disks via block device pseudo-files. Note that the partition must be unmounted.

## Who?
This was created by myself, for myself, but shared in case anyone else is interested. Most of the knowledge needed was gleaned from the following Wikipedia article, which will probably be deleted someday since that's all they seem to do now: https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system