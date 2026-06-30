# text analyzer

A mini project from the little book of C.

Build a simple tool that reads a text file and reports basic statistics, including number of lines, words, and characters it contains. 

Requirements:
1. take a file name as a required command line argument.
1. open the file safely, reporting errors if it does not exist or cannot be read.
1. read the file by streaming its contents (don't load the whole file into memory at once)
1. count total characters, total words, and total lines.
1. compute the most commonly used words and characters, both as counts and frequencies.
1. print a summary at the end to stdout
1. include a CLI option to print the output in json format
1. avoid magic numbers or repeated constants throughout the code. Reuse constant values, making them settable by command line options with sane defaults, rejecting invalid values (e.g. non-positive numbers) with a clear error.
1. structure the project as a main CLI executable using one or more libraries. Write unit tests of the libaries. 
1. Add a help menu to the CLI program

## TODO

1. ~~translate to rust~~ (done — see `rust/`)
1. report word length statistics: mean, min, max, quantiles 25, 50, 75
1. count blank lines, digits, and punctuation marks
1. support reading from stdin
1. support multiple files