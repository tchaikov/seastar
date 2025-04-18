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
from http import HTTPStatus


class TestJson2Code(unittest.TestCase):
    rest_api_httpd = None
    server = None
    port = 15789

    @classmethod
    def setUpClass(cls):
        args = [cls.rest_api_httpd, '--port', f'{cls.port}', '--smp=2']
        cls.server = subprocess.Popen(args,
                                      stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE,
                                      bufsize=0, text=True)
        # wait until the server is ready for serve
        cls.server.stdout.readline()

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()

    def test_path_params(self):
        var1 = 'bon'
        var2 = 'jour'
        query_enum = 'VAL2'
        response = self._do_query(var1, var2, query_enum)
        self.assertEqual(response['var1'], f'/{var1}')
        self.assertEqual(response['var2'], f'/{var2}')
        self.assertEqual(response['enum_var'], query_enum)

    def test_bad_enum(self):
        var1 = 'bon'
        var2 = 'jour'
        query_enum = 'unknown value'
        response = self._do_query(var1, var2, query_enum)
        self.assertEqual(response['var1'], f'/{var1}')
        self.assertEqual(response['var2'], f'/{var2}')
        self.assertEqual(response['enum_var'], 'Unknown')

    def test_missing_path_param(self):
        query_enum = 'VAL2'
        params = urllib.parse.urlencode({'query_enum': query_enum})
        url = f'http://localhost:{self.port}/hello/world/?{params}'
        with self.assertRaises(urllib.error.HTTPError) as e:
            with urllib.request.urlopen(url):
                pass
        ex = e.exception
        self.assertEqual(ex.code, HTTPStatus.NOT_FOUND)
        response = json.loads(ex.read().decode('utf-8'))
        self.assertEqual(response['message'], 'Not found')
        self.assertEqual(response['code'], HTTPStatus.NOT_FOUND)

    def test_malformatted_query_param(self):
        var1 = 'bon'
        var2 = 'jour'
        query_enum = 'VAL2'
        query_integer = 'i-am-not-a-number'
        use_streaming = True
        params = urllib.parse.urlencode({'query_enum': query_enum,
                                         'query_integer': query_integer,
                                         'use_streaming': str(use_streaming).lower()})
        url = f'http://localhost:{self.port}/hello/world/{var1}/{var2}?{params}'
        with self.assertRaises(urllib.error.HTTPError) as e:
            with urllib.request.urlopen(url):
                pass
        ex = e.exception
        self.assertEqual(ex.code, HTTPStatus.BAD_REQUEST)
        response = json.loads(ex.read().decode('utf-8'))
        self.assertEqual(response['message'], 'query_integer')
        self.assertEqual(response['code'], HTTPStatus.BAD_REQUEST)

    def test_arrays(self):
        response = self._do_query('x', 'x', 'VAL2')
        expected = [1, 2, 3]

        self.assertEqual(response['array_var'], expected)
        self.assertEqual(response['chunked_array_var'], expected)

    def _do_query(self, var1: str, var2: str, query_enum: str):
        def query(streaming: bool = False):
            params = urllib.parse.urlencode({
                'query_enum': query_enum,
                'use_streaming': str(streaming).lower()
            })
            url = f'http://localhost:{self.port}/hello/world/{var1}/{var2}?{params}'
            with urllib.request.urlopen(url) as f:
                return json.load(f)

        # always query both the streaming and not-streaming variations and
        # ensure their output is the same
        stream_no, stream_yes = query(False), query(True)
        self.assertEqual(stream_no, stream_yes)
        return stream_no


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--rest-api-httpd',
                        required=True,
                        help='Path of the rest_api_httpd executable')
    opts, remaining = parser.parse_known_args()
    remaining.insert(0, sys.argv[0])
    TestJson2Code.rest_api_httpd = opts.rest_api_httpd
    unittest.main(argv=remaining)
