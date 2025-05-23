/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2015 Cloudius Systems
 */

#ifdef SEASTAR_MODULE
module;
#include <vector>
module seastar;
#else
#include <seastar/http/json_path.hh>
#endif

namespace seastar {

namespace httpd {

using namespace std;

void path_description::set(routes& _routes, handler_base* handler) const {
    for (auto& i : query_params) {
        handler->mandatory(i);
    }

    if (path_params.size() == 0)
        _routes.put(operations.method, path, handler);
    else {
        match_rule* rule = new match_rule(handler);
        rule->add_str(path);
        for (auto&& i : path_params) {
            if (i.type == url_component_type::FIXED_STRING) {
                rule->add_str(i.name);
            } else {
                rule->add_param(i.name, i.type == url_component_type::PARAM_UNTIL_END_OF_PATH);
            }
        }
        _cookie = _routes.add_cookie(rule, operations.method);
    }
}

void path_description::set(routes& _routes,
        const json_request_function& f) const {
    set(_routes, new function_handler(f));
}

void path_description::set(routes& _routes, const future_json_function& f) const {
    set(_routes, new function_handler(f));
}

void path_description::unset(routes& _routes) const {
    if (path_params.size() == 0) {
        _routes.drop(operations.method, path);
    } else {
        auto rule = _routes.del_cookie(_cookie, operations.method);
        delete rule;
    }
}

path_description::path_description(const sstring& path, operation_type method,
        const sstring& nickname,
        const std::vector<std::pair<sstring, bool>>& path_parameters,
        const std::vector<sstring>& mandatory_params)
        : path(path), operations(method, nickname) {

    for (auto man : mandatory_params) {
        push_query_param(parameter{man, parameter_type::unknown, parameter::is_required::yes});
    }
    for (auto& [param, all_path] : path_parameters) {
        pushparam(param, all_path);
    }
}

path_description::path_description(const sstring& path, operation_type method,
        const sstring& nickname,
        const std::initializer_list<path_part>& path_parameters,
        const std::initializer_list<parameter>& query_parameters)
        : path(path)
        , operations(method, nickname)
        , path_params(path_parameters.begin(), path_parameters.end())
        , query_params(query_parameters.begin(), query_parameters.end())
{}

path_description::path_description(const sstring& path, operation_type method,
        const sstring& nickname,
        const std::initializer_list<path_part>& path_parameters,
        const std::vector<sstring>& mandatory_params)
        : path(path), operations(method, nickname) {

    for (auto man : mandatory_params) {
        push_query_param(parameter{man, parameter_type::unknown, parameter::is_required::yes});
    }
    for (auto param : path_parameters) {
        path_params.push_back(param);
    }
}

}

}
