#! /bin/bash
set +e

usage()
{
cat << EOF
usage: $0 options

This script creates a lot of files.

OPTIONS:
   -h      Show this message
   -b      Binding uuid (required)
   -f      Number of files
   -d      Number of dirs
   -s      Start file number
   -t      Start dir number
   -v      Verbose
EOF
}

numfiles=0
numdirs=0
verbose=0
startnum=0
startdir=0
bid="none"

while getopts "hb:d:f:s:t:v" OPTION
do
     case $OPTION in
         h)
             usage
             exit 1
             ;;
         b)
	     bid=$OPTARG
             ;;
         f)
	     numfiles=$OPTARG
             ;;
         d)
	     numdirs=$OPTARG
             ;;
         s)
	     startnum=$OPTARG
             ;;
         t)
	     startdir=$OPTARG
             ;;
         v)
             verbose=1
             ;;
         ?)
             usage
             exit
             ;;
     esac
done

# If no bid was passed in, see if we are in a directory where
# we can extract a valid bid
if [ $bid == "none" ]; then
    if [ -f ../fusedav.conf ]; then
	cd ..
    fi
    if [ ! -f fusedav.conf ]; then
        echo "-b <binding uuid> is required. Exiting ..."
        exit
    fi
    curdir=$(pwd)
    bid=${curdir##*/}
fi

biddir=/srv/bindings/$bid/files

if [ $numdirs -eq 0 ]; then
    numdirs=10
fi

if [ $numfiles -eq 0 ]; then
    numfiles=100
fi

if [ $verbose -eq 1 ]; then
    starttime=$(date +%s)
fi

# append; this also creates a file for later tests
head -c 64 /dev/urandom > urlencode.file

iters=0
numdir=1
while [ $numdir -le $numdirs ]
do
	dirnum=$(( $numdir + $startdir ))
	dirname=$biddir/dir-$dirnum
	if [ $verbose -eq 1 ]; then
		echo "$numdir: mkdir $dirname"
	fi
	mkdir $dirname
	numdir=$(( numdir + 1 ))

	numfile=1
	while [ $numfile -le $numfiles ]
	do
	    filenum=$(( $numfile + $startnum ))
	    filename=$dirname/file-$filenum
	    if [ $verbose -eq 1 ]; then
            echo "$iters: head -c 64 /dev/urandom > $filename"
	    fi
	    head -c 64 /dev/urandom > $filename
	    numfile=$(( numfile + 1 ))
	    iters=$(( iters + 1 ))
	done
done

if [ $verbose -eq 1 ]; then
    endtime=$(date +%s)
    elapsedtime=$(( $endtime - $starttime ))
    echo "Elapsed time: $elapsedtime"
fi
