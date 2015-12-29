#!/usr/bin/python

import sys, getopt, rid

def main(argv):

    prefix = ''

    try:
        opts, args = getopt.getopt(argv,"hp:",["prefix="])
    except getopt.GetoptError:
        print 'test.py -p <prefix>'
        sys.exit(2)

    num_arg = len(sys.argv)

    for opt, arg in opts:
        if opt == '-h' or num_arg == 1:
            print 'test.py -p <prefix>'
            sys.exit()
        elif opt in ("-p", "--prefix"):
            prefix = arg
            print "RID created from '",prefix,"' is", rid.name_to_rid(prefix)

if __name__ == "__main__":
    main(sys.argv[1:])
