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

import os
import shutil

SUPPORTED_MODES = ['release', 'debug']

ROOT_PATH = os.path.realpath(os.path.dirname(__file__))

BUILD_PATHS = { mode: os.path.join(ROOT_PATH, 'build', mode) for mode in SUPPORTED_MODES }

# prefer cmake3 over cmake, the cmake executable packaged by EPEL7 is named "cmake3"
CMAKE_BASIC_ARGS = [shutil.which('cmake3') or shutil.which('cmake'),
                    '-G', 'Ninja']

def translate_arg(arg, new_name, value_when_none='no'):
    """
    Translate a value populated from the command-line into a name to pass to the invocation of CMake.
    """
    if arg is None:
        value = value_when_none
    elif type(arg) is bool:
        value = 'yes' if arg else 'no'
    else:
        value = arg

    return '-DSEASTAR_{}={}'.format(new_name, value)
