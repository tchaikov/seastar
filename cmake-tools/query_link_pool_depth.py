#!/usr/bin/env python

import os

MEMORY_PER_LINK_PROCESS = 7e9

def query_physical_memory():
    return os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')

if __name__ == '__main__':
    link_depth = max(1, int(query_physical_memory() / MEMORY_PER_LINK_PROCESS))
    print(link_depth)
