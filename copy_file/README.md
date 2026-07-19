# copy file

A mini project from the little book of c.

Build a simple utility that copies the contents of one file to another file, just like the `cp` command on linux.

Requirements:
1. take two filenames as command line arguments, the source and the destination
1. support relative paths, and paths beginning with `~` or `~user` (expanded to the
   corresponding home directory)
1. when the destination is an existing directory, copy the source into it under the
   source's base file name, like `cp`
1. open the source file for reading
1. open or create the destination file for writing
1. copy all contents from source to destination
1. close both files and confirm success
1. handle potential errors and report any errors clearly

