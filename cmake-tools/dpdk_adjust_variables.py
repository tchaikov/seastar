#!/usr/bin/python3

"""
Doing this from CMake is a pain due to its poor type system.
"""

import argparse
import os

# Adjust to taste.
DEFAULTS = {
    'CONFIG_RTE_LIBRTE_PMD_BOND': 'n',
    'CONFIG_RTE_MBUF_SCATTER_GATHER': 'n',
    'CONFIG_RTE_LIBRTE_IP_FRAG': 'n',
    'CONFIG_RTE_APP_TEST': 'n',
    'CONFIG_RTE_TEST_PMD': 'n',
    'CONFIG_RTE_MBUF_REFCNT_ATOMIC': 'n',
    'CONFIG_RTE_MAX_MEMSEG': '8192',
    'CONFIG_RTE_EAL_IGB_UIO': 'n',
    'CONFIG_RTE_LIBRTE_KNI': 'n',
    'CONFIG_RTE_KNI_KMOD': 'n',
    'CONFIG_RTE_LIBRTE_JOBSTATS': 'n',
    'CONFIG_RTE_LIBRTE_LPM': 'n',
    'CONFIG_RTE_LIBRTE_ACL': 'n',
    'CONFIG_RTE_LIBRTE_POWER': 'n',
    'CONFIG_RTE_LIBRTE_IP_FRAG': 'n',
    'CONFIG_RTE_LIBRTE_METER': 'n',
    'CONFIG_RTE_LIBRTE_SCHED': 'n',
    'CONFIG_RTE_LIBRTE_DISTRIBUTOR': 'n',
    'CONFIG_RTE_LIBRTE_REORDER': 'n',
    'CONFIG_RTE_LIBRTE_PORT': 'n',
    'CONFIG_RTE_LIBRTE_TABLE': 'n',
    'CONFIG_RTE_LIBRTE_PIPELINE': 'n',
}

parser = argparse.ArgumentParser(description='Update the configuration variables for DPDK.')
parser.add_argument('file', metavar='FILE', help="Path of the DPDK configuration file.")
parser.add_argument('machine', metavar='NAME', help="The architecture identifier for DPDK, like `ivb`.")

args = parser.parse_args()

lines = open(args.file, encoding='UTF-8').readlines()
new_lines = []

for line in lines:
    for var, val in DEFAULTS.items():
        if line.startswith(var + '='):
            line = '{}={}\n'.format(var, val)

    new_lines.append(line)

new_lines += 'CONFIG_RTE_MACHINE={}\n'.format(args.machine)
open(args.file, 'w', encoding='UTF-8').writelines(new_lines)
