#!/usr/bin/env python3
#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
#
# Copyright (C) 2024 Scylladb, Ltd.
#

import argparse
import json
import subprocess
import sys
import unittest
import urllib.request
import urllib.parse


class Metrics:
    def __init__(self, lines: list[str]):
        self.lines: list[str] = lines

    def get(self, name: str):
        pass


class TestPrometheus(unittest.TestCase):
    exporter_path = None
    exporter_process = None
    port = 10001

    @classmethod
    def setUpClass(cls):
        args = [cls.exporter_path, '--port', '10000', '--smp=2']
        cls.exporter_process = subprocess.Popen(args,
                                                stdin=subprocess.PIPE,
                                                stdout=subprocess.PIPE,
                                                bufsize=0, text=True)
        # wait until the server is ready for serve
        cls.exporter_process.stdout.readline()

    @classmethod
    def tearDownClass(cls):
        cls.exporter_process.terminate()

    @classmethod
    def _get(cls, uri, query):
        params = urllib.parse.urlencode({'query_enum': query_enum})
        host = 'localhost'
        url = f'http://{host}:{cls.port}/{uri}?{params}'
        with urllib.request.urlopen(url) as f:
            lines = f.read().decode('utf-8').split('\n')
            return Metrics(lines)

    def test_aggregated(self):
        pass


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exporter',
                        required=True,
                        help='Path of the exporter executable')
    opts, remaining = parser.parse_known_args()
    remaining.insert(0, sys.argv[0])
    TestPrometheus.exporter_path = opts.exporter
    unittest.main(argv=remaining)
