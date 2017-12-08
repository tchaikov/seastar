#!/usr/bin/python3

import argparse
import os
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description="Query necessary CFLAGS for compiling DPDK.")
parser.add_argument('build_dir', metavar='DIR', help="The build directory of DPDK.")
parser.add_argument('machine', help="The architecture identifier for DPDK, like `ivb`.")
args = parser.parse_args()

with tempfile.NamedTemporaryFile() as sfile:
    dpdk_sdk_path = os.path.dirname(os.path.realpath(__file__)) + '/../dpdk'
    dpdk_target = args.build_dir
    dpdk_target_name = 'x86_64-{}-linuxapp-gcc'.format(args.machine)
    dpdk_arch = 'x86_64'

    sfile.file.write(bytes('include ' + dpdk_sdk_path + '/mk/rte.vars.mk' + "\n", 'utf-8'))
    sfile.file.write(bytes('all:' + "\n\t", 'utf-8'))
    sfile.file.write(bytes('@echo $(MACHINE_CFLAGS)' + "\n", 'utf-8'))
    sfile.file.flush()

    dpdk_cflags = subprocess.check_output(['make', '--no-print-directory',
                                           '-f', sfile.name,
                                           'RTE_SDK=' + dpdk_sdk_path,
                                           'RTE_OUTPUT=' + dpdk_target,
                                           'RTE_TARGET=' + dpdk_target_name,
                                           'RTE_SDK_BIN=' + dpdk_target,
                                           'RTE_ARCH=' + dpdk_arch])
    dpdk_cflags_str = dpdk_cflags.decode('utf-8')
    dpdk_cflags_str = re.sub(r'\n+$', '', dpdk_cflags_str)

    # CMake likes comma-separated values.
    print(';'.join(dpdk_cflags_str.split()))
